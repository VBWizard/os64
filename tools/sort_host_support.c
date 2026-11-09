// Host adapters for the real sort command and libos64 argument parser.
#include "os64/os64.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool fail_allocation(void)
{
    static unsigned calls;
    const char *limit = getenv("SORT_FAIL_ALLOC");
    return limit && ++calls == strtoul(limit, NULL, 10);
}

void *os64_malloc(size_t size) { return fail_allocation() ? NULL : malloc(size); }
void *os64_realloc(void *p, size_t size) { return fail_allocation() ? NULL : realloc(p, size); }
void os64_free(void *p) { free(p); }
void *os64_memcpy(void *a, const void *b, size_t n) { return memcpy(a, b, n); }
bool os64_streq(const char *a, const char *b) { return strcmp(a, b) == 0; }
int64_t os64_open(const char *path, const char *mode)
{
    (void)mode;
    return open(path, O_RDONLY);
}
int64_t os64_close(int32_t fd)
{
    int result = close(fd);
    return getenv("SORT_FAIL_CLOSE") ? -1 : result;
}
int64_t os64_stat(const char *path, os64_dirent_t *entry)
{
    struct stat st;
    if (stat(path, &st) < 0) return -1;
    entry->flags = S_ISDIR(st.st_mode) ? OS64_DE_DIR : 0;
    return 0;
}
int64_t os64_read(int32_t fd, void *data, size_t size)
{
    if (getenv("SORT_FAIL_READ")) return -1;
    if (getenv("SORT_SHORT_IO") && size > 7) size = 7;
    return read(fd, data, size);
}
int64_t os64_write(int32_t fd, const void *data, size_t size)
{
    if (getenv("SORT_FAIL_WRITE")) return -1;
    if (getenv("SORT_ZERO_WRITE")) return 0;
    if (getenv("SORT_SHORT_IO") && size > 3) size = 3;
    return write(fd, data, size);
}
int32_t os64_printf(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int result = vprintf(format, ap);
    va_end(ap);
    return result;
}
int32_t os64_hprintf(int32_t fd, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int result = vdprintf(fd, format, ap);
    va_end(ap);
    return result;
}
