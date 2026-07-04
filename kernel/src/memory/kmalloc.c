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

/// @brief Same as kmalloc except the page mappings are physical-to-physical
/// @param length 
/// @return 
void *kmalloc_dma(uint64_t length)
{
	printd(DEBUG_KMALLOC,"kmalloc_dma: Allocating %lu bytes\n", length);
	int a=0;
	if (length >= 0x2000000)
		a++;
	uint64_t addr = allocate_memory_aligned(length);
	uint64_t page_count = length / PAGE_SIZE;
	if (length % PAGE_SIZE != 0)
		page_count++;
	if ((addr & 0x00000FFF) > 0)
		page_count++;

	printd(DEBUG_KMALLOC,"kmalloc_dma: Identity mapping 0x%016lx, for %u pages (PRESENT/WRITE/PCD)\n", addr, page_count);
	paging_map_pages((pt_entry_t*)kKernelPML4v, addr & PAGE_ADDRESS_MASK, addr & PAGE_ADDRESS_MASK, page_count, PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);

	printd(DEBUG_KMALLOC,"kmalloc_dma: memsetting 0x%016lu bytes...\n", length);
	memset((void*)(uintptr_t)addr, 0, length);
	printd(DEBUG_KMALLOC,"kmalloc_dma: returning 0x%016lx ...\n", addr);
	return (void*)(uintptr_t)addr;
}

/// @brief Same as kmalloc except allocates the memory in DMA space
/// @param Physical address to assign
/// @param length
/// @return
void *kmalloc_dma32_address(uint32_t address, uint64_t length)
{
	uint64_t addr = allocate_memory_at_address(address, length, true);
	uint64_t page_count = length / PAGE_SIZE;
	if (length % PAGE_SIZE != 0)
		page_count++;
	paging_map_pages((pt_entry_t*)kKernelPML4v, addr, addr, page_count, PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
	memset((void*)(uintptr_t)address, 0, length);
	return (void*)(uintptr_t)address;
}

/// @brief Free previously kmalloc'd space
/// @param
/// @return
void kfree(void *address)
{
	uintptr_t physicalAddress = (uintptr_t)address > kHHDMOffset?(uintptr_t)address - kHHDMOffset:(uintptr_t)address;
    // Free the allocation (remove the HHDM offset from the address when freeing it)
	printd(DEBUG_KMALLOC, "KMALLOC: Freeing address 0x%016lx (0x%016lx)\n",address, physicalAddress);

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
