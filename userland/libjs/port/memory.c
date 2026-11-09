#include <stddef.h>
#include "os64/str.h"

/* GCC can emit conventional memory calls for aggregate copies even with the
 * source names mapped to os64. Keep those entry points private and shared. */
void *memcpy(void *dst, const void *src, size_t n) { return os64_memcpy(dst, src, n); }
void *memmove(void *dst, const void *src, size_t n) { return os64_memmove(dst, src, n); }
void *memset(void *dst, int c, size_t n) { return os64_memset(dst, c, n); }
int memcmp(const void *a, const void *b, size_t n) { return os64_memcmp(a, b, n); }
