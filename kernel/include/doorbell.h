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
	// THE EDGE: the rung bit and the stamp of the ring that raised it, in ONE
	// word, claimed by compare-and-swap. Bit 0 is RUNG; bits 1-7 are the
	// raising CPU's number (0x7F = a CPU the field cannot name: counted,
	// never timed); bits 8-63 are the low 56 bits of that core's TSC. One
	// word and one CAS because the bit and its stamp are a pair: a ringer
	// that merely set a bit after another's stamp had already been consumed
	// would hand the sleeper a stale clock as if it were its own. The raise
	// either publishes bit+stamp together or finds the bit already up and
	// publishes nothing; the sleeper's take clears the bit and reads the
	// stamp in one operation. The core is recorded because per-core TSCs are
	// not one clock (SCHEDULER.md) — a sleeper times only stamps its own
	// core made — and the sleeper subtracts modulo 2^56, so a TSC that
	// already carries more than 56 bits (a host up for a year, a VM handed
	// its host's count) measures the same as a fresh one.
	volatile uint64_t edge;
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

// The edge word's encoding (`edge` above) and the two atomic verbs on it.
// The ringer's CPU number takes seven bits; one the field cannot hold is
// stored as UNKNOWN, which no CPU's sleeper will take as its own.
#define DOORBELL_EDGE_RUNG          1ull
#define DOORBELL_EDGE_CPU_UNKNOWN   0x7Fu
#define DOORBELL_EDGE_TSC_MASK      ((1ull << 56) - 1)
static inline uint64_t doorbell_edge_make(uint64_t tsc, uint32_t cpu)
{
	uint64_t ringer = cpu < DOORBELL_EDGE_CPU_UNKNOWN ? cpu : DOORBELL_EDGE_CPU_UNKNOWN;
	return ((tsc & DOORBELL_EDGE_TSC_MASK) << 8) | (ringer << 1) | DOORBELL_EDGE_RUNG;
}
static inline bool     doorbell_edge_rung(uint64_t edge) { return (edge & DOORBELL_EDGE_RUNG) != 0; }
static inline uint32_t doorbell_edge_cpu(uint64_t edge)  { return (uint32_t)((edge >> 1) & 0x7Fu); }
static inline uint64_t doorbell_edge_tsc(uint64_t edge)  { return edge >> 8; }
// Cycles from the edge's TSC to `now` on the same core, modulo 2^56 — the
// only subtraction that is right for a truncated stamp.
static inline uint64_t doorbell_edge_elapsed(uint64_t edge, uint64_t now)
{
	return ((now & DOORBELL_EDGE_TSC_MASK) - doorbell_edge_tsc(edge)) & DOORBELL_EDGE_TSC_MASK;
}
static inline bool doorbell_rung(const doorbell_t* db) { return doorbell_edge_rung(db->edge); }

// RAISE the edge: interrupt-safe, lock-free, from any core. Publishes the
// rung bit and this core's stamp together, or finds the bit already up and
// publishes nothing. Returns whether this call made the edge. The ring verbs
// below call it; a sleeper re-arming its own bell calls it too.
bool doorbell_raise(doorbell_t* db);

// TAKE the edge: the sleeper's clear. Lowers the rung bit and returns the
// word as it was — rung or not, and the stamp of the ring being answered —
// in one atomic operation, so the stamp read can never belong to a later
// ring than the bit.
uint64_t doorbell_take(doorbell_t* db);

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
// Returns whether it SLEPT: false when the bell was already rung and the
// park returned at once, true when the thread left the CPU and was woken (by
// a ring or the backstop). The sleeper's diagnostics use the distinction —
// a ring answered after a sleep waited on the scheduler, a ring answered
// without one waited on the sleeper's own loop.
bool doorbell_park(doorbell_t* db, uint64_t backstop_ticks);

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
