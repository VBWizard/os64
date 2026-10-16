#include "os64/os64.h"

#define LS_PATH_MAX 512
#define LS_INITIAL_ENTRIES 64
// The width to lay out for when the terminal cannot be asked.
#define LS_DEFAULT_WIDTH 80
// GNU/POSIX ls's "six months" display boundary: half an average Gregorian
// year. Recent files show HH:MM; older and future-dated files show the year.
#define LS_RECENT_SECONDS (31556952 / 2)

typedef enum {
    LS_SORT_NAME,
    LS_SORT_TIME,
    LS_SORT_SIZE
} ls_sort_t;

// Name order is BYTE order, as in Unix's C locale: every capital sorts before
// every lowercase letter, and nothing depends on a locale os64 does not have.
static int compare_names(const char *first, const char *second)
{
    while (*first != '\0' && *first == *second)
    {
        first++;
        second++;
    }
    return (unsigned char)*first - (unsigned char)*second;
}

// Return negative when first belongs before second. Names are the default
// order; time and size follow the familiar ls rule (newest/largest first),
// and names break their ties deterministically.
static int compare_entries(const os64_dirent_t *first,
                           const os64_dirent_t *second, ls_sort_t sort)
{
    if (sort == LS_SORT_TIME && first->mtime != second->mtime)
        return first->mtime > second->mtime ? -1 : 1;
    if (sort == LS_SORT_SIZE && first->size != second->size)
        return first->size > second->size ? -1 : 1;
    return compare_names(first->name, second->name);
}

static void sift_down(os64_dirent_t *entries, int32_t root,
                      int32_t count, ls_sort_t sort)
{
    while (root < count / 2)
    {
        int32_t child = root * 2 + 1;
        if (child >= count)
            break;
        if (child + 1 < count &&
            compare_entries(&entries[child], &entries[child + 1], sort) < 0)
            child++;
        if (compare_entries(&entries[root], &entries[child], sort) >= 0)
            break;
        os64_dirent_t tmp = entries[root];
        entries[root] = entries[child];
        entries[child] = tmp;
        root = child;
    }
}

static void sort_entries(os64_dirent_t *entries, int32_t count, ls_sort_t sort)
{
    // Heap sort keeps name, time and size ordering practical as a directory
    // grows, without needing a second copy of its entries.
    for (int32_t root = count / 2 - 1; root >= 0; root--)
        sift_down(entries, root, count, sort);
    for (int32_t end = count - 1; end > 0; end--)
    {
        os64_dirent_t tmp = entries[0];
        entries[0] = entries[end];
        entries[end] = tmp;
        sift_down(entries, 0, end, sort);
    }
}

static void format_size(uint64_t size, bool humanReadable,
                        char *out, size_t capacity)
{
    if (humanReadable)
        os64_format_binary_size(size, out, capacity);
    else
        os64_snprintf(out, capacity, "%lu", size);
}

static void format_mtime(uint64_t mtime, int64_t currentEpoch,
                         bool fullTime, char *out, size_t capacity)
{
    if (mtime == 0)
    {
        os64_snprintf(out, capacity, "-");
        return;
    }

    os64_date_t date = {0};
    if (os64_localtime((int64_t)mtime, &date) < 0)
    {
        os64_snprintf(out, capacity, "-");
        return;
    }

    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    const char *month = date.month >= 1 && date.month <= 12
        ? months[date.month - 1] : "???";

    if (fullTime)
    {
        // dirent mtimes carry whole seconds. Print every bit of precision we
        // own, and no decorative .000000000 that would pretend otherwise.
        os64_snprintf(out, capacity, "%04d-%02d-%02d %02d:%02d:%02d",
                      date.year, date.month, date.day,
                      date.hour, date.minute, date.second);
        return;
    }

    bool recent = (int64_t)mtime <= currentEpoch &&
                  currentEpoch - (int64_t)mtime < LS_RECENT_SECONDS;
    if (recent)
        os64_snprintf(out, capacity, "%s %2d %02d:%02d",
                      month, date.day, date.hour, date.minute);
    else
        os64_snprintf(out, capacity, "%s %2d  %04d",
                      month, date.day, date.year);
}

typedef struct {
    bool longMode;
    bool humanReadable;
    bool fullTime;
    int64_t currentEpoch;
    int32_t termWidth;
} ls_options_t;

// A name is measured in BYTES, and on os64's terminals that is also its
// width in cells: the console draws one glyph per byte (Latin-1 or CP437),
// so even a UTF-8 name occupies exactly as many cells as it has bytes.
static int32_t name_width(const os64_dirent_t *entry)
{
    return (int32_t)os64_strlen(entry->name);
}

// The columns are measured from what is being listed, never fixed, and the
// name is never clipped: a listing that shortens a name shows a file that
// does not exist.
//
// Short form fills DOWN the columns (the Unix reading order: the eye runs
// down a column, so the listing's order survives the layout) and chooses the
// most columns whose total width fits.
// Each column is as wide as its longest name plus a two-space gutter, except
// the last, which is never padded. The fit is strictly narrower than the
// terminal, so no row ever depends on whether a write to the last cell wraps.
// A name wider than the terminal gets a row to itself and wraps as it must.
static void print_short_listing(const os64_dirent_t *entries, int32_t count,
                                int32_t termWidth)
{
    const int32_t gutter = 2;
    int32_t rows = count;   // one column: the fallback that always "fits"

    for (int32_t columns = count; columns > 1; columns--)
    {
        int32_t tryRows = (count + columns - 1) / columns;
        // Filling down with tryRows rows may need fewer columns than asked;
        // measure the layout that would actually print.
        int32_t used = (count + tryRows - 1) / tryRows;
        int32_t total = 0;
        for (int32_t column = 0; column < used && total < termWidth; column++)
        {
            int32_t widest = 0;
            for (int32_t row = 0; row < tryRows; row++)
            {
                int32_t index = column * tryRows + row;
                if (index < count && name_width(&entries[index]) > widest)
                    widest = name_width(&entries[index]);
            }
            total += widest + (column + 1 < used ? gutter : 0);
        }
        if (total < termWidth)
        {
            rows = tryRows;
            break;
        }
    }

    int32_t columns = (count + rows - 1) / rows;
    for (int32_t row = 0; row < rows; row++)
    {
        for (int32_t column = 0; column < columns; column++)
        {
            int32_t index = column * rows + row;
            if (index >= count)
                break;
            // The next entry to the right decides whether this cell pads:
            // a row's last name ends the line with no trailing spaces.
            bool last = column + 1 >= columns || index + rows >= count;
            if (last)
            {
                os64_printf("%s", entries[index].name);
                continue;
            }
            int32_t widest = 0;
            for (int32_t r = 0; r < rows; r++)
            {
                int32_t other = column * rows + r;
                if (other < count && name_width(&entries[other]) > widest)
                    widest = name_width(&entries[other]);
            }
            os64_printf("%-*s", widest + gutter, entries[index].name);
        }
        os64_printf("\n");
    }
}

// Long form measures every column but the name, and puts the name LAST —
// the one column of unbounded width goes where nothing follows it, so it is
// never clipped and nothing after it is pushed out of line.
static void print_long_listing(const os64_dirent_t *entries, int32_t count,
                               const ls_options_t *options)
{
    int32_t kindWidth = 0;
    int32_t sizeWidth = 0;
    int32_t timeWidth = 0;
    char text[32];

    for (int32_t i = 0; i < count; i++)
    {
        int32_t width = (entries[i].flags & OS64_DE_DIR) ? 5 : 6;
        if (width > kindWidth)
            kindWidth = width;
        format_size(entries[i].size, options->humanReadable,
                    text, sizeof(text));
        width = (int32_t)os64_strlen(text);
        if (width > sizeWidth)
            sizeWidth = width;
        format_mtime(entries[i].mtime, options->currentEpoch,
                     options->fullTime, text, sizeof(text));
        width = (int32_t)os64_strlen(text);
        if (width > timeWidth)
            timeWidth = width;
    }

    for (int32_t i = 0; i < count; i++)
    {
        char size[32];
        char mtime[32];
        format_size(entries[i].size, options->humanReadable,
                    size, sizeof(size));
        format_mtime(entries[i].mtime, options->currentEpoch,
                     options->fullTime, mtime, sizeof(mtime));
        os64_printf("%-*s  %*s  %*s  %s\n", kindWidth,
                    (entries[i].flags & OS64_DE_DIR) ? "<dir>" : "<file>",
                    sizeWidth, size, timeWidth, mtime, entries[i].name);
    }
}

static void print_listing(const os64_dirent_t *entries, int32_t count,
                          const ls_options_t *options)
{
    if (count == 0)
        return;
    if (options->longMode)
        print_long_listing(entries, count, options);
    else
        print_short_listing(entries, count, options->termWidth);
}

static int32_t get_directory_listing(const char *path,
                                     os64_dirent_t **entries,
                                     int32_t *entryCount)
{
    int32_t directory = (int32_t)os64_opendir(path);
    if (directory < 0)
    {
        os64_hprintf(OS64_STDERR, "ls: cannot open '%s'\n", path);
        return 1;
    }

    int32_t capacity = 0;
    int64_t result = 0;
    bool noMemory = false;
    bool tooMany = false;
    os64_dirent_t next;
    while ((result = os64_readdir(directory, &next)) == 1)
    {
        if (*entryCount == capacity)
        {
            if (capacity > INT32_MAX / 2)
            {
                tooMany = true;
                break;
            }
            int32_t grown = capacity ? capacity * 2 : LS_INITIAL_ENTRIES;
            os64_dirent_t *resized = os64_realloc(*entries,
                                                  (size_t)grown * sizeof(**entries));
            if (resized == NULL)
            {
                noMemory = true;
                break;
            }
            *entries = resized;
            capacity = grown;
        }
        (*entries)[(*entryCount)++] = next;
    }

    int32_t returnCode = 0;
    if (noMemory)
    {
        os64_hprintf(OS64_STDERR, "ls: out of memory listing '%s'\n", path);
        returnCode = 1;
    }
    else if (tooMany)
    {
        os64_hprintf(OS64_STDERR, "ls: too many entries in '%s'\n", path);
        returnCode = 1;
    }
    else if (result < 0)
    {
        os64_hprintf(OS64_STDERR, "ls: cannot read directory '%s'\n", path);
        returnCode = 1;
    }
    if (os64_close(directory) < 0)
    {
        os64_hprintf(OS64_STDERR, "ls: cannot close directory '%s'\n", path);
        returnCode = 1;
    }
    return returnCode;
}

int main(int argc, char **argv)
{
    bool longMode = false;
    bool humanReadable = false;
    bool sortTime = false;
    bool sortSize = false;
    bool fullTime = false;
    os64_args_t args = {0};
    const char **paths = os64_malloc((size_t)(argc + 1) * sizeof(*paths));
    if (paths == NULL)
    {
        os64_hprintf(OS64_STDERR, "ls: out of memory\n");
        return 1;
    }
    const os64_optspec_t specs[] = {
        {'l', "long", false, "one entry per line with size and modification time",
         .flag = &longMode},
        {'h', "human-readable", false, "show sizes in powers of 1024",
         .flag = &humanReadable},
        {'t', "time", false, "sort by modification time, newest first",
         .flag = &sortTime},
        {'S', "size", false, "sort by file size, largest first",
         .flag = &sortSize},
        {'\0', "full-time", false, "show complete modification timestamps",
         .flag = &fullTime}
    };

    os64_args_init(&args, argc, argv, specs, 5);
    args.about = "List directories or files.";
    args.details = "Entries are listed by name unless -t or -S asks otherwise;\n"
                   "when both are present, -t takes precedence.";

    int32_t positionals = os64_args_parse(
        &args, "ls [-lhSt] [--full-time] [PATH ...]", paths, argc + 1);
    if (positionals == OS64_ARG_HELP)
    {
        os64_free(paths);
        return 0;
    }
    if (positionals < 0)
    {
        os64_free(paths);
        return 2;
    }

    if (fullTime)
        longMode = true;

    char cwd[LS_PATH_MAX];
    if (positionals == 0)
    {
        if (os64_getcwd(cwd, sizeof(cwd)) < 0)
        {
            os64_hprintf(OS64_STDERR, "ls: cannot get current directory\n");
            os64_free(paths);
            return 1;
        }
        paths[0] = cwd;
        positionals = 1;
    }

    os64_time_t now = {0};
    if (longMode)
        os64_time(&now);   // failure leaves epoch 0: safely choose year form

    ls_options_t options = {
        .longMode = longMode,
        .humanReadable = humanReadable,
        .fullTime = fullTime,
        .termWidth = LS_DEFAULT_WIDTH
    };
    options.currentEpoch = now.epoch;
    os64_tty_info_t tty = {0};
    if (os64_tty_read(&tty) == 0 && tty.cols > 0)
        options.termWidth = (int32_t)tty.cols;

    ls_sort_t sort = sortTime ? LS_SORT_TIME :
                     sortSize ? LS_SORT_SIZE : LS_SORT_NAME;
    int32_t returnCode = 0;
    bool printedOperand = false;
    for (int32_t operand = 0; operand < positionals; operand++)
    {
        const char *path = paths[operand];
        os64_dirent_t statEntry = {0};
        if (os64_stat(path, &statEntry) < 0)
        {
            os64_hprintf(OS64_STDERR, "ls: cannot stat '%s'\n", path);
            returnCode = 1;
            continue;
        }

        if ((statEntry.flags & OS64_DE_DIR) == 0)
        {
            print_listing(&statEntry, 1, &options);
            printedOperand = true;
            continue;
        }

        if (positionals > 1)
        {
            if (printedOperand)
                os64_printf("\n");
            os64_printf("%s:\n", path);
        }

        int32_t entryCount = 0;
        os64_dirent_t *dirEntries = NULL;
        if (get_directory_listing(path, &dirEntries, &entryCount) != 0)
        {
            os64_free(dirEntries);
            returnCode = 1;
            printedOperand = true;
            continue;
        }
        sort_entries(dirEntries, entryCount, sort);
        print_listing(dirEntries, entryCount, &options);
        os64_free(dirEntries);
        printedOperand = true;
    }
    os64_free(paths);
    return returnCode;
}
