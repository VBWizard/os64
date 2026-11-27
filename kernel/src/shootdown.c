// The waited invalidation's rounds (shootdown.h). Pure. Waiters and flush
// handlers run on different CPUs at once, so every field is reached
// atomically: a round's number is taken with RELEASE after the waiter's
// table writes, and read with ACQUIRE before a handler's flush.

#include "shootdown.h"

uint64_t shootdown_begin(shootdown_t *s)
{
	return __atomic_add_fetch(&s->round, 1, __ATOMIC_ACQ_REL);
}

uint64_t shootdown_flush_covers(shootdown_t *s)
{
	return __atomic_load_n(&s->round, __ATOMIC_ACQUIRE);
}

// One handler at a time per CPU (it runs with interrupts off), and each reads
// its number later than the last, so the store never moves a CPU backwards.
void shootdown_flushed(shootdown_t *s, uint32_t cpu, uint64_t covers)
{
	if (cpu < SHOOTDOWN_MAX_CPUS)
		__atomic_store_n(&s->flushed[cpu], covers, __ATOMIC_RELEASE);
}

bool shootdown_answered(shootdown_t *s, uint32_t cpu, uint64_t round)
{
	return cpu >= SHOOTDOWN_MAX_CPUS || __atomic_load_n(&s->flushed[cpu], __ATOMIC_ACQUIRE) >= round;
}

// True when `value` became the new most.
static bool raise_to(uint64_t *at, uint64_t value)
{
	uint64_t now = __atomic_load_n(at, __ATOMIC_RELAXED);
	while (value > now)
		if (__atomic_compare_exchange_n(at, &now, value, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
			return true;
	return false;
}

bool shootdown_settled(shootdown_t *s, uint64_t waited_us, uint32_t last_cpu, uint64_t late_us)
{
	bool late = waited_us > late_us;
	__atomic_add_fetch(&s->rounds, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&s->window_rounds, 1, __ATOMIC_RELAXED);
	if (late)
	{
		__atomic_add_fetch(&s->late, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&s->window_late, 1, __ATOMIC_RELAXED);
	}
	if (raise_to(&s->wait_most_us, waited_us))
		__atomic_store_n(&s->wait_most_cpu, last_cpu, __ATOMIC_RELAXED);
	if (raise_to(&s->window_wait_most_us, waited_us))
		__atomic_store_n(&s->window_wait_most_cpu, last_cpu, __ATOMIC_RELAXED);
	return late;
}

// A round settling during the read may land in either window; it is never
// lost from the totals.
void shootdown_read(shootdown_t *s, shootdown_counts_t *out)
{
	out->rounds = __atomic_load_n(&s->rounds, __ATOMIC_RELAXED);
	out->late = __atomic_load_n(&s->late, __ATOMIC_RELAXED);
	out->wait_most_us = __atomic_load_n(&s->wait_most_us, __ATOMIC_RELAXED);
	out->wait_most_cpu = __atomic_load_n(&s->wait_most_cpu, __ATOMIC_RELAXED);
	out->window_wait_most_cpu = __atomic_load_n(&s->window_wait_most_cpu, __ATOMIC_RELAXED);
	out->window_rounds = __atomic_exchange_n(&s->window_rounds, 0, __ATOMIC_RELAXED);
	out->window_late = __atomic_exchange_n(&s->window_late, 0, __ATOMIC_RELAXED);
	out->window_wait_most_us = __atomic_exchange_n(&s->window_wait_most_us, 0, __ATOMIC_RELAXED);
}
