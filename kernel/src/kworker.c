#include "kworker.h"

#include "memory/allocator.h"

#include "CONFIG.h"
#include "kernel.h"
#include "printd.h"
#include "signals.h"
#include "smp_core.h"
#include "task.h"
#include "thread.h"
#include "logging/log.h"

#define KWORKER_SLEEP_TICKS (TICKS_PER_SECOND * 2)
#define KWORKER_REAP_BATCH_SIZE 8
// Allocator hygiene knobs: entries examined per visit (bounds the lock hold —
// tens of microseconds, never a full-table sweep), and how many visits pass
// between DEBUG_ALLOCATOR health lines (~10s at the 2s sleep cadence).
#define KWORKER_ALLOC_MAINTAIN_BATCH 32
#define KWORKER_ALLOC_REPORT_EVERY 5

static bool kworker_run_maintenance(void)
{
	bool did_work = false;
	int reaped = task_reap_eligible_zombies(KWORKER_REAP_BATCH_SIZE);

	if (reaped > 0) {
		// "Buried" counts BOTH phases of the two-phase burial (task.c): a
		// corpse unlinked this pass and one freed this pass each score 1.
		printd(DEBUG_TASK | DEBUG_DETAILED, "KWORKER: buried/unlinked %u collected zombie task(s)\n", reaped);
		did_work = true;
	}

	// Allocator table hygiene (allocator.h): coalesce holes the free-time
	// merge couldn't (its neighbors were live then), compact when littered.
	// Deliberately NOT counted as did_work — same reasoning as the log flush
	// below: maintenance that reschedules itself as "work" never sleeps, and
	// a bounded idempotent pass has nothing urgent to stay awake for.
	static uint32_t alloc_report_countdown = KWORKER_ALLOC_REPORT_EVERY;
	allocator_maintain(KWORKER_ALLOC_MAINTAIN_BATCH);
	if (--alloc_report_countdown == 0) {
		alloc_report_countdown = KWORKER_ALLOC_REPORT_EVERY;
		allocator_debug_report();
	}

#if ENABLE_LOG_BUFFERING == 1
	/*
	 * Flushing buffered logs from kworker is useful, but it must not count as
	 * "work" for sleep decisions. Otherwise kworker can keep itself awake by
	 * continuously flushing the trace messages it just emitted.
	 */
	if (kLoggingInitialized) {
		logd_thread(false);
	}
#endif

	return did_work;
}

bool kworker_thread(bool daemon)
{
	core_local_storage_t *cls = get_core_local_storage();
	thread_t *self = cls->currentThread;

	printd(DEBUG_TASK | DEBUG_DETAILED,
		"kworker_thread: starting on APIC %u (thread=0x%08x, daemon=%u)\n",
		cls->apic_id,
		self->threadID,
		daemon);

	while (1) {
		bool did_work = kworker_run_maintenance();

		if (!daemon) {
			printd(DEBUG_TASK | DEBUG_DETAILED,
				"kworker_thread: single-pass exit on APIC %u, did_work=%u\n",
				cls->apic_id,
				did_work);
			return did_work;
		}

		if (!did_work) {
			printd(DEBUG_TASK | DEBUG_DETAILED,
				"kworker_thread: APIC %u sleeping until tick %u\n",
				cls->apic_id,
				kTicksSinceStart + KWORKER_SLEEP_TICKS);
			sigaction(SIGSLEEP, NULL, kTicksSinceStart + KWORKER_SLEEP_TICKS, self);
			printd(DEBUG_TASK | DEBUG_DETAILED,
				"kworker_thread: APIC %u woke at tick %u\n",
				cls->apic_id,
				kTicksSinceStart);
		}
	}
}
