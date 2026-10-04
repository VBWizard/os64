// Host adapter for cut's real entry point, argument parser and string code.
// The environment controls short I/O and failures without changing the app.
#include "os64/os64.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int cut_main(int argc, char **argv);
static size_t allocation_count;
static size_t written;
static size_t read_count;

static size_t setting(const char *name)
{
    const char *value = getenv(name);
    return value ? (size_t)strtoull(value, NULL, 10) : 0;
}

int64_t os64_read(int32_t handle, void *buffer, size_t length)
{
    size_t limit = setting("CUT_READ_CHUNK");
    if (setting("CUT_READ_FAIL") && ++read_count >= setting("CUT_READ_FAIL"))
        return -1;
    if (limit && length > limit) length = limit;
    ssize_t n;
    do { n = read(handle, buffer, length); } while (n < 0 && errno == EINTR);
    return n;
}

int64_t os64_write(int32_t handle, const void *buffer, size_t length)
{
    size_t limit = setting("CUT_WRITE_CHUNK");
    if (handle == OS64_STDOUT)
    {
        size_t failure = setting("CUT_WRITE_FAIL");
        if (failure && written >= failure) return -1;
        if (failure && length > failure - written) length = failure - written;
        if (setting("CUT_WRITE_ZERO")) return 0;
    }
    if (limit && length > limit) length = limit;
    ssize_t n;
    do { n = write(handle, buffer, length); } while (n < 0 && errno == EINTR);
    if (handle == OS64_STDOUT && n > 0) written += (size_t)n;
    return n;
}

int64_t os64_stat(const char *path, os64_dirent_t *entry)
{
    struct stat info;
    if (stat(path, &info) < 0) return -1;
    os64_memset(entry, 0, sizeof(*entry));
    if (S_ISDIR(info.st_mode)) entry->flags = OS64_DE_DIR;
    return 0;
}

int64_t os64_open(const char *path, const char *mode)
{
    (void)mode;
    if (setting("CUT_OPEN_FAIL")) return -1;
    return open(path, O_RDONLY);
}

int64_t os64_close(int32_t handle)
{
    int result = close(handle);
    return setting("CUT_CLOSE_FAIL") ? -1 : result;
}

void *os64_malloc(size_t length)
{
    size_t failure = setting("CUT_ALLOC_FAIL");
    return failure && ++allocation_count == failure ? NULL : malloc(length);
}

void *os64_realloc(void *pointer, size_t length)
{
    size_t failure = setting("CUT_ALLOC_FAIL");
    return failure && ++allocation_count == failure ? NULL : realloc(pointer, length);
}

void os64_free(void *pointer) { free(pointer); }

int main(int argc, char **argv) { return cut_main(argc, argv); }
