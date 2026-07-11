// libos64 io.c — the friendly veneer over the raw write/exit syscalls.

#include "os64/io.h"
#include "os64/syscall.h"

long os64_write(int handle, const void *buf, size_t len)
{
    return (long)os64_syscall3(SYSCALL_WRITE, (uint64_t)handle,
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
