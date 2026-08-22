// reboot.c — ask the kernel to perform an orderly reboot.

#include "os64/os64.h"

int main(int argc, char **argv)
{
    os64_args_t args = {0};
    const char *operand = NULL;

    os64_args_init(&args, argc, argv, NULL, 0);
    args.about = "Reboot the system.";
    args.details = "Execute the shutdown syscall with the reboot parameter";

    int32_t parsed = os64_args_parse(&args, "reboot", &operand, 1);
    if (parsed == OS64_ARG_HELP)
        return 0;
    else if (parsed < 0)
        return 2;
    else if (parsed != 0)
    {
        os64_hprintf(OS64_STDERR, "reboot: unexpected operand: %s\n", operand);
        os64_args_help(&args, "reboot");
        return 2;
    }
    os64_shutdown(OS64_SHUTDOWN_REBOOT);
}
