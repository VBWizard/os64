#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define RESERVED_PAGES 9

typedef struct memory_status_s
{
	uint64_t startAddress;
	uint64_t length;
	bool in_use;

} memory_status_t;

extern uint64_t kMemoryStatusCurrentPtr;
extern memory_status_t *kMemoryStatus;

bool physical_page_is_allocated_on(uintptr_t physical_page_start);

// Verify-then-copy from physical memory via the HHDM alias, atomic against
// frees (the whole range must lie inside live extents, checked and copied
// under kMemoryStatusLock). False = some page is not allocator-live; the
// caller reports "unreadable" instead of the kernel taking the lazy-HHDM
// tripwire fault. Keep len page-sized per call — the lock is interrupts-off.
bool allocator_copy_from_phys(void *dst, uintptr_t phys, size_t len);
uint64_t allocate_memory_at_address(uint64_t address, uint64_t requested_length, bool use_address);
uint64_t allocate_memory_aligned(uint64_t requested_length);
uint64_t allocate_memory(uint64_t requested_length);
bool merge_freed_block(uint64_t freedIndex);
void compact_memory_array();
uint64_t free_memory(uint64_t address);

// ── kworker-side maintenance + observability (2026-08-07) ───────────────────
// The counters: cheap O(1) increments under the allocator lock, readable by
// anyone. exactfit rising ≈ holes being recycled (healthy); splits far
// outpacing merges+exactfit ≈ the table is growing (the disease).
extern uint64_t kAllocExactFitHits;
extern uint64_t kAllocSplits;
extern uint64_t kAllocMerges;
extern uint64_t kAllocCompactions;
extern uint64_t kAllocZeroedEntries;

// One bounded coalesce/compact visit (≤ maxEntries examined) — the kworker's
// periodic job, so requestors stop paying for table hygiene. Returns merges
// performed. Safe on any boot; on kworker-less boots the free path's
// dead-entry backstop covers compaction instead.
uint32_t allocator_maintain(uint32_t maxEntries);

// The DEBUG_ALLOCATOR health line (entries/inuse/free/dead + counters + top-4
// free-hole sizes). Free when the level is off — the walk itself is gated.
void allocator_debug_report(void);
// Atomic {free, used, largest free extent} reading under the allocator lock —
// the source of truth behind SYSCALL_MEMORY, in ONE walk so the numbers agree
// with each other. free + used == kAvailableMemory is an INVARIANT; drift
// means a ledger bug (see the definition). Any out-pointer may be NULL.
void allocator_memory_snapshot(uint64_t *free_bytes, uint64_t *used_bytes,
                               uint64_t *largest_free_extent);
void allocator_init();

#endif