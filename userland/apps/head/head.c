#include "os64/os64.h"

#define DEFAULT_HEAD_LINES 20
#define HEAD_BUF_SIZE 32768

static char buf[HEAD_BUF_SIZE];

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

static int write_all(int32_t handle, const char *data, size_t length)
{
    size_t written = 0;

    while (written < length)
    {
        int64_t n = os64_write(handle, data + written, length - written);
        if (n <= 0)
            return -1;
        written += (size_t)n;
    }

    return 0;
}

static int print_head(int32_t handle, uint64_t lineCount)
{
    uint64_t lines = 0;

    while (lines < lineCount)
    {
        int64_t n = os64_read(handle, buf, sizeof(buf));
        if (n < 0)
            return -1;
        if (n == 0)
            return 0;

        size_t length = (size_t)n;
        for (size_t i = 0; i < (size_t)n; i++)
        {
            if (buf[i] == '\n' && ++lines == lineCount)
            {
                length = i + 1;
                break;
            }
        }

        // A block read may have passed the requested final line. Restore the
        // shared offset for seekable input so a following reader starts at the
        // first byte we did not print. Pipes and consoles simply reject this
        // seek, in which case their existing streaming behavior is unchanged.
        if (length < (size_t)n)
            os64_seek(handle, -(int64_t)((size_t)n - length), OS64_SEEK_CUR);

        if (write_all(OS64_STDOUT, buf, length) < 0)
            return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    uint64_t lineCount = DEFAULT_HEAD_LINES;
    const char *lineCountValue = NULL;
    const char *fileToHead = NULL;
    int64_t fileHandle = OS64_STDIN;
    int32_t returnCode = 0;
    os64_args_t args = {0};
    const os64_optspec_t specs[] = {
        {'n', "lines", true, "how many lines to show",
         .value_out = &lineCountValue, .numeric_alias = true}
    };

    os64_args_init(&args, argc, argv, specs, 1);
    args.about = "Print the first lines of FILE to standard output.";
    args.details = "With no FILE, or when FILE is -, read standard input.";

    int32_t parsed = os64_args_parse(&args, "head [-n NUM] [FILE]",
                                     &fileToHead, 1);
    if (parsed == OS64_ARG_HELP)
        os64_exit(0);
    if (parsed < 0)
        os64_exit(2);

    if (lineCountValue != NULL && !parse_line_count(lineCountValue, &lineCount))
    {
        os64_hprintf(OS64_STDERR, "head: invalid line count: %s\n",
                     lineCountValue);
        os64_exit(2);
    }

    bool useStdin = parsed == 0 || os64_streq(fileToHead, "-");
    if (!useStdin)
    {
        os64_dirent_t entry;
        if (os64_stat(fileToHead, &entry) < 0)
        {
            os64_hprintf(OS64_STDERR, "head: could not stat %s\n", fileToHead);
            returnCode = 3;
        }
        else if (entry.flags & OS64_DE_DIR)
        {
            os64_hprintf(OS64_STDERR, "head: cannot read a directory: %s\n",
                         fileToHead);
            returnCode = 4;
        }
        else
        {
            fileHandle = os64_open(fileToHead, "r");
            if (fileHandle < 0)
            {
                os64_hprintf(OS64_STDERR, "head: unable to open %s\n", fileToHead);
                returnCode = 5;
            }
        }
    }

    if (!returnCode && print_head(fileHandle, lineCount) < 0)
    {
        os64_hprintf(OS64_STDERR, "head: error reading %s\n",
                     useStdin ? "standard input" : fileToHead);
        returnCode = 6;
    }

    if (!useStdin && fileHandle >= 0)
        os64_close(fileHandle);

    os64_exit(returnCode);
}
