#include "bell.h"
#include "scheduler.h"
#include "signals.h"
#include "smp_core.h"
#include "kernel.h"          // kTicksSinceStart
#include "printd.h"
#include "CONFIG.h"

// The bell object (bell.h), driven without a handle so each rule is one
// call and one answer. /tests/belltest proves the same rules through the
// syscalls, with real second threads; what only the kernel can do is stand
// in the window itself — step 4 — because here the registration and the
// park are two separate statements a ring can be placed between.
//
//   1. a ring made before the wait ends it at once, and is consumed;
//   2. an unrung wait runs out its deadline, no sooner;
//   3. two rings before a wait are one;
//   4. THE PARK/RING RACE: this thread registers as the bell's waiter, the
//      ring lands while it is still RUNNING, and only then does it park. The
//      park must return at once (the wake was owed, and the park paid it)
//      and the wait that follows must find the ring. Judged against a 10 s
//      backstop, so a ring that fell into the window is unmistakable;
//   5. a wait that finds another thread registered is refused;
//   6. closed under a holder: the wait and the ring both answer CLOSED, and
//      the holder's release is the one that frees the bell.

#define BELL_TEST_DEADLINE_TICKS 5

static bool failed(const char *what, long got)
{
	printd(DEBUG_TESTS, "\tFAIL: test_bell - %s (got %ld)\n", what, got);
	return false;
}

bool test_bell(void)
{
	thread_t *self = get_core_local_storage()->currentThread;
	bell_t *b = bell_create();
	if (b == NULL)
		return failed("no bell", 0);
	bell_ref(b);   // the holder step 6 needs; released at the end

	// 1. Ring, then wait.
	long r = bell_ring(b);
	if (r != 0)
		return failed("a ring was refused", r);
	uint64_t before = kTicksSinceStart;
	r = bell_wait(b, kTicksSinceStart + 10 * TICKS_PER_SECOND);
	if (r != 1 || kTicksSinceStart - before > 2)
		return failed("a ring made before the wait did not end it at once", r);

	// 2. Unrung: the deadline, and all of it.
	before = kTicksSinceStart;
	r = bell_wait(b, before + BELL_TEST_DEADLINE_TICKS);
	uint64_t took = kTicksSinceStart - before;
	if (r != BELL_ERR_TIMEOUT || took < BELL_TEST_DEADLINE_TICKS)
		return failed("an unrung wait did not run out its deadline", r);

	// 3. Two rings, one wait, and the next wait finds nothing.
	bell_ring(b);
	bell_ring(b);
	r = bell_wait(b, kTicksSinceStart + BELL_TEST_DEADLINE_TICKS);
	if (r != 1)
		return failed("two rings did not end a wait", r);
	r = bell_wait(b, kTicksSinceStart + BELL_TEST_DEADLINE_TICKS);
	if (r != BELL_ERR_TIMEOUT)
		return failed("two rings were counted as two", r);

	// 4. Registered, rung while still running, then parked.
	uint64_t flags = spinlock_acquire_irqsave(&b->lock);
	b->waiter = self;
	spinlock_release_irqrestore(&b->lock, flags);
	bell_ring(b);
	before = kTicksSinceStart;
	signal_raise(SIGSLEEP, kTicksSinceStart + 10 * TICKS_PER_SECOND, self);
	uint64_t parked = kTicksSinceStart - before;
	r = bell_wait(b, kTicksSinceStart + BELL_TEST_DEADLINE_TICKS);
	if (parked > 3 || r != 1)
	{
		printd(DEBUG_TESTS, "\tFAIL: test_bell - a ring between the registration and the park "
		       "left the waiter asleep %lu ticks (wait answered %ld)\n", parked, r);
		return false;
	}
	if (b->waiter != NULL)
		return failed("the wait left its registration behind", 0);

	// 5. Somebody else is waiting. (Any other thread pointer will do: the
	// wait only compares it, and takes it back out before anything could
	// ring it.)
	flags = spinlock_acquire_irqsave(&b->lock);
	b->waiter = (thread_t *)b;
	spinlock_release_irqrestore(&b->lock, flags);
	r = bell_wait(b, kTicksSinceStart + BELL_TEST_DEADLINE_TICKS);
	flags = spinlock_acquire_irqsave(&b->lock);
	b->waiter = NULL;
	spinlock_release_irqrestore(&b->lock, flags);
	if (r != BELL_ERR_BUSY)
		return failed("a second waiter was not refused", r);

	// 6. Closed under a holder.
	bell_close(b);
	r = bell_wait(b, kTicksSinceStart + BELL_TEST_DEADLINE_TICKS);
	if (r != BELL_ERR_CLOSED)
		return failed("a wait on a closed bell did not answer CLOSED", r);
	r = bell_ring(b);
	if (r != BELL_ERR_CLOSED)
		return failed("a ring on a closed bell did not answer CLOSED", r);
	bell_release(b);

	printd(DEBUG_TESTS, "\tPASS: test_bell (unrung wait took %lu ticks of %d; a ring in the "
	       "park window cost %lu)\n", took, BELL_TEST_DEADLINE_TICKS, parked);
	return true;
}
