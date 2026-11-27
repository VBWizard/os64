// Ended threads' stacks, released while their task lives (thread_reap.h;
// THREAD_STACKS.md § The reap has the lifecycle table).

#include <stddef.h>
#include <stdint.h>

#include "thread_reap.h"
#include "reaplist.h"
#include "spinlock.h"
#include "kworker.h"
#include "smp_core.h"
#include "panic.h"
#include "printd.h"
#include "memory/allocator.h"
#include "memory/kmalloc.h"

// Threads released per kworker pass, all under one waited invalidation, so
// a flood of ended threads cannot keep the worker from its other trades;
// what is left waits for the next pass, which comes at once (a drain that
// released something counts as work).
#define REAP_DRAIN_MOST 256

static spinlock_t kReapLock;
static reaplist_t s_reaps;
static uint64_t s_rounds, s_frame_bytes, s_scratches, s_creator_parks, s_creator_proceeded, s_creator_park_most_us;

static thread_t *thread_of(reap_node_t *n)
{
	return (thread_t *)((char *)n - offsetof(thread_t, reap));
}

void thread_reap_hand_locked(thread_t *t)
{
	if (!t->reapOnExit)
		return;
	task_t *task = (task_t *)t->ownerTask;
	uint64_t f = spinlock_acquire_irqsave(&kReapLock);
	bool handed = reaplist_hand(&s_reaps, &t->reap, &task->reaps);
	spinlock_release_irqrestore(&kReapLock, f);
	if (handed)
		kworker_wake_locked();
	else
		printd(DEBUG_EXCEPTIONS, "thread_reap_hand_locked: thread 0x%08lx of %s filed ZOMBIE again, already in reap state %u; left as it is\n",
		       t->threadID, task->exename, (unsigned)t->reap.state);
}

void thread_reap_wake_if_owed_locked(void)
{
	// Read without kReapLock: a count a moment stale is caught next pass.
	if (__atomic_load_n(&s_reaps.queued, __ATOMIC_RELAXED) != 0)
		kworker_wake_locked();
}

// The frames and scratch of a thread whose stacks no CPU can reach: unmapped
// and flushed everywhere, or of a task being buried. `counted`: a handed
// thread's, which /sys/memory/reap counts.
static void free_stacks(thread_t *t, bool counted)
{
	uint64_t bytes = 0;
	if (t->esp3BaseP != 0)
	{
		free_memory(t->esp3BaseP);
		bytes += t->esp3Size;
		t->esp3BaseP = 0;
	}
	if (t->esp0BaseP != 0)
	{
		free_memory(t->esp0BaseP);
		bytes += t->esp0Size;
		t->esp0BaseP = 0;
	}
	if (t->syscallIOScratch != NULL)
	{
		kfree(t->syscallIOScratch);
		t->syscallIOScratch = NULL;
		if (counted)
			__atomic_add_fetch(&s_scratches, 1, __ATOMIC_RELAXED);
	}
	if (counted)
		__atomic_add_fetch(&s_frame_bytes, bytes, __ATOMIC_RELAXED);
}

// Out of the task's tables. The addresses are cleared first, so /proc stops
// drawing the stacks (it loads each once; a row it drew a moment before is
// text about a stack that is going).
static void unmap_stacks(thread_t *t)
{
	task_t *task = (task_t *)t->ownerTask;
	uintptr_t user = __atomic_exchange_n(&t->esp3BaseV, 0, __ATOMIC_RELEASE);
	uintptr_t kernel = __atomic_exchange_n(&t->esp0BaseV, 0, __ATOMIC_RELEASE);
	if (user != 0)
		task_stack_unmap(task, user, t->esp3Size);
	if (kernel != 0)
		task_stack_unmap(task, kernel, t->esp0Size);
}

uint32_t thread_reap_drain(void)
{
	// Claimed off the list into a chain of their own, linked through the
	// nodes' `next` (a claimed node is on no list): RUNNING, so nobody else
	// reaches these stacks; their tasks are alive, because burial runs on
	// this same thread, after.
	reap_node_t *chain = NULL, **tail = &chain;
	uint32_t n = 0;
	uint64_t f = spinlock_acquire_irqsave(&kReapLock);
	for (reap_node_t *node; n < REAP_DRAIN_MOST && (node = reaplist_claim_next(&s_reaps)) != NULL; n++)
	{
		*tail = node;
		tail = &node->next;
	}
	spinlock_release_irqrestore(&kReapLock, f);
	if (n == 0)
		return 0;

	for (reap_node_t *node = chain; node != NULL; node = node->next)
		unmap_stacks(thread_of(node));
	// A sibling on another CPU may still hold a translation of a stack
	// just unmapped; a frame freed before it flushes could be written
	// through it after the frame is someone else's.
	tlb_shootdown_wait();
	for (reap_node_t *node = chain; node != NULL; node = node->next)
		free_stacks(thread_of(node), true);

	f = spinlock_acquire_irqsave(&kReapLock);
	for (reap_node_t *node = chain, *next; node != NULL; node = next)
	{
		next = node->next;
		node->next = NULL;
		(void)reaplist_finish(&s_reaps, node);
	}
	spinlock_release_irqrestore(&kReapLock, f);
	__atomic_add_fetch(&s_rounds, 1, __ATOMIC_RELAXED);
	return n;
}

void thread_reap_at_burial(thread_t *t)
{
	uint64_t f = spinlock_acquire_irqsave(&kReapLock);
	uint8_t was = t->reap.state;
	bool claimed = was == REAP_PENDING && reaplist_claim(&s_reaps, &t->reap);
	spinlock_release_irqrestore(&kReapLock, f);

	// kworker is the undertaker and the only reaper, and it finishes every
	// batch it claims before it buries anyone: a thread mid-release here
	// would mean a reaper this burial cannot see.
	if (was == REAP_RUNNING || (was == REAP_PENDING && !claimed))
		panic("thread_reap_at_burial: thread 0x%08lx of %s is %s at its burial\n", t->threadID,
		      ((task_t *)t->ownerTask)->exename, was == REAP_RUNNING ? "being released" : "pending on no list");
	if (was == REAP_DONE)
		return;
	free_stacks(t, claimed);
	if (claimed)
	{
		f = spinlock_acquire_irqsave(&kReapLock);
		(void)reaplist_finish(&s_reaps, &t->reap);
		spinlock_release_irqrestore(&kReapLock, f);
	}
}

void thread_reap_note_park(uint64_t parked_us, bool proceeded)
{
	__atomic_add_fetch(&s_creator_parks, 1, __ATOMIC_RELAXED);
	if (proceeded)
		__atomic_add_fetch(&s_creator_proceeded, 1, __ATOMIC_RELAXED);
	uint64_t most = __atomic_load_n(&s_creator_park_most_us, __ATOMIC_RELAXED);
	while (parked_us > most &&
	       !__atomic_compare_exchange_n(&s_creator_park_most_us, &most, parked_us, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
}

void thread_reap_read(thread_reap_counts_t *out)
{
	reaplist_counts_t c;
	uint64_t f = spinlock_acquire_irqsave(&kReapLock);
	reaplist_read(&s_reaps, &c);
	spinlock_release_irqrestore(&kReapLock, f);
	out->queued = c.queued;
	out->queued_most = c.queued_most;
	out->window_queued_most = c.window_queued_most;
	out->handed = c.handed;
	out->by_kworker = c.claimed_next;
	out->by_burial = c.claimed_named;
	out->refused = c.refused;
	out->window_handed = c.window_handed;
	out->window_released = c.window_finished;
	out->rounds = __atomic_load_n(&s_rounds, __ATOMIC_RELAXED);
	out->frame_bytes = __atomic_load_n(&s_frame_bytes, __ATOMIC_RELAXED);
	out->scratches = __atomic_load_n(&s_scratches, __ATOMIC_RELAXED);
	out->creator_parks = __atomic_load_n(&s_creator_parks, __ATOMIC_RELAXED);
	out->creator_proceeded = __atomic_load_n(&s_creator_proceeded, __ATOMIC_RELAXED);
	out->creator_park_most_us = __atomic_load_n(&s_creator_park_most_us, __ATOMIC_RELAXED);
}
