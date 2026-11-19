/* js_bench_host_glibc.c — glibc's routines in place of libos64's, for the
 * JS benchmark host twin's experiments (tools/js_bench_host.sh
 * --glibc-malloc, --glibc-string): each switch replaces one group of what
 * libjs's engine calls, so a timing against the plain twin says what that
 * group of libos64's costs the engine, on one machine, with nothing else
 * moved. It answers no question about correctness: it is never shipped. */

#define _GNU_SOURCE
#include <malloc.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef JSB_GLIBC_MALLOC
void *os64_malloc(size_t n) { return malloc(n); }
void *os64_calloc(size_t count, size_t size) { return calloc(count, size); }
void *os64_realloc(void *p, size_t n) { return realloc(p, n); }
void os64_free(void *p) { free(p); }
size_t os64_malloc_size(const void *p) { return p != NULL ? malloc_usable_size((void *)p) : 0; }
void *os64_malloc_aligned(size_t n, size_t alignment)
{
    void *p = NULL;
    return posix_memalign(&p, alignment < sizeof(void *) ? sizeof(void *) : alignment, n) == 0 ? p : NULL;
}
#endif

#ifdef JSB_GLIBC_STRING
void *os64_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
void *os64_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
void *os64_memset(void *d, int c, size_t n) { return memset(d, c, n); }
int os64_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
void *os64_memchr(const void *s, int c, size_t n) { return memchr(s, c, n); }
size_t os64_strlen(const char *s) { return strlen(s); }
int os64_strcmp(const char *a, const char *b) { return strcmp(a, b); }
char *os64_strchr(const char *s, int c) { return strchr(s, c); }
char *os64_strrchr(const char *s, int c) { return strrchr(s, c); }
#endif
