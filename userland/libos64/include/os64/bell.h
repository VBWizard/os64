#ifndef OS64_BELL_H
#define OS64_BELL_H

// os64/bell.h — a thing one thread waits on and another rings.
//
// The wait primitive of this OS is "read a handle with patience", and a
// bell is the handle to read when there are no bytes to wait for: a thread
// that must be told "something changed, look again" by another thread of
// its program. It costs one handle and no buffer (the kernel object is
// SYSCALL_BELL's, in os64/syscall_numbers.h).
//
//     os64_bell_t bell;
//     os64_bell_make(&bell);
//
//     // the waiter                           // a ringer, any thread
//     for (;;) {                              lock(&l);
//         lock(&l);                           queue_push(&q, item);
//         item = queue_pop(&q);               os64_bell_ring(&bell);
//         unlock(&l);                         unlock(&l);
//         if (item) break;
//         if (os64_bell_wait(&bell, 5000) < 0) ...timeout, signal...
//     }
//
// WHAT A BELL IS FOR. One waiter, many ringers, and a ring is never lost:
// one made before the wait is kept and ends the next wait at once. That is
// what makes "look, find nothing, wait" safe with no lock held across the
// wait — a ring that lands between the look and the wait is still there.
//
// WHAT IT IS NOT. Not a counter: two rings before a wait are one, so a
// bell never says how many things happened, only that the waiter should
// look. Every wait is therefore a LOOP that looks again when rung and
// waits again if it found nothing; a ring left over from an earlier wait
// costs one such pass. And not a queue of waiters: a second thread waiting
// on a bell that has one is refused.
//
// THE RULE ITS RINGERS KEEP. A ring is a write to a handle, and a handle is
// a number: a ring made after the owner closed the bell, and after the
// number went to the next open, writes a byte into somebody else's file or
// socket. So whoever keeps a bell in order to ring it keeps it under a
// lock, rings while holding that lock (a ring never blocks, so it may),
// and the owner takes the same lock to take the bell back before closing
// it. The example above does this with `l`.

#include <stdint.h>

typedef struct {
    int32_t handle;         // the kernel bell; negative when there is none
} os64_bell_t;

// A bell that is not one yet, for a struct made before its bell is.
#define OS64_BELL_NONE {-1}

// Make a bell. Returns 0, or negative when the handle table is full (and
// *bell is then OS64_BELL_NONE).
int64_t os64_bell_make(os64_bell_t *bell);

// Wait up to timeout_ms for a ring (0 = only look, OS64_WAIT_FOREVER = no
// limit). Returns 1 when rung, consuming the ring; OS64_ERR_TIMEOUT when
// the patience ran out unrung; OS64_INTERRUPTED when a signal the program
// handles ended the wait; 0 when the bell was closed under the wait; and
// another negative when the bell is none or a second thread is waiting.
int64_t os64_bell_wait(os64_bell_t *bell, uint64_t timeout_ms);

// Ring it, from any thread. Never blocks. Returns 0, or negative when the
// bell is none or closed.
int64_t os64_bell_ring(os64_bell_t *bell);

// Close it and leave *bell OS64_BELL_NONE. A thread waiting in it wakes
// with 0. Closing a bell that is none does nothing.
void os64_bell_close(os64_bell_t *bell);

#endif // OS64_BELL_H
