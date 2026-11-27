#ifndef COUNTED_LOCK_H
#define COUNTED_LOCK_H

// An irqsave spinlock that keeps its own numbers: how often it was taken,
// how often a taker had to wait and for how long, and the worst wait and
// hold, in TSC cycles (kCPUCyclesPerSecond converts). The waits and the
// worst are kept twice: since the lock began, and in a window its reader
// restarts (counted_lock_window_restart), so one stretch of time can be
// measured without the rest in the answer. A holder may say what it did
// under the lock (`hold_tag`, a walk's length, say; 0 when it says nothing),
// and the worst holds keep what their holder said. Every field is written
// under the lock it describes; a reader takes the lock to copy them.

#include <stdint.h>

#include "spinlock.h"
#include "driver/system/x86_64.h"

typedef struct
{
	uint64_t acquires, contended, wait_cycles, max_wait_cycles, max_hold_cycles;
	uint64_t window_acquires, window_contended, window_wait_cycles;
	uint64_t window_max_wait_cycles, window_max_hold_cycles;
	uint64_t max_hold_tag, window_max_hold_tag;   // the worst holds' hold_tag
} counted_lock_stats_t;

typedef struct
{
	spinlock_t lock;
	uint64_t held_since;
	uint64_t hold_tag;   // the current holder's word for its hold, 0 at each acquire
	counted_lock_stats_t stats;
} counted_lock_t;

static inline uint64_t counted_lock_acquire(counted_lock_t *l)
{
	uint64_t waited;
	uint64_t flags = spinlock_acquire_irqsave_measured(&l->lock, rdtsc, &waited);
	counted_lock_stats_t *s = &l->stats;
	s->acquires++;
	s->window_acquires++;
	if (waited != 0)
	{
		s->contended++;
		s->window_contended++;
		s->wait_cycles += waited;
		s->window_wait_cycles += waited;
		if (waited > s->max_wait_cycles)
			s->max_wait_cycles = waited;
		if (waited > s->window_max_wait_cycles)
			s->window_max_wait_cycles = waited;
	}
	l->held_since = rdtsc();
	l->hold_tag = 0;
	return flags;
}

static inline void counted_lock_release(counted_lock_t *l, uint64_t flags)
{
	uint64_t hold = rdtsc() - l->held_since;
	if (hold > l->stats.max_hold_cycles)
	{
		l->stats.max_hold_cycles = hold;
		l->stats.max_hold_tag = l->hold_tag;
	}
	if (hold > l->stats.window_max_hold_cycles)
	{
		l->stats.window_max_hold_cycles = hold;
		l->stats.window_max_hold_tag = l->hold_tag;
	}
	spinlock_release_irqrestore(&l->lock, flags);
}

// The window starts again. Called with the lock held.
static inline void counted_lock_window_restart(counted_lock_t *l)
{
	counted_lock_stats_t *s = &l->stats;
	s->window_acquires = s->window_contended = s->window_wait_cycles = 0;
	s->window_max_wait_cycles = s->window_max_hold_cycles = 0;
	s->window_max_hold_tag = 0;
}

#endif
