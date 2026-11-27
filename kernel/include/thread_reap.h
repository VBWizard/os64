#ifndef THREAD_REAP_H
#define THREAD_REAP_H

// A thread's stacks end with the thread (THREAD_STACKS.md § The reap). A
// thread that ends by thread_exit is handed, by the scheduler pass that files
// it ZOMBIE, to the reap list (reaplist.h); kworker releases its stacks and
// I/O scratch; its thread_t stays on the task's chain until burial. The list
// is kReapLock's, a leaf: the pass takes it under the queue lock, and nothing
// is taken under it.

#include <stdbool.h>
#include <stdint.h>

#include "thread.h"
#include "task.h"

// A task with more of its threads handed and not yet released than this
// makes no new thread until kworker has caught up (syscall_thread), or until
// it has parked THREAD_REAP_PARK_MOST_US: a thread's creation never hangs on
// a daemon. It then proceeds, and the reap's counts say so.
#define THREAD_REAP_BACKLOG 16
#define THREAD_REAP_PARK_MOST_US 500000

// The pass that files `t` ZOMBIE, queue lock held and already off `t`'s
// kernel stack: hand it to the list if it ended by thread_exit, and wake
// kworker.
void thread_reap_hand_locked(thread_t *t);

// processSignals, queue lock held: wake kworker if threads wait on the list.
// The hand-off wakes it too; this catches the hand-off that came while
// kworker was awake, between its drain and its nap.
void thread_reap_wake_if_owed_locked(void);

// kworker: release every handed thread's stacks and scratch: claim every
// one pending (up to a bound a pass), unmap all their stacks, wait ONCE for
// every CPU to flush (tlb_shootdown_wait), then free all their frames, so
// the acknowledgement's latency is paid once a round, not once a thread.
// Thread context, interrupts on, no lock held. Returns how many it released.
uint32_t thread_reap_drain(void);

// task_destroy: free the thread's stacks and scratch unless the reap did.
// A thread still on the list is claimed and freed here; its task's address
// space is being torn down, so nothing is unmapped and nobody is waited for.
void thread_reap_at_burial(thread_t *t);

// syscall_thread parked over the backlog for `parked_us`, and `proceeded`
// past it when the bound ran out: for the counts.
void thread_reap_note_park(uint64_t parked_us, bool proceeded);

// /sys/memory/reap.
typedef struct
{
	uint32_t queued, queued_most, window_queued_most;
	uint64_t handed, by_kworker, by_burial, refused, window_handed, window_released;
	uint64_t rounds, frame_bytes, scratches, creator_parks, creator_proceeded, creator_park_most_us;
} thread_reap_counts_t;
void thread_reap_read(thread_reap_counts_t *out);

#endif
