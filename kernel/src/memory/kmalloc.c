#include "kmalloc.h"
#include "allocator.h"
#include "paging.h"
#include "memset.h"
#include "serial_logging.h"
#include "panic.h"

uint64_t kFreeCallCount=0;
#define KFREE_COMPACT_FREQUENCY 10
void kmalloc_common(uint64_t physical_address, uint64_t virtual_address, uint64_t length)
{
	uint64_t page_count = length / PAGE_SIZE;
	if (length % PAGE_SIZE != 0)
		page_count++;
	if (virtual_address % PAGE_SIZE)
		page_count++;
	paging_map_pages((pt_entry_t*)kKernelPML4v, virtual_address & PAGE_ADDRESS_MASK, physical_address & PAGE_ADDRESS_MASK, page_count, PAGE_PRESENT | PAGE_WRITE);
	// Page contents are already zeroed centrally by the allocator choke point
	// (allocate_memory_at_address_internal). kmalloc always runs after
	// kHHDMMaintenanceEnabled is set, so that zeroing is guaranteed to have run.
}

// Allocate aligned memory for the kernel
void *kmalloc_aligned(uint64_t length)
{
	uint64_t addr = allocate_memory_aligned(length);
	uint64_t virtual_address = addr + kHHDMOffset;
	kmalloc_common(addr, virtual_address, length);
	return (void*)virtual_address;
}

// Allocate unaligned memory for the kernel
void *kmalloc(uint64_t length)
{
	if (length==0)
		panic("kmalloc: Attempt to allocate 0 bytes is invalid\n");
	uint64_t addr = allocate_memory(length);
	uint64_t virtual_address = addr + kHHDMOffset;
	kmalloc_common(addr, virtual_address, length);
	return (void*)virtual_address;
}

/// @brief DMA allocation: HHDM pointer for the kernel, physical for the device.
///
/// THE IDENTITY-MAP ERA ENDED HERE (2026-08-19, paying the DEBTS row). The
/// old version returned the PHYSICAL address as the pointer and identity-
/// mapped it PRESENT|WRITE|PCD so the number worked in both worlds. That
/// convenience had grown four sets of teeth:
///
///   1. A high physical address became a high VIRTUAL address — upper-half
///      KERNEL territory, a collision waiting for a page (e1000.c called
///      this out and refused to use us; its doctrine is now ours).
///   2. The identity mappings were NEVER UNMAPPED on free. Every per-I/O
///      PRP-list page left behind a stale writable PCD mapping over a
///      physical page the allocator had already handed to someone else — a
///      standing hole through the lazy-HHDM use-after-free tripwire, and a
///      stray-write amplifier of exactly the class the NVMe bounce-buffer
///      tripwire (2026-08-14) exists to catch.
///   3. Per-I/O identity mappings above 4GB churned the paging pool and
///      found the intermediate-table CAS race (paging.c, 2026-08-15) —
///      address space no boot had ever touched, mapped ~1,100 times/sec.
///   4. kfree had to GUESS which world a pointer came from (the
///      `> kHHDMOffset` conditional below) — a heuristic where a fact
///      should be.
///
/// Now: the allocator's lazy-HHDM rule already maps every allocated page at
/// phys|kHHDMOffset, so there is NOTHING to map — we allocate, zero through
/// the HHDM alias, and hand back both addresses separately. Freeing is plain
/// kfree, which unmaps the HHDM alias at the allocator's choke point like
/// every other allocation — the tripwire covers DMA pages again.
///
/// CACHEABILITY: the old PCD (uncached) mapping is deliberately NOT
/// recreated. x86 DMA is cache-coherent (the platform snoops), the HHDM is
/// ordinary write-back, and the proof has been in production since 2026-08-06:
/// e1000's rings and buffers are plain HHDM memory, certified on QEMU,
/// VirtualBox, and the P5's RTL8125 sibling. Descriptor rings and bounce
/// buffers gain the cache; the devices never notice. (MMIO register windows
/// are a different animal and keep their explicit UC mappings — this
/// function was never for those.)
void *kmalloc_dma(uint64_t length, uintptr_t *phys_out)
{
	printd(DEBUG_KMALLOC,"kmalloc_dma: Allocating %lu bytes\n", length);
	uint64_t addr = allocate_memory_aligned(length);
	uint64_t virtual_address = addr + kHHDMOffset;

	// Zero through the HHDM alias — the contract every caller inherited from
	// the identity era (and NVMe's PRP builder genuinely relies on: a list
	// page's unused tail entries must read as null pointers, not as the
	// previous owner's bytes).
	memset((void*)virtual_address, 0, length);

	if (phys_out)
		*phys_out = addr;
	printd(DEBUG_KMALLOC,"kmalloc_dma: returning VA 0x%016lx (phys 0x%016lx)\n",
	       virtual_address, addr);
	return (void*)virtual_address;
}

// (kmalloc_dma32_address was buried in the same grave, 2026-08-19: identity
// mapping at a fixed 32-bit physical address, zero callers in the tree. When
// a 32-bit-DMA device actually arrives, its reachability contract deserves a
// designed allocator, not this fossil — the DEBTS row on DMA reachability
// keeps that conversation alive.)

/// @brief Free previously kmalloc'd space
/// @param
/// @return
void kfree(void *address)
{
	// free(NULL) is a no-op — the C convention since forever, and a contract
	// at least one caller (elf_image_free's "kfree of a never-populated table
	// is a NULL no-op") had already assumed in writing. Before 2026-08-06 the
	// assumption was false: NULL fell through the HHDM adjustment unchanged
	// and free_memory(0) panicked "Can't find the index for 0x0". Nobody had
	// ever hit it because elf_image_free only ran on malformed-ELF error
	// paths — until the undertaker (task_destroy) made freeing a healthy
	// static image's NULL dynamic/symtab tables an every-burial event.
	if (address == NULL)
		return;
	// The below-kHHDMOffset arm is VESTIGIAL since 2026-08-19: kmalloc_dma
	// was the last allocator that returned non-HHDM pointers, and it now
	// returns HHDM like everything else. Kept as a defensive pass-through
	// (a wild low pointer will still be rejected loudly by free_memory's
	// index lookup rather than by an underflowed subtraction here).
	uintptr_t physicalAddress = (uintptr_t)address > kHHDMOffset?(uintptr_t)address - kHHDMOffset:(uintptr_t)address;
    // Free the allocation (remove the HHDM offset from the address when freeing it)
	// DETAILED: per-free diary line — same demotion (and reason) as the
	// per-allocation line in allocator.c; plain DEBUG_ALLOCATOR = health line.
	printd(DEBUG_KMALLOC | DEBUG_DETAILED, "KMALLOC: Freeing address 0x%016lx (0x%016lx)\n",address, physicalAddress);

	uint64_t idx = free_memory(physicalAddress);
	if (idx==0xFFFFFFFF)
		panic("kFree: free_memory returned 0xFFFFFFFF indicating it could not find the block of memory to free for physical address 0x%016lx\n",physicalAddress);
	//Merging + periodic compaction now happen inside free_memory, under the
	//allocator lock — they rewrite kMemoryStatus and raced concurrent
	//allocations when done here, outside it. (idx is only valid as an error
	//signal after free_memory returns, not as an array index.)
#ifdef KMALLOC_CLEAR_FREED_POINTERS
	address = (void*)0xBADBADBA;
#endif
}
