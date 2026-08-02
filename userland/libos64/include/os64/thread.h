#ifndef OS64_THREAD_H
#define OS64_THREAD_H

// os64/thread.h — a second line of execution inside one program.
//
// os64's first ring-3 threads (2026-08-02). A thread shares EVERYTHING
// with the program that started it: the same address space, the same
// heap, the same globals, the same open handles. That sharing is the
// definition, and it is also the whole danger — two threads touching one
// variable need a lock, and os64 has no locks yet (deliberately: locks
// get designed when a real consumer needs one, not before).
//
//     int64_t burn(void *arg) { ...spin... return 0; }
//
//     int64_t h = os64_thread(burn, (void *)1);
//     ...
//     int64_t answer;
//     os64_thread_join((int32_t)h, &answer);   // blocks
//
// WHAT YOU GET BACK IS A HANDLE, not a thread id, and that is the design:
//   read(h)   blocks until the thread finishes, and yields its return
//             value — os64_thread_join is a two-line wrapper around it
//   close(h)  detaches: the thread runs on, nobody collects its answer
// So threads need no verbs of their own. (The numeric thread id still
// exists and shows up in /proc and top — the handle is for WAITING, the
// id is for NAMING.)
//
// EXIT MEANS EXIT: if the program's main thread returns, the task ends
// and its threads end with it. A program that wants to wait must say so
// by reading the handles. (Chris's ruling, 2026-08-02 — adjustable if a
// burning need for daemon threads ever turns up, which we doubt.)

#include <stdint.h>

// Start a thread running fn(arg). Returns a HANDLE (>= 0), or negative.
int64_t os64_thread(int64_t (*fn)(void *), void *arg);

// Wait for a thread and collect its value. Returns 0, or negative if the
// handle isn't a thread. (Convenience only — os64_read on the handle does
// exactly this.) retval may be NULL if you only want the wait.
int64_t os64_thread_join(int32_t handle, int64_t *retval);

// End the CALLING thread now, with this value. A thread that simply
// returns from fn does this automatically, via the trampoline the kernel
// seeded on its stack — so most programs never call it.
void os64_thread_exit(int64_t retval);

#endif // OS64_THREAD_H
