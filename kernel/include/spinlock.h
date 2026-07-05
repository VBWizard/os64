#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

// Shared irqsave spinlock primitive.
//
// This is the allocator's lock idiom (see the history in memory/allocator.c),
// promoted to a shared header so every subsystem stops hand-rolling its own
// ad-hoc __sync_lock_test_and_set() loops.
//
// Why interrupts MUST be disabled while the lock is held:
// If a core takes an interrupt while holding a spinlock, and the interrupt
// handler (or anything it calls — page-fault paths allocate memory!) tries to
// take the same lock, that core deadlocks against itself: the holder can't
// resume until the handler returns, and the handler spins forever with IF=0.
// Saving RFLAGS and restoring IF only if the caller had it set makes nesting
// safe: an acquire from an already-cli'd context (e.g. inside an ISR) won't
// spuriously re-enable interrupts on release.

typedef volatile uint32_t spinlock_t;

// Acquire the lock with interrupts disabled. Returns the caller's RFLAGS,
// which MUST be passed back to spinlock_release_irqrestore().
static inline uint64_t spinlock_acquire_irqsave(spinlock_t *lock)
{
	uint64_t flags;
	__asm__ volatile("pushfq\n\tpop %0" : "=r"(flags) :: "memory");
	__asm__ volatile("cli" ::: "memory");
	while (__sync_lock_test_and_set(lock, 1))
		__builtin_ia32_pause();
	return flags;
}

// Release the lock and restore the interrupt flag to its pre-acquire state.
static inline void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags)
{
	__sync_lock_release(lock);
	if (flags & 0x200)  // restore IF only if the caller had interrupts enabled
		__asm__ volatile("sti" ::: "memory");
}

#endif // SPINLOCK_H
