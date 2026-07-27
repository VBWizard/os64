// mem.c — libos64 memory veneer. Two thin stubs today; the interesting code
// that will sit above them (malloc and friends) is deliberately NOT here —
// see os64/mem.h for whose it is.

#include "os64/mem.h"
#include "os64/syscall.h"

void *os64_map(size_t len)
{
    uint64_t r = os64_syscall1(SYSCALL_MAP, (uint64_t)len);
    // In-band failure: sentinels are huge values with the top bit set —
    // no valid lower-half user address looks like that.
    if ((int64_t)r < 0)
        return (void *)0;
    return (void *)r;
}

int64_t os64_unmap(void *base)
{
    return (long)os64_syscall1(SYSCALL_UNMAP, (uint64_t)base);
}

int64_t os64_memory(os64_memory_t *out)
{
    return (int64_t)os64_syscall1(SYSCALL_MEMORY, (uint64_t)out);
}
