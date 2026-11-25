// handle_blocks.c — where a handle's number lives, and the table's counts
// (handle_blocks.h).

#include "handle_blocks.h"

uint32_t handle_block_size(unsigned k)
{
	return k == 0 ? OS64_TASK_HANDLES : (uint32_t)OS64_TASK_HANDLES << (k - 1);
}

uint32_t handle_block_first(unsigned k)
{
	return k == 0 ? 0 : (uint32_t)OS64_TASK_HANDLES << (k - 1);
}

uint32_t handle_room(unsigned added)
{
	return (uint32_t)OS64_TASK_HANDLES << added;
}

bool handle_place(int64_t n, unsigned *block, uint32_t *slot)
{
	if (n < 0 || n >= (int64_t)TASK_HANDLES_CEILING)
		return false;
	if (n < OS64_TASK_HANDLES) {
		*block = 0;
		*slot = (uint32_t)n;
		return true;
	}
	// Block k (k >= 1) holds [64 << (k-1), 64 << k): k is one more than
	// the highest bit of n / 64.
	uint32_t ratio = (uint32_t)n / OS64_TASK_HANDLES;
	unsigned k = 32u - (unsigned)__builtin_clz(ratio);
	*block = k;
	*slot = (uint32_t)n - handle_block_first(k);
	return true;
}

void handle_counts_took(handle_counts_t *c)
{
	uint32_t held = __atomic_add_fetch(&c->held, 1, __ATOMIC_RELAXED);
	uint32_t most = __atomic_load_n(&c->most, __ATOMIC_RELAXED);
	while (held > most &&
	       !__atomic_compare_exchange_n(&c->most, &most, held, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
	}
}

void handle_counts_gave(handle_counts_t *c)
{
	__atomic_sub_fetch(&c->held, 1, __ATOMIC_RELAXED);
}

void handle_counts_refused(handle_counts_t *c)
{
	__atomic_add_fetch(&c->failed, 1, __ATOMIC_RELAXED);
}
