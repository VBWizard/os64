#include "os64/os64.h"

static const os64_optspec_t specs[] = {
    {.letter = 'p', .name = "parents",
     .help = "create missing parents; accept existing directories"},
    {.letter = 'v', .name = "verbose",
     .help = "report each directory created"},
};

static int report_created(const char *path)
{
    char line[OS64_PATH_MAX + 40];
    int32_t length = os64_snprintf(line, sizeof(line),
                                   "mkdir: created directory '%s'\n", path);
    size_t done = 0;
    while (done < (size_t)length)
    {
        int64_t written = os64_write(OS64_STDOUT, line + done, (size_t)length - done);
        if (written <= 0)
        {
            os64_hprintf(OS64_STDERR, "mkdir: cannot write verbose output\n");
            return 1;
        }
        done += (size_t)written;
    }
    return 0;
}

static int create_directory(const char *path, bool parents, bool verbose)
{
    if (os64_mkdir(path) == 0)
        return verbose ? report_created(path) : 0;

    // mkdir has no distinct "already exists" result. Accept a failed create
    // with -p when stat confirms a directory, including a concurrent create.
    os64_dirent_t entry;
    if (parents && os64_stat(path, &entry) == 0 && (entry.flags & OS64_DE_DIR))
        return 0;

    os64_hprintf(OS64_STDERR, "mkdir: cannot create directory '%s'\n", path);
    return 1;
}

static int create_path(const char *operand, bool parents, bool verbose)
{
    char path[OS64_PATH_MAX];
    size_t length = 0;
    while (operand[length] && length < sizeof(path) - 1)
    {
        path[length] = operand[length];
        length++;
    }
    if (operand[length])
    {
        os64_hprintf(OS64_STDERR, "mkdir: directory path too long\n");
        return 1;
    }
    if (length == 0)
    {
        os64_hprintf(OS64_STDERR, "mkdir: cannot create directory ''\n");
        return 1;
    }
    path[length] = '\0';
    if (!parents)
        return create_directory(path, false, verbose);

    while (length > 1 && path[length - 1] == '/')
        path[--length] = '\0';

    // Visit prefixes in order without collapsing dot components: a/../b
    // still requires a to exist, and file/../b must fail at file.
    for (size_t i = 1; i < length; i++)
    {
        if (path[i] != '/' || path[i - 1] == '/')
            continue;
        path[i] = '\0';
        int result = create_directory(path, true, verbose);
        path[i] = '/';
        if (result != 0)
            return result;
    }
    return create_directory(path, true, verbose);
}

int main(int argc, char **argv)
{
    os64_args_t args = {0};
    int32_t result;
    int32_t directoryCount = 0;
    int32_t returnCode = 0;
    bool parents = false, verbose = false;

    os64_args_init(&args, argc, argv, specs, sizeof(specs) / sizeof(specs[0]));
    args.about = "Create directories";

    // Validate the whole command line before creating anything. Besides
    // making help/error behavior predictable, this prevents a bad option at
    // the end from leaving directories created by earlier operands.
    while ((result = os64_args_next(&args)) != OS64_ARG_END)
    {
        if (result == 'p' || result == 'v')
        {
            if (result == 'p') parents = true;
            else verbose = true;
            continue;
        }
        if (result == OS64_ARG_POSITIONAL)
        {
            directoryCount++;
            continue;
        }

        if (result == OS64_ARG_HELP)
        {
            os64_args_help(&args, "mkdir [-pv] DIR...");
            return 0;
        }

        os64_hprintf(OS64_STDERR, "mkdir: invalid option: %s\n", args.value);
        os64_args_help(&args, "mkdir [-pv] DIR...");
        return 1;
    }

    if (directoryCount == 0)
    {
        os64_hprintf(OS64_STDERR, "mkdir: missing directory operand\n");
        os64_args_help(&args, "mkdir [-pv] DIR...");
        return 1;
    }

    // os64_args_t is caller-owned and restartable. Walk the now-validated
    // operands again and attempt every directory, retaining a failure status
    // without preventing later independent operands from being created.
    os64_args_init(&args, argc, argv, specs, sizeof(specs) / sizeof(specs[0]));
    while ((result = os64_args_next(&args)) != OS64_ARG_END)
    {
        if (result == OS64_ARG_POSITIONAL && create_path(args.value, parents, verbose) != 0)
        {
            returnCode = 1;
        }
    }

    return returnCode;
}
