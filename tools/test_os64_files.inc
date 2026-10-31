// test_os64_files.inc — libos64's file calls over POSIX, for the host
// suites that run libway's cache against a real temporary directory
// (tools/test_way_host.c through test_way_cache.inc, and the fetch job's,
// tools/test_yonder_fetch_job_host.c). Each is a few lines. The clock
// (os64_time) is each suite's own.
//
// A handle is a descriptor, or a directory stream, plus FILE_BASE, so it
// cannot be mistaken for the console the rest of a harness writes to: a
// write to anything below it is taken and dropped.

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "os64/io.h"

#define FILE_BASE 1000
#define DIRS_MAX 8
static DIR *s_dirs[DIRS_MAX];

uint64_t os64_taskid(void)
{
    return (uint64_t)getpid();
}

int64_t os64_open(const char *path, const char *mode)
{
    int flags = mode == NULL || mode[0] == 'r' ? O_RDONLY
              : mode[0] == 'x'                 ? O_WRONLY | O_CREAT | O_EXCL
                                               : O_WRONLY | O_CREAT | O_TRUNC;
    int fd = open(path, flags, 0644);
    return fd < 0 ? -1 : fd + FILE_BASE;
}

int64_t os64_read(int32_t handle, void *buf, size_t len)
{
    return handle >= FILE_BASE ? read(handle - FILE_BASE, buf, len) : -1;
}

int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    if (handle >= FILE_BASE)
        return write(handle - FILE_BASE, buf, len);
    (void)buf;
    return (int64_t)len;
}

int64_t os64_close(int32_t handle)
{
    if (handle >= FILE_BASE + 4096 && handle < FILE_BASE + 4096 + DIRS_MAX) {
        closedir(s_dirs[handle - FILE_BASE - 4096]);
        s_dirs[handle - FILE_BASE - 4096] = NULL;
        return 0;
    }
    return handle >= FILE_BASE ? close(handle - FILE_BASE) : -1;
}

int64_t os64_unlink(const char *path)
{
    return unlink(path) == 0 || rmdir(path) == 0 ? 0 : -1;
}

int64_t os64_rename(const char *from, const char *to)
{
    return rename(from, to) == 0 ? 0 : -1;
}

int64_t os64_mkdir(const char *path)
{
    return mkdir(path, 0755) == 0 ? 0 : -1;
}

static void dirent_of(const struct stat *st, const char *name, os64_dirent_t *e)
{
    memset(e, 0, sizeof(*e));
    e->size = S_ISDIR(st->st_mode) ? 0 : (uint64_t)st->st_size;
    e->flags = S_ISDIR(st->st_mode) ? OS64_DE_DIR : 0;
    e->mtime = (uint64_t)st->st_mtime;
    snprintf(e->name, sizeof(e->name), "%s", name);
}

int64_t os64_stat(const char *path, os64_dirent_t *e)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    const char *slash = strrchr(path, '/');
    dirent_of(&st, slash != NULL ? slash + 1 : path, e);
    return 0;
}

static char s_dir_paths[DIRS_MAX][512];

int64_t os64_opendir(const char *path)
{
    for (int i = 0; i < DIRS_MAX; i++)
        if (s_dirs[i] == NULL) {
            s_dirs[i] = opendir(path);
            if (s_dirs[i] == NULL)
                return -1;
            snprintf(s_dir_paths[i], sizeof(s_dir_paths[i]), "%s", path);
            return FILE_BASE + 4096 + i;
        }
    return -1;
}

// Called once, the next time a directory has been read to its end: what
// another thread does between a sweep's count of the directory and its
// last word on it.
static void (*s_after_listing)(void) __attribute__((unused));

int64_t os64_readdir(int32_t handle, os64_dirent_t *e)
{
    int i = handle - FILE_BASE - 4096;
    struct dirent *d;
    while ((d = readdir(s_dirs[i])) != NULL) {
        if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
            continue;
        char path[1024];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", s_dir_paths[i], d->d_name);
        if (stat(path, &st) != 0)
            continue;
        dirent_of(&st, d->d_name, e);
        return 1;
    }
    void (*after)(void) = s_after_listing;
    s_after_listing = NULL;
    if (after != NULL)
        after();
    return 0;
}
