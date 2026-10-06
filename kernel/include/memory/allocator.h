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

// The ledger: small unaligned kernel objects, carved inside chunks lent by
// the frame table (allocator.c, docs/design/pending/FRAMES.md).
extern uint64_t kMemoryStatusCurrentPtr;
extern memory_status_t *kMemoryStatus;

// Copy len bytes out of a TASK's address space, fault-proof: the page-table
// walk AND the data copy run under the frame lock with every page —
// tables and leaf alike — liveness-verified before it is dereferenced, so
// neither a concurrent unmap (data page) nor a concurrent burial (table
// pages, whose recycled garbage entries would otherwise send the walk
// through a wild HHDM alias) can fault ring 0. False = the caller reports
// "unreadable". pml4v is the task's pt_entry_t* (void* to keep paging.h out
// of this header). len must stay within one source page; the lock is
// interrupts-off, so chunk per page and touch nothing that allocates.
bool allocator_copy_from_task_va(void *pml4v, uintptr_t va, void *dst, size_t len);
// Page-aligned, or a page or more: whole frames from the frame table.
// Smaller: the ledger. All of it zeroed, HHDM-mapped while allocated, and
// panicking on exhaustion except allocate_memory_try, which answers 0.
uint64_t allocate_memory_aligned(uint64_t requested_length);
uint64_t allocate_memory(uint64_t requested_length);
uint64_t allocate_memory_try(uint64_t requested_length);
bool merge_freed_block(uint64_t freedIndex);
void compact_memory_array();
// Frees what allocate_memory* returned, by the address it returned; any
// other address panics, naming it. Returns 0.
uint64_t free_memory(uint64_t address);

// The frame table's counts, in frames, at one instant (shutdown, reports).
void allocator_frame_counts(uint64_t *usable, uint64_t *free_frames, uint64_t *run_frames,
                            uint64_t *ledger_frames);
// frames_audit over the live table, under the frame lock: NULL when every
// descriptor, list and counter agrees, else what is wrong and *where.
const char *allocator_audit(uint64_t *where);
// Every allocated extent (frame runs whole, the ledger's live extents), for
// init_os64_paging_tables' retro-map. Holds both allocator locks throughout.
void allocator_for_each_allocated(void (*fn)(void *ctx, uintptr_t phys, uint64_t length), void *ctx);

// ── kworker-side maintenance + observability (2026-08-07) ───────────────────
// The ledger's counters: cheap O(1) increments under the ledger lock,
// readable by anyone. exactfit rising ≈ holes being recycled (healthy);
// splits far outpacing merges+exactfit ≈ the table is growing (the disease).
extern uint64_t kAllocExactFitHits;
extern uint64_t kAllocSplits;
extern uint64_t kAllocMerges;
extern uint64_t kAllocCompactions;
extern uint64_t kAllocZeroedEntries;

// One bounded coalesce/compact visit of the ledger (≤ maxEntries examined) —
// the kworker's periodic job, so requestors stop paying for table hygiene. Returns merges
// performed. Safe on any boot; on kworker-less boots the free path's
// dead-entry backstop covers compaction instead.
uint32_t allocator_maintain(uint32_t maxEntries);

// The DEBUG_ALLOCATOR health lines: the frame table's counters, and the
// ledger's entries/inuse/free/dead, counters and top-4 free-hole sizes.
// Free when the level is off — the ledger walk itself is gated.
void allocator_debug_report(void);
// Atomic {free, used, largest free extent} reading under the allocator locks —
// the source of truth behind SYSCALL_MEMORY, read at one instant so the
// numbers agree with each other. free + used == kAvailableMemory is an
// INVARIANT; drift means a bookkeeping bug in the frame table or the ledger
// (see the definition). Any out-pointer may be NULL.
void allocator_memory_snapshot(uint64_t *free_bytes, uint64_t *used_bytes,
                               uint64_t *largest_free_extent);
void allocator_init();

#endif