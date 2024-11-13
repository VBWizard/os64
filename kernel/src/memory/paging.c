#include <stdint.h>
#include <stddef.h>
#include "paging.h"
#include "CONFIG.h"
#include "kmalloc.h"
#include "allocator.h"
#include "memmap.h"
#include "BasicRenderer.h"

pt_entry_t kKernelPML4;
pt_entry_t kKernelPML4v;
uint64_t kHHMDOffset;

// Helper function to create a page entry with specified flags
static inline pt_entry_t table_entry(uint64_t physical_address, uint64_t flags) {
    return (physical_address & 0x000FFFFFFFFFF000ULL) | flags;
}

// Calculate the index at each level from the virtual address
#define PML4_INDEX(addr)  (((addr) >> 39) & 0x1FF)
#define PDPT_INDEX(addr)  (((addr) >> 30) & 0x1FF)
#define PD_INDEX(addr)    (((addr) >> 21) & 0x1FF)
#define PT_INDEX(addr)    (((addr) >> 12) & 0x1FF)

// Walk the paging table to find the paging entries for a virtual address
void paging_walk_paging_table(pt_entry_t* pml4, uint64_t virtual_address, pt_entry_t* pdpt, pt_entry_t* pd, pt_entry_t* pt, pt_entry_t* page_entry) 
{
    // Get the PML4 entry
    pdpt = (pt_entry_t*)(pml4[PML4_INDEX(virtual_address)] & ~0xFFF) + kHHMDOffset;

    // Get the PDPT entry
    pd = (pt_entry_t*)((pdpt)[PDPT_INDEX(virtual_address)] & ~0xFFF) + kHHMDOffset;

    // Get the PD entry
    pt = (pt_entry_t*)((pd)[PD_INDEX(virtual_address)] & ~0xFFF) + kHHMDOffset;

    // Get the PT entry
    page_entry = (pt_entry_t*)(pt)[PT_INDEX(virtual_address)] + kHHMDOffset;
}

void paging_map_page(
	pt_entry_t* pml4,
	uint64_t virtual_address,
	uint64_t physical_address,
	uint64_t flags)
{
	uint64_t pdpt_entry = 0, pd_entry = 0, pt_entry = 0, page_entry = 0;
	flags = flags & 0xfff;

    // Get the PML4 entry
    pdpt_entry = (pml4[PML4_INDEX(physical_address)] & ~0xFFF) + kHHMDOffset;

	if (pdpt_entry != kHHMDOffset)
	{
		// Get the PDPT entry
		pd_entry = (((pt_entry_t*)pdpt_entry)[PDPT_INDEX(physical_address)] & ~0xFFF) + kHHMDOffset;

		if (pd_entry != kHHMDOffset)
		{
			// Get the PD entry
			pt_entry = (((pt_entry_t*)pd_entry)[PD_INDEX(physical_address)] & ~0xFFF) + kHHMDOffset;

			if (pt_entry != kHHMDOffset)
			// Get the PT entry
				page_entry = ((pt_entry_t*)pt_entry)[PT_INDEX(physical_address)] + kHHMDOffset;
		}
	}
	if (pdpt_entry == kHHMDOffset || pdpt_entry == 0)
	{
		// PDPT page doesn't exist, get one
		pdpt_entry = (uint64_t)kmalloc_aligned(PAGE_SIZE);
	}
	pml4[PML4_INDEX(physical_address)] = table_entry((uint64_t)pdpt_entry, flags);

	if (pd_entry == kHHMDOffset || pd_entry == 0)
	{
		pd_entry = (uint64_t)kmalloc_aligned(PAGE_SIZE);
	}
	 ((pt_entry_t*)pdpt_entry)[PDPT_INDEX(physical_address)] = table_entry((uint64_t)pd_entry, flags);

	if (pt_entry == kHHMDOffset || pt_entry == 0)
	{
		pt_entry = (uint64_t)kmalloc_aligned(PAGE_SIZE);
	}
	 ((pt_entry_t*)pd_entry)[PD_INDEX(physical_address)] = table_entry((uint64_t)pt_entry, flags);

	if (page_entry == kHHMDOffset || page_entry == 0) 
	{
		page_entry = (uint64_t)kmalloc_aligned(PAGE_SIZE);
	}	
	 ((pt_entry_t*)pt_entry)[PT_INDEX(physical_address)] = table_entry(virtual_address, flags);
}

void paging_map_pages(
	pt_entry_t* pml4,
	uint64_t virtual_address,
	uint64_t physical_address,
	uint64_t page_count,
	uint64_t flags)
{
	for (uint64_t cnt=0;cnt<page_count;cnt++)
		paging_map_page(pml4, virtual_address + kHHMDOffset + (PAGE_SIZE * cnt), physical_address + (PAGE_SIZE * cnt), flags);
}

void paging_init()
{
	//Get the lowest available address above or equal to 0x1000 (don't include the zero page)
	uint64_t memoryBaseAddress = getLowestAvailableMemoryAddress(0x1000);
	//PML4 entry 0 points to 0x1000 - 512GB coverage
	*(pt_entry_t*)(kKernelPML4v) = memoryBaseAddress | PAGE_PRESENT | PAGE_WRITE;
	//PDPT entry 0 points to 0x2000 - 1GB coverage
	*(pt_entry_t*)(memoryBaseAddress + kHHMDOffset) = (memoryBaseAddress + PAGE_SIZE) | PAGE_PRESENT | PAGE_WRITE;
	//PD entry 0 points to 0x3000 - 2MB coverage
	*(pt_entry_t*)(memoryBaseAddress + kHHMDOffset + PAGE_SIZE) = (memoryBaseAddress + (PAGE_SIZE * 2)) | PAGE_PRESENT | PAGE_WRITE;

	uint64_t temp =  sizeof(memory_status_t);
	uint64_t page_count_to_map = (INITIAL_MEMORY_STATUS_COUNT * temp) / PAGE_SIZE;
	//Map the allocator starting at 0x4000
	for (pt_entry_t cnt=0;cnt<page_count_to_map;cnt++)
	{
		uint64_t virtual_address = (memoryBaseAddress + kHHMDOffset + PAGE_SIZE * 2) + (cnt*8);
		uint64_t physical_address = (memoryBaseAddress + (PAGE_SIZE * (3 + cnt)));
		*(pt_entry_t*)(virtual_address) = physical_address | PAGE_PRESENT | PAGE_WRITE;
	}

}