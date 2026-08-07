// sync.c — flush every open file on every mounted filesystem.

#include "os64/os64.h"

int main(int argc, char **argv)
{
    os64_args_t args = {0};
    const char *operand = NULL;

    os64_args_init(&args, argc, argv, NULL, 0);
    args.about = "Flush pending filesystem writes.";
    args.details = "Sync every open file on every mounted filesystem.";

    int32_t parsed = os64_args_parse(&args, "sync", &operand, 1);
    if (parsed == OS64_ARG_HELP)
        return 0;
    if (parsed < 0)
        return 2;
    if (parsed != 0)
    {
        os64_hprintf(OS64_STDERR, "sync: unexpected operand: %s\n", operand);
        os64_args_help(&args, "sync");
        return 2;
    }

    if (os64_sync_all() < 0)
    {
        os64_hprintf(OS64_STDERR, "sync: failed to flush all open files\n");
        return 1;
    }

    return 0;
}
