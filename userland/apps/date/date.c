// date — display or set os64's wall clock.

#include "os64/os64.h"

#define USAGE "date [-u] [--resolution] [-s 'YYYY-MM-DD HH:MM:SS'] [+FORMAT]"

static bool digit(char c)
{
    return c >= '0' && c <= '9';
}

static int32_t parse_digits(const char *text, int32_t count, int32_t *value)
{
    int32_t result = 0;
    for (int32_t i = 0; i < count; i++) {
        if (!digit(text[i]))
            return -1;
        result = result * 10 + (text[i] - '0');
    }
    *value = result;
    return 0;
}

static int32_t parse_set_date(const char *text, os64_date_t *date)
{
    size_t length = os64_strlen(text);
    if ((length != 16 && length != 19) ||
        text[4] != '-' || text[7] != '-' ||
        (text[10] != ' ' && text[10] != 'T') || text[13] != ':' ||
        (length == 19 && text[16] != ':'))
        return -1;

    *date = (os64_date_t){0};
    if (parse_digits(text, 4, &date->year) != 0 ||
        parse_digits(text + 5, 2, &date->month) != 0 ||
        parse_digits(text + 8, 2, &date->day) != 0 ||
        parse_digits(text + 11, 2, &date->hour) != 0 ||
        parse_digits(text + 14, 2, &date->minute) != 0 ||
        (length == 19 && parse_digits(text + 17, 2, &date->second) != 0))
        return -1;

    int64_t ignored;
    return os64_date_to_epoch(date, &ignored) == 0 ? 0 : -1;
}

static int print_resolution(void)
{
    os64_ticks_t ticks = {0};
    if (os64_ticks(&ticks) < 0 || ticks.per_second == 0) {
        os64_hprintf(OS64_STDERR, "date: cannot read clock resolution\n");
        return 1;
    }

    if (1000000000u % ticks.per_second == 0) {
        uint64_t nanoseconds = 1000000000u / ticks.per_second;
        os64_printf("%lu.%09lu seconds\n", nanoseconds / 1000000000u,
                    nanoseconds % 1000000000u);
    } else {
        uint64_t approximate_ns = 1000000000u / ticks.per_second;
        os64_printf("1/%u second (~%lu ns)\n", ticks.per_second,
                    approximate_ns);
    }
    return 0;
}

int main(int argc, char **argv)
{
    bool utc = false;
    bool resolution = false;
    const char *set_value = NULL;
    os64_optspec_t specs[] = {
        { 'u', "utc", false, "display or interpret time as UTC", .flag = &utc },
        { '\0', "resolution", false, "print the wall clock's tick resolution", .flag = &resolution },
        { 's', "set", true, "set the running wall clock (does not write the RTC)", .value_out = &set_value },
    };
    os64_args_t args;
    os64_args_init(&args, argc, argv, specs, 3);
    args.about = "Display the current date, or set the running system clock. A leading + introduces strftime formatting.";
    args.details = "Set syntax: YYYY-MM-DD HH:MM[:SS]. Local time is selected by TZ; -u selects UTC.";

    const char *format_arg = NULL;
    int32_t parsed = os64_args_parse(&args, USAGE, &format_arg, 1);
    if (parsed == OS64_ARG_HELP)
        return 0;
    if (parsed == OS64_ARG_ERROR)
        return 2;

    if (format_arg != NULL && format_arg[0] != '+') {
        os64_hprintf(OS64_STDERR, "date: format must begin with '+'\n");
        return 2;
    }
    if (resolution) {
        if (set_value != NULL || format_arg != NULL) {
            os64_hprintf(OS64_STDERR, "date: --resolution cannot be combined with --set or a format\n");
            return 2;
        }
        return print_resolution();
    }

    if (set_value != NULL) {
        os64_date_t requested;
        if (parse_set_date(set_value, &requested) != 0) {
            os64_hprintf(OS64_STDERR,
                         "date: invalid date '%s' (expected YYYY-MM-DD HH:MM[:SS])\n",
                         set_value);
            return 2;
        }

        int64_t epoch;
        int64_t converted = utc ? os64_date_to_epoch(&requested, &epoch)
                                : os64_mktime(&requested, &epoch);
        if (converted != 0) {
            os64_hprintf(OS64_STDERR,
                         "date: '%s' is not a valid local time in the current timezone\n",
                         set_value);
            return 2;
        }
        if (os64_set_time(epoch) < 0) {
            os64_hprintf(OS64_STDERR, "date: the kernel refused to set the clock\n");
            return 1;
        }
    }

    os64_time_t raw;
    if (os64_time(&raw) < 0) {
        os64_hprintf(OS64_STDERR, "date: cannot read the wall clock\n");
        return 1;
    }

    os64_date_t date;
    if (utc)
        os64_date_from_epoch(raw.epoch, &date);
    else if (os64_localtime(raw.epoch, &date) < 0) {
        os64_hprintf(OS64_STDERR, "date: cannot convert the wall clock\n");
        return 1;
    }

    const char *format = format_arg != NULL ? format_arg + 1
                         : (date.zone[0] != '\0'
                            ? "%a %b %e %H:%M:%S %Z %Y"
                            : "%a %b %e %H:%M:%S %z %Y");
    char output[512] = {0};
    size_t written = os64_strftime(output, sizeof(output), format, &date);
    if (written == 0 && output[0] != '\0') {
        os64_hprintf(OS64_STDERR, "date: formatted output is too long\n");
        return 1;
    }
    os64_printf("%s\n", output);
    return 0;
}
