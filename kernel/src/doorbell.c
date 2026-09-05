// doorbell.c — the wake-from-interrupt primitive. doorbell.h is the contract,
// DOORBELL.md the argument; what is here is the mechanism and nothing else.

#include "doorbell.h"
#include "scheduler.h"
#include "smp_core.h"     // send_ipi, IPI_MANUAL_SCHEDULE_VECTOR, BOOTSTRAP_PROCESSOR_ID
#include "signals.h"      // SIGSLEEP, the backstop nap
#include "kernel.h"       // kTicksSinceStart
#include "panic.h"
// kSMPInitDone comes from smp_core.h above

static doorbell_t* s_bells[DOORBELL_MAX];
static volatile uint32_t s_bell_count;

void doorbell_register(doorbell_t* db, thread_t* thread, const char* name)
{
	db->thread = thread;
	db->rung   = false;
	db->rings  = 0;
	db->wakes  = 0;
	db->boosts = 0;
	db->name   = name;

	// Find a free slot, then publish the pointer LAST so a service pass on
	// another core sees either nothing or a whole bell.
	for (uint32_t i = 0; i < DOORBELL_MAX; i++)
	{
		if (s_bells[i] != NULL)
			continue;
		s_bells[i] = db;
		__sync_synchronize();
		if (i >= s_bell_count)
			s_bell_count = i + 1;
		return;
	}
	panic("doorbell_register: no slot for '%s' — DOORBELL_MAX is %u\n", name, DOORBELL_MAX);
}

void doorbell_unregister(doorbell_t* db)
{
	for (uint32_t i = 0; i < DOORBELL_MAX; i++)
		if (s_bells[i] == db)
			s_bells[i] = NULL;
	__sync_synchronize();
	db->thread = NULL;
}

void doorbell_ring(doorbell_t* db)
{
	db->rung = true;
	db->rings++;
	__sync_synchronize();

	thread_t* t = db->thread;
	if (t == NULL || !kSMPInitDone)
		return;   // nobody is parked on it yet, or the BSP's tick is the only pass there is

	// Provoke a pass on the thread's home core. An unpinned thread lives
	// wherever the scheduler last put it; the BSP's pass is the one every
	// unpinned wake already rides, so ring there. send_ipi declines to send
	// a scheduling IPI at a core already in its pass, which is right: that
	// pass answers the bell itself if it has not passed the service point,
	// and the tick answers it within 10ms if it has (DOORBELL.md).
	uint32_t target = (t->mp_apic == THREAD_NO_AFFINITY)
	                  ? BOOTSTRAP_PROCESSOR_ID : (uint32_t)t->mp_apic;
	send_ipi(target, IPI_MANUAL_SCHEDULE_VECTOR, 0, 1, 0);
}

void doorbell_ring_in_pass(doorbell_t* db)
{
	db->rung = true;
	db->rings++;
}

void doorbell_park(doorbell_t* db, uint64_t backstop_ticks)
{
	thread_t* self = get_core_local_storage()->currentThread;

	// The check and the decision to sleep are one act under the queue lock
	// with interrupts off. A ring that lands before the check is seen here
	// and the park returns. A ring that lands after it finds this thread
	// still RUNNING, so the service point before the pick cannot relink it;
	// the pass this park provokes then moves the thread to ISLEEP and the
	// service point AFTER that requeue relinks it in the same pass. That
	// second service point is what makes this window closed rather than
	// merely small (scheduler_run_new_thread).
	uint64_t flags = scheduler_lock_queues();
	if (db->rung)
	{
		scheduler_unlock_queues(flags);
		return;
	}
	self->signals.sigdata[SIGSLEEP] = kTicksSinceStart + backstop_ticks;
	sigset_add(&self->signals.sigind, SIGSLEEP);
	scheduler_unlock_queues(flags);

	// The same door signal_raise(SIGSLEEP) walks through: a pass that finds
	// SIGSLEEP pending on the outgoing thread parks it in ISLEEP.
	scheduler_trigger(NULL);
}

void doorbell_service_locked(void)
{
	uint32_t n = s_bell_count;
	for (uint32_t i = 0; i < n; i++)
	{
		doorbell_t* db = s_bells[i];
		if (db == NULL || !db->rung)
			continue;
		thread_t* t = db->thread;
		if (t == NULL)
			continue;
		if (t->threadState == THREAD_STATE_RUNNABLE)
		{
			// Awake but off the CPU: the ring landed while it ran, and the
			// pass answering the ring has just requeued it behind the
			// service point (or it was already waiting its turn). It will
			// see the bit at the top of its loop; the boost is what makes
			// that loop top come at the NEXT pick instead of after aging.
			// No relink — it is on the runnable queue already. Counted
			// once per ring, not once per pass that walks past it.
			if (!t->expedite)
				db->boosts++;
			t->expedite = true;
			continue;
		}
		if (t->threadState != THREAD_STATE_ISLEEP)
			continue;   // RUNNING, here before its requeue or on another core: it will see the bit at the top of its loop
		// The bit stays SET — the sleeper clears it, so a ring that lands
		// between this relink and the sleeper's loop top is not lost.
		db->wakes++;
		t->expedite = true;
		scheduler_wake_isleep_thread_locked(t);
	}
}

uint32_t doorbell_count(void)
{
	return s_bell_count;
}

doorbell_t* doorbell_at(uint32_t index)
{
	return index < DOORBELL_MAX ? s_bells[index] : NULL;
}

// The fixture's bell (doorbell.h). Defined here rather than in the test file
// so processSignals' ring site links whether or not the test suite is in the
// build.
doorbell_t    kDoorbellTestBell;
volatile bool kDoorbellTestArm;
