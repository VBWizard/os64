// thread.c — os64's ring-3 threading, such as it is.
//
// os64_thread(fn, arg) (or os64_thread_named, which names it) starts a second line of execution inside this same
// program — same address space, same heap, same open handles — and hands
// back a HANDLE. Read the handle to wait for the thread and collect its
// return value; close it to stop caring. There is deliberately no
// thread_wait and no thread_detach, because read and close already mean
// exactly those things (see os64/thread.h for the argument).
// os64_thread_or_nothing is the same start with a failure that means fn
// never ran.

#include <stdint.h>
#include "os64/thread.h"
#include "os64/syscall.h"
#include "os64/syscall_numbers.h"
#include "os64/io.h"          // os64_read — a thread handle is read like any other
#include "os64/mem.h"         // os64_malloc / os64_free, os64_thread_or_nothing's note
#include "os64/proc.h"        // os64_sleep, the stub's wait for the verdict

// Defined in start/launch.S — the address a thread function returns to.
extern void os64_thread_trampoline(void);

int64_t os64_thread_named(int64_t (*fn)(void *), void *arg, const char *name)
{
    if (fn == 0)
        return -1;
    return (int64_t)os64_syscall4(SYSCALL_THREAD, (uint64_t)fn, (uint64_t)arg,
                                  (uint64_t)&os64_thread_trampoline, (uint64_t)name);
}

int64_t os64_thread(int64_t (*fn)(void *), void *arg)
{
    return os64_thread_named(fn, arg, 0);
}

// os64_thread_or_nothing's note between the starter and the stub. The stub
// owns it once the verdict is in: the starter writes `verdict` last and
// never touches the note again.
#define VERDICT_PENDING 0
#define VERDICT_RUN 1
#define VERDICT_REFUSED 2
typedef struct {
    int64_t (*fn)(void *);
    void *arg;
    int verdict;
} start_note_t;

static int64_t start_stub(void *raw)
{
    start_note_t *note = raw;
    int verdict;
    while ((verdict = __atomic_load_n(&note->verdict, __ATOMIC_ACQUIRE)) == VERDICT_PENDING)
        os64_sleep(0);
    int64_t (*fn)(void *) = note->fn;
    void *arg = note->arg;
    os64_free(note);
    return verdict == VERDICT_RUN ? fn(arg) : 0;
}

int64_t os64_thread_or_nothing(int64_t (*fn)(void *), void *arg)
{
    if (fn == 0)
        return -1;
    start_note_t *note = os64_malloc(sizeof(*note));
    if (note == 0)
        return -1;
    *note = (start_note_t){fn, arg, VERDICT_PENDING};
    int64_t h = os64_thread(start_stub, note);
    __atomic_store_n(&note->verdict, h >= 0 ? VERDICT_RUN : VERDICT_REFUSED, __ATOMIC_RELEASE);
    return h;
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
