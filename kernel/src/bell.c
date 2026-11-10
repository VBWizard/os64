// bell.c — a handle whose read waits and whose write rings. See bell.h for
// what a bell is and who asked for it.
//
// THE WAIT IS glass_view_read's SHAPE, not pipe_read's, and the difference
// is one decision. A pipe's wake acts only on a waiter that has finished
// parking, so pipes keep a list and a sweep in processSignals to catch the
// one that had registered and not yet parked. A bell's ringer uses the owed
// wake instead (WAKE_OWED.md): under the bell's lock it reads the slot and
// wakes the waiter if it is parked, or leaves a debt the park itself pays if
// it is not. So there is no list of bells, no sweep, and a ring that lands
// in that window costs nothing instead of a scheduler pass.
//
// What the wait keeps from every park loop in this kernel (CLAUDE.md,
// Signals): awake at the top of a pass it voids its own registration and
// any debt owed for it, under the lock it registered under, before any
// return; it asks signal_park_must_end; and it parks with a backstop, so a
// wake nobody aimed costs a second and never a hang.

#include "bell.h"
#include "kmalloc.h"
#include "memset.h"
#include "smp_core.h"
#include "scheduler.h"
#include "signals.h"
#include "kernel.h"          // kTicksSinceStart — the park's backstop and the deadline
#include "CONFIG.h"          // TICKS_PER_SECOND

// Liveness only: a waiter is woken by its ringer, by close, by its deadline
// or by a signal. This bounds a wake that was never aimed.
#define BELL_BACKSTOP_TICKS TICKS_PER_SECOND

bell_t *bell_create(void)
{
	bell_t *b = (bell_t *)kmalloc(sizeof(bell_t));
	if (b == NULL)
		return NULL;
	memset(b, 0, sizeof(bell_t));
	b->refs = 1;
	return b;
}

void bell_ref(bell_t *b)
{
	__sync_fetch_and_add(&b->refs, 1);
}

void bell_release(bell_t *b)
{
	// The last holder: no handle names the bell and no operation is inside
	// it, so nothing can reach it. A waiter is an operation and holds its
	// own reference, which is why `waiter` cannot name anyone here.
	if (__sync_sub_and_fetch(&b->refs, 1) == 0)
		kfree(b);
}

void bell_close(bell_t *b)
{
	uint64_t flags = spinlock_acquire_irqsave(&b->lock);
	b->closed = true;
	// The slot is the waiter's to clear, at the top of its next pass; this
	// only makes sure it gets there to see `closed` — owed, if it is still
	// on its way to the park.
	if (b->waiter != NULL)
		scheduler_wake_or_owe(b->waiter);
	spinlock_release_irqrestore(&b->lock, flags);
	bell_release(b);
}

long bell_wait(bell_t *b, uint64_t deadline)
{
	core_local_storage_t *cls = get_core_local_storage();
	thread_t *self = cls->currentThread;

	for (;;)
	{
		uint64_t flags = spinlock_acquire_irqsave(&b->lock);

		// Awake at the top: a registration left by the park we came out of
		// is void, and so is a wake owed for it — what it announced is in
		// `rung` or `closed`, and the checks below find it.
		if (b->waiter == self)
			b->waiter = NULL;
		scheduler_forget_owed_wake(self);

		if (b->closed)
		{
			spinlock_release_irqrestore(&b->lock, flags);
			return BELL_ERR_CLOSED;
		}
		// A signal that ends the wait outranks a ring: the ring stays in
		// `rung` for the wait the caller makes when it comes back.
		if (signal_park_must_end(self))
		{
			spinlock_release_irqrestore(&b->lock, flags);
			return BELL_ERR_INTERRUPTED;
		}
		if (b->rung)
		{
			b->rung = false;
			spinlock_release_irqrestore(&b->lock, flags);
			return 1;
		}
		// One waiter. A second thread is refused rather than queued behind
		// the first: one ring wakes one wait, and which of two it would be
		// is a question a bell has no answer to.
		if (b->waiter != NULL)
		{
			spinlock_release_irqrestore(&b->lock, flags);
			return BELL_ERR_BUSY;
		}
		if (deadline != 0 && kTicksSinceStart >= deadline)
		{
			spinlock_release_irqrestore(&b->lock, flags);
			return BELL_ERR_TIMEOUT;
		}

		// Unrung: register in the same hold as the check that found it so,
		// so a ring either landed before it (taken above) or will see us.
		b->waiter = self;
		spinlock_release_irqrestore(&b->lock, flags);   // never park holding the lock

		uint64_t wake = kTicksSinceStart + BELL_BACKSTOP_TICKS;
		if (deadline != 0 && deadline < wake)
			wake = deadline;
		signal_raise(SIGSLEEP, wake, self);
		// Woken by a ring, a close, the deadline, a signal or the backstop:
		// loop and re-test.
	}
}

long bell_ring(bell_t *b)
{
	uint64_t flags = spinlock_acquire_irqsave(&b->lock);
	if (b->closed)
	{
		spinlock_release_irqrestore(&b->lock, flags);
		return BELL_ERR_CLOSED;
	}
	b->rung = true;
	if (b->waiter != NULL)
		scheduler_wake_or_owe(b->waiter);
	spinlock_release_irqrestore(&b->lock, flags);
	return 0;
}
