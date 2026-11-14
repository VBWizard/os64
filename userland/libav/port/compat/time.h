#ifndef OS64_AV_TIME_H
#define OS64_AV_TIME_H
/* Private to the FFmpeg port; string.h beside this file says who sees
 * what. A struct tm, and the conversions FFmpeg uses to WRITE a time
 * (metadata it formats), in UTC: no time zone reaches the library, so
 * local time is UTC. clock() is a stopwatch (micros()), which is all
 * FFmpeg asks of it (random_seed.c's fallback entropy). */
#include <stddef.h>
#include <stdint.h>
typedef int64_t time_t;
typedef int64_t clock_t;
#define CLOCKS_PER_SEC 1000000
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
#ifdef HAVE_AV_CONFIG_H
#include "av_libc.h"
clock_t avport_clock(void);
#define clock avport_clock

/* av_libc.c works in its own struct (its host test includes glibc's
 * <time.h> beside it); these copy field by field, so no object is read
 * through a pointer to the other type. */
static inline struct avlibc_tm avport_tm_in(const struct tm *t)
{
    struct avlibc_tm a = { t->tm_sec, t->tm_min, t->tm_hour, t->tm_mday, t->tm_mon,
                           t->tm_year, t->tm_wday, t->tm_yday, t->tm_isdst };
    return a;
}
static inline struct tm avport_tm_out(const struct avlibc_tm *a)
{
    struct tm t = { a->tm_sec, a->tm_min, a->tm_hour, a->tm_mday, a->tm_mon,
                    a->tm_year, a->tm_wday, a->tm_yday, a->tm_isdst };
    return t;
}
/* C's gmtime answers with one struct of its own; this is one per file
 * that calls it, which no caller can tell apart. */
static inline struct tm *gmtime(const time_t *t)
{
    static struct tm answer;
    answer = avport_tm_out(avlibc_gmtime(t));
    return &answer;
}
static inline struct tm *localtime(const time_t *t) { return gmtime(t); }
static inline time_t mktime(struct tm *tm)
{
    struct avlibc_tm a = avport_tm_in(tm);
    time_t t = avlibc_mktime(&a);
    *tm = avport_tm_out(&a);
    return t;
}
static inline size_t strftime(char *out, size_t cap, const char *fmt, const struct tm *tm)
{
    struct avlibc_tm a = avport_tm_in(tm);
    return avlibc_strftime(out, cap, fmt, &a);
}
#endif
#endif
