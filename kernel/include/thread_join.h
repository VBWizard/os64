#ifndef THREAD_JOIN_H
#define THREAD_JOIN_H

// thread_join.h — the object behind a thread HANDLE.
//
// os64's first ring-3 threads (2026-08-02). A program calls os64_thread()
// and gets back an ordinary handle; reading that handle blocks until the
// thread finishes and yields its return value; closing it says "I don't
// care what you return." No thread_wait verb, no thread_detach verb, no
// id-reuse hazard — the handle model already means all of that, which is
// the whole argument for using it (ruling shape borrowed from the network
// listener: read IS the wait).
//
// WHY A SEPARATE OBJECT rather than pointing the handle at the thread_t:
// the two have independent lifetimes. A thread can finish long before
// anyone reads its handle, and a handle can be closed while the thread
// runs on. This little struct outlives whichever goes first, carrying the
// one fact both sides care about — the answer — and a refcount decides
// when it is safe to free. Pointing a handle at a thread_t would be a
// use-after-free waiting for a race to find it.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "thread.h"

typedef struct thread_join
{
	uint64_t threadID;          // the thread this describes — for /proc, top, and logs
	volatile bool exited;       // set once, by the thread, on its way out
	int64_t retval;             // what it returned (valid only when exited)

	// Two references at birth: one held by the handle, one by the running
	// thread. Whoever drops last frees it. Atomic because the thread and
	// the closer can be on different cores at the same instant.
	volatile uint32_t refcount;

	thread_t* volatile waiter;  // a reader parked in thread_join_read
	struct thread_join* next;   // kThreadJoinList, for the wake sweep
} thread_join_t;

// In-band sentinel, the pipe.c convention.
#define THREAD_JOIN_ERR_INTERRUPTED (-3L)

// Create a ring-3 thread in `task` that starts at `entry` with `arg` in
// RDI, and whose function-return lands on `exit_stub` (a userland address
// — libos64 supplies it; see the ABI note in syscall_numbers.h). Returns
// the join object, or NULL. The thread is submitted to the scheduler
// before this returns, so it may already be running.
thread_join_t* thread_join_create(void* task, uint64_t entry, uint64_t arg,
                                  uint64_t exit_stub);

// Called by the exiting thread (from the thread_exit syscall): records the
// answer, wakes any reader, and drops the thread's reference.
void thread_join_finish(uint64_t threadID, int64_t retval);

// Block until the thread finishes; returns its value. Task context only.
long thread_join_read(thread_join_t* j, int64_t* out);

// Drop the handle's reference ("I don't care about your answer").
void thread_join_close(thread_join_t* j);

// Level-triggered wake sweep, from processSignals beside its siblings.
void thread_join_wake_if_ready(void);

#endif // THREAD_JOIN_H
