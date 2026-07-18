// libos64 io.c — the friendly veneer over the raw write/exit syscalls.

#include "os64/io.h"
#include "os64/syscall.h"

long os64_write(int handle, const void *buf, size_t len)
{
    return (long)os64_syscall3(SYSCALL_WRITE, (uint64_t)handle,
                               (uint64_t)buf, (uint64_t)len);
}

long os64_read(int handle, void *buf, size_t len)
{
    return (long)os64_syscall3(SYSCALL_READ, (uint64_t)handle,
                               (uint64_t)buf, (uint64_t)len);
}

// strlen without libc: freestanding, and we own the whole world here.
static size_t os64_strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

long os64_puts(const char *s)
{
    return os64_write(SYSCALL_HANDLE_CONSOLE_OUT, s, os64_strlen(s));
}

void os64_exit(int code)
{
    os64_syscall1(SYSCALL_EXIT, (uint64_t)(unsigned int)code);
    __builtin_unreachable();
}

void os64_debug_log(const char *s)
{
    os64_syscall1(SYSCALL_DEBUG_LOG, (uint64_t)s);
}

long os64_open(const char *path, const char *mode)
{
    return (long)os64_syscall2(SYSCALL_OPEN, (uint64_t)path, (uint64_t)mode);
}

long os64_seek(int handle, long offset, int whence)
{
    return (long)os64_syscall3(SYSCALL_SEEK, (uint64_t)(int64_t)handle,
                               (uint64_t)offset, (uint64_t)(int64_t)whence);
}

long os64_opendir(const char *path)
{
    return (long)os64_syscall2(SYSCALL_OPEN, (uint64_t)path, (uint64_t)"d");
}

long os64_readdir(int handle, os64_dirent_t *entry)
{
    return (long)os64_syscall2(SYSCALL_READDIR, (uint64_t)(int64_t)handle,
                               (uint64_t)entry);
}

long os64_pipe(int h[2])
{
    return (long)os64_syscall1(SYSCALL_PIPE, (uint64_t)h);
}

long os64_close(int handle)
{
    return (long)os64_syscall1(SYSCALL_CLOSE, (uint64_t)(int64_t)handle);
}
