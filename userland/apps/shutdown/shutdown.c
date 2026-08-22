// shutdown.c — ask the kernel to perform an orderly system shutdown.

#include "os64/os64.h"

int main(int argc, char **argv)
{
    os64_args_t args = {0};
    const char *operand = NULL;
    uint64_t secondsToDelay = 5;

    os64_args_init(&args, argc, argv, NULL, 0);
    args.about = "Shut down and power off the system.";
    args.details = "Retire system services, flush pending writes, and power off with an optional delay. (default is 5 seconds)";

    int32_t parsed = os64_args_parse(&args, "shutdown [delay]", &operand, 1);
    if (parsed == OS64_ARG_HELP)
        return 0;
    else if (parsed < 0)
        return 2;
    else if (parsed == 1)
    {
        if (os64_streq(operand, "now"))
            secondsToDelay = 0;
        else
        {
            // Digits only, checked by hand: os64_atoi("fiveminutes") is 0,
            // and 0 is "power off this instant" — a typo must not be the
            // fastest way to shut the machine down (review, 2026-08-22).
            secondsToDelay = 0;
            bool digits = (*operand != '\0');
            for (const char *p = operand; *p != '\0'; p++)
            {
                if (*p < '0' || *p > '9' || secondsToDelay > 9999)
                {
                    digits = false;
                    break;
                }
                secondsToDelay = secondsToDelay * 10 + (uint64_t)(*p - '0');
            }
            if (!digits || secondsToDelay > 9999)
            {
                os64_hprintf(OS64_STDERR, "shutdown: bad delay '%s'. Must be 'now' or 0..9999 seconds\n", operand);
                os64_args_help(&args, "shutdown");
                return 3;
            }
        }
    }
    else if (parsed != 0)
    {
        os64_hprintf(OS64_STDERR, "shutdown: unexpected operand: %s\n", operand);
        os64_args_help(&args, "shutdown");
        return 2;
    }
    for (int secondsExpired = secondsToDelay; secondsExpired >= 0; secondsExpired--)
    {
        os64_printf("The system will go down in %lu seconds       ", secondsExpired);
        os64_sleep(1000);
        if (secondsExpired > 0)
            os64_printf("\r");
    }
    os64_printf("\n");
    os64_shutdown(OS64_SHUTDOWN_POWEROFF);
}
