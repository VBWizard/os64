// grep.c — stream lines that match a pattern.

#include "os64/os64.h"

#define GREP_LINE_MAX 4096
#define GREP_PATH_MAX 512
#define GREP_MAX_FILES 128
// 16 task handles, three standard handles, and one spare for honest failure.
#define GREP_MAX_DEPTH 11

typedef struct {
    const char *pattern;
    bool insensitive;
    bool extended;
    bool invert;
    bool lineNumbers;
    bool recursive;
    bool showFilename;
    bool found;
    bool error;
} grep_options_t;

static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool segment_matches(const char *line, const char *pattern,
                            size_t patternLength, bool insensitive)
{
    if (patternLength == 0)
        return true;

    for (size_t start = 0; line[start] != '\0'; start++)
    {
        size_t i = 0;
        while (i < patternLength && line[start + i] != '\0')
        {
            char a = line[start + i];
            char b = pattern[i];
            if (insensitive)
            {
                a = ascii_lower(a);
                b = ascii_lower(b);
            }
            if (a != b)
                break;
            i++;
        }
        if (i == patternLength)
            return true;
    }
    return false;
}

static bool line_matches(const char *line, const grep_options_t *options)
{
    if (!options->extended)
        return segment_matches(line, options->pattern,
                               os64_strlen(options->pattern),
                               options->insensitive);

    // The first useful slice of -E: literal alternatives separated by |.
    // No other regexp metacharacters are claimed yet.
    const char *alternative = options->pattern;
    for (const char *p = options->pattern;; p++)
    {
        if (*p == '|' || *p == '\0')
        {
            if (segment_matches(line, alternative, (size_t)(p - alternative),
                                options->insensitive))
                return true;
            if (*p == '\0')
                break;
            alternative = p + 1;
        }
    }
    return false;
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

static void print_match(const char *path, uint64_t lineNumber,
                        const char *line, grep_options_t *options)
{
    if (options->showFilename)
        os64_printf("%s:", path);
    if (options->lineNumbers)
        os64_printf("%lu:", lineNumber);

    if (write_all(OS64_STDOUT, line, os64_strlen(line)) < 0 ||
        write_all(OS64_STDOUT, "\n", 1) < 0)
        options->error = true;
}

static void grep_handle(int32_t handle, const char *path,
                        grep_options_t *options)
{
    char line[GREP_LINE_MAX];
    uint64_t lineNumber = 0;
    int64_t result;

    while ((result = os64_readline(handle, line, sizeof(line))) == 1)
    {
        lineNumber++;
        bool matched = line_matches(line, options);
        if (options->invert)
            matched = !matched;
        if (matched)
        {
            options->found = true;
            print_match(path, lineNumber, line, options);
        }
    }

    if (result < 0)
    {
        os64_hprintf(OS64_STDERR, "grep: error reading '%s'\n", path);
        options->error = true;
    }
}

static void grep_path(const char *path, grep_options_t *options, uint32_t depth)
{
    os64_dirent_t entry = {0};
    if (os64_stat(path, &entry) < 0)
    {
        os64_hprintf(OS64_STDERR, "grep: cannot access '%s'\n", path);
        options->error = true;
        return;
    }

    if ((entry.flags & OS64_DE_DIR) == 0)
    {
        int32_t handle = (int32_t)os64_open(path, "r");
        if (handle < 0)
        {
            os64_hprintf(OS64_STDERR, "grep: cannot open '%s'\n", path);
            options->error = true;
            return;
        }
        grep_handle(handle, path, options);
        os64_close(handle);
        return;
    }

    if (!options->recursive)
    {
        os64_hprintf(OS64_STDERR, "grep: '%s' is a directory (use -r)\n", path);
        options->error = true;
        return;
    }
    if (depth >= GREP_MAX_DEPTH)
    {
        os64_hprintf(OS64_STDERR, "grep: maximum directory depth reached at '%s'\n", path);
        options->error = true;
        return;
    }

    int32_t directory = (int32_t)os64_opendir(path);
    if (directory < 0)
    {
        os64_hprintf(OS64_STDERR, "grep: cannot open directory '%s'\n", path);
        options->error = true;
        return;
    }

    int64_t result;
    os64_dirent_t child = {0};
    while ((result = os64_readdir(directory, &child)) == 1)
    {
        if (os64_streq(child.name, ".") || os64_streq(child.name, ".."))
            continue;

        char childPath[GREP_PATH_MAX];
        size_t pathLength = os64_strlen(path);
        const char *separator = pathLength > 0 && path[pathLength - 1] == '/' ? "" : "/";
        int32_t wanted = os64_snprintf(childPath, sizeof(childPath), "%s%s%s",
                                      path, separator, child.name);
        if (wanted < 0 || (size_t)wanted >= sizeof(childPath))
        {
            os64_hprintf(OS64_STDERR, "grep: path too long beneath '%s'\n", path);
            options->error = true;
            continue;
        }
        grep_path(childPath, options, depth + 1);
    }

    if (result < 0)
    {
        os64_hprintf(OS64_STDERR, "grep: cannot read directory '%s'\n", path);
        options->error = true;
    }
    os64_close(directory);
}

int main(int argc, char **argv)
{
    grep_options_t options = {0};
    const char *positionals[GREP_MAX_FILES + 1];
    os64_args_t args = {0};
    const os64_optspec_t specs[] = {
        {'i', "ignore-case", false, "ignore ASCII case distinctions",
         .flag = &options.insensitive},
        {'E', "extended-regexp", false, "allow literal PATTERN alternatives with |",
         .flag = &options.extended},
        {'v', "invert-match", false, "select lines that do not match",
         .flag = &options.invert},
        {'r', "recursive", false, "search directories recursively",
         .flag = &options.recursive},
        {'n', "line-number", false, "print the line number with each match",
         .flag = &options.lineNumbers}
    };

    os64_args_init(&args, argc, argv, specs, 5);
    args.about = "Print lines that match PATTERN.";
    args.details = "-E currently supports literal alternatives separated by |.";
    int32_t positionalCount = os64_args_parse(
        &args, "grep [-Eivnr] PATTERN [FILE ...]", positionals,
        GREP_MAX_FILES + 1);
    if (positionalCount == OS64_ARG_HELP)
        return 0;
    if (positionalCount < 1)
    {
        if (positionalCount != OS64_ARG_ERROR)
        {
            os64_hprintf(OS64_STDERR, "grep: missing pattern\n");
            os64_args_help(&args, "grep [-Eivnr] PATTERN [FILE ...]");
        }
        return 2;
    }

    options.pattern = positionals[0];
    int32_t fileCount = positionalCount - 1;
    options.showFilename = fileCount > 1 || options.recursive;

    if (fileCount == 0 && options.recursive)
        grep_path(".", &options, 0);
    else if (fileCount == 0)
        grep_handle(OS64_STDIN, "(standard input)", &options);
    else
    {
        for (int32_t i = 0; i < fileCount; i++)
        {
            const char *path = positionals[i + 1];
            if (os64_streq(path, "-"))
                grep_handle(OS64_STDIN, "(standard input)", &options);
            else
                grep_path(path, &options, 0);
        }
    }

    if (options.error)
        return 2;
    return options.found ? 0 : 1;
}
