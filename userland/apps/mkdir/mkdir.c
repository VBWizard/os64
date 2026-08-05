#include "os64/os64.h"

int main(int argc, char **argv)
{
    os64_args_t args = {0};
    int32_t result;
    int32_t directoryCount = 0;
    int32_t returnCode = 0;

    os64_args_init(&args, argc, argv, NULL, 0);
    args.about = "Create directories";

    // Validate the whole command line before creating anything. Besides
    // making help/error behavior predictable, this prevents a bad option at
    // the end from leaving directories created by earlier operands.
    while ((result = os64_args_next(&args)) != OS64_ARG_END)
    {
        if (result == OS64_ARG_POSITIONAL)
        {
            directoryCount++;
            continue;
        }

        if (result == OS64_ARG_HELP)
        {
            os64_args_help(&args, "mkdir DIR...");
            return 0;
        }

        os64_hprintf(OS64_STDERR, "mkdir: invalid option: %s\n", args.value);
        os64_args_help(&args, "mkdir DIR...");
        return 1;
    }

    if (directoryCount == 0)
    {
        os64_hprintf(OS64_STDERR, "mkdir: missing directory operand\n");
        os64_args_help(&args, "mkdir DIR...");
        return 1;
    }

    // os64_args_t is caller-owned and restartable. Walk the now-validated
    // operands again and attempt every directory, retaining a failure status
    // without preventing later independent operands from being created.
    os64_args_init(&args, argc, argv, NULL, 0);
    while ((result = os64_args_next(&args)) != OS64_ARG_END)
    {
        if (result == OS64_ARG_POSITIONAL && os64_mkdir(args.value) != 0)
        {
            os64_hprintf(OS64_STDERR, "mkdir: cannot create directory '%s'\n",
                         args.value);
            returnCode = 1;
        }
    }

    return returnCode;
}
