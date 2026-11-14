#ifndef OS64_AV_TIME_H
#define OS64_AV_TIME_H
/* Private to the FFmpeg port: a struct tm, and the conversions FFmpeg uses
 * to WRITE a time (metadata it formats). No clock reaches the library:
 * time() answers 0, which FFmpeg treats as "unknown", and no time zone
 * does either: local time is UTC. clock() is a stopwatch (micros()), which
 * is all FFmpeg asks of it (random_seed.c's fallback entropy). */
#include <stddef.h>
#include <stdint.h>
#include "av_libc.h"
typedef int64_t time_t;
typedef int64_t clock_t;
#define CLOCKS_PER_SEC 1000000
clock_t avport_clock(void);
#define clock avport_clock
#define time(t) ((time_t)0)

/* av_libc.c's struct, under the C name: the same fields in the same order. */
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
_Static_assert(sizeof(struct tm) == sizeof(struct avlibc_tm), "struct tm is avlibc_tm");
_Static_assert(__builtin_offsetof(struct tm, tm_isdst) == __builtin_offsetof(struct avlibc_tm, tm_isdst),
               "struct tm is avlibc_tm");
static inline struct tm *gmtime(const time_t *t) { return (struct tm *)(void *)avlibc_gmtime(t); }
static inline struct tm *localtime(const time_t *t) { return gmtime(t); }
static inline time_t mktime(struct tm *tm) { return avlibc_mktime((struct avlibc_tm *)(void *)tm); }
static inline size_t strftime(char *out, size_t cap, const char *fmt, const struct tm *tm)
{
    return avlibc_strftime(out, cap, fmt, (const struct avlibc_tm *)(const void *)tm);
}
#endif
