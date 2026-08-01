#ifndef OS64_ABI_TIME_H
#define OS64_ABI_TIME_H

// os64_time_t — what the time() syscall delivers: the raw truth about "now",
// with zero calendar opinions. The kernel keeps a COUNTER (seconds since
// 1970-01-01 00:00:00 UTC, seeded from the RTC at boot, advanced by the
// timer interrupt); knowing what a "March" or a "Tuesday" is happens in
// libos64 (<os64/date.h>). That split is original-vintage Unix and it is a
// genuinely good boundary: calendar math has opinions (leap years, zones,
// someday DST) and counters don't — so the opinions stay in the library,
// where fixing them never means rebooting.
//
// Sub-second: the timer IRQ counts ticks WITHIN the current second on its
// way to rolling epoch over (ticks_into_second / ticks_per_second is the
// fraction). This is the real phase, not a guess — a clock that displays
// tenths from it shows tenths that actually agree with when the second
// changes. All fields are one consistent snapshot: the syscall re-reads
// across the second boundary so you never see second N paired with the
// fraction of second N+1.
//
// tz_offset_minutes is the machine's CONFIGURED zone, minutes east of UTC
// (minutes, not hours: India is +5:30 and Nepal +5:45 — half the world's
// software learns this in production). It is data, not applied: epoch is
// always UTC, and local time is epoch + tz_offset_minutes*60 done BY YOU
// (or by os64_date_now, which does it for you). One clock, many faces —
// the mistake of storing local time in the counter gets fixed at every
// site that ever made it, usually twice a year.

#include <stdint.h>

typedef struct {
    int64_t  epoch;             // seconds since the epoch, UTC, no exceptions
    int32_t  tz_offset_minutes; // configured zone: minutes EAST of UTC
    uint32_t ticks_into_second; // 0..ticks_per_second-1 — sub-second phase
    uint32_t ticks_per_second;  // the live tick rate (same truth as ticks())
    uint32_t reserved;          // keeps the struct 8-byte tidy; always 0
} os64_time_t;

#endif // OS64_ABI_TIME_H
