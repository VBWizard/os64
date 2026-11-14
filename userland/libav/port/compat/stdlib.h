#ifndef OS64_AV_STDLIB_H
#define OS64_AV_STDLIB_H
/* Private to the FFmpeg port; string.h beside this file says who sees
 * what. */
#include <stddef.h>
#include "os64/mem.h"
#define malloc os64_malloc
#define calloc os64_calloc
#define realloc os64_realloc
#define free os64_free
static inline int abs(int x) { return x < 0 ? -x : x; }
static inline long labs(long x) { return x < 0 ? -x : x; }
static inline long long llabs(long long x) { return x < 0 ? -x : x; }
#ifdef HAVE_AV_CONFIG_H
#include "av_libc.h"
/* strtod and abort need FFmpeg's own scanf and the program's exit, so they
 * live in av_port.c rather than the pure av_libc.c. */
double avport_strtod(const char *s, char **end);
_Noreturn void avport_abort(void);
#define strtol avlibc_strtol
#define strtoul avlibc_strtoul
#define strtoll avlibc_strtoll
#define strtoull avlibc_strtoull
#define strtod avport_strtod
#define qsort avlibc_qsort
#define bsearch avlibc_bsearch
#define abort avport_abort
/* No environment reaches the library: FFmpeg reads AV_LOG_FORCE_* here. */
#define getenv(name) ((char *)0)
static inline int atoi(const char *s) { return (int)avlibc_strtol(s, (char **)0, 10); }
#endif
#endif
