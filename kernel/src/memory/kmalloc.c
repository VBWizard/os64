#include "kmalloc.h"
#include "allocator.h"
#include "paging.h"

// Allocate aligned memory for the kernel
void *kmalloc_aligned(uint64_t length)
{
	uint64_t addr = allocate_memory_aligned(length);
	paging_map_page((pt_entry_t*)kKernelPML4v, addr  + kHHMDOffset, addr, PAGE_PRESENT | PAGE_WRITE);
	return (void*)addr;
}

// Allocate unaligned memory for the kernel
void *kmalloc(uint64_t length)
{
	uint64_t addr = allocate_memory(length);
	paging_map_page((pt_entry_t*)kKernelPML4v, addr  + kHHMDOffset, addr, PAGE_PRESENT | PAGE_WRITE);
	return (void*)addr + kHHMDOffset;
}