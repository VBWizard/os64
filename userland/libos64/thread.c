// thread.c — os64's ring-3 threading, such as it is: one call.
//
// os64_thread(fn, arg) starts a second line of execution inside this same
// program — same address space, same heap, same open handles — and hands
// back a HANDLE. Read the handle to wait for the thread and collect its
// return value; close it to stop caring. There is deliberately no
// thread_wait and no thread_detach, because read and close already mean
// exactly those things (see os64/thread.h for the argument).

#include <stdint.h>
#include "os64/thread.h"
#include "os64/syscall.h"
#include "os64/syscall_numbers.h"
#include "os64/io.h"          // os64_read — a thread handle is read like any other

// Defined in start/launch.S — the address a thread function returns to.
extern void os64_thread_trampoline(void);

int64_t os64_thread(int64_t (*fn)(void *), void *arg)
{
    if (fn == 0)
        return -1;
    return (int64_t)os64_syscall3(SYSCALL_THREAD, (uint64_t)fn, (uint64_t)arg,
                                  (uint64_t)&os64_thread_trampoline);
}

int64_t os64_thread_join(int32_t handle, int64_t *retval)
{
    int64_t value = 0;
    int64_t n = os64_read(handle, &value, sizeof(value));
    if (n != (int64_t)sizeof(value))
        return -1;
    if (retval != 0)
        *retval = value;
    return 0;
}

void os64_thread_exit(int64_t retval)
{
    os64_syscall1(SYSCALL_THREAD_EXIT, (uint64_t)retval);
    for (;;) { }   // never reached
}
