// timetest.c — time() syscall + libos64 calendar fixture.
//
// Two layers, tested in the right order:
//   1. The CALENDAR is pure math, so it proves itself against known answers
//      first — epoch 0 (the birthday), 951782400 (Feb 29 2000: the century
//      leap day, the one the 4/100/400 rules exist for), and -1 (the last
//      second of 1969: floor-division's favorite ambush).
//   2. THEN the syscall: sanity-band the epoch (a machine running this code
//      knows it's not 2023 and not 2096), and check the sub-second phase
//      stays inside the tick rate.
// Verdict to both sinks: console for the human, serial for the harness.

#include "os64/os64.h"

static int check_date(int64_t epoch, int32_t yr, int32_t mo, int32_t dy,
                      int32_t hh, int32_t mm, int32_t ss, int32_t wd,
                      const char *label)
{
    os64_date_t d;
    os64_date_from_epoch(epoch, &d);
    if (d.year != yr || d.month != mo || d.day != dy ||
        d.hour != hh || d.minute != mm || d.second != ss || d.weekday != wd) {
        os64_printf("timetest: FAIL %s -> %d-%02d-%02d %02d:%02d:%02d wd=%d\n",
                    label, d.year, d.month, d.day, d.hour, d.minute, d.second,
                    d.weekday);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    int failures = 0;

    // 1: the calendar against known answers.
    failures += check_date(0, 1970, 1, 1, 0, 0, 0, 4, "epoch 0");
    failures += check_date(951782400, 2000, 2, 29, 0, 0, 0, 2, "Y2K leap day");
    failures += check_date(-1, 1969, 12, 31, 23, 59, 59, 3, "epoch -1");

    // 1b: the TZ engine — parser, then the two seconds a year that earn
    // their keep. Spring forward 2026: 06:59:59 UTC is 1:59:59 EST, and one
    // second later is 3:00:00 EDT (2 AM never happens). Fall back: 05:59:59
    // UTC is 1:59:59 EDT, one second later is 1:00:00 EST (1 AM happens
    // twice). Known-answer epochs, checked to the second.
    os64_tz_t tz;
    if (os64_tz_parse("EST5EDT", &tz) != 0 || tz.std_offset_min != -300 ||
        !tz.has_dst || tz.dst_offset_min != -240) {
        os64_puts("timetest: FAIL — EST5EDT didn't parse right\n");
        failures++;
    } else {
        os64_date_t d;
        os64_date_from_epoch_tz(1772953199, &tz, &d);   // 2026-03-08 06:59:59Z
        if (d.hour != 1 || d.minute != 59 || d.second != 59) {
            os64_printf("timetest: FAIL — pre-spring %02d:%02d:%02d\n",
                        d.hour, d.minute, d.second);
            failures++;
        }
        os64_date_from_epoch_tz(1772953200, &tz, &d);   // one second later
        if (d.hour != 3 || d.minute != 0 || d.second != 0) {
            os64_printf("timetest: FAIL — spring lands %02d:%02d:%02d\n",
                        d.hour, d.minute, d.second);
            failures++;
        }
        os64_date_from_epoch_tz(1793512799, &tz, &d);   // 2026-11-01 05:59:59Z
        if (d.hour != 1 || d.minute != 59 || d.second != 59) {
            os64_printf("timetest: FAIL — pre-fall %02d:%02d:%02d\n",
                        d.hour, d.minute, d.second);
            failures++;
        }
        os64_date_from_epoch_tz(1793512800, &tz, &d);   // one second later
        if (d.hour != 1 || d.minute != 0 || d.second != 0) {
            os64_printf("timetest: FAIL — fall lands %02d:%02d:%02d\n",
                        d.hour, d.minute, d.second);
            failures++;
        }
    }

    // DST off is spelled by absence: EST5 must stay standard in July.
    os64_tz_t est;
    if (os64_tz_parse("EST5", &est) != 0 || est.has_dst ||
        os64_tz_offset_at(&est, 1785343594) != -300) {
        os64_puts("timetest: FAIL — EST5 (no DST) misbehaved\n");
        failures++;
    }
    if (os64_tz_parse("garbage", &tz) == 0) {
        os64_puts("timetest: FAIL — nonsense TZ accepted\n");
        failures++;
    }

    // 2: the syscall, sanity-banded.
    os64_time_t t;
    if (os64_time(&t) != 0) {
        os64_puts("timetest: FAIL — time() errored\n");
        failures++;
    } else {
        if (t.epoch < 1700000000 || t.epoch > 4000000000) {
            os64_printf("timetest: FAIL — epoch %ld outside sanity band\n",
                        (long)t.epoch);
            failures++;
        }
        if (t.ticks_per_second == 0 ||
            t.ticks_into_second >= t.ticks_per_second) {
            os64_printf("timetest: FAIL — phase %u/%u\n",
                        t.ticks_into_second, t.ticks_per_second);
            failures++;
        }
        if (os64_time(NULL) == 0) {
            os64_puts("timetest: FAIL — NULL out pointer accepted\n");
            failures++;
        }
    }

    // The human-readable readout (also proves date_now's tz application).
    os64_date_t now;
    if (os64_date_now(&now, NULL) == 0)
        os64_printf("timetest: local %d-%02d-%02d %02d:%02d:%02d (wd %d)\n",
                    now.year, now.month, now.day,
                    now.hour, now.minute, now.second, now.weekday);

    if (failures == 0) {
        os64_puts("timetest: PASS — counter and calendar agree\n");
        os64_debug_log("timetest: PASS");
    } else {
        os64_debug_log("timetest: FAIL");
    }
    return failures == 0 ? 0 : 1;
}
