// uptime — one monotonic snapshot, three useful views, no invented load.

#include "os64/os64.h"

#define USAGE "uptime [-p | -s]"

static void print_pretty(uint64_t seconds)
{
    uint64_t days = seconds / 86400;
    uint64_t hours = (seconds / 3600) % 24;
    uint64_t minutes = (seconds / 60) % 60;
    uint64_t secs = seconds % 60;

    os64_puts("up ");
    if (days != 0)
        os64_printf("%lu day%s, ", days, days == 1 ? "" : "s");
    if (days != 0 || hours != 0)
        os64_printf("%lu hour%s, ", hours, hours == 1 ? "" : "s");
    if (days != 0 || hours != 0 || minutes != 0)
        os64_printf("%lu minute%s, ", minutes, minutes == 1 ? "" : "s");
    os64_printf("%lu second%s\n", secs, secs == 1 ? "" : "s");
}

int main(int argc, char **argv)
{
    bool pretty = false;
    bool since = false;
    os64_optspec_t specs[] = {
        { 'p', "pretty", false, "show uptime as words", .flag = &pretty },
        { 's', "since",  false, "show the local date/time when the system started", .flag = &since },
    };
    os64_args_t args;
    os64_args_init(&args, argc, argv, specs, 2);
    args.about = "Show how long os64 has been running. Load averages are omitted until the kernel has them.";

    int32_t parsed = os64_args_parse(&args, USAGE, NULL, 0);
    if (parsed == OS64_ARG_HELP)
        return 0;
    if (parsed == OS64_ARG_ERROR)
        return 2;
    if (pretty && since) {
        os64_hprintf(OS64_STDERR, "uptime: --pretty and --since are mutually exclusive\n");
        return 2;
    }

    os64_ticks_t ticks = {0};
    if (os64_ticks(&ticks) < 0 || ticks.per_second == 0) {
        os64_hprintf(OS64_STDERR, "uptime: cannot read the monotonic clock\n");
        return 1;
    }
    uint64_t seconds = ticks.ticks / ticks.per_second;

    if (pretty) {
        print_pretty(seconds);
        return 0;
    }

    if (since) {
        os64_time_t now;
        if (os64_time(&now) < 0) {
            os64_hprintf(OS64_STDERR, "uptime: cannot read the wall clock\n");
            return 1;
        }
        os64_date_t boot;
        os64_localtime(now.epoch - (int64_t)seconds, &boot);
        char text[32];
        if (os64_strftime(text, sizeof(text), "%F %T", &boot) == 0) {
            os64_hprintf(OS64_STDERR, "uptime: cannot format the boot time\n");
            return 1;
        }
        os64_printf("%s\n", text);
        return 0;
    }

    os64_date_t now;
    if (os64_date_now(&now, NULL) < 0) {
        os64_hprintf(OS64_STDERR, "uptime: cannot read the wall clock\n");
        return 1;
    }
    char clock[16];
    if (os64_strftime(clock, sizeof(clock), "%T", &now) == 0) {
        os64_hprintf(OS64_STDERR, "uptime: cannot format the current time\n");
        return 1;
    }

    uint64_t days = seconds / 86400;
    uint64_t hours = (seconds / 3600) % 24;
    uint64_t minutes = (seconds / 60) % 60;
    uint64_t secs = seconds % 60;
    if (days != 0)
        os64_printf(" %s up %lu day%s, %02lu:%02lu:%02lu\n",
                    clock, days, days == 1 ? "" : "s", hours, minutes, secs);
    else
        os64_printf(" %s up %02lu:%02lu:%02lu\n", clock, hours, minutes, secs);
    return 0;
}
