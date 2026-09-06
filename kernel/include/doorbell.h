#ifndef DOORBELL_H
#define DOORBELL_H

// doorbell.h — the wake-from-interrupt primitive (DOORBELL.md is the design
// record; this header is the contract).
//
// A doorbell is a thread's address and a bit. Anything may RING it, from any
// context including an interrupt handler, because ringing takes no lock: it
// stores the bit and provokes a scheduler pass on the thread's home core.
// The pass ANSWERS it, under the queue lock it already holds, by relinking
// the parked thread and marking it expedited so the pick that follows takes
// it. The sleeper CLEARS the bit at the top of its loop, before the work,
// and parks only if the bit is still clear when it looks under the lock —
// the pipe's level-triggered discipline, re-evaluate the condition, never
// remember an edge.
//
// WHY THE RING MAY NOT TAKE THE QUEUE LOCK: the scheduler's own pass holds
// kSchedulerSwitchTasksLock with interrupts ENABLED (scheduler.c's doctrine
// block, the 9badced rule). An interrupt handler on that core that spun on
// the lock would wait for a holder that is itself, forever. Every other
// wake in the kernel is either thread context holding the lock with IF off,
// or code inside the pass that already holds it; an interrupt handler is
// neither, which is why the network's wakes used to wait for the tick.
//
// WHY THE SELF-IPI IS SAFE: IPI_MANUAL_SCHEDULE_VECTOR sits in the scheduler
// timer's own LAPIC priority class (SCHEDULER_REENTRANCY.md, Fix 3), so the
// hardware holds it until the ringing handler EOIs, and until any pass
// already running EOIs. It lands in a fresh pass, never inside one.

#include <stdint.h>
#include <stdbool.h>
#include "thread.h"

typedef struct doorbell
{
	thread_t*     thread;    // the sleeper this bell belongs to (NULL until registered)
	volatile bool rung;      // set by a ringer, cleared by the sleeper
	uint64_t      rings;     // every ring, whoever rang — diagnostics, racy by design
	uint64_t      wakes;     // rings that found the thread parked and relinked it
	uint64_t      rung_runnable; // rings that found it awake and off the CPU — answered by aging, not the boost
	const char*   name;      // for /sys and the log
} doorbell_t;

// How many bells the registry holds. A bell is a kernel daemon's; there are
// not many of those.
#define DOORBELL_MAX 8

// Publish a bell for `thread` (its OWN thread, in practice: a daemon
// registers itself at the top of its body). Registration is not
// interrupt-safe; it runs once, in thread context, before the first ring
// that matters.
void doorbell_register(doorbell_t* db, thread_t* thread, const char* name);
void doorbell_unregister(doorbell_t* db);

// RING. Interrupt-safe, lock-free, from anywhere: stores the bit, provokes a
// pass on the thread's home core.
void doorbell_ring(doorbell_t* db);

// Ring from INSIDE a scheduler pass (processSignals runs in one). The pass
// that is running will answer it; no IPI is needed and none is sent.
void doorbell_ring_in_pass(doorbell_t* db);

// PARK: sleep until rung, or until `backstop_ticks` have passed — the
// backstop is the house rule that a missed edge costs a tick, never a hang.
// Thread context only. Returns immediately, without sleeping, if the bell
// was rung between the sleeper clearing it and this call.
void doorbell_park(doorbell_t* db, uint64_t backstop_ticks);

// ANSWER: the scheduler's half. Called from inside a pass, under the queue
// lock, at both service points scheduler.c names. For every bell that is
// rung: a PARKED thread is relinked and marked expedited — one pick, the
// wake's. A RUNNABLE or RUNNING one needs nothing: it sees the bit at the
// top of its loop and competes on aging for its next turn, which is the
// flood guard (DOORBELL.md) and not an omission.
void doorbell_service_locked(void);

// The registry, for /sys. Index by position; a slot can be NULL.
uint32_t    doorbell_count(void);
doorbell_t* doorbell_at(uint32_t index);

// The doorbell fixture's bell and its trigger (kernel/test/test_main.c):
// processSignals rings the bell on the pass after the arm is set, so the test
// exercises the in-pass half of the primitive that no QEMU interrupt can.
extern doorbell_t    kDoorbellTestBell;
extern volatile bool kDoorbellTestArm;

#endif // DOORBELL_H
