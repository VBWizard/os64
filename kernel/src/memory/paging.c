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
#include "printd.h"
#include "video.h"
#include "gdt.h"
#include "idt.h"
#include "pci_lookup.h"


extern uintptr_t kKernelBaseAddressV;
extern uintptr_t kKernelBaseAddressP;
extern pci_device_id_t *kPCIIdsData;
extern uint32_t kPCIIdsCount;
extern struct limine_smp_response *kLimineSMPInfo;
uintptr_t kKernelPageMappings[KERNEL_PAGE_COUNT][2]={0};
int kKernelPageMappingsCount=0;

#define KERNEL_PAGE_MAPPINGS_VIRTUAL_IDX 0
#define KERNEL_PAGE_MAPPINGS_PHYSICAL_IDX 1

//Kernel paging pml4 table physical address
pt_entry_t kKernelPML4;
//Kernel paging pml4 table virtual (higher half) address
pt_entry_t kKernelPML4v;
//Higher Half Direct Mapping offset
uint64_t kHHDMOffset;
uint64_t kPagingPagesCount;
uintptr_t kPagingPagesBaseAddressV, kPagingPagesBaseAddressP;
uintptr_t kPagingPagesCurrentPtr;

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


uintptr_t paging_walk_paging_table_keep_flags(pt_entry_t* pml4, uint64_t virtual_address, bool keepPageFlags) 
{
    // Get the PML4 entry
    uintptr_t pdpt_entry = pml4[PML4_INDEX(virtual_address)];
    if ((pdpt_entry & 0x1) == 0) { // Check Present bit
        return 0xbadbadba; // PML4 entry is invalid
    }
    pt_entry_t* pdpt = (pt_entry_t*)((pdpt_entry & ~0xFFF) | kHHDMOffset);

    // Get the PDPT entry
    uintptr_t pd_entry = pdpt[PDPT_INDEX(virtual_address)];
    if ((pd_entry & 0x1) == 0) { // Check Present bit
        return 0xbadbadba; // PDPT entry is invalid
    }
    pt_entry_t* pd = (pt_entry_t*)((pd_entry & ~0xFFF) | kHHDMOffset);

    // Get the PD entry
    uintptr_t pd_entry_value = pd[PD_INDEX(virtual_address)];
    if ((pd_entry_value & 0x1) == 0) { // Check Present bit
        return 0xbadbadba; // PD entry is invalid
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
        return 0xbadbadba; // PT entry is invalid
    }

	uintptr_t physical_address = pt_entry;

	if (!keepPageFlags)
	{
		//Removing page attribute bits
		physical_address &= ~0xFFF;
		//Add the virtual address' last 12 bits back on
	 	physical_address |= (virtual_address & 0xFFF);
	}
	//   Third, get rid of any HH parts
	physical_address &= 0x0000FFFFFFFFFFFF;
    return physical_address;
}

// Walk the paging table to find the paging entries for a virtual address, returns the PTE value
uintptr_t paging_walk_paging_table(pt_entry_t* pml4, uint64_t virtual_address) 
{
	return paging_walk_paging_table_keep_flags(pml4, virtual_address, false);
}

void paging_map_page(pt_entry_t *pml4, uint64_t virtual_address, uint64_t physical_address, uint64_t flags) {
    // Align addresses to 4 KB boundaries
    physical_address &= PAGE_ADDRESS_MASK;
    virtual_address &= PAGE_ADDRESS_MASK;

	uint8_t tableRequiredFlags = (flags & PAGE_WRITE)?PAGE_WRITE:0;

    printd(DEBUG_PAGING | DEBUG_DETAILED, "PAGING: Map 0x%016lx to 0x%016lx flags 0x%08lx\n", physical_address, virtual_address, flags);

    // Step 1: Traverse or allocate the PDPT table
    pt_entry_t *pdpt_page;
    uint64_t pml4e = pml4[PML4_INDEX(virtual_address)];

    if (pml4e & PAGE_PRESENT) {
        // Combine existing flags with new flags
        pml4[PML4_INDEX(virtual_address)] = (pml4e & ~0xFFF) | ((pml4e | tableRequiredFlags) & 0xFFF);
        uint64_t pdpt_phys = pml4[PML4_INDEX(virtual_address)] & ~0xFFF;
        pdpt_page = (pt_entry_t *)PHYS_TO_VIRT(pdpt_phys);
	    printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "\tPDPT present @ 0x%016lx\n",pdpt_page);
    } else {
        // Allocate new PDPT page
	    printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "\tPDPT not present - allocating it\n");
        uint64_t new_pdpt_phys = get_paging_table_page();
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
	    printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "\tPD present @ 0x%016lx\n", pd_page);
    } else {
	    printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "\tPD not present - allocating it\n");
        // Allocate new PD page
        uint64_t new_pd_phys = get_paging_table_page();
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
	    printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "\tPT present @ 0x%016lx\n", pt_page);
    } else {
        // Allocate new PT page
	    printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "\tPT not present - allocating it\n");
        uint64_t new_pt_phys = get_paging_table_page();
        pt_entry_t *new_pt_page = (pt_entry_t *)PHYS_TO_VIRT(new_pt_phys);
        memset(new_pt_page, 0, PAGE_SIZE);
        if ((((uintptr_t)new_pt_page >> 32) & 0xFFFFFFFF) != 0xFFFF8000)
			panic("Bad page table entry address. (0x%016lx)  kHHDMOffset = 0x%016lx\n", new_pt_page, kHHDMOffset);
		pd_page[PD_INDEX(virtual_address)] = new_pt_phys | tableRequiredFlags | PAGE_PRESENT;
        pt_page = new_pt_page;
    }

	uint16_t finalFlags =  flags | PAGE_PRESENT;
    printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "\tSetting page table entry at 0x%016lx, index 0x%04x, to 0x%016lx, flags 0x%08x\n", pt_page, PT_INDEX(virtual_address), physical_address, finalFlags);
    // Step 4: Map the final page in the PT table
    pt_page[PT_INDEX(virtual_address)] = physical_address | finalFlags;
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
	__uint128_t temp;

	if ((uintptr_t)pml4 < kHHDMOffset)
		pml4 = (uintptr_t*)((uintptr_t)pml4 | kHHDMOffset);

	if ((physical_address & 0x00000FFF) > 0)
	{
		physical_address &= 0xFFFFFFFFFFFFF000;
		printd(DEBUG_PAGING, "Adjusted physical address to 0x%016lx due to address not being aligned to page boundry\n", physical_address);
	}
	if ((virtual_address & 0x00000FFF) > 0)
	{
		virtual_address &= 0xFFFFFFFFFFFFF000;
		page_count++;
		printd(DEBUG_PAGING, "Adjusted virtual address to 0x%016lx and incremented page count by 1 due to address not being aligned to page boundry\n", virtual_address);
	}


//	if (physical_address < 0x1000)
//		panic("paging_map_pages: Attempt to map physical address 0x%016lx to virtual address 0x%016lx\n", physical_address, virtual_address);

	printd(DEBUG_PAGING, "PAGING: Mapping 0x%08x pages at 0x%016lx to 0x%016lx with flags 0x%08x\n", page_count, physical_address, virtual_address, flags);

	if (page_count > 0x10)
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

uintptr_t get_paging_table_page()
{
	uintptr_t retVal = kPagingPagesCurrentPtr;
	kPagingPagesCurrentPtr += PAGE_SIZE;
	return retVal;
}

uintptr_t get_paging_table_pageV()
{
	uintptr_t retVal = get_paging_table_page();
	retVal |= kHHDMOffset;
	return retVal;
}

void save_kernel_mappings()
{
	uintptr_t physAddrLookup;

	for (uintptr_t addrV=kKernelBaseAddressV;addrV < kKernelBaseAddressV + (KERNEL_PAGE_COUNT * PAGE_SIZE); addrV+=PAGE_SIZE)
	{
		physAddrLookup = paging_walk_paging_table_keep_flags((pt_entry_t*)kKernelPML4v, addrV, true);
		if (physAddrLookup != 0 && physAddrLookup != 0xbadbadba)
		{
			kKernelPageMappings[kKernelPageMappingsCount][KERNEL_PAGE_MAPPINGS_VIRTUAL_IDX] = addrV;
			kKernelPageMappings[kKernelPageMappingsCount++][KERNEL_PAGE_MAPPINGS_PHYSICAL_IDX] = physAddrLookup;
			printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "\t\tPAGING: Vaddr 0x%016lx maps to Paddr 0x%016lx\n", addrV, physAddrLookup);
		}
	}
}

void paging_map_kernel_into_pml4(uintptr_t* pml4v)
{
	printd(DEBUG_PAGING | DEBUG_DETAILED,"PAGING (paging_map_kernel_into_pml4): Mapping virtual kernel (0x%016lx) to physical kernel (0x%016lx), %u pages in new page tables\n", kKernelBaseAddressV, kKernelBaseAddressP, kKernelPageMappingsCount);
	for (int cnt=0;cnt<kKernelPageMappingsCount;cnt++)
	{
			uintptr_t virt = kKernelPageMappings[cnt][KERNEL_PAGE_MAPPINGS_VIRTUAL_IDX];
			if (virt > 0)
			{
				uintptr_t phys = kKernelPageMappings[cnt][KERNEL_PAGE_MAPPINGS_PHYSICAL_IDX];
				uint64_t flags = phys & 0xFFF;
				paging_map_page(pml4v, virt, phys & 0xFFFFFFFFFFFFF000, flags);
			}
	}
	printd(DEBUG_PAGING | DEBUG_DETAILED, "PAGING (paging_map_kernel_into_pml4): %u kernel page mappings copied\n",kKernelPageMappingsCount);
}

void init_os64_paging_tables()
{
	
	uint64_t pagesToMap = 0;
	uint64_t rsp = 0;
	uintptr_t physAddrLookup = 0;

	uint64_t allocSize = kMaxPhysicalAddress / PAGE_SIZE;
	kPagingPagesCount = allocSize / PAGE_SIZE;
	if (allocSize % PAGE_SIZE)
		kPagingPagesCount++;
	//Preallocate mapped pages for use when a new paging page is required by paging_map_page
	kPagingPagesBaseAddressP = (uintptr_t)allocate_memory_aligned(allocSize);
	kPagingPagesBaseAddressV = kPagingPagesBaseAddressP | kHHDMOffset;
	kPagingPagesCurrentPtr = kPagingPagesBaseAddressP;


	//Make sure all the pages are empty
	memset((void*)kPagingPagesBaseAddressV, 0, allocSize);

	save_kernel_mappings();

	printd(DEBUG_PAGING | DEBUG_DETAILED,"PAGING: Allocated page pool - 0x%08x pages at 0x%016x (virtual=0x%016lx)\n",
			kMaxPhysicalAddress / PAGE_SIZE, kPagingPagesBaseAddressP, kPagingPagesBaseAddressV);

    uintptr_t* pml4p = (uintptr_t*)get_paging_table_page();
	uintptr_t* pml4v = (uintptr_t*)((uintptr_t)pml4p | kHHDMOffset);

	printd(DEBUG_PAGING | DEBUG_DETAILED,"PAGING: Mapping existing items into the new pml4\n");
	printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map PML4\n");
	printd(DEBUG_PAGING | DEBUG_DETAILED,"\tPAGING: Mapping virtual pml4 (%p) to physical pml4 (%p)\n", pml4v, pml4p);
	paging_map_page(pml4v, (uintptr_t)pml4v, (uintptr_t)pml4p, PAGE_PRESENT | PAGE_WRITE);

	//make page 0 invalid
	printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map page 0\n");
	printd(DEBUG_PAGING | DEBUG_DETAILED,"\tPAGING: Mapping virtual page 0 to physical page 0 (not present)\n");
	paging_map_page(pml4v, 0, 0, 0); 

	//Map the page pool into the new structure
	printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map paging page pool\n");
	printd(DEBUG_PAGING | DEBUG_DETAILED,"\tPAGING: Mapping virtual page pool (0x%016lx) to physical page pool (0x%016lx)\n", kPagingPagesBaseAddressV, kPagingPagesBaseAddressP);
	paging_map_pages(pml4v, kPagingPagesBaseAddressV, kPagingPagesBaseAddressP, kPagingPagesCount, PAGE_PRESENT | PAGE_WRITE);

	printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map kernel\n");
	paging_map_kernel_into_pml4(pml4v);
	
	//Map the renderer struct
	printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map renderer\n");
	physAddrLookup = paging_walk_paging_table((pt_entry_t*)kKernelPML4v, (uintptr_t)&kRenderer);
	printd(DEBUG_PAGING | DEBUG_DETAILED,"\tPAGING: Mapping virtual renderer (0x%016lx) to physical render (0x%016lx), %u pages in new page tables\n", &kRenderer, physAddrLookup, PAGE_SIZE);
	paging_map_pages(pml4v, (uintptr_t)&kRenderer, physAddrLookup, PAGE_SIZE, PAGE_PRESENT | PAGE_WRITE);	

	//Map the psf1_font
	printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map console font\n");
	physAddrLookup = paging_walk_paging_table((pt_entry_t*)kKernelPML4v, (uintptr_t)kRenderer.psf1_font);
	pagesToMap = sizeof(struct PSF1_FONT);
	if (sizeof(struct PSF1_FONT) % PAGE_SIZE)
		pagesToMap++;
	printd(DEBUG_PAGING | DEBUG_DETAILED,"\tPAGING: Mapping virtual font (0x%016lx) to physical font (0x%016lx), %u pages in new page tables\n", kRenderer.psf1_font, physAddrLookup, pagesToMap);
	paging_map_pages(pml4v, (uintptr_t)kRenderer.psf1_font, physAddrLookup, pagesToMap, PAGE_PRESENT | PAGE_WRITE);

	//Map the PSF1_HEADER
	printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map font header\n");
	physAddrLookup = paging_walk_paging_table((pt_entry_t*)kKernelPML4v, (uintptr_t)kRenderer.psf1_font->psf1_header);
	pagesToMap = sizeof(struct PSF1_HEADER);
	if (sizeof(struct PSF1_HEADER) % PAGE_SIZE)
		pagesToMap++;
	printd(DEBUG_PAGING | DEBUG_DETAILED,"\tPAGING: Mapping virtual font header (0x%016lx) to physical font header (0x%016lx), %u pages in new page tables\n", kRenderer.psf1_font->psf1_header, physAddrLookup, pagesToMap);
	paging_map_pages(pml4v, (uintptr_t)kRenderer.psf1_font->psf1_header, physAddrLookup, pagesToMap, PAGE_PRESENT | PAGE_WRITE);

	printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map virtual font glyph buffer\n");
	physAddrLookup = paging_walk_paging_table((pt_entry_t*)kKernelPML4v, (uintptr_t)kRenderer.psf1_font->glyph_buffer);
	pagesToMap = 4;
	printd(DEBUG_PAGING | DEBUG_DETAILED,"\tPAGING: Mapping virtual font glyph buffer (0x%016lx) to physical font glyph buffer (0x%016lx), %u pages in new page tables\n", kRenderer.psf1_font, physAddrLookup, pagesToMap);
	paging_map_pages(pml4v, (uintptr_t)kRenderer.psf1_font->glyph_buffer, physAddrLookup, pagesToMap, PAGE_PRESENT | PAGE_WRITE);

	//Map the framebuffer struct
	printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map framebuffer object\n");
	physAddrLookup = paging_walk_paging_table((pt_entry_t*)kKernelPML4v,(uintptr_t) &kFrameBuffer);
	pagesToMap = sizeof(struct Framebuffer) / PAGE_SIZE;
	if (sizeof(struct Framebuffer) % PAGE_SIZE)
		pagesToMap++;
	printd(DEBUG_PAGING | DEBUG_DETAILED,"\tPAGING: Mapping virtual framebuffer object (0x%016lx) to physical framebuffer object (0x%016lx), %u pages in new page tables\n", &kFrameBuffer, physAddrLookup, pagesToMap);
	paging_map_pages(pml4v, (uintptr_t)&kFrameBuffer, physAddrLookup, sizeof(struct Framebuffer), PAGE_PRESENT | PAGE_WRITE);

	//Map the actual framebuffer hardware addresses
	printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map framebuffer base address\n");
	physAddrLookup = paging_walk_paging_table((pt_entry_t*)kKernelPML4v,(uintptr_t)kFrameBuffer.base_address);
	pagesToMap = kFrameBuffer.buffer_size / PAGE_SIZE;
	if (kFrameBuffer.buffer_size % PAGE_SIZE)
		pagesToMap++;
	printd(DEBUG_PAGING | DEBUG_DETAILED,"\tPAGING: Mapping virtual framebuffer base (0x%016lx) to physical framebuffer base (0x%016lx), %u pages in new page tables\n", kFrameBuffer.base_address, physAddrLookup, pagesToMap);
	paging_map_pages(pml4v, (uintptr_t)kFrameBuffer.base_address, physAddrLookup, pagesToMap, PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);

	//Map the allocator struct array
	printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map memory status structures\n");
	physAddrLookup = paging_walk_paging_table((pt_entry_t*)kKernelPML4v,(uintptr_t)kMemoryStatus);
	pagesToMap = (INITIAL_MEMORY_STATUS_COUNT * sizeof(memory_status_t))/PAGE_SIZE;
	if ((INITIAL_MEMORY_STATUS_COUNT * sizeof(memory_status_t))%PAGE_SIZE)
	pagesToMap++;
	printd(DEBUG_PAGING | DEBUG_DETAILED,"\tPAGING: Mapping virtual memory status (0x%016lx) to physical memory status (0x%016lx), %u pages in new page tables\n", kMemoryStatus, physAddrLookup, pagesToMap);
	paging_map_pages(pml4v, (uintptr_t)kMemoryStatus, physAddrLookup, pagesToMap, PAGE_PRESENT | PAGE_WRITE);

	//Map the PCI ID data
	printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map PCI ID data\n");
	physAddrLookup = paging_walk_paging_table((pt_entry_t*)kKernelPML4v, (uintptr_t)kPCIIdsData);
	pagesToMap = (kPCIIdsCount * sizeof(pci_device_id_t)) / PAGE_SIZE;
	if ((kPCIIdsCount * sizeof(pci_device_id_t)) % PAGE_SIZE)
		pagesToMap++;
	printd(DEBUG_PAGING | DEBUG_DETAILED,"\tPAGING: Mapping virtual PCIID data (0x%016lx) to physical PCIID data (0x%016lx), %u pages in new page tables\n", kPCIIdsData, physAddrLookup, pagesToMap);
	paging_map_pages(pml4v, (uintptr_t)kPCIIdsData, physAddrLookup, pagesToMap, PAGE_PRESENT | PAGE_WRITE);

	//Map the limine SMP info
	printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map Limine SMP Info structures\n");
	physAddrLookup = paging_walk_paging_table((pt_entry_t*)kKernelPML4v, (uintptr_t)kLimineSMPInfo);
	pagesToMap = sizeof(struct limine_smp_response) / PAGE_SIZE;
	if (sizeof(struct limine_smp_response) % PAGE_SIZE)
		pagesToMap++;
	printd(DEBUG_PAGING | DEBUG_DETAILED,"\tPAGING: Mapping virtual SMPInfo (0x%016lx) to physical SMPInfo (0x%016lx), %u pages in new page tables\n", kLimineSMPInfo, physAddrLookup, pagesToMap);
	paging_map_pages(pml4v, (uintptr_t)kLimineSMPInfo, physAddrLookup, pagesToMap, PAGE_PRESENT | PAGE_WRITE);

	printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map Limine SMP CPUs pointer\n");
	physAddrLookup = paging_walk_paging_table((pt_entry_t*)kKernelPML4v, (uintptr_t)kLimineSMPInfo->cpus);
	paging_map_pages(pml4v, (uintptr_t)kLimineSMPInfo->cpus, physAddrLookup, 1, PAGE_PRESENT | PAGE_WRITE);

	printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map Limine SMP CPUs struct\n");
	physAddrLookup = paging_walk_paging_table((pt_entry_t*)kKernelPML4v, (uintptr_t)*kLimineSMPInfo->cpus);
	pagesToMap = (sizeof(struct limine_smp_info) * kLimineSMPInfo->cpu_count) / PAGE_SIZE;
	if ((sizeof(struct limine_smp_info) * kLimineSMPInfo->cpu_count) % PAGE_SIZE)
		pagesToMap++;
	paging_map_pages(pml4v, (uintptr_t)*kLimineSMPInfo->cpus, physAddrLookup, pagesToMap, PAGE_PRESENT | PAGE_WRITE);


	printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map stack\n");
	asm volatile("mov %0, rsp" : "=r" (rsp));
	// Get the physical address corresponding to the current RSP
	physAddrLookup = paging_walk_paging_table((pt_entry_t*)kKernelPML4v, rsp);
	// Align both the virtual and physical addresses to the start of the stack range
	uintptr_t stackBaseVirtual = rsp & 0xFFFFFFFFFFFFF000; // Align to 64 KB boundary
	uintptr_t stackBasePhysical = physAddrLookup & 0xFFFFFFFFFFFFF000;
	// Map the entire 64 KB stack range (16 pages * 4 KB = 64 KB)
	printd(DEBUG_PAGING | DEBUG_DETAILED,"\tPAGING: Mapping virtual stack (0x%016lx) to physical stack (0x%016lx), %u pages in new page tables\n", stackBaseVirtual, stackBasePhysical, 16);
	paging_map_pages(pml4v, stackBaseVirtual, stackBasePhysical, 16, PAGE_PRESENT | PAGE_WRITE);

	//Map the GDT
	printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map GDT\n");
	gdt_pointer_t gdtr;
	asm volatile("sgdt %0" : "=m"(gdtr));
	physAddrLookup = paging_walk_paging_table((pt_entry_t*)kKernelPML4v,gdtr.base);
	pagesToMap = gdtr.limit / PAGE_SIZE;
	if (gdtr.limit % PAGE_SIZE)
		pagesToMap++;
	printd(DEBUG_PAGING | DEBUG_DETAILED,"\tPAGING: Mapping virtual GDT (0x%016lx) to physical GDT (0x%016lx), %u pages in new page tables\n", gdtr.base, physAddrLookup, pagesToMap);
	paging_map_pages(pml4v, gdtr.base & PAGE_ADDRESS_MASK, physAddrLookup & PAGE_ADDRESS_MASK, pagesToMap, PAGE_PRESENT | PAGE_WRITE);

	printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map IDT\n");
	struct IDTPointer idtr;
	asm volatile("sidt %0" : "=m"(idtr));
	physAddrLookup = paging_walk_paging_table((pt_entry_t*)kKernelPML4v,idtr.base);
	pagesToMap = idtr.limit / PAGE_SIZE;
	if (idtr.limit % PAGE_SIZE)
		pagesToMap++;
	printd(DEBUG_PAGING | DEBUG_DETAILED,"\tPAGING: Mapping virtual IDT (0x%016lx) to physical IDT (0x%016lx), %u pages in new page tables\n", idtr.base, physAddrLookup, pagesToMap);
	paging_map_pages(pml4v, idtr.base & PAGE_ADDRESS_MASK, physAddrLookup & PAGE_ADDRESS_MASK, pagesToMap, PAGE_PRESENT | PAGE_WRITE);

	kKernelPML4 = (uintptr_t)pml4p;
	kKernelPML4v = (uintptr_t)pml4v;

	asm volatile ("cli\nmov cr3, %0\nsti" :: "r"(kKernelPML4) : "memory");
}