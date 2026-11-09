#include "platform.h"
#include "os64/date.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/proc.h"
#include "os64/js.h"

JSPortFile jsport_stdout = {1}, jsport_stderr = {2};

int jsport_gettimeofday(struct jsport_timeval *tv, void *zone)
{
    os64_time_t now;
    (void)zone;
    if (tv == NULL || os64_time(&now) < 0 || now.ticks_per_second == 0 ||
        now.ticks_into_second >= now.ticks_per_second)
        jsport_fatal("wall clock unavailable", __FILE__, __LINE__);
    tv->tv_sec = now.epoch;
    tv->tv_usec = (int64_t)((uint64_t)now.ticks_into_second * 1000000 / now.ticks_per_second);
    return 0;
}

int jsport_timezone_offset(int64_t milliseconds)
{
    /* Floor to the UTC second: -1 ms belongs to the preceding second. */
    int64_t seconds = milliseconds / 1000;
    if (milliseconds % 1000 < 0)
        seconds--;
    os64_date_t date;
    /* The engine's TimeClip range is +/- 100 million days. Clamp native
     * misuse before libos64's calendar arithmetic; script dates fit unchanged. */
    if (seconds < -8640000000000LL) seconds = -8640000000000LL;
    if (seconds > 8640000000000LL) seconds = 8640000000000LL;
    if (os64_localtime(seconds, &date) < 0)
        jsport_fatal("local clock unavailable", __FILE__, __LINE__);
    return -date.utc_offset_minutes;
}

void jsport_fatal(const char *expression, const char *file, int line)
{
    os64_hprintf(2, "libjs: engine invariant failure: %s (%s:%d)\n", expression, file, line);
    os64_exit(OS64_JS_FATAL_EXIT);
}
