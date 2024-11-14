#include "kmalloc.h"
#include "allocator.h"
#include "paging.h"

// Allocate aligned memory for the kernel
void *kmalloc_aligned(uint64_t length)
{
	uint64_t addr = allocate_memory_aligned(length);
	uint64_t virtual_address = addr + kHHDMOffset;
	paging_map_page((pt_entry_t*)kKernelPML4v, virtual_address, addr, PAGE_PRESENT | PAGE_WRITE);
	return (void*)addr;
}

// Allocate unaligned memory for the kernel
void *kmalloc(uint64_t length)
{
	uint64_t addr = allocate_memory(length);
	uint64_t virtual_address = addr + kHHDMOffset;
	paging_map_page((pt_entry_t*)kKernelPML4v, virtual_address, addr, PAGE_PRESENT | PAGE_WRITE);
	return (void*)addr + kHHDMOffset;
}