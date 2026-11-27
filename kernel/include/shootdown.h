#ifndef SHOOTDOWN_H
#define SHOOTDOWN_H

// The waited invalidation's rounds (THREAD_STACKS.md § The waited
// invalidation). Pure: no kernel headers. tools/test_shootdown_host.sh runs
// it beside a model of page tables and per-CPU translations.
//
// A waiter changes page tables, then BEGINS a round, whose number is one more
// than every round begun before it, and asks every other CPU to flush. A
// CPU's flush handler reads the newest round begun BEFORE it flushes
// (shootdown_flush_covers), flushes, then says so (shootdown_flushed). A
// flush therefore covers every round whose number it read: those rounds'
// table changes were made before their numbers were, so the flush cannot
// have kept a translation they removed. A round is answered by a CPU once
// that CPU has said it flushed a round at least as new.
//
// Several waiters may run rounds at once; each waits for its own number, and
// a flush made for a later round answers an earlier one too.

#include <stdbool.h>
#include <stdint.h>

#define SHOOTDOWN_MAX_CPUS 64

typedef struct
{
	uint64_t round;                       // the newest round begun
	uint64_t flushed[SHOOTDOWN_MAX_CPUS]; // the newest round each CPU's flush covers
	uint64_t rounds, late, wait_most_us;
	uint64_t window_rounds, window_late, window_wait_most_us;
	uint32_t wait_most_cpu, window_wait_most_cpu;   // which CPU answered the longest round last
} shootdown_t;

typedef struct
{
	uint64_t rounds, late, wait_most_us;
	uint64_t window_rounds, window_late, window_wait_most_us;
	uint32_t wait_most_cpu, window_wait_most_cpu;
} shootdown_counts_t;

// A waiter, after its table changes: the round's number.
uint64_t shootdown_begin(shootdown_t *s);

// A flush handler, BEFORE it flushes: the newest round its flush will cover.
uint64_t shootdown_flush_covers(shootdown_t *s);

// A flush handler on CPU `cpu`, after flushing what it read `covers` before.
void shootdown_flushed(shootdown_t *s, uint32_t cpu, uint64_t covers);

// Has CPU `cpu` flushed for round `round`?
bool shootdown_answered(shootdown_t *s, uint32_t cpu, uint64_t round);

// A waiter whose round every CPU answered: how long it waited, which CPU
// answered last, and whether the wait passed `late_us`. Returns whether it
// was late. The longest wait keeps its last CPU beside it (two rounds
// settling at once may pair one's wait with the other's CPU).
bool shootdown_settled(shootdown_t *s, uint64_t waited_us, uint32_t last_cpu, uint64_t late_us);

// The counts, since the first round and since the last read; the read
// starts the window again.
void shootdown_read(shootdown_t *s, shootdown_counts_t *out);

#endif
