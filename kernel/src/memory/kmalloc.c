#include "kmalloc.h"
#include "allocator.h"
#include "paging.h"
#include "memset.h"
#include "serial_logging.h"
#include "panic.h"

uint64_t kFreeCallCount=0;
#define KFREE_COMPACT_FREQUENCY 100
void kmalloc_common(uint64_t physical_address, uint64_t virtual_address, uint64_t length)
{
	uint64_t page_count = length / PAGE_SIZE;
	if (length % PAGE_SIZE != 0)
		page_count++;
	paging_map_pages((pt_entry_t*)kKernelPML4v, virtual_address & PAGE_ADDRESS_MASK, physical_address & PAGE_ADDRESS_MASK, page_count, PAGE_PRESENT | PAGE_WRITE);
	memset((void*)virtual_address, 0, length);
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

void kfree(void *address) 
{
	uintptr_t physicalAddress = (uintptr_t)address > kHHDMOffset?(uintptr_t)address - kHHDMOffset:(uintptr_t)address;
    // Free the allocation (remove the HHDM offset from the address when freeing it)
	printd(DEBUG_KMALLOC, "KMALLOC: Freeing address 0x%016lx (0x%16lx)\n",address, physicalAddress);

	uint64_t idx = free_memory(physicalAddress);
	if (idx==0xFFFFFFFF)
		panic("kFree: free_memory returned 0xFFFFFFFF indicating it could not find the block of memory to free for physical address 0x%016lx\n",physicalAddress);
    //merge_freed_block(idx);
	//if (++kFreeCallCount%KFREE_COMPACT_FREQUENCY==0)
	//	compact_memory_array();
#ifdef KMALLOC_CLEAR_FREED_POINTERS
	address = (void*)0xBADBADBA;
#endif
}
