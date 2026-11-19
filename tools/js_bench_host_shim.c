/* js_bench_host_shim.c — the system calls os64's own objects make, answered
 * by Linux, so the JS benchmark host twin (tools/js_bench_host.sh) can link
 * the objects the cross compiler built for os64 — the engine core, libjs's
 * runtime, libmath, libos64's allocator and string code — and time them on
 * the host. Only the operating system's half is replaced; every routine a
 * benchmark exercises is os64's, built as os64 builds it.
 *
 * os64_map hands out page-rounded, zeroed regions with a guard page after
 * (os64/mem.h); mmap gives the same here, and the region's length is kept so
 * os64_unmap, which takes only the base, can give it back. */

#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "os64/dirent.h"
#include "os64/heap.h"
#include "os64/time.h"

#define SHIM_PAGE 4096u
#define SHIM_REGIONS 65536u

static struct { void *base; size_t length; } s_regions[SHIM_REGIONS];

void *os64_map(size_t len)
{
    size_t length = (len + SHIM_PAGE - 1) / SHIM_PAGE * SHIM_PAGE + SHIM_PAGE;
    void *base = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        return NULL;
    mprotect((char *)base + length - SHIM_PAGE, SHIM_PAGE, PROT_NONE);
    for (size_t i = 0; i < SHIM_REGIONS; i++)
        if (s_regions[i].base == NULL) {
            s_regions[i].base = base;
            s_regions[i].length = length;
            return base;
        }
    munmap(base, length);
    return NULL;
}

int64_t os64_unmap(void *base)
{
    for (size_t i = 0; i < SHIM_REGIONS; i++)
        if (s_regions[i].base == base) {
            int rc = munmap(base, s_regions[i].length);
            s_regions[i].base = NULL;
            return rc == 0 ? 0 : -1;
        }
    return -1;
}

int64_t os64_heap_publish(const os64_heap_report_t *report)
{
    (void)report;
    return 0;
}

/* The wall clock in os64's shape (os64/time.h): UTC seconds and the phase
 * into the second, counted here in microseconds. The zone is left to
 * libos64's calendar, which reads TZ as it does on os64. */
int64_t os64_time(os64_time_t *t)
{
    struct timespec now;
    if (t == NULL || clock_gettime(CLOCK_REALTIME, &now) != 0)
        return -1;
    memset(t, 0, sizeof(*t));
    t->epoch = now.tv_sec;
    t->ticks_into_second = (uint32_t)(now.tv_nsec / 1000);
    t->ticks_per_second = 1000000;
    return 0;
}

int64_t os64_set_time(int64_t epoch)
{
    (void)epoch;
    return -1;
}

int64_t os64_micros(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

int64_t os64_open(const char *path, const char *mode)
{
    int flags = mode != NULL && mode[0] == 'w' ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY;
    int fd = open(path, flags, 0644);
    return fd < 0 ? -1 : fd;
}

int64_t os64_read(int32_t handle, void *buf, size_t len)
{
    ssize_t got = read(handle, buf, len);
    return got < 0 ? -1 : got;
}

int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    ssize_t put = write(handle, buf, len);
    return put < 0 ? -1 : put;
}

int64_t os64_close(int32_t handle)
{
    return close(handle) == 0 ? 0 : -1;
}

int64_t os64_stat(const char *path, os64_dirent_t *entry)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    memset(entry, 0, sizeof(*entry));
    entry->size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
    entry->flags = S_ISDIR(st.st_mode) ? OS64_DE_DIR : 0;
    return 0;
}

const char *os64_getenv(const char *key)
{
    return getenv(key);
}

void os64_serial_log(const char *s)
{
    (void)s;
}

void os64_yield(void)
{
    sched_yield();
}

void os64_exit(int32_t code)
{
    _exit(code);
}
