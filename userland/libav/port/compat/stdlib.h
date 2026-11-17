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
/* av_malloc's door (config_overrides answers HAVE_POSIX_MEMALIGN 1): FFmpeg
 * aligns its buffers to ALIGN for its SIMD and later frees and reallocs them
 * through plain free and realloc, which libos64's aligned block allows: it
 * is an ordinary block. Inline, so the name never leaves the library. */
static inline int posix_memalign(void **out, size_t alignment, size_t size)
{
    void *p = os64_malloc_aligned(size, alignment < 16 ? 16 : alignment);
    if (p == (void *)0)
        return 12; /* ENOMEM */
    *out = p;
    return 0;
}
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
