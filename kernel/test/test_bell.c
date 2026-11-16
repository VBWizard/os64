#include "bell.h"
#include "scheduler.h"
#include "signals.h"
#include "smp_core.h"
#include "kernel.h"          // kTicksSinceStart
#include "time.h"            // nap
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
//      and a debt the wait never needed (the waiter came back by itself and
//      found the ring) is void once the wait has returned: a nap right
//      after it sleeps its whole time;
//   5. a wait that finds another thread registered is refused, and leaves
//      a ring that is there for the thread it was made for;
//   6. closed under a holder, with this thread registered and not yet
//      parked: the park returns at once (the close's wake was owed), the
//      close took the handle's hold and left the holder's, and the wait and
//      the ring both answer CLOSED.

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

	// The same window, but the waiter never parks: it comes round by itself
	// and takes the ring. The debt the ring left must not outlive that wait.
	// nap is the instrument for the reason test_wake_owed gives: one park,
	// no loop, so a leaked debt cuts it short where a looping sleep would
	// hide it.
	flags = spinlock_acquire_irqsave(&b->lock);
	b->waiter = self;
	spinlock_release_irqrestore(&b->lock, flags);
	bell_ring(b);
	r = bell_wait(b, kTicksSinceStart + BELL_TEST_DEADLINE_TICKS);
	before = kTicksSinceStart;
	nap(100);
	uint64_t napped = kTicksSinceStart - before;
	if (r != 1 || napped < 100 / MS_PER_TICK - 1)
	{
		printd(DEBUG_TESTS, "\tFAIL: test_bell - a debt outlived the wait it was owed to: a 100 ms "
		       "nap ended after %lu ticks (wait answered %ld)\n", napped, r);
		return false;
	}

	// 5. Somebody else is waiting, and the bell is rung. (Any other thread
	// pointer will do for the slot: the wait only compares it. The ring is
	// set by hand, since bell_ring would try to wake that pointer.)
	flags = spinlock_acquire_irqsave(&b->lock);
	b->waiter = (thread_t *)b;
	b->rung = true;
	spinlock_release_irqrestore(&b->lock, flags);
	r = bell_wait(b, kTicksSinceStart + BELL_TEST_DEADLINE_TICKS);
	flags = spinlock_acquire_irqsave(&b->lock);
	b->waiter = NULL;
	bool kept = b->rung;
	spinlock_release_irqrestore(&b->lock, flags);
	if (r != BELL_ERR_BUSY || !kept)
		return failed("a second waiter was not refused, or took the first one's ring", r);
	r = bell_wait(b, kTicksSinceStart + BELL_TEST_DEADLINE_TICKS);
	if (r != 1)
		return failed("the ring a refused waiter left was not there afterwards", r);

	// 6. Closed under a holder, in the park window.
	flags = spinlock_acquire_irqsave(&b->lock);
	b->waiter = self;
	spinlock_release_irqrestore(&b->lock, flags);
	bell_close(b);
	before = kTicksSinceStart;
	signal_raise(SIGSLEEP, kTicksSinceStart + 10 * TICKS_PER_SECOND, self);
	uint64_t closed_park = kTicksSinceStart - before;
	if (closed_park > 3)
		return failed("a close between the registration and the park left the waiter asleep",
		              (long)closed_park);
	if (b->refs != 1)
		return failed("the close did not leave exactly the holder's reference", (long)b->refs);
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
