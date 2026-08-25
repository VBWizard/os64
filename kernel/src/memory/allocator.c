#include "CONFIG.h"
#include "allocator.h"
#include "memmap.h"
#include "paging.h"
#include "memset.h"
#include "serial_logging.h"
#include "panic.h"
#include "memcpy.h"
#include "spinlock.h"

memory_status_t *kMemoryStatus;
//Points to the next available kernel status - increment AFTER use
uint64_t kMemoryStatusCurrentPtr = 0;
uintptr_t memoryBaseAddress;

// Serializes ALL allocator state (kMemoryStatus, kMemoryStatusCurrentPtr) and
// the HHDM map/unmap that rides along with allocate/free. Allocations happen
// concurrently from page-fault handlers on multiple cores (CoW privatization,
// demand paging, shared-object page resolution all kmalloc in fault context),
// and this ledger was previously completely unguarded. Interrupts are
// disabled while held (irqsave pattern): a holder preempted mid-update on one
// core would deadlock a fault-context spinner (IF=0) on that same core.
// The lock/unlock mechanics live in spinlock.h (this was their birthplace).
static spinlock_t kMemoryStatusLock = 0;

static inline uint64_t allocator_lock(void)
{
	return spinlock_acquire_irqsave(&kMemoryStatusLock);
}

static inline void allocator_unlock(uint64_t flags)
{
	spinlock_release_irqrestore(&kMemoryStatusLock, flags);
}

//NOTE: Will return the passed address if it is already page aligned
static inline uintptr_t round_up_to_nearest_page(uintptr_t addr) {
    return (addr + 0xFFF) & ~0xFFF;
}

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

//Identify whether any statuses allocate on the page passed.
bool physical_page_is_allocated_on(uintptr_t physical_page)
{
	for (uint64_t cnt=0;cnt<kMemoryStatusCurrentPtr;cnt++)
		if ((kMemoryStatus[cnt].startAddress & 0xFFFFFFFFFFFFF000) == physical_page && kMemoryStatus[cnt].in_use == true)
			return true;
	return false;
}

// Is this physical page inside a LIVE allocator extent? Caller holds
// kMemoryStatusLock. A RANGE test, not physical_page_is_allocated_on above —
// that helper answers "does an extent START here?", which is false for every
// interior page of a multi-page allocation. A page that passes is
// HHDM-mapped and safe to dereference; whether its CONTENTS are current is
// the caller's validation problem (magic words, seqlocks), never a fault.
static bool phys_page_live_locked(uintptr_t page)
{
	for (uint64_t i = 0; i < kMemoryStatusCurrentPtr; i++)
	{
		if (!kMemoryStatus[i].in_use)
			continue;
		uintptr_t s = kMemoryStatus[i].startAddress & ~(uintptr_t)0xFFF;
		uintptr_t e = round_up_to_nearest_page(kMemoryStatus[i].startAddress
		                                       + kMemoryStatus[i].length);
		if (page >= s && page < e)
			return true;
	}
	return false;
}

// Copy len bytes OUT OF A TASK'S ADDRESS SPACE, fault-proof (PR #26, rounds
// two and seven). The whole journey — walking the task's page tables AND the
// final data copy — happens under kMemoryStatusLock, with EVERY page
// liveness-verified before it is dereferenced:
//
//   Round two's race: the task unmaps the DATA region from another thread
//   mid-copy; free_memory HHDM-unmaps the alias and a raw memcpy faults
//   ring 0. Cured by verifying the leaf page under the lock.
//
//   Round seven's race, one level up: phase-2 burial's arena_destroy frees
//   the task's PAGE TABLES mid-walk. The arena is kmalloc-backed, so the
//   table pages themselves stay HHDM-mapped as recycled garbage — but a
//   GARBAGE table entry points anywhere, and dereferencing the next level
//   through `garbage | kHHDMOffset` can hit genuinely unmapped territory:
//   the fault is one hop removed, not absent. Cured by verifying every
//   table page too, so a wild "next level" fails the range test instead of
//   being followed. Garbage that happens to land in live extents copies
//   garbage BYTES — which the caller's magic/version/seqlock validation
//   exists to reject. Nonsense is survivable; faults are not.
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
	uint64_t flags = allocator_lock();

	uint64_t entry = 0;
	for (int level = 0; level < 4; level++)
	{
		uintptr_t table_phys = (table_virt - kHHDMOffset) & ~(uintptr_t)0xFFF;
		if (!phys_page_live_locked(table_phys))
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
		if (!phys_page_live_locked(data_phys))
			goto out;                       // the round-two race, caught at the leaf
		memcpy(dst, (const void *)((data_phys | kHHDMOffset) + (va & (uintptr_t)0xFFF)), len);
		ok = true;
	}

out:
	allocator_unlock(flags);
	return ok;
}

// Maintenance/observability counters (allocator.h has the tour). All are
// bumped under kMemoryStatusLock, so plain increments are safe.
uint64_t kAllocExactFitHits = 0;   // a recycled hole was reused whole
uint64_t kAllocSplits = 0;         // a fit carved a block (leftover entry born)
uint64_t kAllocMerges = 0;         // free-time neighbor merges (either side)
uint64_t kAllocCompactions = 0;    // compact_memory_array passes
uint64_t kAllocZeroedEntries = 0;  // dead (length 0) entries awaiting compaction

/// @brief Find a free block for the request: EXACT fit first, else first fit.
/// @param requestedLength
/// @param aligned
/// @return
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
/// looking for an exact hole. Honesty note: old first-fit stopped at the first
/// adequate block; this scans to the end when no exact hole exists. That trade
/// is the bargain: a full scan of the SMALL table this policy maintains costs
/// less than the old early exit over the ever-growing one it caused.
memory_status_t* get_status_entry_for_first_available_address(uint64_t requested_length, bool page_aligned)
{
	memory_status_t *firstFit = NULL;

	for (uint64_t cnt = 0; cnt < kMemoryStatusCurrentPtr; cnt++)
	{
		memory_status_t *entry = &kMemoryStatus[cnt];

		// Don't allow page 0 to be allocated.
		if (entry->startAddress == 0 || entry->in_use)
			continue;

		if (!page_aligned)
		{
			if (entry->length == requested_length)
			{
				kAllocExactFitHits++;
				return entry;      // the whole point: reuse, don't carve
			}
			if (firstFit == NULL && entry->length >= requested_length)
				firstFit = entry;
			continue;
		}

		// An aligned allocation consumes the bytes before the next page boundary too;
		// checking only requested_length can select a block that is actually too small.
		if (requested_length > entry->length)
			continue;
		uint64_t alignment_padding = round_up_to_nearest_page(entry->startAddress) - entry->startAddress;

		// Already-aligned AND exactly sized: the aligned flavor of a perfect
		// recycle (padding 0 means the == below is the whole block).
		if (alignment_padding == 0 && entry->length == requested_length)
		{
			kAllocExactFitHits++;
			return entry;
		}

		// Use <= so a block whose padding plus request exactly fills it is a valid fit.
		if (firstFit == NULL && alignment_padding <= entry->length - requested_length)
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

memory_status_t* get_status_entry_for_requested_address(uint64_t address,uint64_t requested_length, bool in_use)
{
	for (uint64_t cnt = 0; cnt < kMemoryStatusCurrentPtr; cnt++)
	{
		if ( (kMemoryStatus[cnt].startAddress <= address && kMemoryStatus[cnt].startAddress + kMemoryStatus[cnt].length > address) &&
			kMemoryStatus[cnt].in_use == in_use &&
			kMemoryStatus[cnt].length >= requested_length
		)
			return &kMemoryStatus[cnt];
	}
	panic("get_status_entry_for_requested_address: Can't find the index!!! :-(\n");
	return NULL;
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
	// table at allocator_init and nothing has ever grown it or checked it — the
	// word INITIAL was carrying the whole plan.
	//
	// What that costs, in full: when the table fills, the next entry is written
	// PAST THE END, into whatever allocation follows it in physical memory. On
	// the P5 that neighbour is the paging pool, whose very first page is the
	// KERNEL'S OWN PML4 — so entry number 100,001 landed on PML4[0] and cleared
	// its flag bits, unmapping the entire lower half of the kernel address
	// space in one store. The machine kept running (kernel text and the HHDM
	// live in the upper half) until the next disk write touched the NVMe DMA
	// bounce buffer at its low identity address, and died there, four levels
	// and a whole subsystem away from the actual bug.
	//
	// The tell was the timing: it happened between 5200 and 5800 seconds into
	// EVERY run of the same workload (`watch -n 1 "ps -ef"` — a husk and a ps
	// created and buried every second). That is not a race, it is a monotonic
	// overrun reaching a fixed wall at a fixed rate. Chris called "it's being
	// re-allocated" off the zeroes on the screen, and a hardware watchpoint on
	// PML4[0] named this function on the first hit.
	//
	// So: PANIC at the wall, never write past it. This guard is deliberately
	// panic-ONLY (2026-08-15 review find): its first cut compacted here, but
	// the carve path (allocate_memory_at_address_internal) holds `memaddr` —
	// a raw pointer INTO kMemoryStatus — across its calls to this function,
	// and compaction RELOCATES entries, so the carve's fixup would then write
	// through a dangling pointer onto an arbitrary row: handing out memory
	// somebody owns, the exact crime this guard exists to prevent. The
	// compact-with-headroom pass now runs at the TOP of the carve, before
	// any pointer into the table is taken; by the time execution reaches
	// here, a full table means compaction already failed to help.
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

uint64_t allocate_memory_at_address_internal(uint64_t requested_address, uint64_t requested_length, bool use_address, bool page_aligned)
{
	uint64_t irqflags = allocator_lock();

	// THE TABLE-FULL GUARD'S COMPACTION LIVES HERE, NOT AT MINT TIME
	// (2026-08-15 review find). A carve mints up to TWO new entries (the
	// allocation, plus a block-before split) while holding `memaddr` — a raw
	// pointer INTO kMemoryStatus — across the mints, then writes the leftover
	// fixup through it. Compacting inside make_new_status_entry relocates the
	// entry memaddr names and that fixup rewrites an arbitrary row. So the
	// recovery attempt runs NOW, before any pointer into the table exists;
	// the guard at mint time is a panic-only backstop that can no longer
	// corrupt what it protects. Headroom of 2 = this function's worst case.
	if (kMemoryStatusCurrentPtr + 2 >= INITIAL_MEMORY_STATUS_COUNT)
	{
		printd(DEBUG_ALLOCATOR, "allocator: status table full (%lu entries) — compacting before the carve\n",
		       (uint64_t)kMemoryStatusCurrentPtr);
		compact_memory_array();
	}

	memory_status_t* memaddr;
	uint64_t retVal = 0;
	uint64_t found_block_original_length = 0;
	uint64_t block_before_length = 0;
	uint64_t aligned_start;
	uint64_t aligned_length = 0;
	// The full extent recorded in kMemoryStatus for this allocation — captured
	// per-branch (memaddr gets repurposed to describe the leftover block below,
	// so it can't be read afterwards) and HHDM-mapped just before returning.
	// This is what makes `phys | kHHDMOffset` valid for every allocator-owned
	// byte, on every memory map — see paging_hhdm_map_range (paging.h).
	uint64_t hhdm_extent_start = 0;
	uint64_t hhdm_extent_length = 0;

	if (requested_length >= 200000000)
	{
		int a = 0;
		a+=1;
	}

	//Find the appropriate memory status page
	if (!use_address)
	{
		//When a specific address is NOT requested, internally align request to 8 bytes since our architecture is 64-bit
		// Align to the next multiple of 8
		requested_length = (requested_length + 7) & ~((size_t)7);
		memaddr = get_status_entry_for_first_available_address(requested_length, page_aligned);
		// OUT OF MEMORY IS A PANIC, NOT A HALT (2026-08-25, Fable's review of
		// PR #29 rd15). This was `cli; hlt` — with the allocator lock HELD, a
		// few lines up. The core went dark with no panic line and no serial
		// byte, and every other core then spun forever, interrupts off, on
		// its next allocation. A silent whole-machine wedge for the one
		// condition a kernel most needs to explain.
		//
		// It also made a promise the rest of the kernel believed: there are
		// dozens of `if (kmalloc(...) == NULL)` guards in the tree, and NOT
		// ONE can fire — this function never returns 0 for "no memory", and
		// even if it did, kmalloc adds kHHDMOffset to it. A whole review
		// round (rd15) was spent building a fallback for that unreachable
		// branch. So the contract, stated once where it is enforced: THE
		// ALLOCATOR NEVER RETURNS "NO MEMORY". It panics, by name, with the
		// size. Tripwires over silence.
		if (memaddr == NULL)
			panic("allocator: OUT OF MEMORY — no free block for %lu bytes (%s)\n",
			      requested_length, page_aligned ? "page-aligned" : "unaligned");
	}
	else
	{
		memaddr = get_status_entry_for_requested_address(requested_address, requested_length, false);
		if ( memaddr == NULL)
			panic("allocator: no free block covering the requested address 0x%016lx (%lu bytes)\n",
			      requested_address, requested_length);
	}
	if (memaddr->length < requested_length)
		retVal = 0;
	else if (memaddr->length == requested_length)
	{
		memaddr->in_use = true;
		retVal = memaddr->startAddress;
		hhdm_extent_start = memaddr->startAddress;
		hhdm_extent_length = requested_length;
	}
	else //memory available is > requested memory
	{
		kAllocSplits++;   // a carve mints at least one new table entry
		found_block_original_length = memaddr->length;

		//The starting address for the new Status entry, aligned if necessary.  THIS IS ONLY USED AS A RETURN VALUE FROM THIS METHOD
		uint64_t true_start = memaddr->startAddress;
		if (page_aligned)
		{
			aligned_start = round_up_to_nearest_page(memaddr->startAddress);
			//Count the extra bytes between the aligned start and the real start, and add the requested length
			aligned_length = aligned_start - memaddr->startAddress + requested_length;
		}
		else
		{
			aligned_start = memaddr->startAddress;
			aligned_length = requested_length;
		}
		uint64_t aligned_end = true_start + aligned_length;

		//Create an entry for the memory being utilized
		//NOTE that even if an aligned address was requested, the new entry will start with the unaligned start address. 
		//The address RETURNED will be the aligned address
		/*memory_status_t* new_entry = */make_new_status_entry(
							  use_address?requested_address:
							  	memaddr->startAddress,
							  aligned_length,
							  true);
		//Same extent the status entry just recorded — free_memory will unmap this same range later
		hhdm_extent_start = use_address?requested_address:memaddr->startAddress;
		hhdm_extent_length = aligned_length;
		//If a specific address was requested and there was memory before the requested address, make a block from its starting address to the requested address - 1
		if (use_address && memaddr->startAddress != requested_address)
		{
			block_before_length = requested_address - memaddr->startAddress;
			make_new_status_entry(memaddr->startAddress, requested_address - memaddr->startAddress, false);
		}

		//Fixup the exiting entry to just point to what's left after the allocated memory
		//If a specific address was requested, make the current status point to the address after the requested memory
		if (use_address)
			update_existing_status_entry(memaddr, requested_address + requested_length, found_block_original_length - block_before_length - requested_length, false);
		else
			//Otherwise just update the starting address and length of the existing address to point to after the allocated memory
			update_existing_status_entry(memaddr, 
			                             //The end of the created block which is the new startAddress of the existing block
										 aligned_end,
										 memaddr->length - aligned_length,
										 false
										 );
		retVal = use_address?requested_address:aligned_start;

	}
	// DETAILED since 2026-08-07: this fires on EVERY allocation, and plain
	// DEBUG_ALLOCATOR now means the ~10s health line, not a per-call diary —
	// Chris's first flag-on run exploded the log in minutes on this line
	// (and the noise's own logd-buffer churn skewed the very free-hole
	// numbers the health line exists to watch).
	printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "allocate_memory_at_address_internal: Allocated 0x%08x bytes at phys address 0x%08x (%s - %s)\n", aligned_length, use_address?requested_address:aligned_start,
			use_address?"requested address":"",page_aligned?"aligned":"");

	//Keep the "allocated <=> HHDM-mapped" invariant: map the extent's pages at
	//their HHDM addresses in the kernel tables (no-op until the real kernel
	//page tables are live — early allocations are retro-mapped when
	//init_os64_paging_tables builds them).
	if (retVal != 0)
	{
		paging_hhdm_map_range(hhdm_extent_start, hhdm_extent_length);

		//SECURITY: never hand out memory containing another allocation's stale
		//data. Zero the whole extent through its HHDM alias, now that it's mapped.
		//This is THE single zero-on-alloc choke point — it covers kmalloc(),
		//allocate_memory(), allocate_memory_aligned(), and the demand-paged
		//anonymous/BSS/heap pages resolved through the fault path — so callers no
		//longer memset individually (except kmalloc_dma, which must zero through
		//its uncached mapping for device coherency).
		//
		//Guarded on kHHDMMaintenanceEnabled because before the real page tables
		//are live the HHDM alias is not valid (paging_hhdm_map_range above is a
		//no-op then). The only pre-flag allocations are page-table pages, which
		//are fully written when populated and so need no zeroing here.
		if (kHHDMMaintenanceEnabled)
			memset((void *)(hhdm_extent_start | kHHDMOffset), 0, hhdm_extent_length);
	}

	allocator_unlock(irqflags);
	return retVal;
}

/// @brief Allocate memory, possibly at a specific address (not block aligned unless you pass a block aligned address)
/// @param address - The address of the requested memory range.  Pass 0 if no specific address is requested
/// @param requestedLength - The length of the requested memory range.  If 0 method returns 0
/// @param aligned - Should the starting address be aligned to a block based on system block size.
/// @return 
uint64_t allocate_memory_at_address(uint64_t address, uint64_t requested_length, bool use_address)
{
	return allocate_memory_at_address_internal(address, requested_length, use_address, false);
}

uint64_t allocate_memory_aligned(uint64_t requested_length)
{
	return allocate_memory_at_address_internal(0, requested_length, false, true);
}

//NOTE: Only the kernel can request unaligned memory.  User space allocations MUST be on a page boundry and be the full page
uint64_t allocate_memory(uint64_t requested_length)
{
	return allocate_memory_at_address_internal(0, requested_length, false, false);
}

//The free-path compaction BACKSTOP: normally the kworker's maintenance pass
//(allocator_maintain) compacts on its own schedule and this never fires. It
//exists for kworker-less boots, where dead entries would otherwise pile up
//unbounded — the same disease the 2026-08-07 fix cured, via a second door.
#define FREE_COMPACT_BACKSTOP_DEAD_ENTRIES 512

uint64_t free_memory(uint64_t address)
{
	printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "allocator: Freeing memory at 0x%016lx\n", address);
	uint64_t irqflags = allocator_lock();
	uint64_t statusIdx = get_status_index_for_requested_address(address, 0, true);
	memory_status_t *status_entry = &kMemoryStatus[statusIdx];
	if (status_entry != NULL)
	{
		// THE EXACT-BASE TRIPWIRE (2026-08-14, the scribbled-text hunt). The
		// lookup above matches by CONTAINMENT: any address INSIDE a live
		// extent finds that extent, so a stray or stale free — a page-table
		// walk that resolved to somebody else's frame, a double-free of an
		// address that was reallocated in the interim — silently released the
		// ENTIRE innocent extent, whose frames the allocator then re-issued
		// and zeroed under their living owner. That is how a running task's
		// TEXT page turns into garbage instructions mid-run (hog -n 6, task
		// 55, three simultaneous impossible segfaults). Every legitimate
		// caller in the tree frees the extent BASE it was handed: kfree, the
		// guarded-stack free, the trampoline free, unmap's per-page extents.
		// A mid-extent free can only be the bug class above — refuse loudly,
		// naming the address, the extent it landed inside, and its owner-shape.
		//
		// TWO legal shapes, not one (the first cut of this tripwire wedged
		// boot at NVMe init by rejecting the second): an ALIGNED allocation's
		// extent is recorded from the UNALIGNED carve start, while the caller
		// is handed the rounded-up address — the allocator's own carve comment
		// says so in as many words. So a free of either the extent base OR its
		// first page boundary is a caller returning exactly what it was given;
		// anything else landed mid-extent through arithmetic or staleness.
		if (address != status_entry->startAddress &&
		    address != round_up_to_nearest_page(status_entry->startAddress))
			panic("free_memory: 0x%016lx is INSIDE extent 0x%016lx (len 0x%lx) but is neither its base "
			      "nor its aligned base — stray or stale free; refusing to release the extent\n",
			      address, status_entry->startAddress, status_entry->length);
		printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "allocator: Found block to free, address = 0x%016lx, length=0x%016lx\n", status_entry->startAddress, status_entry->length);
		status_entry->in_use = false;
		// POISON-ON-FREE, enabled 2026-08-14 for the scribbled-text hunt and
		// kept on by ruling (Chris, 2026-08-15) as a CONFIG.h knob — see
		// ALLOCATOR_POISON_ON_FREE there for the what and the why. Its first
		// out-of-guest catch came the day of the ruling: a remote qISleep
		// walk stepped through a stale ->next into a wall of 0xFE and knew
		// INSTANTLY it was reading a freed thread_t. Runtime-gated on
		// kHHDMMaintenanceEnabled EXACTLY like the zero-on-allocate choke
		// point above: before maintenance is live the HHDM alias is not
		// guaranteed mapped, and the first ungated build wedged at tick 8
		// poisoning an early-boot free.
#if ALLOCATOR_POISON_ON_FREE
		if (kHHDMMaintenanceEnabled)
			memset((void*)(status_entry->startAddress + kHHDMOffset), 0xFE, status_entry->length);
#endif

		//Drop the HHDM mapping of every page this extent fully owns (partial
		//boundary pages that may host live neighbouring allocations stay
		//mapped). From here on, touching this memory through the HHDM faults:
		//that's the use-after-free tripwire, by design. Also broadcasts a TLB
		//shootdown to the other cores.
		paging_hhdm_unmap_range(status_entry->startAddress, status_entry->length);

		//Coalescing and periodic compaction moved here from kfree so they run
		//under the allocator lock — both rewrite kMemoryStatus (and compaction
		//invalidates every index), so doing them outside the lock raced any
		//concurrent allocation on another core. NOTE: after these, statusIdx
		//may no longer refer to the freed entry — callers must not use it to
		//index kMemoryStatus, only to detect failure.
		merge_freed_block(statusIdx);
		//Compaction is a KWORKER job now (allocator_maintain below) — the
		//requestor stops paying an O(table) sweep every tenth free. This
		//backstop stays for boots without a kworker (no KWORKER flag) and
		//for the window before it starts: only when the DEAD-entry count
		//says the table is actually littered does the free path sweep.
		if (kAllocZeroedEntries >= FREE_COMPACT_BACKSTOP_DEAD_ENTRIES)
			compact_memory_array();

		allocator_unlock(irqflags);
		return statusIdx;
	}
	panic("ALLOCATOR: Did not find kMemoryStatus entry to mark not in use, address was: 0x%016lx\n",address);
	return 0xFFFFFFFF;
}

// One atomic reading of the ledger for SYSCALL_MEMORY (and anyone else who
// asks): free bytes, used bytes, and the largest contiguous free extent, all
// captured under the allocator lock in ONE walk so the numbers describe the
// SAME instant — separate walks could disagree after a context switch.
// The ledger is seeded from every USABLE memmap entry (allocator_init), so
// free + used must equal kAvailableMemory EXACTLY, forever. That identity is
// the point of counting used here rather than deriving it: the moment a
// merge/compaction/split bug drops or double-counts an extent, the books
// stop balancing and the drift is visible from ring 3 (the memory_test
// fixture asserts the reconciliation every boot).
// O(kMemoryStatusCurrentPtr) under an irqsave spinlock: microseconds at our
// entry counts, and top polls at human speed. If a profile ever disagrees,
// running counters slot in behind this same signature.
void allocator_memory_snapshot(uint64_t *free_bytes, uint64_t *used_bytes,
                               uint64_t *largest_free_extent)
{
	uint64_t irqflags = allocator_lock();

	uint64_t free_total = 0;
	uint64_t used_total = 0;
	uint64_t largest = 0;
	for (uint64_t cnt = 0; cnt < kMemoryStatusCurrentPtr; cnt++)
	{
		if (kMemoryStatus[cnt].length == 0)
			continue;   // cleared slot (compaction/merge leftovers)
		if (kMemoryStatus[cnt].in_use)
			used_total += kMemoryStatus[cnt].length;
		else
		{
			free_total += kMemoryStatus[cnt].length;
			if (kMemoryStatus[cnt].length > largest)
				largest = kMemoryStatus[cnt].length;
		}
	}

	allocator_unlock(irqflags);

	if (free_bytes)
		*free_bytes = free_total;
	if (used_bytes)
		*used_bytes = used_total;
	if (largest_free_extent)
		*largest_free_extent = largest;
}

void allocator_init()
{

	//uint64_t allocate_size = sizeof(memory_status_t) * INITIAL_MEMORY_STATUS_COUNT;
	//uint64_t page_count = round_up_to_nearest_page(allocate_size) / PAGE_SIZE;
	
	//Get the lowest available address above or equal to 0x1000 (don't include the zero page)
	//NOTE: First pages went to paging structures
	memoryBaseAddress = getLowestAvailableMemoryAddress(0x1000) + (RESERVED_PAGES * PAGE_SIZE);

	//Update the kernel page tables for the memory used by kMemoryStatus
	//paging_map_pages((pt_entry_t*)kKernelPML4v, memoryBaseAddress, memoryBaseAddress, page_count, PAGE_PRESENT | PAGE_WRITE);

	//Create an allocator entry for kMemoryStatus which is MAX_MEMORY_STATUS_COUNT entries long
	//kMemoryStatus = (memory_status_t*)allocate_memory(allocate_size);
	kMemoryStatus = (memory_status_t*)(memoryBaseAddress + kHHDMOffset);

	//Parse the memory map into the newly created kMemoryStatus
	for (uint64_t cnt=0;cnt<kMemMapEntryCount;cnt++)
	{
		if (kMemMap[cnt]->type == LIMINE_MEMMAP_USABLE)
		{
			kMemoryStatus[kMemoryStatusCurrentPtr].startAddress = kMemMap[cnt]->base;
			kMemoryStatus[kMemoryStatusCurrentPtr].length = kMemMap[cnt]->length;
			//CLR 11/24/2024 - Removed claiming LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE as usable memory
			kMemoryStatus[kMemoryStatusCurrentPtr].in_use = kMemMap[cnt]->type != LIMINE_MEMMAP_USABLE;  //If the type isn't 0/5 then the area is in use
			kMemoryStatusCurrentPtr++;
		}
	}

	//Officially allocate our allocator entries and map them, along with the reverved pages where we created our initial paging pages
	uint64_t size = sizeof(memory_status_t);
	uint64_t allocSize = size*INITIAL_MEMORY_STATUS_COUNT;
	uint64_t newAddress = allocate_memory(allocSize) | kHHDMOffset;
	uint64_t mapSize = allocSize/PAGE_SIZE;
	if (allocSize%PAGE_SIZE)
		mapSize++;
	//paging_map_pages((pt_entry_t*)kKernelPML4v, newAddress, newAddress - kHHDMOffset, mapSize, PAGE_PRESENT | PAGE_WRITE);
	memcpy((void*)newAddress, kMemoryStatus, kMemoryStatusCurrentPtr * size);
	kMemoryStatus = (memory_status_t*)newAddress;
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
// boots.

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

	uint64_t irqflags = allocator_lock();

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

	allocator_unlock(irqflags);
	return merges;
}

// The DEBUG_ALLOCATOR ledger line — the 2026-08-07 autopsy, self-service.
// Designed to cost nothing when off: the kDebugLevel check gates the whole
// walk, and the caller (kworker) invokes at a human cadence, not per-alloc.
// One O(table) walk under the lock (same cost class as
// allocator_memory_snapshot, which top polls every second) collecting the
// counts and the top-4 free sizes — the exact shape that named first-fit
// fragmentation the day pmemsave dragged the table out of a live guest.
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

	uint64_t irqflags = allocator_lock();
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
	allocator_unlock(irqflags);

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
	       "allocator: entries=%lu inuse=%lu free=%lu dead=%lu | exactfit=%lu splits=%lu merges=%lu compactions=%lu\n",
	       entries, inUse, freeCnt, dead,
	       kAllocExactFitHits, kAllocSplits, kAllocMerges, kAllocCompactions);
	printd(DEBUG_ALLOCATOR,
	       "allocator: top free holes: %lux%lu %lux%lu %lux%lu %lux%lu\n",
	       topCount[0], topSize[0], topCount[1], topSize[1],
	       topCount[2], topSize[2], topCount[3], topSize[3]);
}
