#include "kmalloc.h"
#include "allocator.h"
#include "paging.h"
#include "memset.h"

void kmalloc_common(uint64_t physical_address, uint64_t virtual_address, uint64_t length)
{
	uint64_t page_count = length / PAGE_SIZE;
	if (length % PAGE_SIZE != 0)
		page_count++;
	paging_map_pages((pt_entry_t*)kKernelPML4v, virtual_address, physical_address, page_count, PAGE_PRESENT | PAGE_WRITE);
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

void *kmalloc_dma32(uint32_t address, uint64_t length)
{
	uint64_t addr = allocate_memory_at_address(address, length, true);
	uint64_t page_count = length / PAGE_SIZE;
	if (length % PAGE_SIZE != 0)
		page_count++;
	paging_map_pages((pt_entry_t*)kKernelPML4v, address, address, page_count, PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
	memset((void*)(uintptr_t)address, 0, length);
	return (void*)(uintptr_t)address;
}

// Allocate unaligned memory for the kernel
void *kmalloc(uint64_t length)
{
	uint64_t addr = allocate_memory(length);
	uint64_t virtual_address = addr + kHHDMOffset;
	kmalloc_common(addr, virtual_address, length);
	return (void*)virtual_address;
}

void kfree(void *address) {
    // Free the allocation (remove the HHDM offset from the address when freeing it)
    int freed_length = free_memory((uintptr_t)address - kHHDMOffset);

    // Iterate over each page within the freed range
    for (int cnt = 0; cnt < (freed_length + PAGE_SIZE - 1) / PAGE_SIZE; cnt++) {
        // Calculate the virtual address for each page in the range
        uintptr_t page_virtual_address = (uintptr_t)address + (cnt * PAGE_SIZE);

        // If there are no other allocations on this page
        if (!physical_page_is_allocated_on(((uintptr_t)page_virtual_address - kHHDMOffset) & 0xFFFFFFFFFFFFF000)) {
            // Unmap the page if it's no longer in use
            paging_unmap_page((pt_entry_t*)kKernelPML4v, page_virtual_address);
        }
    }
}
