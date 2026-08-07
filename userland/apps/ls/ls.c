#include "os64/os64.h"

#define MAX_DIR_ENTRIES 512
#define LS_PATH_MAX 512
// GNU/POSIX ls's "six months" display boundary: half an average Gregorian
// year. Recent files show HH:MM; older and future-dated files show the year.
#define LS_RECENT_SECONDS (31556952 / 2)

typedef enum {
    LS_SORT_DIRECTORY,
    LS_SORT_TIME,
    LS_SORT_SIZE
} ls_sort_t;

static os64_dirent_t dirEntries[MAX_DIR_ENTRIES];

static int compare_names(const char *first, const char *second)
{
    while (*first != '\0' && *first == *second)
    {
        first++;
        second++;
    }
    return (unsigned char)*first - (unsigned char)*second;
}

// Return negative when first belongs before second. Time and size follow the
// familiar ls rule (newest/largest first); names break ties deterministically.
static int compare_entries(const os64_dirent_t *first,
                           const os64_dirent_t *second, ls_sort_t sort)
{
    if (sort == LS_SORT_TIME && first->mtime != second->mtime)
        return first->mtime > second->mtime ? -1 : 1;
    if (sort == LS_SORT_SIZE && first->size != second->size)
        return first->size > second->size ? -1 : 1;
    return compare_names(first->name, second->name);
}

static void sort_entries(os64_dirent_t *entries, int32_t count, ls_sort_t sort)
{
    if (sort == LS_SORT_DIRECTORY)
        return;

    // Directory listings are capped at 512 entries. Insertion sort is small,
    // stable, allocation-free, and plenty quick at that honest upper bound.
    for (int32_t i = 1; i < count; i++)
    {
        os64_dirent_t moving = entries[i];
        int32_t j = i;
        while (j > 0 && compare_entries(&moving, &entries[j - 1], sort) < 0)
        {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = moving;
    }
}

static void format_size(uint64_t size, bool humanReadable,
                        char *out, size_t capacity)
{
    if (!humanReadable || size < 1024)
    {
        os64_snprintf(out, capacity, humanReadable ? "%luB" : "%lu", size);
        return;
    }

    static const char *units[] = {"K", "M", "G", "T", "P", "E"};
    uint64_t divisor = 1024;
    int32_t unit = 0;
    while (unit < 5 && size >= divisor * 1024)
    {
        divisor *= 1024;
        unit++;
    }

    uint64_t whole = size / divisor;
    uint64_t tenth = (size % divisor) * 10 / divisor;
    os64_snprintf(out, capacity, "%lu.%lu%s", whole, tenth, units[unit]);
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

static void print_entry(const os64_dirent_t *entry, bool longMode,
                        bool humanReadable, bool fullTime,
                        int64_t currentEpoch)
{
    if (!longMode)
    {
        os64_printf("%-20.19s", entry->name);
        return;
    }

    char size[32];
    char mtime[32];
    format_size(entry->size, humanReadable, size, sizeof(size));
    format_mtime(entry->mtime, currentEpoch, fullTime,
                 mtime, sizeof(mtime));
    int32_t timeWidth = fullTime ? 19 : 12;
    os64_printf("%-32.31s %10s  %*s  %s\n", entry->name, size,
                timeWidth, mtime,
                (entry->flags & OS64_DE_DIR) ? "<dir>" : "<file>");
}

static int32_t get_directory_listing(const char *path,
                                     os64_dirent_t *entries,
                                     int32_t *entryCount)
{
    int32_t directory = (int32_t)os64_opendir(path);
    if (directory < 0)
    {
        os64_hprintf(OS64_STDERR, "ls: cannot open '%s'\n", path);
        return 1;
    }

    int64_t result = 0;
    while (*entryCount < MAX_DIR_ENTRIES &&
           (result = os64_readdir(directory, &entries[*entryCount])) == 1)
        (*entryCount)++;

    // Distinguish an exactly-full listing from a truncated one without ever
    // writing past the array.
    bool tooMany = false;
    if (*entryCount == MAX_DIR_ENTRIES)
    {
        os64_dirent_t extra = {0};
        result = os64_readdir(directory, &extra);
        tooMany = result == 1;
    }

    int32_t returnCode = 0;
    if (result < 0)
    {
        os64_hprintf(OS64_STDERR, "ls: cannot read directory '%s'\n", path);
        returnCode = 1;
    }
    else if (tooMany)
    {
        os64_hprintf(OS64_STDERR,
                     "ls: directory '%s' exceeds the %d-entry limit\n",
                     path, MAX_DIR_ENTRIES);
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
    const char *path = NULL;
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
    args.about = "List a directory or file.";
    args.details = "When both -t and -S are present, -t takes precedence.";

    int32_t positionals = os64_args_parse(
        &args, "ls [-lhSt] [--full-time] [PATH]", &path, 1);
    if (positionals == OS64_ARG_HELP)
        return 0;
    if (positionals < 0)
        return 2;

    if (fullTime)
        longMode = true;

    char cwd[LS_PATH_MAX];
    if (path == NULL)
    {
        if (os64_getcwd(cwd, sizeof(cwd)) < 0)
        {
            os64_hprintf(OS64_STDERR, "ls: cannot get current directory\n");
            return 1;
        }
        path = cwd;
    }

    os64_dirent_t statEntry = {0};
    if (os64_stat(path, &statEntry) < 0)
    {
        os64_hprintf(OS64_STDERR, "ls: cannot stat '%s'\n", path);
        return 1;
    }

    os64_time_t now = {0};
    if (longMode)
        os64_time(&now);   // failure leaves epoch 0: safely choose year form

    if ((statEntry.flags & OS64_DE_DIR) == 0)
    {
        print_entry(&statEntry, longMode, humanReadable, fullTime, now.epoch);
        if (!longMode)
            os64_printf("\n");
        return 0;
    }

    int32_t entryCount = 0;
    if (get_directory_listing(path, dirEntries, &entryCount) != 0)
        return 1;

    ls_sort_t sort = sortTime ? LS_SORT_TIME :
                     sortSize ? LS_SORT_SIZE : LS_SORT_DIRECTORY;
    sort_entries(dirEntries, entryCount, sort);

    for (int32_t i = 0; i < entryCount; i++)
    {
        print_entry(&dirEntries[i], longMode, humanReadable,
                    fullTime, now.epoch);
        if (!longMode && (i + 1) % 5 == 0)
            os64_printf("\n");
    }
    if (!longMode && entryCount % 5 != 0)
        os64_printf("\n");

    return 0;
}
