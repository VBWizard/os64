#include "os64/os64.h"

#define RM_PATH_MAX 256
// A task has 16 handle slots, with 0/1/2 occupied by its console handles.
// Recursive removal holds one parent directory handle open per level. Stop
// with our honest depth error while 12 are open, leaving the thirteenth free
// slot as breathing room instead of failing later with a misleading opendir
// error from an exhausted handle table.
#define RM_MAX_DEPTH 12

static bool unsafe_recursive_operand(const char *path)
{
    size_t end = os64_strlen(path);

    while (end > 1 && path[end - 1] == '/')
        end--;

    if (end == 0 || (end == 1 && path[0] == '/'))
        return true;

    size_t start = end;
    while (start > 0 && path[start - 1] != '/')
        start--;

    size_t componentLength = end - start;
    return (componentLength == 1 && path[start] == '.') ||
           (componentLength == 2 && path[start] == '.' && path[start + 1] == '.');
}

static int32_t remove_path(const char *path, bool recursive, uint32_t depth)
{
    os64_dirent_t entry = {0};

    if (os64_stat(path, &entry) != 0)
    {
        os64_hprintf(OS64_STDERR, "rm: cannot remove '%s': file not found\n", path);
        return 1;
    }

    if ((entry.flags & OS64_DE_DIR) == 0)
    {
        if (os64_unlink(path) != 0)
        {
            os64_hprintf(OS64_STDERR, "rm: cannot remove '%s'\n", path);
            return 1;
        }
        return 0;
    }

    if (!recursive)
    {
        os64_hprintf(OS64_STDERR, "rm: cannot remove '%s': is a directory\n", path);
        return 1;
    }

    if (entry.flags & OS64_DE_MOUNT)
    {
        os64_hprintf(OS64_STDERR,
                     "rm: refusing to cross filesystem boundary at '%s'\n", path);
        return 1;
    }

    if (unsafe_recursive_operand(path))
    {
        os64_hprintf(OS64_STDERR, "rm: refusing to recursively remove '%s'\n", path);
        return 1;
    }

    if (depth >= RM_MAX_DEPTH)
    {
        os64_hprintf(OS64_STDERR, "rm: maximum directory depth reached at '%s'\n", path);
        return 1;
    }

    int64_t dirHandle = os64_opendir(path);
    if (dirHandle < 0)
    {
        os64_hprintf(OS64_STDERR, "rm: cannot open directory '%s'\n", path);
        return 1;
    }

    int32_t returnCode = 0;
    int64_t readResult;
    os64_dirent_t child = {0};

    while ((readResult = os64_readdir((int32_t)dirHandle, &child)) == 1)
    {
        if (os64_streq(child.name, ".") || os64_streq(child.name, ".."))
            continue;

        char childPath[RM_PATH_MAX];
        size_t pathLength = os64_strlen(path);
        const char *separator = (pathLength > 0 && path[pathLength - 1] == '/') ? "" : "/";
        int32_t wanted = os64_snprintf(childPath, sizeof(childPath), "%s%s%s",
                                      path, separator, child.name);

        if (wanted < 0 || (size_t)wanted >= sizeof(childPath))
        {
            os64_hprintf(OS64_STDERR, "rm: path too long beneath '%s'\n", path);
            returnCode = 1;
            continue;
        }

        // Use the mount attribute returned with this directory entry so the
        // recursive walk cannot cross the boundary between reading the name
        // and descending into it. The stat check above also protects a mount
        // point supplied directly as an operand.
        if (child.flags & OS64_DE_MOUNT)
        {
            os64_hprintf(OS64_STDERR,
                         "rm: refusing to cross filesystem boundary at '%s'\n",
                         childPath);
            returnCode = 1;
            continue;
        }

        if (remove_path(childPath, true, depth + 1) != 0)
            returnCode = 1;
    }

    if (readResult < 0)
    {
        os64_hprintf(OS64_STDERR, "rm: cannot read directory '%s'\n", path);
        returnCode = 1;
    }

    if (os64_close((int32_t)dirHandle) != 0)
    {
        os64_hprintf(OS64_STDERR, "rm: cannot close directory '%s'\n", path);
        returnCode = 1;
    }

    if (returnCode == 0 && os64_unlink(path) != 0)
    {
        os64_hprintf(OS64_STDERR, "rm: cannot remove directory '%s'\n", path);
        returnCode = 1;
    }

    return returnCode;
}

int main(int argc, char **argv)
{
    os64_args_t args = {0};
    int32_t result;
    int32_t fileCount = 0;
    int32_t returnCode = 0;
    bool recursive = false;
    const os64_optspec_t specs[] = {
        {'r', "recursive", false, "remove directories and their contents", .flag = &recursive}};

    os64_args_init(&args, argc, argv, specs, 1);
    args.about = "Remove files or directories";
    args.details = "Directories require -r and are removed depth-first.";

    // Validate the complete command line before deleting anything. This keeps
    // `rm first-file --bad-option` from removing first-file and only then
    // discovering that the command was invalid.
    while ((result = os64_args_next(&args)) != OS64_ARG_END)
    {
        if (result == OS64_ARG_POSITIONAL)
        {
            fileCount++;
            continue;
        }

        if (result == 'r')
        {
            recursive = true;
            continue;
        }

        if (result == OS64_ARG_HELP)
        {
            os64_args_help(&args, "rm [-r] FILE...");
            return 0;
        }

        os64_hprintf(OS64_STDERR, "rm: invalid option: %s\n", args.value);
        os64_args_help(&args, "rm [-r] FILE...");
        return 1;
    }

    if (fileCount == 0)
    {
        os64_hprintf(OS64_STDERR, "rm: missing file operand\n");
        os64_args_help(&args, "rm [-r] FILE...");
        return 1;
    }

    // The parser is caller-owned and restartable, so make a fresh pass now
    // that the whole command line is known to be valid.
    os64_args_init(&args, argc, argv, specs, 1);
    while ((result = os64_args_next(&args)) != OS64_ARG_END)
    {
        if (result == OS64_ARG_POSITIONAL)
        {
            if (remove_path(args.value, recursive, 0) != 0)
                returnCode = 1;
        }
    }

    return returnCode;
}
