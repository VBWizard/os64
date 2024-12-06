#include "paging.h"
#include "CONFIG.h"
#include "kmalloc.h"
#include "allocator.h"
#include "memmap.h"
#include "BasicRenderer.h"
#include "memory/memset.h"
#include "CONFIG.h"
#include "serial_logging.h"
#include "memcpy.h"
#include "panic.h"

extern uintptr_t kDebugLevel;

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

void validatePagingHierarchy(uintptr_t address) {
    uintptr_t* pml4 = (uintptr_t*)kKernelPML4v;
    uintptr_t pml4Index = (address >> 39) & 0x1FF;
    uintptr_t pdptIndex = (address >> 30) & 0x1FF;
    uintptr_t pdIndex = (address >> 21) & 0x1FF;
    uintptr_t ptIndex = (address >> 12) & 0x1FF;

    uintptr_t pml4Entry = pml4[pml4Index];
    printd(DEBUG_PAGING, "PAGING: PML4[%zu]: 0x%016lx\n", pml4Index, pml4Entry);
    if (!(pml4Entry & 0x1)) {
        printd(DEBUG_PAGING, "PAGING: PML4 entry not present\n");
        return;
    }

    uintptr_t* pdpt = (uintptr_t*)((kHHDMOffset | pml4Entry) & ~0xFFF);
    uintptr_t pdptEntry = pdpt[pdptIndex];
    printd(DEBUG_PAGING, "PAGING: PDPT[%zu]: 0x%016lx\n", pdptIndex, pdptEntry);
    if (!(pdptEntry & 0x1)) {
        printd(DEBUG_PAGING, "PAGING: PDPT entry not present\n");
        return;
    }

    uintptr_t* pd = (uintptr_t*)((kHHDMOffset | pdptEntry) & ~0xFFF);
    uintptr_t pdEntry = pd[pdIndex];
    printd(DEBUG_PAGING, "PAGING: PD[%zu]: 0x%016lx\n", pdIndex, pdEntry);
    if (!(pdEntry & 0x1)) {
        printd(DEBUG_PAGING, "PAGING: PD entry not present\n");
        return;
    }

    uintptr_t* pt = (uintptr_t*)((kHHDMOffset | pdEntry) & ~0xFFF);
    uintptr_t pte = pt[ptIndex];
    printd(DEBUG_PAGING, "PAGING: PT[%zu]: 0x%016lx\n", ptIndex, pte);
}

// Walk the paging table to find the paging entries for a virtual address, returns the PTE value
uintptr_t paging_walk_paging_table(pt_entry_t* pml4, uint64_t virtual_address) 
{
    // Get the PML4 entry
    uintptr_t pdpt_entry = pml4[PML4_INDEX(virtual_address)];
    if ((pdpt_entry & 0x1) == 0) { // Check Present bit
        return 0; // PML4 entry is invalid
    }
    pt_entry_t* pdpt = (pt_entry_t*)((pdpt_entry & ~0xFFF) | kHHDMOffset);

    // Get the PDPT entry
    uintptr_t pd_entry = pdpt[PDPT_INDEX(virtual_address)];
    if ((pd_entry & 0x1) == 0) { // Check Present bit
        return 0; // PDPT entry is invalid
    }
    pt_entry_t* pd = (pt_entry_t*)((pd_entry & ~0xFFF) | kHHDMOffset);

    // Get the PD entry
    uintptr_t pd_entry_value = pd[PD_INDEX(virtual_address)];
    if ((pd_entry_value & 0x1) == 0) { // Check Present bit
        return 0; // PD entry is invalid
    }

    // Check for a 2 MiB page
    if (pd_entry_value & (1 << 7)) { // PS bit set
        // Calculate the physical address for a 2 MiB page
        uintptr_t physical_address = (pd_entry_value & ~0x1FFFFF) | (virtual_address & 0x1FFFFF);
        return physical_address;
    }

    // Get the PT entry (for 4 KiB pages)
    pt_entry_t* pt = (pt_entry_t*)((pd_entry_value & ~0xFFF) | kHHDMOffset);
    uintptr_t pt_entry = pt[PT_INDEX(virtual_address)];
    if ((pt_entry & 0x1) == 0) { // Check Present bit
        return 0; // PT entry is invalid
    }

    // Calculate the physical address for a 4 KiB page
    uintptr_t physical_address = (pt_entry & ~0xFFF) | (virtual_address & 0xFFF);
    return physical_address;
}

void paging_map_page(pt_entry_t *pml4, uint64_t virtual_address, uint64_t physical_address, uint64_t flags) {
    // Align addresses to 4 KB boundaries
    physical_address &= PAGE_ADDRESS_MASK;
    virtual_address &= PAGE_ADDRESS_MASK;

	uint8_t tableRequiredFlags = (flags & PAGE_WRITE)?PAGE_WRITE:0;

    printd(DEBUG_PAGING, "PAGING: Map 0x%016lx to 0x%016lx flags 0x%08lx\n", physical_address, virtual_address, flags);

    // Step 1: Traverse or allocate the PDPT table
    pt_entry_t *pdpt_page;
    uint64_t pml4e = pml4[PML4_INDEX(virtual_address)];

    if (pml4e & PAGE_PRESENT) {
        // Combine existing flags with new flags
        pml4[PML4_INDEX(virtual_address)] = (pml4e & ~0xFFF) | ((pml4e | tableRequiredFlags) & 0xFFF);
        uint64_t pdpt_phys = pml4[PML4_INDEX(virtual_address)] & ~0xFFF;
        pdpt_page = (pt_entry_t *)PHYS_TO_VIRT(pdpt_phys);
	    printd(DEBUG_PAGING, "\tPDPT present @ 0x%016x\n",pdpt_page);
    } else {
        // Allocate new PDPT page
	    printd(DEBUG_PAGING, "\tPDPT not present - allocating it\n");
        uint64_t new_pdpt_phys = allocate_memory_aligned(PAGE_SIZE);
        pt_entry_t *new_pdpt_page = (pt_entry_t *)PHYS_TO_VIRT(new_pdpt_phys);
        memset(new_pdpt_page, 0, PAGE_SIZE);
        pml4[PML4_INDEX(virtual_address)] = new_pdpt_phys | tableRequiredFlags | PAGE_PRESENT;
        pdpt_page = new_pdpt_page;
    }

    // Step 2: Traverse or allocate the PD table
    pt_entry_t *pd_page;
    uint64_t pdpt_entry = pdpt_page[PDPT_INDEX(virtual_address)];

    if (pdpt_entry & PAGE_PRESENT) {
        // Combine existing flags with new flags
        pdpt_page[PDPT_INDEX(virtual_address)] = (pdpt_entry & ~0xFFF) | ((pdpt_entry | tableRequiredFlags) & 0xFFF);
        uint64_t pd_phys = pdpt_page[PDPT_INDEX(virtual_address)] & ~0xFFF;
        pd_page = (pt_entry_t *)PHYS_TO_VIRT(pd_phys);
	    printd(DEBUG_PAGING, "\tPD present @ 0x%016x\n", pd_page);
    } else {
	    printd(DEBUG_PAGING, "\tPD not present - allocating it\n");
        // Allocate new PD page
        uint64_t new_pd_phys = allocate_memory_aligned(PAGE_SIZE);
        pt_entry_t *new_pd_page = (pt_entry_t *)PHYS_TO_VIRT(new_pd_phys);
        memset(new_pd_page, 0, PAGE_SIZE);
        pdpt_page[PDPT_INDEX(virtual_address)] = new_pd_phys | tableRequiredFlags | PAGE_PRESENT;
        pd_page = new_pd_page;
    }

    // Step 3: Traverse or allocate the PT table
    pt_entry_t *pt_page;
    uint64_t pd_entry = pd_page[PD_INDEX(virtual_address)];

    if (pd_entry & PAGE_PRESENT) {
       // Combine existing flags with new flags
        pd_page[PD_INDEX(virtual_address)] = (pd_entry & ~0xFFF) | ((pd_entry | tableRequiredFlags) & 0xFFF);
        uint64_t pt_phys = pd_page[PD_INDEX(virtual_address)] & ~0xFFF;
        pt_page = (pt_entry_t *)PHYS_TO_VIRT(pt_phys);
	    printd(DEBUG_PAGING, "\tPT present @ 0x%016lx\n", pt_page);
    } else {
        // Allocate new PT page
	    printd(DEBUG_PAGING, "\tPT not present - allocating it\n");
        uint64_t new_pt_phys = allocate_memory_aligned(PAGE_SIZE);
        pt_entry_t *new_pt_page = (pt_entry_t *)PHYS_TO_VIRT(new_pt_phys);
        memset(new_pt_page, 0, PAGE_SIZE);
        if ((((uintptr_t)new_pt_page >> 32) & 0xFFFFFFFF) != 0xFFFF8000)
			panic("Bad page table entry address. (0x%016lx)  kHHDMOffset = 0x%016lx\n", new_pt_page, kHHDMOffset);
		pd_page[PD_INDEX(virtual_address)] = new_pt_phys | tableRequiredFlags | PAGE_PRESENT;
        pt_page = new_pt_page;
    }

    printd(DEBUG_PAGING, "\tSetting page table entry at 0x%016lx, index 0x%04x, to 0x%016lx\n", pt_page, PT_INDEX(virtual_address), physical_address);
    // Step 4: Map the final page in the PT table
    pt_page[PT_INDEX(virtual_address)] = physical_address | flags | PAGE_PRESENT;
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
	uint64_t temp;

	if ((physical_address & 0x00000FFF) > 0)
	{
		physical_address &= 0xFFFFF000;
		printd(DEBUG_PAGING, "Adjusted physical address to 0x%016lx due to address not being aligned to page boundry\n", physical_address);
	}
	if ((virtual_address & 0x00000FFF) > 0)
	{
		virtual_address &= 0xFFFFF000;
		page_count++;
		printd(DEBUG_PAGING, "Adjusted virtual address to 0x%016lx and incremented page count by 1 due to address not being aligned to page boundry\n", virtual_address);
	}


	printd(DEBUG_PAGING, "PAGING: Mapping 0x%08x pages at 0x%016lx to 0x%016lx with flags 0x%08x\n", page_count, physical_address, virtual_address, flags);

	if (page_count > 0xA1)
	{
		temp = kDebugLevel;
		kDebugLevel = 0;
	}

	for (uint64_t cnt=0;cnt<page_count;cnt++)
		paging_map_page(pml4, virtual_address + (PAGE_SIZE * cnt), physical_address + (PAGE_SIZE * cnt), flags);
	
	if (page_count > 0xA1)
	{
		kDebugLevel = temp;
	}
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
	*(pt_entry_t*)(memoryBaseAddress + kHHDMOffset) = (memoryBaseAddress  + PAGE_SIZE) | PAGE_PRESENT | PAGE_WRITE;
	//PD entry 0 points to 0x3000 - 2MB coverage
	*(pt_entry_t*)(memoryBaseAddress + kHHDMOffset + PAGE_SIZE) = (memoryBaseAddress  + (PAGE_SIZE * 2)) | PAGE_PRESENT | PAGE_WRITE;

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