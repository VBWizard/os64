// pipe.c — the bounded byte stream. See pipe.h for the design and the WHY
// (kernel-owned ring, the limit IS the flow control, two refcounts).
//
// The blocking discipline here is the one console_read() already proved:
//   1. take the lock, test the condition
//   2. if it can't proceed, register as the waiter, DROP THE LOCK, then park
//      (SIGSLEEP with a backstop tick)
//   3. on wake, LOOP AND RE-TEST — a wake is a hint, never a promise. Another
//      thread may have taken the data/space before we got the CPU back.
// Never hold the spinlock across the park: the whole point of parking is to
// give up the CPU, and sleeping while holding a lock that the thread who must
// wake you needs is a deadlock with extra steps.

#include "pipe.h"
#include "memory/kmalloc.h"
#include "memcpy.h"
#include "memset.h"
#include "scheduler.h"
#include "signals.h"
#include "smp_core.h"
#include "thread.h"
#include "serial_logging.h"
#include "BasicRenderer.h"   // printf, for pipe_dump_all
#include "CONFIG.h"

extern volatile uint64_t kTicksSinceStart;

// Every open pipe, so pipe_wake_if_ready() can sweep them once per scheduler
// pass. Guarded by kPipeListLock. A handful of pipes at a time — an O(n) walk
// of a 3-element list per pass is not a cost worth optimizing.
static pipe_t * volatile kPipeList = NULL;
static spinlock_t kPipeListLock = 0;

// Backstop only — NOT a polling interval. A parked reader/writer is normally
// woken in microseconds by its counterpart, or within one scheduler pass
// (~10ms) by the level-triggered sweep. This guarantees liveness if a wake is
// ever missed entirely: worst case the thread re-checks within a second.
#define PIPE_BACKSTOP_TICKS TICKS_PER_SECOND

static void pipe_list_add(pipe_t *p)
{
	uint64_t flags = spinlock_acquire_irqsave(&kPipeListLock);
	p->next = kPipeList;
	kPipeList = p;
	spinlock_release_irqrestore(&kPipeListLock, flags);
}

static void pipe_list_remove(pipe_t *p)
{
	uint64_t flags = spinlock_acquire_irqsave(&kPipeListLock);
	pipe_t **link = (pipe_t **)&kPipeList;
	while (*link != NULL)
	{
		if (*link == p)
		{
			*link = p->next;
			break;
		}
		link = &(*link)->next;
	}
	spinlock_release_irqrestore(&kPipeListLock, flags);
}

// Wake a parked thread. Only a thread that has ACTUALLY parked (ISLEEP) can be
// woken here; one still RUNNING has not called sigaction yet, so clearing its
// SIGSLEEP would do nothing and it would park anyway. That race is exactly what
// the level-triggered sweep below exists to catch — leave the waiter slot set
// and let the next pass find it.
static void pipe_wake_thread(thread_t *w)
{
	// The check-clear-relink now lives in the scheduler behind its queue
	// lock: this wake runs in the WAKER's thread context, which since the
	// tickless fan-out can be an AP executing in true parallel with the
	// BSP's tick pass over the same queues. The unlocked version of this
	// relink is what corrupted the scheduler lists and wedged VBox.
	scheduler_wake_isleep_thread(w);
}

// Take a registered waiter OUT of its slot — but only if it has actually
// finished parking. Called under p->lock.
//
// THE TRAP THIS GUARDS (found on the net branch, 2026-08-01): there is a
// window between a thread REGISTERING as waiter (under the lock) and it
// actually reaching ISLEEP (the sigaction park runs after the lock drops).
// The scheduler's wake primitive only moves parked threads — waking a thread
// still in that window is a silent no-op. If the waker has ALSO cleared the
// slot, the registration is gone, no later sweep can find the sleeper, and it
// eats its full 1-second backstop. The signature is unmistakable once seen:
// the net stack's first ICMP boot measured every echo at exactly 100 ticks of
// "round trip" — that is TICKS_PER_SECOND, the backstop, not the network.
// Leaving a not-yet-parked waiter REGISTERED instead costs one scheduler pass
// (~10ms worst case): the next sweep re-tests the condition, finds the waiter
// parked by then, and wakes it. This helper is what makes the "leave the
// waiter slot set" promise above pipe_wake_thread actually TRUE — the old
// code cleared the slot first and hoped.
static thread_t *pipe_claim_parked_waiter(thread_t *volatile *slot)
{
	thread_t *t = *slot;
	if (t == NULL || t->threadState != THREAD_STATE_ISLEEP)
		return NULL;   // nobody, or mid-park: leave the registration for the sweep
	*slot = NULL;
	return t;
}

pipe_t *pipe_create(void)
{
	pipe_t *p = (pipe_t *)kmalloc(sizeof(pipe_t));
	if (p == NULL)
		return NULL;

	memset(p, 0, sizeof(pipe_t));

	p->buffer = (uint8_t *)kmalloc(PIPE_CAPACITY);
	if (p->buffer == NULL)
	{
		kfree(p);
		return NULL;
	}

	// The creator holds BOTH ends. It will hand one to each child and then
	// close its own copies — and it MUST, or the reader never sees EOF.
	p->readers = 1;
	p->writers = 1;

	pipe_list_add(p);
	printd(DEBUG_PIPE, "pipe_create: pipe 0x%016lx, %u byte ring at 0x%016lx (readers=1 writers=1)\n",
		(uintptr_t)p, PIPE_CAPACITY, (uintptr_t)p->buffer);
	return p;
}

// Free only when BOTH ends are gone. Called with no lock held.
// Tear the pipe down. Reached by exactly ONE caller per pipe — the closer
// whose decrement completed the 0/0 state, decided INSIDE the same critical
// section as the decrement (see the doctrine at the close functions). This
// used to be pipe_destroy_if_orphaned(), which re-took the lock and re-tested
// the counts on its own — and that gap between "my decrement" and "my test"
// was THE SCRIBBLED-TEXT BUG (2026-08-14): the last reader-close and last
// writer-close racing on two cores could BOTH observe 0/0 and BOTH run the
// kfrees below. Worse, a closer preempted in that gap (six hogs on four cores
// stretch a microsecond window into seconds) came back and freed a 64KB ring
// address that had since been recycled into a block-cache LINE — an
// exact-base free of somebody else's live extent, invisible to every
// tripwire. The allocator then carved the line into fresh demand-load frames
// while the cache's stale data pointer kept refilling raw disk blocks over
// them: running programs' text, overwritten mid-run, three "impossible"
// segfaults at a time (hog task 55 and task 85, both post-mortemed to this
// line). One decision, one destroyer, no gap.
static void pipe_destroy(pipe_t *p)
{
	pipe_list_remove(p);
	printd(DEBUG_PIPE, "pipe_destroy: pipe 0x%016lx (both ends closed)\n", (uintptr_t)p);
	kfree(p->buffer);
	kfree(p);
}

// Every refcount change is traced. This is not ceremony: the refcounts ARE the
// answer to a hung pipeline. A reader stuck forever means writers != 0, which
// means somebody (usually the shell) still holds a write end it should have
// closed. Being able to SEE the count go 2 -> 1 -> 0 turns a mystery into a
// two-line log read.
void pipe_ref_read_end(pipe_t *p)
{
	uint64_t flags = spinlock_acquire_irqsave(&p->lock);
	p->readers++;
	uint32_t r = p->readers, w = p->writers;
	spinlock_release_irqrestore(&p->lock, flags);

	printd(DEBUG_PIPE, "pipe_ref_read: pipe 0x%016lx readers=%u writers=%u\n",
		(uintptr_t)p, r, w);
}

void pipe_ref_write_end(pipe_t *p)
{
	uint64_t flags = spinlock_acquire_irqsave(&p->lock);
	p->writers++;
	uint32_t r = p->readers, w = p->writers;
	spinlock_release_irqrestore(&p->lock, flags);

	printd(DEBUG_PIPE, "pipe_ref_write: pipe 0x%016lx readers=%u writers=%u\n",
		(uintptr_t)p, r, w);
}

void pipe_close_read_end(pipe_t *p)
{
	uint64_t flags = spinlock_acquire_irqsave(&p->lock);
	// THE DESTRUCTION DECISION LIVES IN THIS CRITICAL SECTION, fused to the
	// decrement. The lock serializes the two final closes, so exactly one of
	// them observes both counts at zero AS A RESULT OF ITS OWN DECREMENT —
	// and only that one destroys. Deciding in a separate lock acquisition
	// (the old pipe_destroy_if_orphaned) let both final closers see 0/0 and
	// double-free the ring; see the funeral notice at pipe_destroy().
	bool didDecrement = false;
	if (p->readers > 0)
	{
		p->readers--;
		didDecrement = true;
	}
	// Last reader gone: any writer blocked for space is now writing into the
	// void. Wake it so it can discover that and fail (SIGPIPE) rather than
	// sleep forever waiting for a drain that will never come. (A writer still
	// mid-park stays registered; the sweep's readers==0 arm delivers the news.)
	thread_t *w = (p->readers == 0) ? pipe_claim_parked_waiter(&p->writeWaiter) : NULL;
	uint32_t nr = p->readers, nw = p->writers;
	bool shouldDestroy = didDecrement && nr == 0 && nw == 0;
	spinlock_release_irqrestore(&p->lock, flags);

	printd(DEBUG_PIPE, "pipe_close_read: pipe 0x%016lx readers=%u writers=%u%s\n",
		(uintptr_t)p, nr, nw,
		(nr == 0) ? "  <-- LAST READER GONE: writers now face EPIPE" : "");

	pipe_wake_thread(w);
	if (shouldDestroy)
		pipe_destroy(p);
}

void pipe_close_write_end(pipe_t *p)
{
	uint64_t flags = spinlock_acquire_irqsave(&p->lock);
	// Same fused decrement-and-decide as pipe_close_read_end — the doctrine
	// and the scar are documented there and at pipe_destroy().
	bool didDecrement = false;
	if (p->writers > 0)
	{
		p->writers--;
		didDecrement = true;
	}
	// Last writer gone: THIS IS EOF. A reader blocked on an empty pipe must be
	// woken to discover it — otherwise it waits forever for a byte that can
	// never arrive. (The single most important wake in the whole file. A reader
	// still mid-park stays registered; the sweep's writers==0 arm is its EOF.)
	thread_t *r = (p->writers == 0) ? pipe_claim_parked_waiter(&p->readWaiter) : NULL;
	uint32_t nr = p->readers, nw = p->writers;
	bool shouldDestroy = didDecrement && nr == 0 && nw == 0;
	spinlock_release_irqrestore(&p->lock, flags);

	printd(DEBUG_PIPE, "pipe_close_write: pipe 0x%016lx readers=%u writers=%u%s\n",
		(uintptr_t)p, nr, nw,
		(nw == 0) ? "  <-- LAST WRITER GONE: this is EOF for the reader" : "");

	pipe_wake_thread(r);
	if (shouldDestroy)
		pipe_destroy(p);
}

long pipe_read(pipe_t *p, char *buf, size_t len)
{
	if (len == 0)
		return 0;

	core_local_storage_t *cls = get_core_local_storage();
	thread_t *self = cls->currentThread;

	for (;;)
	{
		// A pending TERMINATE outranks the read — the READER is being killed
		// (Ctrl+C, or a write to its /proc ctl file). Checked before the lock,
		// at the top of every pass: this is how a reader parked below (woken by
		// processSignals when the bit appeared) exits instead of re-parking
		// forever. Buffered bytes stay put — a dying stage has no further use
		// for them.
		if (self->signals.sigind & SIGNALS_TERMINATING)
			return PIPE_ERR_INTERRUPTED;

		uint64_t flags = spinlock_acquire_irqsave(&p->lock);

		if (p->count > 0)
		{
			// SHORT read on purpose: hand back whatever is there, right now.
			size_t n = (len < p->count) ? len : p->count;
			for (size_t i = 0; i < n; i++)
			{
				buf[i] = (char)p->buffer[p->tail];
				p->tail = (p->tail + 1) % PIPE_CAPACITY;
			}
			p->count -= n;
			size_t left = p->count;

			// We just freed space — a writer parked for room can proceed.
			// (One still mid-park stays registered for the sweep instead.)
			thread_t *w = pipe_claim_parked_waiter(&p->writeWaiter);
			spinlock_release_irqrestore(&p->lock, flags);

			if (w)
				printd(DEBUG_PIPE, "pipe_read: pipe 0x%016lx drained %lu — WAKING parked writer\n",
					(uintptr_t)p, (uint64_t)n);
			pipe_wake_thread(w);

			printd(DEBUG_PIPE | DEBUG_DETAILED, "pipe_read: pipe 0x%016lx got %lu bytes (%lu still buffered)\n",
				(uintptr_t)p, (uint64_t)n, (uint64_t)left);
			return (long)n;
		}

		// Empty. If the last writer is gone, that is EOF — and EOF is not a
		// byte in the stream, it is the absence of writers.
		if (p->writers == 0)
		{
			spinlock_release_irqrestore(&p->lock, flags);
			printd(DEBUG_PIPE, "pipe_read: pipe 0x%016lx EOF (empty, no writers left)\n",
				(uintptr_t)p);
			return 0;
		}

		// Empty, but a writer still exists: park until one shows up.
		p->readWaiter = self;
		uint32_t nw = p->writers;
		spinlock_release_irqrestore(&p->lock, flags);   // never park holding the lock

		// The line you want when a pipeline hangs: a reader is asleep waiting
		// for bytes, and writers is NOT zero — so somebody still holds a write
		// end open. If nobody is actually going to write, that somebody forgot
		// to close (classically: the shell keeping its own copy of the end).
		printd(DEBUG_PIPE, "pipe_read: pipe 0x%016lx PARKING reader (empty, writers=%u still open)\n",
			(uintptr_t)p, nw);

		sigaction(SIGSLEEP, NULL, kTicksSinceStart + PIPE_BACKSTOP_TICKS, self);
		// Woken (by a writer, the sweep, or the backstop) — loop and RE-TEST.
		// The wake was a hint; another reader may have taken the bytes.
	}
}

long pipe_write(pipe_t *p, const char *buf, size_t len)
{
	if (len == 0)
		return 0;

	core_local_storage_t *cls = get_core_local_storage();
	thread_t *self = cls->currentThread;

	size_t written = 0;

	while (written < len)
	{
		// Same rule as pipe_read: a pending TERMINATE means the WRITER is being
		// killed — stop pushing bytes and let the syscall boundary do the
		// honors. Bytes already landed stay landed (they were real).
		if (self->signals.sigind & SIGNALS_TERMINATING)
			return PIPE_ERR_INTERRUPTED;

		// A write of <= PIPE_CAPACITY lands WHOLE (our atomicity rule): wait for
		// room for all of it, then copy it in one shot under the lock, so two
		// writers can never interleave. A write LARGER than the capacity can
		// never fit, so it is the one case that must chunk — and therefore the
		// one case where interleaving is possible. Honest and easy to state.
		size_t want = len - written;
		if (want > PIPE_CAPACITY)
			want = PIPE_CAPACITY;

		uint64_t flags = spinlock_acquire_irqsave(&p->lock);

		// Nobody left to read it: this write goes into the void.
		if (p->readers == 0)
		{
			spinlock_release_irqrestore(&p->lock, flags);
			printd(DEBUG_PIPE, "pipe_write: pipe 0x%016lx EPIPE (no readers; %lu bytes had landed)\n",
				(uintptr_t)p, (uint64_t)written);
			// Report bytes already delivered if any — they really did land.
			return (written > 0) ? (long)written : (long)PIPE_ERR_CLOSED;
		}

		size_t space = PIPE_CAPACITY - p->count;
		if (space >= want)
		{
			for (size_t i = 0; i < want; i++)
			{
				p->buffer[p->head] = (uint8_t)buf[written + i];
				p->head = (p->head + 1) % PIPE_CAPACITY;
			}
			p->count += want;
			written += want;
			size_t buffered = p->count;

			// Bytes are in: a reader parked on an empty pipe can proceed.
			// (One still mid-park stays registered for the sweep instead.)
			thread_t *r = pipe_claim_parked_waiter(&p->readWaiter);
			spinlock_release_irqrestore(&p->lock, flags);

			if (r)
				printd(DEBUG_PIPE, "pipe_write: pipe 0x%016lx wrote %lu — WAKING parked reader\n",
					(uintptr_t)p, (uint64_t)want);
			pipe_wake_thread(r);

			printd(DEBUG_PIPE | DEBUG_DETAILED, "pipe_write: pipe 0x%016lx put %lu bytes (%lu buffered)\n",
				(uintptr_t)p, (uint64_t)want, (uint64_t)buffered);
			continue;
		}

		// Not enough room for the whole write: park until the reader drains.
		// THIS is the backpressure — the producer is now running at exactly the
		// consumer's speed, asleep and costing nothing while it waits.
		p->writeWaiter = self;
		size_t held = p->count;
		spinlock_release_irqrestore(&p->lock, flags);   // never park holding the lock

		// Backpressure, made visible. This is NOT an error — it is the system
		// working exactly as designed (a fast producer being throttled to its
		// consumer's speed). Seeing it repeatedly just means the reader is the
		// slow one, which is usually the truth you were looking for.
		printd(DEBUG_PIPE, "pipe_write: pipe 0x%016lx PARKING writer (want %lu, only %lu free of %u) — backpressure\n",
			(uintptr_t)p, (uint64_t)want, (uint64_t)(PIPE_CAPACITY - held), PIPE_CAPACITY);

		sigaction(SIGSLEEP, NULL, kTicksSinceStart + PIPE_BACKSTOP_TICKS, self);
		// Woken — loop and RE-TEST. Another writer may have taken the space.
	}

	return (long)written;
}

// Dump every live pipe: buffered bytes, both refcounts, and who is parked.
//
// Meant to be called FROM THE DEBUGGER when a pipeline is wedged:
//     (gdb) call pipe_dump_all()
// One glance answers the only question that matters. A reader parked with
// writers != 0 means somebody still holds a write end they should have closed.
// A writer parked with a full ring and a live reader is just backpressure —
// the system working. A writer parked with readers == 0 should be impossible
// (it would have been woken to take its EPIPE), so if you ever see it, THAT is
// the bug.
//
// Unconditional printf, NOT printd: you call this deliberately, and it would be
// a cruel joke to have it print nothing because DEBUG_PIPE happened to be off.
void pipe_dump_all(void)
{
	uint64_t listFlags = spinlock_acquire_irqsave(&kPipeListLock);

	printf("--- pipes ---\n");
	int n = 0;
	for (pipe_t *p = kPipeList; p != NULL; p = p->next)
	{
		printf("  pipe 0x%016lx: %lu/%u bytes buffered, readers=%u writers=%u, "
		       "reader %s, writer %s\n",
			(uintptr_t)p,
			(uint64_t)p->count, PIPE_CAPACITY,
			p->readers, p->writers,
			p->readWaiter ? "PARKED" : "-",
			p->writeWaiter ? "PARKED" : "-");
		n++;
	}
	if (n == 0)
		printf("  (none open)\n");

	spinlock_release_irqrestore(&kPipeListLock, listFlags);
}

void pipe_wake_if_ready(void)
{
	// Level-triggered sweep (see pipe.h). Catches the one race the direct wake
	// cannot: a thread that registered as waiter but had not yet parked when
	// its counterpart fired the wake. We re-evaluate the CONDITION here, not a
	// remembered edge, so a wake can never be lost — only delayed by a pass.
	uint64_t listFlags = spinlock_acquire_irqsave(&kPipeListLock);

	for (pipe_t *p = kPipeList; p != NULL; p = p->next)
	{
		uint64_t flags = spinlock_acquire_irqsave(&p->lock);

		// A reader can proceed if there are bytes, OR if the last writer left
		// (that is its EOF, and it must be woken to see it). The claim helper
		// only takes a waiter that has FINISHED parking — one registered but
		// not yet asleep stays in the slot for the next pass, because waking
		// it now would silently do nothing and lose the registration.
		thread_t *r = NULL;
		if (p->count > 0 || p->writers == 0)
			r = pipe_claim_parked_waiter(&p->readWaiter);

		// A writer can proceed if there is any room, OR if the last reader left
		// (that is its EPIPE, and it must be woken to see it).
		thread_t *w = NULL;
		if (p->count < PIPE_CAPACITY || p->readers == 0)
			w = pipe_claim_parked_waiter(&p->writeWaiter);

		spinlock_release_irqrestore(&p->lock, flags);

		// _locked variants: this sweep runs from processSignals, which
		// already holds the scheduler queue lock — the public wake would
		// re-acquire it and self-deadlock.
		scheduler_wake_isleep_thread_locked(r);
		scheduler_wake_isleep_thread_locked(w);
	}

	spinlock_release_irqrestore(&kPipeListLock, listFlags);
}
