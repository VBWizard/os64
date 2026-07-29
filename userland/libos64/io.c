// libos64 io.c — the friendly veneer over the raw write/exit syscalls.

#include "os64/io.h"
#include "os64/str.h"      // os64_strlen — was a private static here until
                           // env.c needed string helpers too and it graduated
#include "os64/syscall.h"

int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    return (long)os64_syscall3(SYSCALL_WRITE, (uint64_t)handle,
                               (uint64_t)buf, (uint64_t)len);
}

int64_t os64_read(int32_t handle, void *buf, size_t len)
{
    return (long)os64_syscall3(SYSCALL_READ, (uint64_t)handle,
                               (uint64_t)buf, (uint64_t)len);
}

int64_t os64_puts(const char *s)
{
    return os64_write(SYSCALL_HANDLE_CONSOLE_OUT, s, os64_strlen(s));
}

int64_t os64_printat(uint32_t x, uint32_t y, const char *s)
{
    return (int64_t)os64_syscall3(SYSCALL_PRINTAT, (uint64_t)x, (uint64_t)y,
                                  (uint64_t)s);
}

void os64_debug_log(const char *s)
{
    os64_syscall1(SYSCALL_DEBUG_LOG, (uint64_t)s);
}

int64_t os64_open(const char *path, const char *mode)
{
    return (long)os64_syscall2(SYSCALL_OPEN, (uint64_t)path, (uint64_t)mode);
}

int64_t os64_seek(int32_t handle, int64_t offset, int32_t whence)
{
    return (long)os64_syscall3(SYSCALL_SEEK, (uint64_t)(int64_t)handle,
                               (uint64_t)offset, (uint64_t)(int64_t)whence);
}

int64_t os64_opendir(const char *path)
{
    return (long)os64_syscall2(SYSCALL_OPEN, (uint64_t)path, (uint64_t)"d");
}

int64_t os64_stat(const char *path, os64_dirent_t *entry)
{
    return (long)os64_syscall2(SYSCALL_STAT, (uint64_t)path, (uint64_t)entry);
}

int64_t os64_readdir(int32_t handle, os64_dirent_t *entry)
{
    return (long)os64_syscall2(SYSCALL_READDIR, (uint64_t)(int64_t)handle,
                               (uint64_t)entry);
}

int64_t os64_pipe(int32_t h[2])
{
    return (long)os64_syscall1(SYSCALL_PIPE, (uint64_t)h);
}

int64_t os64_close(int32_t handle)
{
    return (long)os64_syscall1(SYSCALL_CLOSE, (uint64_t)(int64_t)handle);
}
