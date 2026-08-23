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
    // Sleep BETWEEN the numbers, never after the last one. Sleeping after
    // every value made the documented five-second delay take six, and made
    // `shutdown now` — a delay of zero, which is to say THIS INSTANT — stand
    // there for a second first. (Codex review, 2026-08-22.)
    //
    // The counter is int64_t and printed with %ld, matching what the format
    // says it is reading: `%lu` against a plain int is a varargs mismatch
    // reading 64 bits of a 32-bit argument, and it was only ever right by the
    // grace of whatever the compiler left in the top half of the register.
    for (int64_t remaining = (int64_t)secondsToDelay; remaining >= 0; remaining--)
    {
        os64_printf("The system will go down in %ld seconds       ", remaining);
        if (remaining == 0)
            break;
        os64_sleep(1000);
        os64_printf("\r");
    }
    os64_printf("\n");
    os64_shutdown(OS64_SHUTDOWN_POWEROFF);
}
