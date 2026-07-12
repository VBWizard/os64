// libos64 proc.c — process control veneer (scaffolding: yield only for now).

#include "os64/proc.h"
#include "os64/syscall.h"

void os64_yield(void)
{
    os64_syscall0(SYSCALL_YIELD);
}

long os64_spawn(const char *path, char *const argv[])
{
    return (long)os64_syscall2(SYSCALL_SPAWN, (uint64_t)path, (uint64_t)argv);
}

long os64_wait(long pid, int *exit_code)
{
    return (long)os64_syscall2(SYSCALL_WAIT, (uint64_t)pid, (uint64_t)exit_code);
}
