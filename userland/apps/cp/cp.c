// cp.c — copy files and directory trees through the ordinary VFS interface.

#include "os64/os64.h"

#define CP_BUFFER_SIZE  (1024 * 1024)
#define CP_PATH_MAX     128
#define CP_MAX_OPERANDS 32
// Three standard handles + ten open source directories + a source and
// destination file = fifteen of the task's sixteen handle slots.
#define CP_MAX_DEPTH    10

typedef struct {
    bool recursive;
    bool interactive;
    bool verbose;
    bool progress;
} cp_options_t;

// Match the kernel's 1 MiB syscall I/O bounce size. Static storage keeps the
// transfer buffer off the comparatively small user stack.
static char copyBuffer[CP_BUFFER_SIZE];

// Userland mirror of the VFS's lexical path normalization. os64 currently has
// no links, so two canonical pathnames identify the same file; comparing the
// raw operands would miss ordinary aliases such as "file" and "./file" and
// let opening the destination truncate the source.
static int canonical_path(const char *path, char *out, size_t capacity)
{
    char cwd[CP_PATH_MAX];
    const char *sources[2];
    int32_t sourceCount = 0;
    size_t length = 0;

    if (path[0] != '/')
    {
        if (os64_getcwd(cwd, sizeof(cwd)) < 0)
            return -1;
        sources[sourceCount++] = cwd;
    }
    sources[sourceCount++] = path;

    for (int32_t s = 0; s < sourceCount; s++)
    {
        const char *p = sources[s];
        while (*p != '\0')
        {
            while (*p == '/')
                p++;
            if (*p == '\0')
                break;

            const char *start = p;
            while (*p != '\0' && *p != '/')
                p++;
            size_t componentLength = (size_t)(p - start);

            if (componentLength == 1 && start[0] == '.')
                continue;
            if (componentLength == 2 && start[0] == '.' && start[1] == '.')
            {
                while (length > 0 && out[length - 1] != '/')
                    length--;
                if (length > 0)
                    length--;
                continue;
            }

            if (length + componentLength + 2 > capacity)
                return -1;
            out[length++] = '/';
            for (size_t i = 0; i < componentLength; i++)
                out[length++] = start[i];
        }
    }

    if (length == 0)
        out[length++] = '/';
    out[length] = '\0';
    return 0;
}

static const char *path_basename(const char *path)
{
    const char *name = path;

    for (const char *p = path; *p != '\0'; p++)
        if (*p == '/' && p[1] != '\0')
            name = p + 1;

    return name;
}

static int join_path(char *out, size_t capacity, const char *directory,
                     const char *name)
{
    size_t length = os64_strlen(directory);
    const char *separator = length > 0 && directory[length - 1] == '/' ? "" : "/";
    int32_t wanted = os64_snprintf(out, capacity, "%s%s%s",
                                   directory, separator, name);
    return wanted >= 0 && (size_t)wanted < capacity ? 0 : -1;
}

static bool same_path(const char *first, const char *second)
{
    char canonicalFirst[CP_PATH_MAX];
    char canonicalSecond[CP_PATH_MAX];

    return canonical_path(first, canonicalFirst, sizeof(canonicalFirst)) == 0 &&
           canonical_path(second, canonicalSecond, sizeof(canonicalSecond)) == 0 &&
           os64_streq(canonicalFirst, canonicalSecond);
}

static bool destination_inside_source(const char *source, const char *destination)
{
    char canonicalSource[CP_PATH_MAX];
    char canonicalDestination[CP_PATH_MAX];

    if (canonical_path(source, canonicalSource, sizeof(canonicalSource)) < 0 ||
        canonical_path(destination, canonicalDestination,
                       sizeof(canonicalDestination)) < 0)
        return false;

    size_t sourceLength = os64_strlen(canonicalSource);
    if (sourceLength == 1 && canonicalSource[0] == '/')
        return canonicalDestination[0] == '/';

    for (size_t i = 0; i < sourceLength; i++)
        if (canonicalSource[i] != canonicalDestination[i])
            return false;

    return canonicalDestination[sourceLength] == '/';
}

static bool confirm_overwrite(const char *destination)
{
    char answer[16];

    os64_hprintf(OS64_STDERR, "cp: overwrite '%s'? ", destination);
    int64_t result = os64_readline(OS64_STDIN, answer, sizeof(answer));
    return result == 1 && (answer[0] == 'y' || answer[0] == 'Y');
}

static void show_progress(const char *source, uint64_t copied, uint64_t total,
                          const os64_ticks_t *started, bool finished)
{
    uint64_t percent = total == 0 ? 100 : copied * 100 / total;
    if (percent > 100)
        percent = 100;

    os64_ticks_t now = {0};
    if (started == NULL || started->per_second == 0 ||
        os64_ticks(&now) < 0)
    {
        os64_hprintf(OS64_STDERR, "\rcp: %s: %lu/%lu bytes (%lu%%)%s",
                     source, copied, total, percent, finished ? "\n" : "");
        return;
    }

    uint64_t elapsedTicks = now.ticks - started->ticks;
    uint64_t elapsedMS = elapsedTicks * 1000 / started->per_second;
    uint64_t bytesPerSecond = elapsedTicks == 0 ? 0 :
        copied * started->per_second / elapsedTicks;

    // Always print exact integer B/s for benchmark reports, with a compact
    // binary-unit rendering beside it for human eyes. The trailing spaces
    // erase remnants when a later carriage-returned update is shorter.
    if (bytesPerSecond >= 1024 * 1024)
    {
        uint64_t whole = bytesPerSecond / (1024 * 1024);
        uint64_t tenth = bytesPerSecond % (1024 * 1024) * 10 / (1024 * 1024);
        os64_hprintf(OS64_STDERR,
                     "\rcp: %s: %lu/%lu bytes (%lu%%) %lu.%lu MiB/s "
                     "(%lu B/s), %lu ms                %s",
                     source, copied, total, percent, whole, tenth,
                     bytesPerSecond, elapsedMS, finished ? "\n" : "");
    }
    else if (bytesPerSecond >= 1024)
    {
        uint64_t whole = bytesPerSecond / 1024;
        uint64_t tenth = bytesPerSecond % 1024 * 10 / 1024;
        os64_hprintf(OS64_STDERR,
                     "\rcp: %s: %lu/%lu bytes (%lu%%) %lu.%lu KiB/s "
                     "(%lu B/s), %lu ms                %s",
                     source, copied, total, percent, whole, tenth,
                     bytesPerSecond, elapsedMS, finished ? "\n" : "");
    }
    else
    {
        os64_hprintf(OS64_STDERR,
                     "\rcp: %s: %lu/%lu bytes (%lu%%) %lu B/s, "
                     "%lu ms                %s",
                     source, copied, total, percent, bytesPerSecond,
                     elapsedMS, finished ? "\n" : "");
    }
}

static int copy_regular(const char *source, const char *destination,
                        uint64_t sourceSize, const cp_options_t *options)
{
    os64_dirent_t destinationEntry = {0};
    bool destinationExists = os64_stat(destination, &destinationEntry) == 0;

    if (destinationExists && (destinationEntry.flags & OS64_DE_DIR))
    {
        os64_hprintf(OS64_STDERR,
                     "cp: cannot overwrite directory '%s' with file '%s'\n",
                     destination, source);
        return -1;
    }
    if (same_path(source, destination))
    {
        os64_hprintf(OS64_STDERR, "cp: '%s' and '%s' are the same file\n",
                     source, destination);
        return -1;
    }
    if (destinationExists && options->interactive &&
        !confirm_overwrite(destination))
        return 0;

    int32_t sourceHandle = (int32_t)os64_open(source, "r");
    if (sourceHandle < 0)
    {
        os64_hprintf(OS64_STDERR, "cp: cannot open '%s' for reading\n", source);
        return -1;
    }

    int32_t destinationHandle = (int32_t)os64_open(destination, "w");
    if (destinationHandle < 0)
    {
        os64_hprintf(OS64_STDERR, "cp: cannot open '%s' for writing\n",
                     destination);
        os64_close(sourceHandle);
        return -1;
    }

    uint64_t copied = 0;
    bool failed = false;
    os64_ticks_t started = {0};
    bool timing = options->progress && os64_ticks(&started) == 0;
    for (;;)
    {
        int64_t bytesRead = os64_read(sourceHandle, copyBuffer,
                                      sizeof(copyBuffer));
        if (bytesRead < 0)
        {
            failed = true;
            break;
        }
        if (bytesRead == 0)
            break;

        size_t written = 0;
        while (written < (size_t)bytesRead)
        {
            int64_t n = os64_write(destinationHandle, copyBuffer + written,
                                   (size_t)bytesRead - written);
            if (n <= 0)
            {
                failed = true;
                break;
            }
            written += (size_t)n;
            copied += (uint64_t)n;
        }
        if (options->progress)
            show_progress(source, copied, sourceSize,
                          timing ? &started : NULL, false);
        if (failed)
            break;
    }

    if (os64_close(destinationHandle) < 0)
    {
        os64_hprintf(OS64_STDERR, "cp: cannot close '%s'\n", destination);
        failed = true;
    }
    if (os64_close(sourceHandle) < 0)
    {
        os64_hprintf(OS64_STDERR, "cp: cannot close '%s'\n", source);
        failed = true;
    }

    // The final average includes close: a filesystem may defer its last
    // dirty blocks or directory-entry size until then, and "copy complete"
    // must measure the cost required to make that promise true.
    if (options->progress)
        show_progress(source, copied, sourceSize,
                      timing ? &started : NULL, true);

    if (failed)
    {
        os64_hprintf(OS64_STDERR, "cp: error copying '%s' to '%s'\n",
                     source, destination);
        return -1;
    }

    if (options->verbose)
        os64_printf("'%s' -> '%s'\n", source, destination);
    return 0;
}

static int copy_path(const char *source, const char *destination,
                     const cp_options_t *options, uint32_t depth);

static int copy_directory(const char *source, const char *destination,
                          const cp_options_t *options, uint32_t depth)
{
    if (!options->recursive)
    {
        os64_hprintf(OS64_STDERR,
                     "cp: omitting directory '%s' (use -R to copy recursively)\n",
                     source);
        return -1;
    }
    if (depth >= CP_MAX_DEPTH)
    {
        os64_hprintf(OS64_STDERR,
                     "cp: maximum directory depth reached at '%s'\n", source);
        return -1;
    }
    if (same_path(source, destination) ||
        destination_inside_source(source, destination))
    {
        os64_hprintf(OS64_STDERR,
                     "cp: cannot copy directory '%s' into itself as '%s'\n",
                     source, destination);
        return -1;
    }

    os64_dirent_t destinationEntry = {0};
    if (os64_stat(destination, &destinationEntry) == 0)
    {
        if ((destinationEntry.flags & OS64_DE_DIR) == 0)
        {
            os64_hprintf(OS64_STDERR,
                         "cp: cannot overwrite file '%s' with directory '%s'\n",
                         destination, source);
            return -1;
        }
    }
    else
    {
        if (os64_mkdir(destination) < 0)
        {
            os64_hprintf(OS64_STDERR, "cp: cannot create directory '%s'\n",
                         destination);
            return -1;
        }
        if (options->verbose)
            os64_printf("'%s' -> '%s'\n", source, destination);
    }

    int32_t directory = (int32_t)os64_opendir(source);
    if (directory < 0)
    {
        os64_hprintf(OS64_STDERR, "cp: cannot open directory '%s'\n", source);
        return -1;
    }

    int result = 0;
    int64_t readResult;
    os64_dirent_t child = {0};
    while ((readResult = os64_readdir(directory, &child)) == 1)
    {
        if (os64_streq(child.name, ".") || os64_streq(child.name, ".."))
            continue;

        char childSource[CP_PATH_MAX];
        char childDestination[CP_PATH_MAX];
        if (join_path(childSource, sizeof(childSource), source, child.name) < 0 ||
            join_path(childDestination, sizeof(childDestination), destination,
                      child.name) < 0)
        {
            os64_hprintf(OS64_STDERR,
                         "cp: path too long beneath '%s'\n", source);
            result = -1;
            continue;
        }

        if (copy_path(childSource, childDestination, options, depth + 1) < 0)
            result = -1;
    }

    if (readResult < 0)
    {
        os64_hprintf(OS64_STDERR, "cp: cannot read directory '%s'\n", source);
        result = -1;
    }
    if (os64_close(directory) < 0)
    {
        os64_hprintf(OS64_STDERR, "cp: cannot close directory '%s'\n", source);
        result = -1;
    }
    return result;
}

static int copy_path(const char *source, const char *destination,
                     const cp_options_t *options, uint32_t depth)
{
    os64_dirent_t sourceEntry = {0};
    if (os64_stat(source, &sourceEntry) < 0)
    {
        os64_hprintf(OS64_STDERR, "cp: cannot stat '%s'\n", source);
        return -1;
    }

    if (sourceEntry.flags & OS64_DE_DIR)
        return copy_directory(source, destination, options, depth);
    return copy_regular(source, destination, sourceEntry.size, options);
}

int main(int argc, char **argv)
{
    cp_options_t options = {0};
    os64_args_t args = {0};
    const char *operands[CP_MAX_OPERANDS] = {0};
    const os64_optspec_t specs[] = {
        {'R', "recursive", false, "copy directories recursively",
         .flag = &options.recursive},
        {'i', "interactive", false, "ask before overwriting a file",
         .flag = &options.interactive},
        {'v', "verbose", false, "print each copy operation",
         .flag = &options.verbose},
        {'\0', "progress", false, "show per-file copy progress",
         .flag = &options.progress}
    };

    os64_args_init(&args, argc, argv, specs, 4);
    args.about = "Copy files and directory trees.";
    args.details = "Multiple SOURCE operands require DEST to be a directory.";

    int32_t operandCount = os64_args_parse(
        &args, "cp [-Riv] [--progress] SOURCE... DEST",
        operands, CP_MAX_OPERANDS);
    if (operandCount == OS64_ARG_HELP)
        return 0;
    if (operandCount < 0)
        return 2;
    if (operandCount < 2)
    {
        os64_hprintf(OS64_STDERR, "cp: missing source or destination operand\n");
        os64_args_help(&args, "cp [-Riv] [--progress] SOURCE... DEST");
        return 2;
    }

    const char *target = operands[operandCount - 1];
    int32_t sourceCount = operandCount - 1;
    os64_dirent_t targetEntry = {0};
    bool targetIsDirectory = os64_stat(target, &targetEntry) == 0 &&
                             (targetEntry.flags & OS64_DE_DIR) != 0;

    if (sourceCount > 1 && !targetIsDirectory)
    {
        os64_hprintf(OS64_STDERR,
                     "cp: target '%s' is not a directory\n", target);
        return 1;
    }

    int32_t returnCode = 0;
    for (int32_t i = 0; i < sourceCount; i++)
    {
        const char *destination = target;
        char destinationPath[CP_PATH_MAX];

        if (targetIsDirectory)
        {
            char canonicalSource[CP_PATH_MAX];
            if (canonical_path(operands[i], canonicalSource,
                               sizeof(canonicalSource)) < 0 ||
                join_path(destinationPath, sizeof(destinationPath), target,
                          path_basename(canonicalSource)) < 0)
            {
                os64_hprintf(OS64_STDERR,
                             "cp: destination path for '%s' is too long\n",
                             operands[i]);
                returnCode = 1;
                continue;
            }
            destination = destinationPath;
        }

        if (copy_path(operands[i], destination, &options, 0) < 0)
            returnCode = 1;
    }

    return returnCode;
}
