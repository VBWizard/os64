#include "os64/os64.h"

#define DEFAULT_TAIL_LINES 20

static char buf[32768];

static int write_all(int32_t handle, const char *data, size_t len)
{
    size_t written = 0;

    while (written < len)
    {
        int64_t n = os64_write(handle, data + written, len - written);
        if (n <= 0)
            return -1;
        written += (size_t)n;
    }

    return 0;
}

static bool parse_line_count(const char *value, uint64_t *lineCount)
{
    if (value == NULL || *value == '\0')
        return false;

    uint64_t result = 0;
    for (const char *p = value; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
            return false;

        uint64_t digit = (uint64_t)(*p - '0');
        if (result > (UINT64_MAX - digit) / 10)
            return false;
        result = result * 10 + digit;
    }

    *lineCount = result;
    return true;
}

static int64_t find_tail_start(int32_t handle, uint64_t fileSize,
                               uint64_t lineCount)
{
    if (lineCount == 0)
        return (int64_t)fileSize;

    uint64_t blockEnd = fileSize;
    uint64_t newlines = 0;

    while (blockEnd > 0)
    {
        size_t blockSize = blockEnd > sizeof(buf) ? sizeof(buf) : (size_t)blockEnd;
        uint64_t blockStart = blockEnd - blockSize;

        if (os64_seek(handle, (int64_t)blockStart, OS64_SEEK_SET) < 0)
            return -1;

        size_t filled = 0;
        while (filled < blockSize)
        {
            int64_t n = os64_read(handle, buf + filled, blockSize - filled);
            if (n < 0)
                return -1;
            if (n == 0)
                break;
            filled += (size_t)n;
        }

        for (size_t i = filled; i > 0; i--)
        {
            uint64_t absolute = blockStart + i - 1;

            if (buf[i - 1] != '\n' || absolute == fileSize - 1)
                continue;

            newlines++;
            if (newlines == lineCount)
                return (int64_t)(absolute + 1);
        }

        blockEnd = blockStart;
    }

    return 0;
}

static int print_tail(int32_t handle, uint64_t fileSize, uint64_t lineCount)
{
    int64_t start = find_tail_start(handle, fileSize, lineCount);
    if (start < 0 || os64_seek(handle, start, OS64_SEEK_SET) < 0)
        return -1;

    int64_t n;
    while ((n = os64_read(handle, buf, sizeof(buf))) > 0)
        if (write_all(OS64_STDOUT, buf, (size_t)n) < 0)
            return -1;

    return n < 0 ? -1 : 0;
}

static int refresh_file(int32_t *handle, const char *path, int64_t position)
{
    int32_t replacement = (int32_t)os64_open(path, "r");
    if (replacement < 0)
        return -1;

    if (os64_seek(replacement, position, OS64_SEEK_SET) < 0)
    {
        os64_close(replacement);
        return -1;
    }

    os64_close(*handle);
    *handle = replacement;
    return 0;
}

static int follow_file(int32_t *handle, const char *path)
{
    for (;;)
    {
        int64_t n = os64_read(*handle, buf, sizeof(buf));
        if (n < 0)
            return -1;
        if (n > 0)
        {
            if (write_all(OS64_STDOUT, buf, (size_t)n) < 0)
                return -1;
            continue;
        }

        os64_dirent_t entry;
        int64_t position = os64_seek(*handle, 0, OS64_SEEK_CUR);
        if (position < 0 || os64_stat(path, &entry) < 0)
            return -1;

        if (entry.flags & OS64_DE_DIR)
            return -1;

        if (entry.size != (uint64_t)position)
        {
            int64_t resumeAt = entry.size < (uint64_t)position ? 0 : position;
            if (refresh_file(handle, path, resumeAt) < 0)
                return -1;
            if (resumeAt == 0)
                os64_printf("*** file truncated ***\n");
            continue;
        }

        os64_sleep(100);
    }
}

int main(int argc, char **argv)
{
    int returnCode = 0;
    bool optFollow = false;
    uint64_t lineCount = DEFAULT_TAIL_LINES;
    const char *lineCountValue = NULL;
    const char *fileToTail = NULL;
    int32_t fileHandle = -1;
    os64_dirent_t statEntry;
    os64_args_t args = {0};
    const os64_optspec_t specs[] = {
        {'f', "follow", false, "stay attached and print data appended to FILE", .flag = &optFollow},
        {'n', "lines", true, "how many lines to show",
         .value_out = &lineCountValue, .numeric_alias = true}
    };

    os64_args_init(&args, argc, argv, specs, 2);
    args.about = "Print the last lines of FILE to standard output.";
    args.details = "With --follow, wait for and print data appended to FILE.";

    int32_t parsed = os64_args_parse(&args, "tail [-f] [-n NUM] FILE", &fileToTail, 1);
    if (parsed == OS64_ARG_HELP)
        os64_exit(0);
    if (parsed != 1)
        os64_exit(2);

    if (lineCountValue != NULL && !parse_line_count(lineCountValue, &lineCount))
    {
        os64_hprintf(OS64_STDERR, "tail: invalid line count: %s\n", lineCountValue);
        os64_exit(2);
    }

    if (os64_stat(fileToTail, &statEntry) < 0)
    {
        os64_hprintf(OS64_STDERR, "tail: could not stat %s\n", fileToTail);
        returnCode = 3;
    }
    else if (statEntry.flags & OS64_DE_DIR)
    {
        os64_hprintf(OS64_STDERR, "tail: cannot read a directory: %s\n", fileToTail);
        returnCode = 4;
    }
    else
    {
        fileHandle = (int32_t)os64_open(fileToTail, "r");
        if (fileHandle < 0)
        {
            os64_hprintf(OS64_STDERR, "tail: unable to open %s\n", fileToTail);
            returnCode = 5;
        }
    }

    if (!returnCode && print_tail(fileHandle, statEntry.size, lineCount) < 0)
    {
        os64_hprintf(OS64_STDERR, "tail: error reading %s\n", fileToTail);
        returnCode = 6;
    }

    if (!returnCode && optFollow && follow_file(&fileHandle, fileToTail) < 0)
    {
        os64_hprintf(OS64_STDERR, "tail: error following %s\n", fileToTail);
        returnCode = 7;
    }

    if (fileHandle >= 0)
        os64_close(fileHandle);

    os64_exit(returnCode);
}
