// sleep — suspend execution for a requested duration.

#include "os64/os64.h"

#define USAGE "sleep [-M] NUMBER[SUFFIX]..."

static bool add_duration(const char *text, bool milliseconds_default,
                         uint64_t *total_ms)
{
    if (text == NULL || *text == '\0')
        return false;

    const char *p = text;
    uint64_t whole = 0;
    bool have_whole = false;
    while (*p >= '0' && *p <= '9')
    {
        uint64_t digit = (uint64_t)(*p - '0');
        if (whole > (UINT64_MAX - digit) / 10)
            return false;
        whole = whole * 10 + digit;
        have_whole = true;
        p++;
    }

    uint64_t fraction = 0;
    uint64_t fraction_scale = 1;
    bool discarded_nonzero = false;
    bool have_fraction = false;
    if (*p == '.')
    {
        p++;
        while (*p >= '0' && *p <= '9')
        {
            // Milliseconds are the ABI's finest unit. Retain enough decimal
            // digits for every supported suffix; further zeroes are harmless.
            if (fraction_scale < 1000000000)
            {
                fraction = fraction * 10 + (uint64_t)(*p - '0');
                fraction_scale *= 10;
            }
            else if (*p != '0')
                discarded_nonzero = true;
            have_fraction = true;
            p++;
        }
    }
    if (!have_whole && !have_fraction)
        return false;

    uint64_t unit_ms;
    switch (*p)
    {
    case '\0': unit_ms = milliseconds_default ? 1 : 1000; break;
    case 's':  unit_ms = 1000;     p++; break;
    case 'm':  unit_ms = 60000;    p++; break;
    case 'h':  unit_ms = 3600000;  p++; break;
    case 'd':  unit_ms = 86400000; p++; break;
    default: return false;
    }
    if (*p != '\0' || whole > UINT64_MAX / unit_ms)
        return false;

    uint64_t duration_ms = whole * unit_ms;
    if (fraction != 0 || discarded_nonzero)
    {
        if (fraction > UINT64_MAX / unit_ms)
            return false;
        uint64_t numerator = fraction * unit_ms;
        uint64_t fraction_ms = numerator / fraction_scale;
        // The syscall promises to sleep at least the requested duration, so
        // round a sub-millisecond remainder up rather than waking early.
        if (numerator % fraction_scale != 0 || discarded_nonzero)
            fraction_ms++;
        if (duration_ms > UINT64_MAX - fraction_ms)
            return false;
        duration_ms += fraction_ms;
    }

    if (*total_ms > UINT64_MAX - duration_ms)
        return false;
    *total_ms += duration_ms;
    return true;
}

int main(int argc, char **argv)
{
    bool milliseconds = false;
    const os64_optspec_t specs[] = {
        {'M', "milliseconds", false,
         "interpret unsuffixed numbers as milliseconds", .flag = &milliseconds}
    };
    os64_args_t args = {0};
    os64_args_init(&args, argc, argv, specs, 1);
    args.about = "Suspend execution for the combined duration of all operands.";
    args.details = "Suffixes: s seconds, m minutes, h hours, d days. "
                   "With no suffix, NUMBER is seconds unless -M is used.";

    const char *durations[argc > 0 ? argc : 1];
    int operands = 0;
    int32_t result;
    while ((result = os64_args_next(&args)) != OS64_ARG_END)
    {
        if (result == OS64_ARG_HELP)
        {
            os64_args_help(&args, USAGE);
            return 0;
        }
        if (result == 'M')
        {
            milliseconds = true;
            continue;
        }
        if (result == OS64_ARG_POSITIONAL)
        {
            durations[operands++] = args.value;
            continue;
        }

        os64_args_help(&args, USAGE);
        return 2;
    }

    if (operands == 0)
    {
        os64_args_help(&args, USAGE);
        return 2;
    }

    uint64_t total_ms = 0;
    for (int i = 0; i < operands; i++)
    {
        if (!add_duration(durations[i], milliseconds, &total_ms))
        {
            os64_hprintf(OS64_STDERR,
                         "sleep: invalid duration '%s'\n", durations[i]);
            return 2;
        }
    }

    if (os64_sleep(total_ms) < 0)
    {
        os64_hprintf(OS64_STDERR, "sleep: sleep syscall failed\n");
        return 1;
    }
    return 0;
}
