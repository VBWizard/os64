// df.c — report mounted-filesystem space from the mount table's text file.
//
// /sys/mounts is the interface: df needs no private statfs syscall and no
// filesystem-specific knowledge. Rows without a space answer (the synthetic
// filesystems today) remain visible with dashes; absence is useful mount-table
// information and must not be restated as zero capacity.

#include "os64/os64.h"
#include "os64/slurp.h"

#define DF_MOUNTS_CAP 8192
#define DF_MAX_MOUNTS 64
#define DF_FIELD_COUNT 12

typedef struct {
    const char *prefix;
    const char *fstype;
    const char *device;
    const char *part;
    const char *name;
    uint64_t total;
    uint64_t free;
    bool hasSpace;
    char filesystem[64];
} df_mount_t;

// Keep the report rows out of the 16 KiB user stack. Their strings still
// point into the one slurped buffer and remain valid until printing ends.
static df_mount_t mounts[DF_MAX_MOUNTS];

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

// Split one writable report row in place. The producer promises whitespace
// columns, so retaining exactly twelve fields also makes a format change fail
// visibly instead of quietly assigning totals to the wrong headings.
static int32_t split_fields(char *line, char **fields)
{
    int32_t count = 0;
    char *p = line;

    for (;;)
    {
        while (is_space(*p))
            p++;
        if (*p == '\0')
            return count;
        if (count == DF_FIELD_COUNT)
            return DF_FIELD_COUNT + 1;

        fields[count++] = p;
        while (*p != '\0' && !is_space(*p))
            p++;
        if (*p != '\0')
            *p++ = '\0';
    }
}

static bool parse_mount(char *line, df_mount_t *mount)
{
    char *fields[DF_FIELD_COUNT];
    if (split_fields(line, fields) != DF_FIELD_COUNT)
        return false;

    mount->prefix = fields[0];
    mount->fstype = fields[1];
    mount->device = fields[2];
    mount->part = fields[3];
    mount->name = fields[4];

    bool noTotal = os64_streq(fields[8], "-");
    bool noFree = os64_streq(fields[9], "-");
    if (noTotal != noFree)
        return false;

    mount->hasSpace = !noTotal;
    if (mount->hasSpace &&
        (!os64_parse_u64(fields[8], &mount->total) ||
         !os64_parse_u64(fields[9], &mount->free) ||
         mount->free > mount->total))
        return false;

    size_t wanted;
    if (!os64_streq(mount->name, "-"))
        wanted = os64_strcopy(mount->filesystem, sizeof(mount->filesystem),
                              mount->name);
    else if (!os64_streq(mount->device, "-") &&
             !os64_streq(mount->part, "-"))
        wanted = (size_t)os64_snprintf(mount->filesystem,
                                       sizeof(mount->filesystem), "%s:%s",
                                       mount->device, mount->part);
    else
        wanted = os64_strcopy(mount->filesystem, sizeof(mount->filesystem),
                              mount->fstype);

    return wanted < sizeof(mount->filesystem);
}

static uint32_t use_percent(uint64_t used, uint64_t total)
{
    if (total == 0)
        return 0;

    // Find ceil(used * 100 / total) without multiplying either 64-bit input.
    // floor(percent * total / 100) is safe in the factored form below.
    for (uint32_t percent = 0; percent < 100; percent++)
    {
        uint64_t threshold = (total / 100) * percent +
                             ((total % 100) * percent) / 100;
        if (threshold >= used)
            return percent;
    }
    return 100;
}

static void format_amount(uint64_t bytes, bool humanReadable,
                          char *out, size_t capacity)
{
    if (humanReadable)
    {
        os64_format_binary_size(bytes, out, capacity);
        return;
    }

    // The default follows familiar df output and names the unit in its
    // heading. Round partial KiB upward so nonzero space never prints as 0.
    uint64_t kib = bytes / 1024 + (bytes % 1024 != 0);
    os64_snprintf(out, capacity, "%lu", kib);
}

static void print_mount(const df_mount_t *mount, bool humanReadable,
                        bool printType, int32_t filesystemWidth,
                        int32_t typeWidth)
{
    char size[32] = "-";
    char used[32] = "-";
    char available[32] = "-";
    char percent[8] = "-";

    if (mount->hasSpace)
    {
        uint64_t usedBytes = mount->total - mount->free;
        format_amount(mount->total, humanReadable, size, sizeof(size));
        format_amount(usedBytes, humanReadable, used, sizeof(used));
        format_amount(mount->free, humanReadable, available, sizeof(available));
        os64_snprintf(percent, sizeof(percent), "%u%%",
                      use_percent(usedBytes, mount->total));
    }

    os64_printf("%-*s ", filesystemWidth, mount->filesystem);
    if (printType)
        os64_printf("%-*s ", typeWidth, mount->fstype);
    os64_printf("%10s %10s %10s %4s %s\n",
                size, used, available, percent, mount->prefix);
}

int main(int argc, char **argv)
{
    bool humanReadable = false;
    bool printType = false;
    const os64_optspec_t specs[] = {
        {'h', "human-readable", false, "print sizes in powers of 1024",
         .flag = &humanReadable},
        {'T', "print-type", false, "print each filesystem's type",
         .flag = &printType}
    };
    os64_args_t args = {0};
    os64_args_init(&args, argc, argv, specs, 2);
    args.about = "Report space usage for every mounted filesystem.";
    args.details = "Reads /sys/mounts; filesystems without space accounting are shown with dashes.";

    int32_t parsed = os64_args_parse(&args, "df [-hT]", NULL, 0);
    if (parsed == OS64_ARG_HELP)
        return 0;
    if (parsed < 0)
        return 2;

    uint8_t *contents = NULL;
    size_t length = 0;
    os64_slurp_status_t status = os64_slurp("/sys/mounts", DF_MOUNTS_CAP,
                                            &contents, &length);
    if (status != OS64_SLURP_OK)
    {
        os64_hprintf(OS64_STDERR, "df: /sys/mounts: %s\n",
                     os64_slurp_status_name(status));
        return 1;
    }

    int32_t mountCount = 0;
    uint32_t lineNumber = 0;
    char *cursor = (char *)contents;
    while ((size_t)(cursor - (char *)contents) < length)
    {
        char *line = cursor;
        while ((size_t)(cursor - (char *)contents) < length && *cursor != '\n')
            cursor++;
        if ((size_t)(cursor - (char *)contents) < length)
            *cursor++ = '\0';
        lineNumber++;

        char *first = line;
        while (is_space(*first))
            first++;
        if (*first == '\0' || *first == '#')
            continue;
        if (mountCount == DF_MAX_MOUNTS ||
            !parse_mount(first, &mounts[mountCount]))
        {
            os64_hprintf(OS64_STDERR,
                         "df: malformed /sys/mounts row at line %u\n",
                         lineNumber);
            os64_free(contents);
            return 1;
        }
        mountCount++;
    }

    int32_t filesystemWidth = (int32_t)os64_strlen("Filesystem");
    int32_t typeWidth = (int32_t)os64_strlen("Type");
    for (int32_t i = 0; i < mountCount; i++)
    {
        int32_t width = (int32_t)os64_strlen(mounts[i].filesystem);
        if (width > filesystemWidth)
            filesystemWidth = width;
        width = (int32_t)os64_strlen(mounts[i].fstype);
        if (width > typeWidth)
            typeWidth = width;
    }

    os64_printf("%-*s ", filesystemWidth, "Filesystem");
    if (printType)
        os64_printf("%-*s ", typeWidth, "Type");
    os64_printf("%10s %10s %10s %4s %s\n",
                humanReadable ? "Size" : "1K-blocks",
                "Used", "Available", "Use%", "Mounted on");
    for (int32_t i = 0; i < mountCount; i++)
        print_mount(&mounts[i], humanReadable, printType,
                    filesystemWidth, typeWidth);

    os64_free(contents);
    return 0;
}
