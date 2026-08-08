// shutdown.c — ask the kernel to perform an orderly system shutdown.

#include "os64/os64.h"

int main(int argc, char **argv)
{
    os64_args_t args = {0};
    const char *operand = NULL;

    os64_args_init(&args, argc, argv, NULL, 0);
    args.about = "Shut down and power off the system.";
    args.details = "Retire system services, flush pending writes, and power off.";

    int32_t parsed = os64_args_parse(&args, "shutdown", &operand, 1);
    if (parsed == OS64_ARG_HELP)
        return 0;
    if (parsed < 0)
        return 2;
    if (parsed != 0)
    {
        os64_hprintf(OS64_STDERR, "shutdown: unexpected operand: %s\n", operand);
        os64_args_help(&args, "shutdown");
        return 2;
    }

    os64_shutdown();
}
