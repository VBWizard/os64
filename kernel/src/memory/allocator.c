#include "CONFIG.h"
#include "allocator.h"
#include "frames.h"
#include "memmap.h"
#include "paging.h"
#include "memset.h"
#include "serial_logging.h"
#include "panic.h"
#include "memcpy.h"
#include "spinlock.h"

// ── Two owners of physical memory (docs/design/pending/FRAMES.md) ────────────
//
// THE FRAME TABLE (frames.c) owns every usable 4 KiB frame, one 8-byte
// descriptor each, so allocating, freeing and "is this frame live?" never
// search. Page-aligned requests and anything a page or larger are frame runs.
//
// THE LEDGER (kMemoryStatus) carves the kernel's small unaligned objects at
// 8-byte granularity, by the same exact-fit-first policy it always has, out
// of chunks it borrows from the frame table. Its rows describe only those
// chunks, so it stays as long as the kernel's small objects are numerous,
// however much memory tasks use. Chunks are never returned.
//
// LOCKS. kFrameLock guards the frame table, kMemoryStatusLock the ledger.
// Both are irqsave: allocations happen concurrently from page-fault handlers
// on multiple cores, and a holder preempted mid-update would deadlock a
// fault-context spinner (IF=0) on the same core. The order is ledger, then
// frames, never the reverse.
//
// HHDM ("allocated <=> HHDM-mapped", paging.h). A frame run is mapped before
// kFrameLock is released at allocation and unmapped before the run is
// released at free, so a frame that reads as live is mapped. Inside a ledger
// chunk the LEDGER owns HHDM state: a chunk is lent unmapped, the ledger maps
// each extent it carves and unmaps the pages its frees fully contain, and it
// pins the chunk's frames under an extent (frames_ledger_pin) after mapping
// and unpins them before unmapping, so a pinned frame is mapped too.

static frames_t kFrames;
static spinlock_t kFrameLock = 0;

// Usable bytes the frame table never describes (frame 0, if a usable entry
// starts there, and any part of an entry that is not whole frames). The
// snapshot reports them as used, so free + used == usable still holds.
static uint64_t kFramesHeldBytes;

// A ledger chunk: 64 KiB, so each borrow serves many small objects.
#define LEDGER_CHUNK_FRAMES 16

memory_status_t *kMemoryStatus;
//Points to the next available kernel status - increment AFTER use
uint64_t kMemoryStatusCurrentPtr = 0;

static spinlock_t kMemoryStatusLock = 0;

static inline uint64_t frames_lock(void)
{
	return spinlock_acquire_irqsave(&kFrameLock);
}

static inline void frames_unlock(uint64_t flags)
{
	spinlock_release_irqrestore(&kFrameLock, flags);
}

static inline uint64_t ledger_lock(void)
{
	return spinlock_acquire_irqsave(&kMemoryStatusLock);
}

static inline void ledger_unlock(uint64_t flags)
{
	spinlock_release_irqrestore(&kMemoryStatusLock, flags);
}

//NOTE: Will return the passed address if it is already page aligned
static inline uintptr_t round_up_to_nearest_page(uintptr_t addr) {
    return (addr + 0xFFF) & ~0xFFF;
}

static const char *frames_status_name(frames_status_t s)
{
	switch (s)
	{
		case FRAMES_OK:             return "ok";
		case FRAMES_OUT_OF_RANGE:   return "past the frame table";
		case FRAMES_NOT_ALLOCATED:  return "not allocated (a double or stray free)";
		case FRAMES_NOT_A_START:    return "inside a run, not its first frame";
		case FRAMES_LEDGER_PINNED:  return "a ledger chunk still holding live extents";
		case FRAMES_NOT_LEDGER:     return "not a ledger chunk frame";
		case FRAMES_PIN_RANGE:      return "pin count out of range";
		case FRAMES_NOT_RESERVED:   return "already described";
		case FRAMES_BAD_ARGUMENT:   return "bad argument";
	}
	return "unknown";
}

// ── The ledger: small objects inside borrowed chunks ────────────────────────

void compact_memory_array() {
    size_t writeIndex = 0; // Where the next valid entry will be written
	kAllocCompactions++;
	kAllocZeroedEntries = 0;   // every dead entry dies in the sweep below
	printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "allocator: Compacting memory status array\n");

    for (size_t i = 0; i < kMemoryStatusCurrentPtr; i++)
	{
		//If the current entry is in use
        if (kMemoryStatus[i].length > 0)
		{
            //And the "write to" index isn't the same as the current entry
            if (i != writeIndex)
			{
			// Copy the valid entry to the "write to" index
                kMemoryStatus[writeIndex] = kMemoryStatus[i];
            }
			//Increment the "write to" index regardless
            writeIndex++;
        }
    }

	printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "\tallocator: Clearing out compacted entries\n");
    // Clear remaining entries after the last valid index
    for (size_t i = writeIndex; i < kMemoryStatusCurrentPtr; i++) {
        kMemoryStatus[i].startAddress = 0;
        kMemoryStatus[i].length = 0;
        kMemoryStatus[i].in_use = false;
    }
	kMemoryStatusCurrentPtr=writeIndex;
}

bool merge_freed_block(uint64_t freedIndex) {
    memory_status_t *freedBlock = &kMemoryStatus[freedIndex];

	// ONE scan collecting BOTH neighbors, then merge whatever was found.
	// The old version returned after the FIRST merge, so a block freed
	// between two free neighbors left two entries where one belonged —
	// one of the three ingredients of the 2026-08-07 table explosion
	// (see get_status_entry_for_first_available_address for the story).
	memory_status_t *pred = NULL;   // free block ending exactly at ours
	memory_status_t *succ = NULL;   // free block starting exactly past ours
    printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "allocator: Looking for entries to merge ours at index %u, address 0x%016lx, with\n", freedIndex, freedBlock->startAddress);
	for (size_t idx = 0; idx < kMemoryStatusCurrentPtr; idx++) {
        if (idx == freedIndex) continue; // Skip the block being freed

        memory_status_t *candidate = &kMemoryStatus[idx];
		if (candidate->startAddress == 0x0) continue;
		if (candidate->in_use || candidate->length == 0) continue;

        if (candidate->startAddress + candidate->length == freedBlock->startAddress)
            pred = candidate;
        else if (freedBlock->startAddress + freedBlock->length == candidate->startAddress)
            succ = candidate;

        if (pred && succ)
            break;   // a block has at most one of each — done looking
    }

	if (pred == NULL && succ == NULL)
	{
		printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "\t allocator: Did not find a candidate to merge with\n");
		return false;
	}

	if (pred != NULL)
	{
		// Grow the preceding block over ours...
		pred->length += freedBlock->length;
		freedBlock->startAddress = 0;
		freedBlock->length = 0;
		freedBlock->in_use = false;
		kAllocZeroedEntries++;
		kAllocMerges++;
		// ...and if a successor also touches, swallow it too: three entries
		// become one, which is what "coalesce" was always supposed to mean.
		if (succ != NULL)
		{
			pred->length += succ->length;
			succ->startAddress = 0;
			succ->length = 0;
			succ->in_use = false;
			kAllocZeroedEntries++;
			kAllocMerges++;
		}
		printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "\tallocator: Merged into preceding block: start=0x%016lx, length=0x%016lx%s\n",
				pred->startAddress, pred->length, succ ? " (successor swallowed too)" : "");
	}
	else
	{
		// Only a successor: it inherits our start and grows backward over us.
		succ->length += freedBlock->length;
		succ->startAddress = freedBlock->startAddress;
		freedBlock->startAddress = 0;
		freedBlock->length = 0;
		freedBlock->in_use = false;
		kAllocZeroedEntries++;
		kAllocMerges++;
		printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "\tallocator: Merged into following block: start=0x%016lx, length=0x%016lx\n",
				succ->startAddress, succ->length);
	}
	return true;
}

// Copy len bytes OUT OF A TASK'S ADDRESS SPACE, fault-proof (PR #26, rounds
// two and seven). The whole journey — walking the task's page tables AND the
// final data copy — happens under kFrameLock, with EVERY page
// liveness-verified before it is dereferenced. Liveness is the frame table's
// one-load answer (frames_live), and holding kFrameLock is what keeps it
// true until the copy is done: a frame run is unmapped only under that lock,
// and a ledger frame only after its pins are dropped under it.
//
//   Round two's race: the task unmaps the DATA region from another thread
//   mid-copy; free_memory HHDM-unmaps the alias and a raw memcpy faults
//   ring 0. Cured by verifying the leaf page under the lock.
//
//   Round seven's race, one level up: phase-2 burial's arena_destroy frees
//   the task's PAGE TABLES mid-walk, and the memory they lived in may be
//   reissued: a GARBAGE table entry points anywhere, and dereferencing the
//   next level through `garbage | kHHDMOffset` can hit genuinely unmapped
//   territory, so the fault is one hop removed, not absent. Cured by
//   verifying every table page too, so a wild "next level" fails the
//   liveness test instead of being followed. Garbage that happens to land in
//   live frames copies garbage BYTES — which the caller's
//   magic/version/seqlock validation exists to reject. Nonsense is
//   survivable; faults are not.
//
// Large pages (PS) where a table should be are refused rather than decoded:
// task address spaces are built 4K-only, so a PS bit here IS garbage.
//
// RULES FOR CALLERS: len must stay within one source page (chunk per page),
// the lock is interrupts-off and held across the walk + memcpy, and nothing
// on this path may allocate, free, or fault. Microseconds, bounded, honest.
bool allocator_copy_from_task_va(void *pml4v, uintptr_t va,
                                 void *dst, size_t len)
{
	// void* for the same reason block_cache_covers takes one: keeping
	// paging.h's types out of allocator.h's face. It IS a pt_entry_t*.
	if (dst == NULL || len == 0 || pml4v == NULL)
		return false;
	if (((va & (uintptr_t)0xFFF) + len) > PAGE_SIZE)
		return false;   // caller must chunk per page — see RULES above

	// The four level indexes, spelled the way paging.c spells them privately
	// (its PML4_INDEX family is file-local; four shifts don't earn a header
	// migration): bits 39/30/21/12, nine bits each — the 4-level contract.
	uint64_t idx[4] = { (va >> 39) & 0x1FF, (va >> 30) & 0x1FF,
	                    (va >> 21) & 0x1FF, (va >> 12) & 0x1FF };
	uintptr_t table_virt = (uintptr_t)pml4v;
	if (table_virt < kHHDMOffset)
		table_virt |= kHHDMOffset;

	bool ok = false;
	uint64_t flags = frames_lock();

	uint64_t entry = 0;
	for (int level = 0; level < 4; level++)
	{
		uintptr_t table_phys = (table_virt - kHHDMOffset) & ~(uintptr_t)0xFFF;
		if (!frames_live(&kFrames, table_phys / PAGE_SIZE))
			goto out;                       // a freed table — burial won the race
		entry = ((pt_entry_t *)table_virt)[idx[level]];
		if (!(entry & PAGE_PRESENT))
			goto out;                       // honestly unmapped (or garbage saying so)
		if (level < 3 && (entry & (1ULL << 7)))
			goto out;                       // PS bit in a task walk = garbage; refuse
		// Strip flags AND bit 63 (NX rides on leaves now) to get the next hop.
		table_virt = ((entry & ~0xFFFULL) & 0x0000FFFFFFFFFFFFULL) | kHHDMOffset;
	}

	{
		uintptr_t data_phys = (entry & ~0xFFFULL) & 0x0000FFFFFFFFFFFFULL;
		if (!frames_live(&kFrames, data_phys / PAGE_SIZE))
			goto out;                       // the round-two race, caught at the leaf
		memcpy(dst, (const void *)((data_phys | kHHDMOffset) + (va & (uintptr_t)0xFFF)), len);
		ok = true;
	}

out:
	frames_unlock(flags);
	return ok;
}

// Maintenance/observability counters (allocator.h has the tour). All are
// bumped under kMemoryStatusLock, so plain increments are safe.
uint64_t kAllocExactFitHits = 0;   // a recycled hole was reused whole
uint64_t kAllocSplits = 0;         // a fit carved a block (leftover entry born)
uint64_t kAllocMerges = 0;         // free-time neighbor merges (either side)
uint64_t kAllocCompactions = 0;    // compact_memory_array passes
uint64_t kAllocZeroedEntries = 0;  // dead (length 0) entries awaiting compaction

/// @brief Find a free ledger block for the request: EXACT fit first, else first fit.
///
/// Why exact-first (2026-08-07, the "top slows down" autopsy): first-fit alone
/// always carved fresh pieces off the big low-index donor block, while the
/// identically-sized holes freed by steady churn (procfs open/close is the
/// heaviest customer) accumulated behind it FOREVER — 12,819 free entries in a
/// 12-minute soak, 9,020 of them one repeating size, and every kmalloc/kfree
/// in the kernel scanning past all of them under an IRQs-off spinlock. Churn
/// repeats the same sizes by nature, so preferring an exact-size hole recycles
/// yesterday's free instead of minting a new one, and the table plateaus.
/// Still ONE pass: remember the first adequate block as the fallback and keep
/// looking for an exact hole, which scans to the end when none exists. That
/// costs the length of the ledger, which since FRAMES holds only the kernel's
/// small objects; the page frames that once filled it are the frame table's.
memory_status_t* get_status_entry_for_first_available_address(uint64_t requested_length)
{
	memory_status_t *firstFit = NULL;

	for (uint64_t cnt = 0; cnt < kMemoryStatusCurrentPtr; cnt++)
	{
		memory_status_t *entry = &kMemoryStatus[cnt];

		// Dead entries (start 0, length 0) and live ones are not candidates.
		if (entry->startAddress == 0 || entry->in_use)
			continue;

		if (entry->length == requested_length)
		{
			kAllocExactFitHits++;
			return entry;      // the whole point: reuse, don't carve
		}
		if (firstFit == NULL && entry->length >= requested_length)
			firstFit = entry;
	}
	return firstFit;
}

uint64_t get_status_index_for_requested_address(uint64_t address,uint64_t requested_length, bool in_use)
{
	for (uint64_t cnt = 0; cnt < kMemoryStatusCurrentPtr; cnt++)
	{
		if ( (kMemoryStatus[cnt].startAddress <= address && kMemoryStatus[cnt].startAddress + kMemoryStatus[cnt].length > address) &&
			kMemoryStatus[cnt].in_use == in_use &&
			kMemoryStatus[cnt].length >= requested_length
		)
			return cnt;
	}
	// Say WHICH address died nameless — a panic that names its victim turns a
	// bisect session into a single screendump read (learned the hard way the
	// night the undertaker's first burial handed this exact panic an address
	// it refused to identify, 2026-08-06).
	panic("get_status_index_for_requested_address: Can't find the index for 0x%016lx (len=0x%lx, in_use=%u)!!! :-(\n",
	      address, requested_length, in_use);
	return 0;
}

void update_existing_status_entry(memory_status_t* entry, uint64_t address, uint64_t length, bool in_use)
{
	entry->startAddress = address;
	entry->length = length;
	entry->in_use = in_use;
}

// How full the table has ever been. The SLOPE of this under a workload is the
// diagnostic — a table that climbs steadily is a fragmentation problem wearing
// a countdown timer.
uint64_t kMemoryStatusHighWater = 0;

memory_status_t* make_new_status_entry(uint64_t address, uint64_t length, bool in_use)
{
	// THE GUARD THAT WAS NEVER HERE (2026-08-15).
	//
	// This function appended an entry and incremented the index, forever, with
	// no bound of any kind. INITIAL_MEMORY_STATUS_COUNT (CONFIG.h) sizes the
	// table and nothing grows it — the word INITIAL was carrying the whole
	// plan. When the table filled, the next entry was written PAST THE END,
	// into whatever allocation followed it in physical memory; on the P5 that
	// neighbour was the paging pool, whose first page is the KERNEL'S OWN
	// PML4, and entry number 100,001 unmapped the lower half of the kernel
	// address space in one store. A hardware watchpoint on PML4[0] named this
	// function on the first hit.
	//
	// So: PANIC at the wall, never write past it. This guard is deliberately
	// panic-ONLY (2026-08-15 review find): the carve path (ledger_alloc) holds
	// `memaddr` — a raw pointer INTO kMemoryStatus — across its calls to this
	// function, and compaction RELOCATES entries, so compacting here would
	// leave that pointer naming an arbitrary row. The compact-with-headroom
	// pass runs at the TOP of the carve, before any pointer into the table is
	// taken; by the time execution reaches here, a full table means
	// compaction already failed to help. Since FRAMES the ledger holds only
	// small kernel objects, so reaching this wall is that population's own
	// capacity limit (FRAMES.md § Phase 1).
	if (kMemoryStatusCurrentPtr >= INITIAL_MEMORY_STATUS_COUNT)
		panic("allocator: memory status table is full (%lu of %u entries). The next "
		      "entry would be written PAST THE END of the table, over whatever "
		      "allocation follows it. Raise INITIAL_MEMORY_STATUS_COUNT, or find "
		      "what is fragmenting memory this badly.\n",
		      (uint64_t)kMemoryStatusCurrentPtr, INITIAL_MEMORY_STATUS_COUNT);

	// Announce the approach, once per 10% crossed, so the wall is visible long
	// before it is hit — the whole point of a high-water mark is that somebody
	// sees the climb.
	if (kMemoryStatusCurrentPtr > kMemoryStatusHighWater)
	{
		uint64_t tenth = INITIAL_MEMORY_STATUS_COUNT / 10;
		if (tenth != 0 &&
		    (kMemoryStatusCurrentPtr / tenth) > (kMemoryStatusHighWater / tenth))
			printd(DEBUG_ALLOCATOR, "allocator: status table high-water %lu of %u entries (%lu%%)\n",
			       (uint64_t)kMemoryStatusCurrentPtr, INITIAL_MEMORY_STATUS_COUNT,
			       (uint64_t)(kMemoryStatusCurrentPtr * 100 / INITIAL_MEMORY_STATUS_COUNT));
		kMemoryStatusHighWater = kMemoryStatusCurrentPtr;
	}

	kMemoryStatus[kMemoryStatusCurrentPtr].startAddress = address;
	kMemoryStatus[kMemoryStatusCurrentPtr].length = length;
	kMemoryStatus[kMemoryStatusCurrentPtr].in_use = in_use;
	kMemoryStatusCurrentPtr++;
	return &kMemoryStatus[kMemoryStatusCurrentPtr-1];
}

// Pin or unpin the chunk frames under a ledger extent. Every frame the
// extent touches is pinned, partial ones included: a frame stays live while
// ANY live extent overlaps it, which is exactly when it stays mapped.
static void ledger_pin_extent(uint64_t start, uint64_t length, int delta)
{
	uint64_t first = start / PAGE_SIZE;
	uint64_t last = (start + length - 1) / PAGE_SIZE;
	uint64_t flags = frames_lock();
	frames_status_t s = frames_ledger_pin(&kFrames, first, last - first + 1, delta);
	frames_unlock(flags);
	if (s != FRAMES_OK)
		panic("allocator: ledger extent 0x%016lx (len 0x%lx) cannot %s its frames: %s\n",
		      start, length, delta > 0 ? "pin" : "unpin", frames_status_name(s));
}

// Borrow a chunk for the ledger: frames lent UNMAPPED (the ledger maps what
// it carves), added as one free extent. Caller holds kMemoryStatusLock and
// has left a row of headroom for the new entry.
static bool ledger_borrow_chunk(void)
{
	uint64_t flags = frames_lock();
	uint64_t frame = frames_alloc(&kFrames, LEDGER_CHUNK_FRAMES, FRAMES_LEDGER);
	frames_unlock(flags);
	if (frame == FRAMES_NONE)
		return false;
	make_new_status_entry(frame * PAGE_SIZE, LEDGER_CHUNK_FRAMES * PAGE_SIZE, false);
	return true;
}

static uint64_t ledger_alloc(uint64_t requested_length, bool fallible)
{
	if (requested_length == 0 || requested_length > UINT64_MAX - 7)
	{
		if (fallible)
			return 0;
		panic("allocator: a %lu-byte allocation cannot be served\n", requested_length);
	}
	uint64_t irqflags = ledger_lock();

	// THE TABLE-FULL GUARD'S COMPACTION LIVES HERE, NOT AT MINT TIME
	// (2026-08-15 review find). This function holds `memaddr` — a raw
	// pointer INTO kMemoryStatus — across a mint, then writes the leftover
	// through it, so compaction must run NOW, before any pointer into the
	// table exists. Headroom of 3: a borrowed chunk's entry, the carve's
	// entry, and the one the guard refuses.
	if (kMemoryStatusCurrentPtr + 3 >= INITIAL_MEMORY_STATUS_COUNT)
	{
		printd(DEBUG_ALLOCATOR, "allocator: status table full (%lu entries) — compacting before the carve\n",
		       (uint64_t)kMemoryStatusCurrentPtr);
		compact_memory_array();
	}
	if (fallible && kMemoryStatusCurrentPtr + 3 >= INITIAL_MEMORY_STATUS_COUNT) {
		ledger_unlock(irqflags);
		return 0;
	}

	// Align to 8 bytes: the architecture is 64-bit.
	requested_length = (requested_length + 7) & ~((uint64_t)7);
	memory_status_t *memaddr = get_status_entry_for_first_available_address(requested_length);
	if (memaddr == NULL)
	{
		// No hole fits: borrow a chunk. Every ledger request is smaller than a
		// chunk, so the search after it finds one.
		if (!ledger_borrow_chunk())
		{
			ledger_unlock(irqflags);
			if (fallible)
				return 0;
			panic("allocator: OUT OF MEMORY — no frames left to lend the ledger for %lu bytes\n",
			      requested_length);
		}
		memaddr = get_status_entry_for_first_available_address(requested_length);
	}
	// The try entry point prepares its HHDM tables before carving, so mapping
	// after ownership changes cannot exhaust the table pool.
	if (fallible && !paging_hhdm_prepare_range(memaddr->startAddress, requested_length)) {
		ledger_unlock(irqflags);
		return 0;
	}

	uint64_t start = memaddr->startAddress;
	if (memaddr->length == requested_length)
		memaddr->in_use = true;
	else
	{
		kAllocSplits++;   // a carve mints a new table entry
		make_new_status_entry(start, requested_length, true);
		update_existing_status_entry(memaddr, start + requested_length,
		                             memaddr->length - requested_length, false);
	}
	// DETAILED since 2026-08-07: this fires on every ledger allocation, and
	// plain DEBUG_ALLOCATOR means the ~10s health line, not a per-call diary.
	printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "allocator: ledger allocated 0x%08x bytes at phys 0x%08x\n",
	       requested_length, start);

	// Map, then pin: a pinned frame is a mapped frame (see the top of file).
	paging_hhdm_map_range(start, requested_length);
	ledger_pin_extent(start, requested_length, 1);

	//SECURITY: never hand out memory containing another allocation's stale
	//data. Zero through the HHDM alias, now that it's mapped. Guarded on
	//kHHDMMaintenanceEnabled because before the real page tables are live
	//the HHDM alias is not guaranteed.
	if (kHHDMMaintenanceEnabled)
		memset((void *)(start | kHHDMOffset), 0, requested_length);

	ledger_unlock(irqflags);
	return start;
}

//The free-path compaction BACKSTOP: normally the kworker's maintenance pass
//(allocator_maintain) compacts on its own schedule and this never fires. It
//exists for kworker-less boots, where dead entries would otherwise pile up
//unbounded — the same disease the 2026-08-07 fix cured, via a second door.
#define FREE_COMPACT_BACKSTOP_DEAD_ENTRIES 512

static void ledger_free(uint64_t address)
{
	uint64_t irqflags = ledger_lock();
	uint64_t statusIdx = get_status_index_for_requested_address(address, 0, true);
	memory_status_t *status_entry = &kMemoryStatus[statusIdx];

	// THE EXACT-BASE TRIPWIRE (2026-08-14, the scribbled-text hunt). The
	// lookup above matches by CONTAINMENT: any address INSIDE a live extent
	// finds that extent, so a stray or stale free — a page-table walk that
	// resolved to somebody else's frame, a double-free of an address that was
	// reallocated in the interim — would release the ENTIRE innocent extent
	// to be reissued and zeroed under its living owner. That is how a running
	// task's TEXT page turned into garbage instructions mid-run (hog -n 6).
	// A ledger extent is unaligned and handed out at its base, so its base is
	// the only legal free.
	if (address != status_entry->startAddress)
		panic("free_memory: 0x%016lx is INSIDE ledger extent 0x%016lx (len 0x%lx) but is not "
		      "its base — stray or stale free; refusing to release the extent\n",
		      address, status_entry->startAddress, status_entry->length);
	printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "allocator: ledger freeing 0x%016lx, length=0x%016lx\n",
	       status_entry->startAddress, status_entry->length);
	status_entry->in_use = false;
	// POISON-ON-FREE, enabled 2026-08-14 for the scribbled-text hunt and
	// kept on by ruling (Chris, 2026-08-15) as a CONFIG.h knob — see
	// ALLOCATOR_POISON_ON_FREE there for the what and the why. Runtime-gated
	// on kHHDMMaintenanceEnabled exactly like the zeroing: before maintenance
	// is live the HHDM alias is not guaranteed mapped.
#if ALLOCATOR_POISON_ON_FREE
	if (kHHDMMaintenanceEnabled)
		memset((void*)(status_entry->startAddress + kHHDMOffset), 0xFE, status_entry->length);
#endif

	// Unpin, then unmap: once the pins drop, a liveness check under
	// kFrameLock already says "not live" for the pages about to go.
	ledger_pin_extent(status_entry->startAddress, status_entry->length, -1);
	//Drop the HHDM mapping of every page this extent fully owns (partial
	//boundary pages that may host live neighbouring allocations stay
	//mapped). From here on, touching this memory through the HHDM faults:
	//that's the use-after-free tripwire, by design. Also broadcasts a TLB
	//shootdown to the other cores.
	paging_hhdm_unmap_range(status_entry->startAddress, status_entry->length);

	//Coalescing and the compaction backstop run under the ledger lock — both
	//rewrite kMemoryStatus (and compaction invalidates every index).
	merge_freed_block(statusIdx);
	if (kAllocZeroedEntries >= FREE_COMPACT_BACKSTOP_DEAD_ENTRIES)
		compact_memory_array();

	ledger_unlock(irqflags);
}

// ── Frame runs ──────────────────────────────────────────────────────────────

static uint64_t frames_alloc_bytes(uint64_t length, bool fallible)
{
	if (length == 0 || length > (FRAMES_NONE - 1) * PAGE_SIZE)
	{
		if (fallible)
			return 0;
		panic("allocator: a %lu-byte allocation cannot be served\n", length);
	}
	uint64_t count = (length + PAGE_SIZE - 1) / PAGE_SIZE;
	uint64_t flags = frames_lock();
	uint64_t frame = frames_alloc(&kFrames, count, FRAMES_RUN);
	if (frame == FRAMES_NONE)
	{
		frames_unlock(flags);
		if (fallible)
			return 0;
		panic("allocator: OUT OF MEMORY — no free run of %lu frames (%lu bytes); longest free run is %lu frames\n",
		      count, length, frames_largest_free(&kFrames));
	}
	uint64_t phys = frame * PAGE_SIZE, bytes = count * PAGE_SIZE;
	// The try entry point prepares HHDM tables first, so the map below cannot
	// exhaust the table pool once the run is owned; refused, the run goes back.
	if (fallible && !paging_hhdm_prepare_range(phys, bytes))
	{
		frames_free(&kFrames, frame, NULL);
		frames_unlock(flags);
		return 0;
	}
	// Mapped before kFrameLock goes, so no liveness check can see this run
	// live and unmapped. Mapping is a no-op until the real kernel page tables
	// exist; the retro-map pass covers runs handed out before that.
	paging_hhdm_map_range(phys, bytes);
	frames_unlock(flags);

	printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "allocator: %lu frames at phys 0x%016lx\n", count, phys);

	// SECURITY: zero on allocate, through the HHDM alias. Outside the lock:
	// the run is already its caller's and nobody else can reach it, and a
	// 16 MiB zero no longer holds every other core's allocations waiting.
	if (kHHDMMaintenanceEnabled)
		memset((void *)(phys | kHHDMOffset), 0, bytes);
	return phys;
}

static void frames_free_bytes(uint64_t address)
{
	uint64_t frame = address / PAGE_SIZE;
	uint64_t flags = frames_lock();
	frames_kind_t kind;
	uint64_t count;
	frames_status_t s = frames_check_start(&kFrames, frame, &kind, &count);
	// THE EXACT-BASE TRIPWIRE, frame edition: a run is freed by its first
	// byte and nothing else. An address inside a run is a stray or stale free
	// (see ledger_free for the hunt that earned the rule); name it and the
	// run it landed in, and release nothing.
	if (s == FRAMES_OK && (address & (PAGE_SIZE - 1)) != 0)
		s = FRAMES_NOT_A_START;
	if (s != FRAMES_OK)
	{
		uint64_t run = frames_run_start(&kFrames, frame);
		panic("free_memory: 0x%016lx refused, %s (run at frame 0x%lx) — refusing to release anything\n",
		      address, frames_status_name(s), run);
	}
	uint64_t bytes = count * PAGE_SIZE;
#if ALLOCATOR_POISON_ON_FREE
	if (kHHDMMaintenanceEnabled)
		memset((void *)(address | kHHDMOffset), 0xFE, bytes);
#endif
	// Unmapped before the run is released, under the same lock, so no
	// liveness check sees it free and still mapped, or live and unmapped.
	paging_hhdm_unmap_range(address, bytes);
	frames_free(&kFrames, frame, NULL);
	frames_unlock(flags);
	printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "allocator: freed %lu frames at phys 0x%016lx\n", count, address);
}

// ── The public entry points ─────────────────────────────────────────────────

uint64_t allocate_memory_aligned(uint64_t requested_length)
{
	return frames_alloc_bytes(requested_length, false);
}

//NOTE: Only the kernel can request unaligned memory.  User space allocations MUST be on a page boundry and be the full page
uint64_t allocate_memory(uint64_t requested_length)
{
	return requested_length >= PAGE_SIZE ? frames_alloc_bytes(requested_length, false)
	                                     : ledger_alloc(requested_length, false);
}

// Optional allocations may refuse; existing allocation APIs retain their
// panic-on-exhaustion contract. No physical address zero is handed out.
uint64_t allocate_memory_try(uint64_t length)
{
	return length >= PAGE_SIZE ? frames_alloc_bytes(length, true)
	                           : ledger_alloc(length, true);
}

uint64_t free_memory(uint64_t address)
{
	printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "allocator: Freeing memory at 0x%016lx\n", address);
	// A frame lent to the ledger stays the ledger's (chunks are never
	// returned), so reading its kind without kFrameLock cannot race a change
	// of owner. Anything else is the frame table's to judge.
	if (frames_kind(&kFrames, address / PAGE_SIZE) == FRAMES_LEDGER)
		ledger_free(address);
	else
		frames_free_bytes(address);
	return 0;
}

// One atomic reading of the books for SYSCALL_MEMORY (and anyone else who
// asks): free bytes, used bytes, and the largest contiguous free extent, all
// captured with both locks held so the numbers describe the SAME instant.
// free + used must equal kAvailableMemory EXACTLY, forever: the frame table
// counts every usable frame as free, a run, or lent to the ledger; the
// ledger's free and live bytes add up to what it was lent; and the usable
// bytes no frame describes are counted as used. The moment any of those
// books drifts, the identity stops holding and the drift is visible from
// ring 3 (the memory_test fixture asserts it every boot).
// The ledger walk is a few thousand rows; top polls at human speed.
void allocator_memory_snapshot(uint64_t *free_bytes, uint64_t *used_bytes,
                               uint64_t *largest_free_extent)
{
	uint64_t ledger_used = 0, ledger_free_total = 0, ledger_largest = 0;
	uint64_t lflags = ledger_lock();
	for (uint64_t cnt = 0; cnt < kMemoryStatusCurrentPtr; cnt++)
	{
		if (kMemoryStatus[cnt].length == 0)
			continue;   // cleared slot (compaction/merge leftovers)
		if (kMemoryStatus[cnt].in_use)
			ledger_used += kMemoryStatus[cnt].length;
		else
		{
			ledger_free_total += kMemoryStatus[cnt].length;
			if (kMemoryStatus[cnt].length > ledger_largest)
				ledger_largest = kMemoryStatus[cnt].length;
		}
	}
	uint64_t fflags = frames_lock();
	uint64_t frames_free_bytes_now = kFrames.free_frames * PAGE_SIZE;
	uint64_t frames_run_bytes = kFrames.run_frames * PAGE_SIZE;
	uint64_t largest_run = frames_largest_free(&kFrames) * PAGE_SIZE;
	frames_unlock(fflags);
	ledger_unlock(lflags);

	if (free_bytes)
		*free_bytes = frames_free_bytes_now + ledger_free_total;
	if (used_bytes)
		*used_bytes = frames_run_bytes + ledger_used + kFramesHeldBytes;
	if (largest_free_extent)
		*largest_free_extent = largest_run > ledger_largest ? largest_run : ledger_largest;
}

void allocator_frame_counts(uint64_t *usable, uint64_t *free_frames, uint64_t *run_frames,
                            uint64_t *ledger_frames)
{
	uint64_t flags = frames_lock();
	if (usable) *usable = kFrames.usable_frames;
	if (free_frames) *free_frames = kFrames.free_frames;
	if (run_frames) *run_frames = kFrames.run_frames;
	if (ledger_frames) *ledger_frames = kFrames.ledger_frames;
	frames_unlock(flags);
}

const char *allocator_audit(uint64_t *where)
{
	uint64_t flags = frames_lock();
	const char *why = frames_audit(&kFrames, where);
	frames_unlock(flags);
	return why;
}

// Every allocated extent, for init_os64_paging_tables' retro-map: frame
// runs whole, and the ledger's LIVE extents (not its chunks, whose free
// holes must stay unmapped).
void allocator_for_each_allocated(void (*fn)(void *ctx, uintptr_t phys, uint64_t length), void *ctx)
{
	uint64_t lflags = ledger_lock();
	uint64_t fflags = frames_lock();
	uint64_t cursor = 0, first, count;
	frames_kind_t kind;
	while (frames_next_run(&kFrames, &cursor, &first, &count, &kind))
		if (kind == FRAMES_RUN)
			fn(ctx, first * PAGE_SIZE, count * PAGE_SIZE);
	frames_unlock(fflags);
	for (uint64_t cnt = 0; cnt < kMemoryStatusCurrentPtr; cnt++)
		if (kMemoryStatus[cnt].in_use && kMemoryStatus[cnt].length != 0)
			fn(ctx, kMemoryStatus[cnt].startAddress, kMemoryStatus[cnt].length);
	ledger_unlock(lflags);
}

// ── Boot ────────────────────────────────────────────────────────────────────

// Describe the usable stretch [lo, hi) as free, minus the ranges already
// described as allocated (`skip`, sorted, non-overlapping). Only whole frames
// are described; anything else is counted into kFramesHeldBytes by the caller.
static void add_usable(uint64_t lo, uint64_t hi, const uint64_t skip[][2], int nskip)
{
	uint64_t first = (lo + PAGE_SIZE - 1) / PAGE_SIZE, end = hi / PAGE_SIZE;
	for (int i = 0; i <= nskip && first < end; i++)
	{
		uint64_t stop = end, next = end;
		if (i < nskip)
		{
			if (skip[i][1] <= first || skip[i][0] >= end)
				continue;
			stop = skip[i][0] > first ? skip[i][0] : first;
			next = skip[i][1];
		}
		if (stop > first)
		{
			frames_status_t s = frames_add_region(&kFrames, first, stop - first);
			if (s != FRAMES_OK)
				panic("allocator: usable frames 0x%lx..0x%lx cannot be described: %s\n",
				      first, stop, frames_status_name(s));
		}
		first = next;
	}
}

void allocator_init()
{
	// 1. The highest usable frame sizes the table: one descriptor per frame
	//    below it, holes in the physical map included.
	uint64_t top = 0;
	for (uint64_t cnt = 0; cnt < kMemMapEntryCount; cnt++)
		if (kMemMap[cnt]->type == LIMINE_MEMMAP_USABLE &&
		    kMemMap[cnt]->base + kMemMap[cnt]->length > top)
			top = kMemMap[cnt]->base + kMemMap[cnt]->length;
	uint64_t nframes = top / PAGE_SIZE;
	if (nframes > FRAMES_NONE)
		nframes = FRAMES_NONE;   // past 4 TiB; frames above go undescribed
	uint64_t table_bytes = round_up_to_nearest_page(frames_table_bytes(nframes));

	// 2. paging_init wrote its early page tables into the first pages of the
	//    lowest usable region; they are live until init_os64_paging_tables
	//    switches CR3, so they are allocated, never handed out.
	uint64_t boot_base = getLowestAvailableMemoryAddress(0x1000);
	uint64_t boot_end = boot_base + RESERVED_PAGES * PAGE_SIZE;
	for (uint64_t cnt = 0; cnt < kMemMapEntryCount; cnt++)
		if (kMemMap[cnt]->type == LIMINE_MEMMAP_USABLE && kMemMap[cnt]->base == boot_base &&
		    boot_end > boot_base + kMemMap[cnt]->length)
			boot_end = boot_base + kMemMap[cnt]->length;

	// 3. The table goes in the first usable region with room for it, clear of
	//    those pages, written through Limine's full HHDM like everything the
	//    allocator touches before the real kernel tables exist.
	uint64_t table_phys = 0;
	for (uint64_t cnt = 0; cnt < kMemMapEntryCount && table_phys == 0; cnt++)
	{
		if (kMemMap[cnt]->type != LIMINE_MEMMAP_USABLE)
			continue;
		uint64_t lo = round_up_to_nearest_page(kMemMap[cnt]->base);
		uint64_t hi = (kMemMap[cnt]->base + kMemMap[cnt]->length) & ~(uint64_t)(PAGE_SIZE - 1);
		if (lo < PAGE_SIZE)
			lo = PAGE_SIZE;
		if (lo < boot_end && boot_base < hi)
			lo = boot_end;
		if (hi > lo && hi - lo >= table_bytes)
			table_phys = lo;
	}
	if (table_phys == 0)
		panic("allocator: no usable region can hold the %lu-byte frame table\n", table_bytes);
	if (frames_init(&kFrames, (frame_t *)(table_phys | kHHDMOffset), nframes) != FRAMES_OK)
		panic("allocator: frame table init refused %lu frames\n", nframes);

	// 4. What is already in use, then everything else usable as free.
	uint64_t skip[2][2] = {
		{ boot_base / PAGE_SIZE, boot_end / PAGE_SIZE },
		{ table_phys / PAGE_SIZE, (table_phys + table_bytes) / PAGE_SIZE },
	};
	if (skip[0][0] > skip[1][0])
	{
		uint64_t t0 = skip[0][0], t1 = skip[0][1];
		skip[0][0] = skip[1][0]; skip[0][1] = skip[1][1];
		skip[1][0] = t0; skip[1][1] = t1;
	}
	for (int i = 0; i < 2; i++)
	{
		frames_status_t s = frames_add_allocated(&kFrames, skip[i][0], skip[i][1] - skip[i][0], FRAMES_RUN);
		if (s != FRAMES_OK)
			panic("allocator: frames 0x%lx..0x%lx cannot be reserved at boot: %s\n",
			      skip[i][0], skip[i][1], frames_status_name(s));
	}
	for (uint64_t cnt = 0; cnt < kMemMapEntryCount; cnt++)
		if (kMemMap[cnt]->type == LIMINE_MEMMAP_USABLE)
		{
			uint64_t lo = kMemMap[cnt]->base, hi = lo + kMemMap[cnt]->length;
			if (hi > nframes * PAGE_SIZE)
				hi = nframes * PAGE_SIZE;
			if (hi > lo)
				add_usable(lo, hi, (const uint64_t (*)[2])skip, 2);
		}
	kFramesHeldBytes = kAvailableMemory - kFrames.usable_frames * PAGE_SIZE;

	// 5. The ledger's rows come from the frame table, and the ledger starts
	//    empty: it borrows its first chunk on its first allocation.
	uint64_t ledger_bytes = sizeof(memory_status_t) * INITIAL_MEMORY_STATUS_COUNT;
	uint64_t ledger_frame = frames_alloc(&kFrames, (ledger_bytes + PAGE_SIZE - 1) / PAGE_SIZE, FRAMES_RUN);
	if (ledger_frame == FRAMES_NONE)
		panic("allocator: no frames for the ledger's %lu-byte table\n", ledger_bytes);
	kMemoryStatus = (memory_status_t *)((ledger_frame * PAGE_SIZE) | kHHDMOffset);
	kMemoryStatusCurrentPtr = 0;

	printd(DEBUG_BOOT, "allocator: frame table %lu frames (%lu KiB) at 0x%016lx; %lu usable, %lu held\n",
	       nframes, table_bytes / 1024, table_phys, kFrames.usable_frames, kFramesHeldBytes);
}

// ── kworker-side maintenance + observability (2026-08-07) ────────────────────
//
// Chris's design question, answered in code: "could compaction be a kworker
// job rather than costing each memory requestor time?" Yes — the LOCK still
// exists (everything here rewrites kMemoryStatus, so it must be held), but
// WHO pays moves: the hot alloc/free paths never sweep, and the kworker's
// passes are BOUNDED (a cursor walks at most maxEntries per visit), so any
// requestor unlucky enough to contend waits out a short pass, not a
// full-table sweep. The free-path backstop above fires only on kworker-less
// boots. The frame table needs none of this: it has no dead entries and
// merges at free time.

extern __uint128_t kDebugLevel;   // printd's runtime gate — checked here so a
                                  // disabled DEBUG_ALLOCATOR skips the WALK,
                                  // not just the print

// One bounded maintenance visit: try to coalesce free entries the free-time
// merge missed (its two neighbors were live THEN — lifetimes interleave, so
// mergeable pairs appear later), then compact when enough dead entries have
// accumulated to be worth a sweep. Returns the number of merges performed.
uint32_t allocator_maintain(uint32_t maxEntries)
{
	static uint64_t sCursor = 0;   // kworker-only caller — no reentrancy
	uint32_t merges = 0;

	uint64_t irqflags = ledger_lock();

	if (kMemoryStatusCurrentPtr > 0)
	{
		if (sCursor >= kMemoryStatusCurrentPtr)
			sCursor = 0;
		uint64_t visits = maxEntries;
		while (visits-- > 0)
		{
			memory_status_t *entry = &kMemoryStatus[sCursor];
			if (!entry->in_use && entry->length > 0 && entry->startAddress != 0)
				if (merge_freed_block(sCursor))
					merges++;
			if (++sCursor >= kMemoryStatusCurrentPtr)
			{
				sCursor = 0;
				break;   // one full lap max per visit, even on tiny tables
			}
		}
	}

	// Compact on the kworker's schedule: cheaper thresholds than the free
	// path's backstop, because HERE nobody's allocation is waiting on us
	// (they'd only contend, briefly, on the lock).
	if (kAllocZeroedEntries >= 64)
		compact_memory_array();

	ledger_unlock(irqflags);
	return merges;
}

// The DEBUG_ALLOCATOR health lines — the 2026-08-07 autopsy, self-service:
// the ledger's walk (counts and the top-4 free-hole sizes, the exact shape
// that named first-fit fragmentation the day pmemsave dragged the table out
// of a live guest) and the frame table's counters, which need no walk.
// The caller (kworker) invokes at a human cadence, not per allocation.
void allocator_debug_report(void)
{
	if (!(kDebugLevel & DEBUG_ALLOCATOR))
		return;

	// Top-4 free-hole sizes by count, gathered in one pass with a tiny
	// insertion table — 16 tracked sizes is plenty for a health line, and
	// a bounded tracker can't grow into its own version of the disease.
	#define AR_TRACKED 16
	uint64_t sizes[AR_TRACKED] = {0};
	uint64_t counts[AR_TRACKED] = {0};
	uint32_t tracked = 0;
	uint64_t inUse = 0, freeCnt = 0, dead = 0;

	uint64_t irqflags = ledger_lock();
	uint64_t entries = kMemoryStatusCurrentPtr;
	for (uint64_t i = 0; i < entries; i++)
	{
		memory_status_t *e = &kMemoryStatus[i];
		if (e->length == 0) { dead++; continue; }
		if (e->in_use) { inUse++; continue; }
		freeCnt++;
		for (uint32_t s = 0; s < AR_TRACKED; s++)
		{
			if (s == tracked && tracked < AR_TRACKED)
			{
				sizes[tracked] = e->length;
				counts[tracked] = 1;
				tracked++;
				break;
			}
			if (sizes[s] == e->length)
			{
				counts[s]++;
				break;
			}
		}
	}
	uint64_t exactfit = kAllocExactFitHits, splits = kAllocSplits;
	uint64_t merges = kAllocMerges, compactions = kAllocCompactions;
	uint64_t fflags = frames_lock();
	frames_t f = kFrames;   // the counters, captured at one instant
	frames_unlock(fflags);
	ledger_unlock(irqflags);

	// Pick the top 4 by count (tiny N — selection is fine).
	uint64_t topSize[4] = {0}, topCount[4] = {0};
	for (uint32_t s = 0; s < tracked; s++)
	{
		for (int t = 0; t < 4; t++)
			if (counts[s] > topCount[t])
			{
				for (int m = 3; m > t; m--)
				{
					topCount[m] = topCount[m-1];
					topSize[m] = topSize[m-1];
				}
				topCount[t] = counts[s];
				topSize[t] = sizes[s];
				break;
			}
	}

	printd(DEBUG_ALLOCATOR,
	       "allocator: frames usable=%lu free=%lu run=%lu ledger=%lu free_runs=%lu | allocs=%lu refusals=%lu fallback_walks=%lu examined=%lu\n",
	       f.usable_frames, f.free_frames, f.run_frames, f.ledger_frames, f.free_runs,
	       f.allocs, f.refusals, f.fallback_walks, f.fallback_examined);
	printd(DEBUG_ALLOCATOR,
	       "allocator: ledger entries=%lu inuse=%lu free=%lu dead=%lu | exactfit=%lu splits=%lu merges=%lu compactions=%lu\n",
	       entries, inUse, freeCnt, dead,
	       exactfit, splits, merges, compactions);
	printd(DEBUG_ALLOCATOR,
	       "allocator: top free holes: %lux%lu %lux%lu %lux%lu %lux%lu\n",
	       topCount[0], topSize[0], topCount[1], topSize[1],
	       topCount[2], topSize[2], topCount[3], topSize[3]);
}
