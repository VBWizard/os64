#ifndef BELL_H
#define BELL_H

#include <stdbool.h>
#include <stdint.h>
#include "spinlock.h"
#include "thread.h"

// bell.h — a handle whose read WAITS and whose write RINGS.
//
// Ring 3 waits by reading a handle with patience (read's 4th argument). A
// thread with no bytes to wait for, only "something happened, look again",
// could wait on a pipe, at two handles and a 64 KiB ring for that one bit.
// A bell is the bit and nothing else: one handle, no buffer, one waiter.
// libfetch's HTTP/2 work is the consumer (docs/design/pending/HTTP2.md
// § 2.4): a fetch parks on its bell while another thread owns the socket.
//
//   read   returns 1 once the bell has been rung, consuming the ring; waits
//          for one under the caller's patience otherwise
//   write  rings: sets the flag and wakes the waiter. Never blocks
//   close  a waiter parked in the bell wakes to "closed"
//
// A RING IS NEVER LOST AND NEVER COUNTED. One that lands before the wait is
// kept in `rung` and ends the next wait at once; two that land before a
// wait are one. A bell says "look again", and its waiter looks at whatever
// the ringers changed — the count of rings is not information.
//
// NOT THE DOORBELL. doorbell.h is the kernel's own primitive, for kernel
// daemons a hardware interrupt must be able to wake without a lock. This is
// a ring-3 object behind a handle, rung by a syscall.

typedef struct bell
{
	spinlock_t lock;

	bool rung;                // a ring no wait has consumed
	bool closed;              // the handle is gone: waits and rings answer CLOSED

	// The one thread parked in (or on its way into) a wait, registered under
	// `lock`. The waiter clears its own slot at the top of every pass of its
	// loop; a ringer reads it under the same lock and wakes or owes
	// (scheduler_wake_or_owe), so a ring that lands between the registration
	// and the park is paid by the park itself (WAKE_OWED.md).
	thread_t *waiter;

	// Holders: the handle, and each operation in flight on it (handle.c
	// § The pin). The last one out frees the bell.
	uint32_t refs;
} bell_t;

// Results (negative; a wait's success is 1, a ring's is 0).
#define BELL_ERR_CLOSED      (-1) // the handle was closed
#define BELL_ERR_INTERRUPTED (-3) // the caller has a signal pending that ends
                                  // the wait (signal_park_must_end); the
                                  // syscall boundary kills or answers
                                  // OS64_INTERRUPTED; never reaches ring 3
#define BELL_ERR_TIMEOUT     (-4) // the deadline passed unrung
#define BELL_ERR_BUSY        (-5) // another thread is already waiting

// A new bell, unrung, with one holder: the handle it is about to be given.
// NULL when there is no memory for it.
bell_t *bell_create(void);

// An operation's hold, and its release. The last release of any holder
// frees the bell.
void bell_ref(bell_t *b);
void bell_release(bell_t *b);

// The handle's close: marks the bell closed, wakes a parked waiter to see
// it, and releases the handle's hold.
void bell_close(bell_t *b);

// Wait for a ring. Returns 1 when the bell was rung (the ring consumed),
// or one of the errors above. `deadline` is a kTicksSinceStart value, or 0
// to wait without limit; a ring already made outranks a deadline already
// passed, so a zero-patience wait still collects it.
long bell_wait(bell_t *b, uint64_t deadline);

// Ring. Returns 0, or BELL_ERR_CLOSED. Never blocks.
long bell_ring(bell_t *b);

#endif // BELL_H
