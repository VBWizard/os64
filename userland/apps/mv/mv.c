// mv.c — move files and directories with the VFS rename operation. Moving to
// an unused name is one filesystem operation; replacement is atomic on ext2
// and retains FAT's legacy remove-first behavior.

#include "os64/os64.h"

#define MV_PATH_MAX     256
#define MV_MAX_OPERANDS 512

typedef struct {
    bool interactive;
    bool verbose;
} mv_options_t;

static const char *path_basename(const char *path, size_t *nameLength)
{
    size_t end = os64_strlen(path);

    while (end > 1 && path[end - 1] == '/')
        end--;

    size_t start = end;
    while (start > 0 && path[start - 1] != '/')
        start--;

    *nameLength = end - start;
    return path + start;
}

static int join_destination(char *out, size_t capacity, const char *directory,
                            const char *source)
{
    size_t nameLength;
    const char *name = path_basename(source, &nameLength);
    size_t directoryLength = os64_strlen(directory);
    const char *separator = directoryLength > 0 &&
                            directory[directoryLength - 1] == '/' ? "" : "/";
    int32_t wanted = os64_snprintf(out, capacity, "%s%s%.*s", directory,
                                   separator, (int32_t)nameLength, name);

    return wanted >= 0 && (size_t)wanted < capacity ? 0 : -1;
}

static bool confirm_overwrite(const char *destination)
{
    char answer[16];

    os64_hprintf(OS64_STDERR, "mv: overwrite '%s'? ", destination);
    int64_t result = os64_readline(OS64_STDIN, answer, sizeof(answer));
    return result == 1 && (answer[0] == 'y' || answer[0] == 'Y');
}

static int move_path(const char *source, const char *destination,
                     const mv_options_t *options)
{
    os64_dirent_t sourceEntry = {0};
    if (os64_stat(source, &sourceEntry) < 0)
    {
        os64_hprintf(OS64_STDERR, "mv: cannot stat '%s'\n", source);
        return -1;
    }

    os64_dirent_t destinationEntry = {0};
    bool destinationExists = os64_stat(destination, &destinationEntry) == 0;
    // The syscall's only replacement operation is regular-file onto
    // regular-file. Other existing-destination combinations are refusals,
    // not overwrites, so do not ask a misleading overwrite question.
    bool replacesFile = destinationExists &&
                        (sourceEntry.flags & OS64_DE_DIR) == 0 &&
                        (destinationEntry.flags & OS64_DE_DIR) == 0;
    if (replacesFile && options->interactive &&
        !confirm_overwrite(destination))
        return 0;

    if (os64_rename(source, destination) < 0)
    {
        os64_hprintf(OS64_STDERR, "mv: cannot move '%s' to '%s'\n",
                     source, destination);
        return -1;
    }

    if (options->verbose)
        os64_printf("'%s' -> '%s'\n", source, destination);
    return 0;
}

int main(int argc, char **argv)
{
    mv_options_t options = {0};
    os64_args_t args = {0};
    const char *operands[MV_MAX_OPERANDS] = {0};
    const os64_optspec_t specs[] = {
        {'i', "interactive", false, "ask before overwriting a file",
         .flag = &options.interactive},
        {'v', "verbose", false, "print each move operation",
         .flag = &options.verbose}
    };

    os64_args_init(&args, argc, argv, specs, 2);
    args.about = "Move files and directories.";
    args.details = "Multiple SOURCE operands require DEST to be a directory.";

    int32_t operandCount = os64_args_parse(
        &args, "mv [-iv] SOURCE... DEST", operands, MV_MAX_OPERANDS);
    if (operandCount == OS64_ARG_HELP)
        return 0;
    if (operandCount < 0)
        return 2;
    if (operandCount < 2)
    {
        os64_hprintf(OS64_STDERR, "mv: missing source or destination operand\n");
        os64_args_help(&args, "mv [-iv] SOURCE... DEST");
        return 2;
    }

    const char *target = operands[operandCount - 1];
    int32_t sourceCount = operandCount - 1;
    os64_dirent_t targetEntry = {0};
    bool targetIsDirectory = os64_stat(target, &targetEntry) == 0 &&
                             (targetEntry.flags & OS64_DE_DIR) != 0;

    if (sourceCount > 1 && !targetIsDirectory)
    {
        os64_hprintf(OS64_STDERR, "mv: target '%s' is not a directory\n", target);
        return 1;
    }

    int32_t returnCode = 0;
    for (int32_t i = 0; i < sourceCount; i++)
    {
        const char *destination = target;
        char destinationPath[MV_PATH_MAX];

        if (targetIsDirectory)
        {
            if (join_destination(destinationPath, sizeof(destinationPath),
                                 target, operands[i]) < 0)
            {
                os64_hprintf(OS64_STDERR,
                             "mv: destination path for '%s' is too long\n",
                             operands[i]);
                returnCode = 1;
                continue;
            }
            destination = destinationPath;
        }

        if (move_path(operands[i], destination, &options) < 0)
            returnCode = 1;
    }

    return returnCode;
}
