// libos64 proc.c — process control veneer (scaffolding: yield only for now).

#include "os64/proc.h"
#include "os64/syscall.h"

void os64_yield(void)
{
    os64_syscall0(SYSCALL_YIELD);
}
