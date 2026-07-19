#include "os64/os64.h"

int main(int argc, char **argv)
{
    os64_args_t args = {0};
    char cwd[512] = {0};
    int returnCode = 0, retVal = 0;
    (void)argc; // Silences the unused parameter error
    (void)argv; // Same
    static const os64_optspec_t specs[] = {
        {}};

    os64_args_init(&args, argc, argv, specs, 0);
    args.about = "Print the current working directory";

    while ((retVal = os64_args_next(&args)) != OS64_ARG_END)
    {
        switch (retVal)
        {
        case OS64_ARG_HELP:
            os64_args_help(&args, "pwd");
            returnCode = 2;
            break;
        default:
            os64_args_help(&args, "pwd");
            returnCode = 3;
            break;
        }
    }
    if (!returnCode)
    {
        retVal = os64_getcwd(cwd, 512);
        if (retVal < 0)
        {
            os64_hprintf(OS64_STDERR, "Error calling syscall_getcwd: %ld\n", retVal);
            retVal = -1;
        }
        else
            os64_printf("%s\n", cwd);
    }
    return retVal;
}