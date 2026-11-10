// bell.c — os64/bell.h over the kernel bell (SYSCALL_BELL): one handle,
// waited on by reading it and rung by writing it.
#include "os64/bell.h"
#include "os64/io.h"
#include "os64/syscall.h"

int64_t os64_bell_make(os64_bell_t *bell)
{
    int64_t h = (int64_t)os64_syscall0(SYSCALL_BELL);
    bell->handle = h < 0 ? -1 : (int32_t)h;
    return h < 0 ? h : 0;
}

int64_t os64_bell_wait(os64_bell_t *bell, uint64_t timeout_ms)
{
    if (bell->handle < 0)
        return -1;
    char rung;
    return os64_read_for(bell->handle, &rung, 1, timeout_ms);
}

int64_t os64_bell_ring(os64_bell_t *bell)
{
    if (bell->handle < 0)
        return -1;
    char ring = 1;
    int64_t wrote = os64_write(bell->handle, &ring, 1);
    return wrote == 1 ? 0 : wrote < 0 ? wrote : -1;
}

void os64_bell_close(os64_bell_t *bell)
{
    if (bell->handle >= 0)
        os64_close(bell->handle);
    bell->handle = -1;
}
