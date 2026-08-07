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

// ── The PLAIN variant: interrupts stay ON ────────────────────────────────────
// For locks that are NEVER taken from an interrupt or fault path AND may be
// held across real I/O. The irqsave variant above is WRONG for those: disk
// completion paths read kTicksSinceStart, and a core that disables interrupts
// around an NVMe wait freezes its own tick clock and times out against a
// stopped watch (the e1000 probe learned the storm-flavored version of this
// lesson the day it was born — 2026-08-06, twice in one day).
//
// The discipline is the mirror image of irqsave's: if ANY acquirer of a given
// lock can run in interrupt context, every acquirer must use irqsave; a plain
// lock's acquirers must ALL be task/kernel-thread context. First customer:
// the VFS open-file registry (open/close/sync-all — syscall context only).
// Note a preempted holder leaves waiters spinning through their quantum —
// acceptable while critical sections are short and callers are rare; a
// sleeping mutex is the upgrade path if that ever stops being true.
static inline void spinlock_acquire(spinlock_t *lock)
{
	while (__sync_lock_test_and_set(lock, 1))
		__builtin_ia32_pause();
}

static inline void spinlock_release(spinlock_t *lock)
{
	__sync_lock_release(lock);
}

#endif // SPINLOCK_H
