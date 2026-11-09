#ifndef OS64_JSPORT_STDLIB_H
#define OS64_JSPORT_STDLIB_H
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include "os64/mem.h"
#include "platform.h"
#define alloca(size) __builtin_alloca(size)
#define malloc os64_malloc
#define calloc os64_calloc
#define realloc os64_realloc
#define free os64_free
#define abort() jsport_fatal("abort", __FILE__, __LINE__)
static inline int abs(int n) { return n < 0 ? -n : n; }
#endif
