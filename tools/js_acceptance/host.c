/* Controlled OS services for both engine profiles. The runner selects target
 * or instrumented QuickJS; M1's target maths objects are linked unchanged. */
#define _POSIX_C_SOURCE 200809L
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "os64/date.h"
#include "os64/js.h"

typedef union { max_align_t alignment; struct { size_t size; } metadata; } Block;
static size_t live, refused;
static int deny_allocations, host_failures;
os64_time_t js_test_clock = {.epoch = 1700000000, .ticks_per_second = 100};
int js_test_clock_failed;

void *os64_malloc(size_t size)
{
    if (deny_allocations) { refused++; return NULL; }
    if (size > SIZE_MAX - sizeof(Block) - 15) return NULL;
    size = (size + 15) & ~(size_t)15;
    Block *block = malloc(sizeof(*block) + size);
    if (!block) return NULL;
    block->metadata.size = size;
    live++;
    return block + 1;
}

size_t os64_malloc_size(const void *pointer)
{
    return ((const Block *)pointer)[-1].metadata.size;
}

void os64_free(void *pointer)
{
    if (pointer) { live--; free((Block *)pointer - 1); }
}

void *os64_realloc(void *pointer, size_t size)
{
    if (!pointer) return os64_malloc(size);
    if (!size) { os64_free(pointer); return NULL; }
    void *replacement = os64_malloc(size);
    if (replacement) {
        size_t old = os64_malloc_size(pointer);
        memcpy(replacement, pointer, size < old ? size : old);
        os64_free(pointer);
    }
    return replacement;
}

const char *os64_getenv(const char *name) { return getenv(name); }

int64_t os64_micros(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return now.tv_sec * INT64_C(1000000) + now.tv_nsec / 1000;
}

int64_t os64_write(int32_t handle, const void *bytes, size_t size)
{
    if (handle != 1 && handle != 2) return -83;
    return (int64_t)fwrite(bytes, 1, size, handle == 1 ? stdout : stderr);
}

int64_t os64_open(const char *path, const char *mode)
{
    (void)path; (void)mode;
    host_failures++;
    return -84;
}
int64_t os64_read(int32_t handle, void *bytes, size_t size)
{
    (void)handle; (void)bytes; (void)size;
    host_failures++;
    return -84;
}
int64_t os64_close(int32_t handle)
{
    (void)handle;
    host_failures++;
    return -84;
}

void os64_exit(int32_t status)
{
    fprintf(stderr, "acceptance target exit: %08x\n", (unsigned)status);
    _Exit(status == OS64_JS_FATAL_EXIT ? 99 : 98);
}

void acceptance_refuse_allocations(void) { deny_allocations = 1; }

int acceptance_host_failures(void)
{
    if (!refused || live) {
        fprintf(stderr, "FAIL host: %zu refused, %zu live allocations\n", refused, live);
        host_failures++;
    }
    deny_allocations = 0;
    printf("Host diagnostic injection: %zu refused, %zu live allocations\n", refused, live);
    return host_failures;
}
