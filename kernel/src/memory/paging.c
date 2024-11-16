#include "paging.h"
#include "CONFIG.h"
#include "kmalloc.h"
#include "allocator.h"
#include "memmap.h"
#include "BasicRenderer.h"

//Kernel paging pml4 table physical address
pt_entry_t kKernelPML4;
//Kernel paging pml4 table virtual (higher half) address
pt_entry_t kKernelPML4v;
//Higher Half Direct Mapping offset
uint64_t kHHDMOffset;

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
    pdpt = (pt_entry_t*)(pml4[PML4_INDEX(virtual_address)] & ~0xFFF);

    // Get the PDPT entry
    pd = (pt_entry_t*)((pdpt)[PDPT_INDEX(virtual_address)] & ~0xFFF);

    // Get the PD entry
    pt = (pt_entry_t*)((pd)[PD_INDEX(virtual_address)] & ~0xFFF);

    // Get the PT entry
    page_entry = (pt_entry_t*)(pt)[PT_INDEX(virtual_address)];
}

#define PHYS_TO_VIRT(addr) ((addr) + kHHDMOffset)

void paging_map_page(pt_entry_t *pml4, uint64_t virtual_address, uint64_t physical_address, uint64_t flags) {
    // Step 1: Traverse or allocate the PDPT table
    pt_entry_t *pdpt;
    if (pml4[PML4_INDEX(virtual_address)] & PAGE_PRESENT) 
	{
        pdpt = (pt_entry_t *)PHYS_TO_VIRT(pml4[PML4_INDEX(virtual_address)] & ~0xFFF);
    } 
	else 
	{
        pdpt = (pt_entry_t*)allocate_memory_aligned(PAGE_SIZE);
        pml4[PML4_INDEX(virtual_address)] = ((uint64_t)pdpt - kHHDMOffset) | flags | PAGE_PRESENT;
        pdpt = (pt_entry_t *)PHYS_TO_VIRT((uint64_t)pdpt);  // Use virtual address
    }

    // Step 2: Traverse or allocate the PD table
    pt_entry_t *pd;
    if (pdpt[PDPT_INDEX(virtual_address)] & PAGE_PRESENT) 
	{
        pd = (pt_entry_t *)PHYS_TO_VIRT(pdpt[PDPT_INDEX(virtual_address)] & ~0xFFF);
    } 
	else 
	{
        pd = (pt_entry_t*)allocate_memory_aligned(PAGE_SIZE);
        pdpt[PDPT_INDEX(virtual_address)] = ((uint64_t)pd - kHHDMOffset) | flags | PAGE_PRESENT;
        pd = (pt_entry_t *)PHYS_TO_VIRT((uint64_t)pd);  // Use virtual address
    }

    // Step 3: Traverse or allocate the PT table
    pt_entry_t *pt;
    if (pd[PD_INDEX(virtual_address)] & PAGE_PRESENT) 
	{
        pt = (pt_entry_t *)PHYS_TO_VIRT(pd[PD_INDEX(virtual_address)] & ~0xFFF);
    } else 
	{
        pt = (pt_entry_t*)allocate_memory_aligned(PAGE_SIZE);
        pd[PD_INDEX(virtual_address)] = ((uint64_t)pt - kHHDMOffset) | flags | PAGE_PRESENT;
        pt = (pt_entry_t *)PHYS_TO_VIRT((uint64_t)pt);  // Use virtual address
    }

    // Step 4: Map the final page in the PT table
    pt[PT_INDEX(virtual_address)] = physical_address | flags | PAGE_PRESENT;
}

void paging_unmap_page(pt_entry_t *pml4, uint64_t virtual_address) {
    // Step 1: Traverse the PDPT table
    pt_entry_t *pdpt;
    if (pml4[PML4_INDEX(virtual_address)] & PAGE_PRESENT) {
        pdpt = (pt_entry_t *)PHYS_TO_VIRT(pml4[PML4_INDEX(virtual_address)] & ~0xFFF);
    } else {
        // The page is not mapped, so nothing to unmap
        return;
    }

    // Step 2: Traverse the PD table
    pt_entry_t *pd;
    if (pdpt[PDPT_INDEX(virtual_address)] & PAGE_PRESENT) {
        pd = (pt_entry_t *)PHYS_TO_VIRT(pdpt[PDPT_INDEX(virtual_address)] & ~0xFFF);
    } else {
        // The page is not mapped, so nothing to unmap
        return;
    }

    // Step 3: Traverse the PT table
    pt_entry_t *pt;
    if (pd[PD_INDEX(virtual_address)] & PAGE_PRESENT) {
        pt = (pt_entry_t *)PHYS_TO_VIRT(pd[PD_INDEX(virtual_address)] & ~0xFFF);
    } else {
        // The page is not mapped, so nothing to unmap
        return;
    }

    // Step 4: Unmap the final page in the PT table
    if (pt[PT_INDEX(virtual_address)] & PAGE_PRESENT) {
        pt[PT_INDEX(virtual_address)] = 0;  // Clear the page entry to unmap it
        // Flush the TLB entry for this virtual address
       asm volatile("invlpg [%0]" : : "r"(virtual_address) : "memory");
    }
}

void paging_map_pages(pt_entry_t* pml4,uint64_t virtual_address,uint64_t physical_address,uint64_t page_count,uint64_t flags)
{
	for (uint64_t cnt=0;cnt<page_count;cnt++)
		paging_map_page(pml4, virtual_address + kHHDMOffset + (PAGE_SIZE * cnt), physical_address + (PAGE_SIZE * cnt), flags);
}

void paging_unmap_pages(pt_entry_t *pml4, uint64_t virtual_address, size_t length) {
    // Align the virtual address down to the nearest page boundary
    uint64_t aligned_address = virtual_address & ~(PAGE_SIZE - 1);

    // Adjust the length to account for any extra bytes due to alignment
    size_t end_address = virtual_address + length;
    size_t aligned_length = end_address - aligned_address;
    size_t num_pages = (aligned_length + PAGE_SIZE - 1) / PAGE_SIZE;

    // Unmap each page in the range
    for (size_t i = 0; i < num_pages; i++) {
        paging_unmap_page(pml4, aligned_address + i * PAGE_SIZE);
    }
}

void paging_init()
{
	//Get the lowest available address above or equal to 0x1000 (don't include the zero page)
	uint64_t memoryBaseAddress = getLowestAvailableMemoryAddress(0x1000);
	//PML4 entry 0 points to 0x1000 - 512GB coverage
	*(pt_entry_t*)(kKernelPML4v) = (memoryBaseAddress) | PAGE_PRESENT | PAGE_WRITE;
	//PDPT entry 0 points to 0x2000 - 1GB coverage
	*(pt_entry_t*)(memoryBaseAddress + kHHDMOffset) = ((memoryBaseAddress + kHHDMOffset)  + PAGE_SIZE) | PAGE_PRESENT | PAGE_WRITE;
	//PD entry 0 points to 0x3000 - 2MB coverage
	*(pt_entry_t*)(memoryBaseAddress + kHHDMOffset + PAGE_SIZE) = ((memoryBaseAddress + kHHDMOffset)  + (PAGE_SIZE * 2)) | PAGE_PRESENT | PAGE_WRITE;

	uint64_t temp =  sizeof(memory_status_t);
	uint64_t page_count_to_map = (INITIAL_MEMORY_STATUS_COUNT * temp) / PAGE_SIZE;
	//Map the allocator starting at 0x4000
	for (pt_entry_t cnt=0;cnt<page_count_to_map;cnt++)
	{
		uint64_t virtual_address = (memoryBaseAddress + kHHDMOffset + (PAGE_SIZE * 2)) + (cnt*8);
		uint64_t physical_address = (memoryBaseAddress + (PAGE_SIZE * (3 + cnt)));
		*(pt_entry_t*)(virtual_address) = physical_address | PAGE_PRESENT | PAGE_WRITE;
	}

}