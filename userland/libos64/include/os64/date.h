#ifndef OS64_DATE_H
#define OS64_DATE_H

// libos64's calendar — the OTHER half of the time() split (LIBOS64.md layer).
// The kernel keeps a counter (<os64/time.h>: UTC epoch seconds, tz offset,
// sub-second phase); everything that knows what a "March" or a "Tuesday" is
// lives HERE, in the library, where opinions can be fixed without rebooting.
// Unix drew this exact line in the early 70s and never moved it: time(2) in
// the kernel, localtime/ctime in libc. We inherit the line, not the wart
// museum behind it (struct tm's 0-based months and years-since-1900 have
// caused more off-by-forever bugs than any other struct in C — ours counts
// like a human).

#include <stdint.h>
#include <stddef.h>
#include "os64/time.h"   // os64_time_t — the raw syscall struct (abi)

// A moment on the civil calendar, fields counted the way people count them:
// January is 1, the 3rd is 3, 2026 is 2026. Weekday 0 is Sunday (the one
// arbitrary choice; it matches what every wall calendar in the room shows).
typedef struct {
    int32_t year;      // e.g. 2026
    int32_t month;     // 1..12 — January is 1, like on paper
    int32_t day;       // 1..31
    int32_t hour;      // 0..23
    int32_t minute;    // 0..59
    int32_t second;    // 0..59
    int32_t weekday;   // 0..6, 0 = Sunday
    int32_t utc_offset_minutes; // minutes east of UTC for %z
    char    zone[8];            // display name for %Z ("UTC", "EST", ...)
} os64_date_t;

// The raw syscall: fill *t with the kernel's snapshot (UTC epoch, tz offset
// in minutes, sub-second tick phase). Returns 0, or negative on a bad
// pointer. Use this directly when you want the phase (a clock painting
// tenths) or UTC itself; use os64_date_now() when you just want the wall.
int64_t os64_time(os64_time_t *t);

// Set the kernel's UTC wall clock. This changes the running system clock but
// deliberately does not write the battery-backed RTC; that is a separate
// hardware-policy operation. The monotonic ticks clock is never affected.
int64_t os64_set_time(int64_t epoch);

// Break an epoch (seconds since 1970-01-01 UTC — ANY epoch, not just now)
// into calendar fields. Pure math, no syscall: feed it epoch + tz*60 for
// local time, or the raw epoch for UTC, or a file's timestamp when stat
// learns to deliver one. Handles pre-1970 epochs correctly (negative
// seconds are the past, not an error — RTC batteries die).
void os64_date_from_epoch(int64_t epoch, os64_date_t *out);

// The inverse conversion: validate human-counted calendar fields and turn
// them into a UTC epoch. The offset/zone/weekday fields are ignored. Returns
// 0 and fills *epoch, or negative for an impossible date.
int64_t os64_date_to_epoch(const os64_date_t *date, int64_t *epoch);

// Interpret calendar fields as LOCAL time under the same TZ policy as
// os64_date_now/os64_localtime. Nonexistent DST times are rejected; when the
// fall transition repeats an hour, standard time wins deterministically.
int64_t os64_mktime(const os64_date_t *date, int64_t *epoch);

// Format an os64_date_t with the standard strftime vocabulary used by the
// utilities. Supported conversions: a A b B c d e F H I j m M n p S t T u
// w x X y Y z Z and %. Returns bytes written (not including NUL), or 0 if
// the output does not fit. No locale is installed yet, so names are English.
size_t os64_strftime(char *buffer, size_t capacity, const char *format,
                     const os64_date_t *date);

// The everyday call: "what does the wall clock say, right here?" — one
// syscall, timezone applied, calendar broken out. Returns 0, or negative if
// the syscall failed. If `raw` is non-NULL the kernel snapshot is stored
// there too, so a clock gets its tenths (raw->ticks_into_second) and its
// face from ONE snapshot instead of two racing ones.
//
// TIMEZONE RESOLUTION: if the environment carries a parseable TZ (below),
// that is the answer — offset and daylight-saving policy both. Otherwise
// the kernel's configured standard offset (the syscall's tz_offset_minutes)
// applies, with no DST. The env is per-process and inherited through spawn,
// so one TZ= on the kernel cmdline reaches everything husk ever launches.
int64_t os64_date_now(os64_date_t *out, os64_time_t *raw);

// os64_date_now for a moment that ISN'T now: convert any epoch (a dirent's
// mtime is the founding customer) to local calendar fields under the SAME
// timezone resolution as os64_date_now — env TZ first with full DST policy,
// kernel's standard offset as the fallback. DST is evaluated at the moment
// being converted, not at the moment of asking: a January file renders in
// EST while your July prompt lives in EDT, which is the correct answer and
// the reason this lives in the library once instead of in every utility
// differently. Returns 0 (the fallback degrades to UTC if even the time
// syscall declines, which is a boot-order curiosity, not a real day).
//
//     os64_dirent_t e;  os64_date_t d;
//     os64_stat(path, &e);
//     os64_localtime((int64_t)e.mtime, &d);   // d.year/d.month/d.day/...
int64_t os64_localtime(int64_t epoch, os64_date_t *out);

// ---- The TZ string, parsed -------------------------------------------------
//
// os64 adopts the classic TZ format (V7 Unix, 1979 — later POSIX), because
// every Unix hand alive already knows it, one string carries the whole
// policy, and this OS's own ancestor (os32) spoke a dialect of it:
//
//     TZ=EST5EDT              offset + DST on, US switch dates
//     TZ=EST5                 offset only, DST off (the toggle IS the
//                             presence of the daylight name)
//     TZ=CET-1CEST,M3.5.0,M10.5.0    the EU, with explicit rules
//
// Grammar: <std name><offset>[<dst name>[<offset>]][,<start>,<end>]
//  * Names are 3+ letters. The DAYLIGHT name's presence enables DST; its
//    offset defaults to one hour ahead of standard (right for virtually
//    everyone on Earth).
//  * Offsets are h[h][:mm], counting hours WEST of UTC — V7's little prank:
//    Eastern is 5, Tokyo is -9. Everyone hits this once.
//  * Rules are M<month>.<week>.<weekday>[/h[:mm]] — weekday 0 = Sunday,
//    week 5 = "last", transition time defaults to 2:00 local. Rules omitted
//    = the US rule (M3.2.0,M11.1.0), matching POSIX's default. The J/day-
//    number rule forms are declined until someone actually needs them.

typedef struct {
    int32_t month;    // 1..12
    int32_t week;     // 1..5, 5 = last <weekday> of the month
    int32_t weekday;  // 0..6, 0 = Sunday
    int32_t minute;   // transition time, minutes after local midnight
} os64_tz_rule_t;

typedef struct {
    char           std_name[8];      // "EST" — display, not semantics
    char           dst_name[8];      // "EDT", or "" when DST is off
    int32_t        std_offset_min;   // minutes EAST of UTC (parser converts)
    int32_t        dst_offset_min;   // meaningful only when has_dst
    int32_t        has_dst;          // the toggle
    os64_tz_rule_t dst_start;        // when daylight time begins...
    os64_tz_rule_t dst_end;          // ...and ends (interpreted in DST time)
} os64_tz_t;

// Parse a TZ string. Returns 0 and fills *tz, or negative on a malformed
// string (*tz is then unusable). Pure string work, no syscall.
int64_t os64_tz_parse(const char *s, os64_tz_t *tz);

// Is daylight saving in force at this UTC epoch, under this zone?
// (Southern-hemisphere zones — start after end — wrap the year correctly.)
int32_t os64_tz_dst_at(const os64_tz_t *tz, int64_t epoch);

// The zone's offset at this UTC epoch, minutes east of UTC — std or dst,
// whichever the rules say. This is the one clock displays should use.
int32_t os64_tz_offset_at(const os64_tz_t *tz, int64_t epoch);

// Break a UTC epoch into LOCAL calendar fields under a parsed zone —
// os64_date_from_epoch with the zone's live offset applied for you.
void os64_date_from_epoch_tz(int64_t epoch, const os64_tz_t *tz,
                             os64_date_t *out);

#endif // OS64_DATE_H
