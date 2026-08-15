#include "paging.h"
#include "CONFIG.h"
#include "kmalloc.h"
#include "memory/arena.h"   // arena_alloc_aligned — task table pages (PAGING_ARENA.md)
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
#include "msr.h"   // rdmsr64/wrmsr64 — pat_init_this_core programs IA32_PAT


extern uintptr_t kKernelBaseAddressV;
extern uintptr_t kKernelBaseAddressP;
extern pci_device_id_t *kPCIIdsData;
extern uint32_t kPCIIdsCount;
extern void* kRamdiskModuleAddress;
extern uint64_t kRamdiskModuleSize;
extern uint64_t kKernelFileAddress;   // the kernel's own ELF file (main.c) —
extern uint64_t kKernelFileSize;      // symbols_init reads .symtab from it
extern struct limine_smp_response *kLimineSMPInfo;
extern void mpSendInvTLB();  // smp_core.c — TLB-shootdown IPI to the other cores
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

/// @brief Fund one table page from `source` (a task's arena) or the pool.
///
/// The ONE place map-time table pages come from since the paging-arena work
/// (PAGING_ARENA.md): a task's tables ride its arena and die with it at
/// burial; the kernel's ride the pool and are eternal. Returns the PHYSICAL
/// address either way — arena memory is kmalloc-backed, so the HHDM math
/// (virt - kHHDMOffset) is exact, and both sources hand back zeroed pages.
///
/// An arena that returns NULL could not GROW — that is kmalloc out of memory,
/// the same class of catastrophe as pool exhaustion, and it gets the same
/// loud death rather than a quiet fallback that would strand this task's
/// tables in two owners' books.
static uintptr_t draw_table_page(struct arena *source, const char *level, uint64_t va)
{
    if (source != NULL) {
        void *page = arena_alloc_aligned(source, PAGE_SIZE, PAGE_SIZE);
        if (page == NULL)
            panic("draw_table_page: task table arena could not grow (kmalloc OOM) mapping VA 0x%016lx\n", va);
        printd(DEBUG_PAGING | DEBUG_DETAILED, "PAGING: arena draw (%s) for VA 0x%016lx\n", level, va);
        return (uintptr_t)page - kHHDMOffset;
    }
    // Pool-draw probe: plain DEBUG_PAGING (not DETAILED) so a soak can log
    // draws — rare events now that task tables ride arenas — without the
    // per-mapping spam.
    printd(DEBUG_PAGING, "PAGING: pool draw (%s) for VA 0x%016lx — %lu/%lu used\n",
           level, va, paging_pool_pages_used() + 1, kPagingPagesCount);
    return get_paging_table_page();
}

/// @brief One level of the map walk: return the next-level table, creating it
/// atomically if absent.
///
/// CONCURRENCY (2026-08-15 — the burn's second catch). paging_map_page has no
/// lock, and for two years never needed one: every intermediate table under
/// the kernel PML4 was built at boot, on one core, so concurrent callers only
/// ever stored LEAF entries — different 8-byte slots, no conflict. The first
/// workload that made two cores create the SAME missing intermediate
/// simultaneously (concurrent per-I/O kmalloc_dma PRP-list allocations
/// landing above 4GB — identity space no boot had ever touched) hit the
/// textbook lost update: both saw not-present, both drew a fresh table, both
/// stored, last store won, and the loser's freshly-written PTE sat in an
/// orphaned table no walk would ever reach. kmalloc_dma then memset a VA it
/// had "just mapped" and died on a not-present #PF (QEMU burn: CR2
/// 0x100c80000, every intermediate present, PT[128] == 0 — read from the
/// halted guest's own tables via the monitor).
///
/// The fix is CAS, deliberately NOT a lock: draw_table_page can kmalloc (an
/// arena grows on demand), kmalloc takes kMemoryStatusLock, and the
/// allocator's HHDM choke points call back into paging while HOLDING that
/// lock — a create-lock here is an ABBA deadlock with a fuse timed to the
/// first concurrent workload. Losing the install CAS orphans the drawn page:
/// an arena page dies with its task at burial anyway, and a pool page is a
/// one-page leak on an event rare enough to be a log line (two cores racing
/// to create the SAME entry). Announced when it happens, never silent.
///
/// The present-entry flag merge is a CAS loop for the same reason: two cores
/// OR-ing different rights (one WRITE, one USER) through a plain
/// read-modify-write could drop one of them. WRITE ONLY IF IT CHANGES
/// (2026-08-14) is preserved exactly: an entry that already carries the
/// needed rights is never stored to — it would dirty the most contended
/// cache line in the machine on every walk, storm any hardware watchpoint
/// armed on the entry (the #DB triple-fault that taught us), and destroy the
/// forensic meaning of "who last wrote this entry?".
static pt_entry_t *paging_walk_or_create_level(pt_entry_t *table, uint64_t idx,
                                               uint64_t tableRequiredFlags,
                                               struct arena *tableSource,
                                               const char *level, uint64_t va)
{
    uint64_t entry = table[idx];

    while (1) {
        if (entry & PAGE_PRESENT) {
            uint64_t updated = (entry & ~0xFFFULL) | ((entry | tableRequiredFlags) & 0xFFFULL);
            if (updated == entry)
                return (pt_entry_t *)PHYS_TO_VIRT(entry & ~0xFFFULL);
            uint64_t witnessed = __sync_val_compare_and_swap(&table[idx], entry, updated);
            if (witnessed == entry)
                return (pt_entry_t *)PHYS_TO_VIRT(updated & ~0xFFFULL);
            entry = witnessed;   // someone else moved it — re-evaluate from their value
            continue;
        }

        // Absent: draw, zero, THEN publish — a table must never be reachable
        // before it is blank. The CAS is the publication.
        uint64_t new_phys = draw_table_page(tableSource, level, va);
        pt_entry_t *new_page = (pt_entry_t *)PHYS_TO_VIRT(new_phys);
        // Sanity: the drawn page's VA must land in the HHDM window. The old
        // form of this check compared bits 32-63 against 0xFFFF8000, which
        // silently demanded phys < 4GB — false the moment the (honestly
        // funded, ~20MB) pool lands above that line. Masking off the low 47
        // bits (the physical part of an HHDM alias) and comparing what
        // remains against kHHDMOffset asks the intended question at any
        // physical address.
        if (((uintptr_t)new_page & ~(uintptr_t)0x7FFFFFFFFFFFULL) != kHHDMOffset)
            panic("Bad page table page address. (0x%016lx)  kHHDMOffset = 0x%016lx\n", new_page, kHHDMOffset);
        memset(new_page, 0, PAGE_SIZE);

        uint64_t desired = new_phys | tableRequiredFlags | PAGE_PRESENT;
        uint64_t witnessed = __sync_val_compare_and_swap(&table[idx], entry, desired);
        if (witnessed == entry)
            return new_page;

        // Lost the creation race: adopt the winner's table and loop — their
        // entry may still need our required flags OR'd in, which the present
        // branch above handles. Our drawn page is orphaned (see the block
        // comment for why that beats a deadlock-prone lock).
        printd(DEBUG_PAGING,
               "PAGING: %s creation race at VA 0x%016lx — winner's table adopted, %s page 0x%016lx orphaned\n",
               level, va, tableSource ? "arena" : "pool", new_phys);
        entry = witnessed;
    }
}

void paging_map_page(pt_entry_t *pml4v, uint64_t virtual_address, uint64_t physical_address, uint64_t flags) {
    // Align addresses to 4 KB boundaries
    physical_address &= PAGE_ADDRESS_MASK;
    virtual_address &= PAGE_ADDRESS_MASK;

	// Flags that must be present at EVERY level of the walk, not just the PTE:
	// on x86-64 an access is only permitted if the needed right exists in the
	// PML4E, PDPTE, and PDE as well as the leaf.  WRITE was always propagated;
	// PAGE_USER must be too — a ring-3 access to a page whose leaf says USER
	// but whose PDE doesn't still faults (#PF error 0x15 on a fetch: present +
	// user + instruction).  This bit never mattered before the first true
	// ring-3 task: supervisor accesses ignore U/S, so ring-0 ELF tasks ran
	// happily on USER-less intermediate tables.  Permissions still ENFORCE at
	// the leaf — a USER intermediate over a supervisor-only PTE grants nothing.
	uint8_t tableRequiredFlags = (flags & (PAGE_WRITE | PAGE_USER));

	if ((uintptr_t)pml4v < kHHDMOffset)
		pml4v = (pt_entry_t *)((uintptr_t)pml4v | kHHDMOffset);

    printd(DEBUG_PAGING | DEBUG_DETAILED, "PAGING: Map 0x%016lx to 0x%016lx flags 0x%08lx\n", physical_address, virtual_address, flags);

    // Whose money funds new table pages under THIS pml4? Resolved ONCE per
    // call (not per level): a task pml4 names its arena, the kernel pml4
    // (and anything unrecognized) names the pool. See the seam's contract in
    // paging.h and the design's charter in PAGING_ARENA.md.
    struct arena *tableSource = paging_table_arena_for(pml4v);

    // Steps 1-3: walk (or atomically create) the three intermediate levels.
    // Each used to be an open-coded block here; the lost-update race between
    // two creators of the same missing table, the CAS that fixes it, and the
    // preserved WRITE-ONLY-IF-IT-CHANGES discipline (2026-08-14) are all
    // documented once, on paging_walk_or_create_level above.
    pt_entry_t *pdpt_page = paging_walk_or_create_level(pml4v, PML4_INDEX(virtual_address),
                                                        tableRequiredFlags, tableSource,
                                                        "PDPT", virtual_address);

    pt_entry_t *pd_page = paging_walk_or_create_level(pdpt_page, PDPT_INDEX(virtual_address),
                                                      tableRequiredFlags, tableSource,
                                                      "PD", virtual_address);

    pt_entry_t *pt_page = paging_walk_or_create_level(pd_page, PD_INDEX(virtual_address),
                                                      tableRequiredFlags, tableSource,
                                                      "PT", virtual_address);

	uint16_t finalFlags =  flags | PAGE_PRESENT;
    printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "\tSetting page table entry at 0x%016lx, index 0x%04x, to 0x%016lx, flags 0x%08x\n", pt_page, PT_INDEX(virtual_address), physical_address, finalFlags);
    // Step 4: Map the final page in the PT table
    pt_page[PT_INDEX(virtual_address)] = physical_address | finalFlags;
}

void paging_unmap_page(pt_entry_t *pml4v, uint64_t virtual_address) {
    if ((uintptr_t)pml4v < kHHDMOffset)
        pml4v = (pt_entry_t *)((uintptr_t)pml4v | kHHDMOffset);

    // Step 1: Traverse the PDPT table
    pt_entry_t *pdpt;
    if (pml4v[PML4_INDEX(virtual_address)] & PAGE_PRESENT) {
        pdpt = (pt_entry_t *)PHYS_TO_VIRT(pml4v[PML4_INDEX(virtual_address)] & ~0xFFF);
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

// See paging.h for the design rationale (lazy HHDM: map-on-alloc /
// unmap-on-free with the boundary-page rule, instead of an eager full
// direct map).
volatile bool kHHDMMaintenanceEnabled = false;

void paging_hhdm_map_range(uintptr_t phys_start, uint64_t length)
{
	if (!kHHDMMaintenanceEnabled || length == 0)
		return;

	// Every page the extent OVERLAPS gets mapped (round the start down, the
	// end up) — a caller handed bytes anywhere in a page must be able to
	// dereference that whole page's HHDM alias. Overlap with a neighbouring
	// extent's boundary page is fine: the mapping is idempotent.
	uintptr_t first_page = phys_start & PAGE_ADDRESS_MASK;
	uintptr_t end_page = (phys_start + length + PAGE_SIZE - 1) & PAGE_ADDRESS_MASK;

	paging_map_pages((pt_entry_t *)kKernelPML4v, first_page | kHHDMOffset, first_page,
	                 (end_page - first_page) / PAGE_SIZE, PAGE_PRESENT | PAGE_WRITE);
}

void paging_hhdm_unmap_range(uintptr_t phys_start, uint64_t length)
{
	if (!kHHDMMaintenanceEnabled || length == 0)
		return;

	// Only pages FULLY CONTAINED in the extent (round the start up, the end
	// down): a partial boundary page may host live neighbouring allocations
	// whose HHDM access must keep working, so it stays mapped.
	uintptr_t first_page = (phys_start + PAGE_SIZE - 1) & PAGE_ADDRESS_MASK;
	uintptr_t end_page = (phys_start + length) & PAGE_ADDRESS_MASK;
	if (end_page <= first_page)
		return;  // extent smaller than a page, or only partial pages — nothing we own outright

	for (uintptr_t page = first_page; page < end_page; page += PAGE_SIZE)
		paging_unmap_page((pt_entry_t *)kKernelPML4v, page | kHHDMOffset);  // invlpg's locally

	// Cross-core shootdown so the other cores' TLBs drop the stale entries
	// too. Fire-and-forget is safe here: HHDM virt<->phys is a fixed 1:1
	// relation, so a stale entry that survives a beat can never produce a
	// WRONG translation (remapping recreates the identical PTE) — it can
	// only let that core miss the use-after-free tripwire for that beat.
	mpSendInvTLB();
}

void paging_map_pages(pt_entry_t* pml4v,uint64_t virtual_address,uint64_t physical_address,uint64_t page_count,uint64_t flags)
{
	if ((uintptr_t)pml4v < kHHDMOffset)
		pml4v = (uintptr_t*)((uintptr_t)pml4v | kHHDMOffset);

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

	// if (page_count > 0x10)
	// {
	// 	temp = kDebugLevel;
	// 	kDebugLevel = 0;
	// }

	for (uint64_t cnt=0;cnt<page_count;cnt++)
		paging_map_page(pml4v, virtual_address + (PAGE_SIZE * cnt), physical_address + (PAGE_SIZE * cnt), flags);
	
	// if (page_count > 0xA1)
	// {
	// 	kDebugLevel = temp;
	// }
}

void paging_unmap_pages(pt_entry_t *pml4v, uint64_t virtual_address, size_t length) {
    // Align the virtual address down to the nearest page boundary
    uint64_t aligned_address = virtual_address & ~(PAGE_SIZE - 1);

    // Adjust the length to account for any extra bytes due to alignment
    size_t end_address = virtual_address + length;
    size_t aligned_length = end_address - aligned_address;
    size_t num_pages = (aligned_length + PAGE_SIZE - 1) / PAGE_SIZE;

    // Unmap each page in the range
    for (size_t i = 0; i < num_pages; i++) {
        paging_unmap_page(pml4v, aligned_address + i * PAGE_SIZE);
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

	// Set CR0.WP (bit 16) so that ring-0 code respects page write-protection bits.
	// Without this bit, kernel code can write to read-only pages and CoW faults
	// never fire — the hardware enforces WP only for ring-3 code by default.
	__asm__ volatile(
		"mov rax, cr0\n\t"
		"or rax, 0x10000\n\t"
		"mov cr0, rax\n\t"
		::: "rax"
	);
}

uintptr_t get_paging_table_page()
{
	// Exhaustion tripwire: this is a bump allocator that never frees. Without
	// this check, running past the pool silently hands out pages the physical
	// allocator ALSO owns — two owners, one page, corruption with no
	// fingerprints. A loud panic here names the culprit instead.
	if (kPagingPagesCurrentPtr >= kPagingPagesBaseAddressP + (kPagingPagesCount * PAGE_SIZE))
		panic("get_paging_table_page: paging page pool exhausted (%lu pages) — grow the pool sizing in init_os64_paging_tables\n", kPagingPagesCount);
	uintptr_t retVal = kPagingPagesCurrentPtr;
	kPagingPagesCurrentPtr += PAGE_SIZE;
	return retVal;
}

// The pool's odometer (probe, 2026-08-04): how many pages has the bump
// allocator handed out? Consumption here is MONOTONE — pages never come
// back — so this number only climbs, and its SLOPE during a workload is the
// diagnostic (the 640-page exhaustion hunt: allocator address-march ate one
// pool page per 2MB of virgin territory toured). Reported once at
// boot-complete and once a minute by the kernel park loop (shutdown.c).
uint64_t paging_pool_pages_used(void)
{
	return (kPagingPagesCurrentPtr - kPagingPagesBaseAddressP) / PAGE_SIZE;
}

// Program PAT entry 7 = write-combining on this core (contract in paging.h;
// the framebuffer's PAGE_WC mapping selects entry 7). IA32_PAT is 0x277:
// eight one-byte entries, one per {PAT,PCD,PWT} index. We touch ONLY byte 7
// — entries 0-6 keep their power-on values, so every existing mapping
// (WB=0, PWT=1, PCD=2, PCD|PWT=3) means exactly what it always meant.
//
// The SDM's full memory-type-change liturgy (cache disable, wbinvd, TLB
// flush, repeat) exists for REmapping pages a core has already cached under
// the old type. Both call sites here run before this core has ever touched
// the WC-tagged pages — the BSP before the kernel tables exist, each AP
// during its own bring-up — so a wbinvd on either side of the write plus
// the CR3 reload every core does moments later is the honest sufficient
// version of the ceremony.
#define IA32_PAT_MSR      0x277
#define PAT_TYPE_WC       0x01ULL

void pat_init_this_core(void)
{
	uint64_t pat = rdmsr64(IA32_PAT_MSR);
	pat &= ~(0xFFULL << 56);              // clear entry 7 (default UC-)
	pat |=  (PAT_TYPE_WC << 56);          // entry 7 = write-combining
	__asm__ volatile("wbinvd" ::: "memory");
	wrmsr64(IA32_PAT_MSR, pat);
	__asm__ volatile("wbinvd" ::: "memory");
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

	// Fund "map every physical page once, at 4KB granularity" — for real this
	// time. The old formula (kMaxPhysicalAddress / PAGE_SIZE, used as a BYTE
	// size) was a unit collision: it yielded one pool page per 16MB of
	// physical space, 8x short of the intent, and the shortfall went unnoticed
	// for as long as nothing toured much address territory. Then the ext2
	// write era arrived (2026-08-04): its scratch churn — 1KB kmalloc/kfree
	// at ~1,100/sec — rode the allocator's next-fit address march (the carve
	// path advances the wilderness block's start on every allocation, and its
	// early array index wins the walk over every stranded hole), touring
	// ~1MB/sec of virgin territory whose lazy-HHDM mappings drew one pool
	// page per 2MB. 640 pages died in 809 seconds, and Chris called the march
	// sight-unseen on a bet against the code. (The march itself is the
	// allocator's to fix — hole-first walk order; this sizing just makes the
	// pool honest regardless.)
	//
	// The math: one PT maps 512 pages (2MB), so PTs = maxphys/2MB; the
	// PD/PDPT/PML4 levels above them are ~1/512th more each — round up
	// generously with a flat slack term that also absorbs task-table churn
	// (task page tables come from this pool too, and are not yet reclaimed
	// at task death). On an 8GB guest this lands ~5,200 pages = ~20MB: real
	// money in 1995, a rounding error today.
	uint64_t poolPages = (kMaxPhysicalAddress / PAGE_SIZE) / 512   // PTs: map-once funded
	                     + (kMaxPhysicalAddress / PAGE_SIZE) / (512 * 512) // PDs
	                     + 64;                                     // PDPTs/PML4s + slack
	// The ramdisk module's retro-map consumes pool pages in proportion to the
	// MODULE's size — one page table per 2MB mapped — and the module isn't
	// counted in kMaxPhysicalAddress's map-once budget, so fund it explicitly.
	if (kRamdiskModuleSize > 0)
		poolPages += (kRamdiskModuleSize / (512 * PAGE_SIZE)) + 8;
	uint64_t allocSize = poolPages * PAGE_SIZE;
	kPagingPagesCount = poolPages;
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
	// WRITE-COMBINING since 2026-08-04 (was PAGE_PCD = full uncached): every
	// store to the glass now batches into bursts instead of paying the
	// uncached toll one write at a time. This is the other half of the
	// shadow-buffer work — the shadow killed the VRAM READS, this kills the
	// blit's store cost. See PAGE_WC in paging.h for the PAT plumbing and
	// the fail-safe (a core without the PAT entry sees UC-, i.e. the old
	// behavior).
	paging_map_pages(pml4v, (uintptr_t)kFrameBuffer.base_address, physAddrLookup, pagesToMap, PAGE_PRESENT | PAGE_WRITE | PAGE_WC);

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

	//Map the kernel's own ELF file (Limine's kernel_file — same recipe as the
	//font and PCI-ID modules: physically contiguous, memmap type KERNEL_AND_
	//MODULES keeps the allocator away, so re-mapping it at Limine's VA keeps
	//kKernelFileAddress valid for the life of the kernel). symbols_init()
	//walks its .symtab for the names behind symbolized kernel reports, and
	//the mapping STAYS afterward on purpose: .debug_line lives in the same
	//blob, and the file:line slice will want it. READ-ONLY — nothing ever
	//has business writing the kernel's own file image.
	if (kKernelFileAddress != 0)
	{
		printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map kernel ELF file (for .symtab)\n");
		physAddrLookup = paging_walk_paging_table((pt_entry_t*)kKernelPML4v, (uintptr_t)kKernelFileAddress);
		pagesToMap = kKernelFileSize / PAGE_SIZE;
		if (kKernelFileSize % PAGE_SIZE)
			pagesToMap++;
		printd(DEBUG_PAGING | DEBUG_DETAILED,"\tPAGING: Mapping virtual kernel file (0x%016lx) to physical kernel file (0x%016lx), %u pages in new page tables\n", kKernelFileAddress, physAddrLookup, pagesToMap);
		paging_map_pages(pml4v, (uintptr_t)kKernelFileAddress, physAddrLookup, pagesToMap, PAGE_PRESENT);
	}

	//Map the RAMDisk module if the boot entry passed one (same recipe as the
	//font and PCI-ID modules above: Limine loaded it physically contiguous
	//and handed us its HHDM VA; its memmap type — KERNEL_AND_MODULES — keeps
	//the allocator away from it, so re-mapping it at the same VA keeps
	//kRamdiskModuleAddress valid for the life of the kernel).
	if (kRamdiskModuleAddress != NULL)
	{
		printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Map RAMDisk module\n");
		physAddrLookup = paging_walk_paging_table((pt_entry_t*)kKernelPML4v, (uintptr_t)kRamdiskModuleAddress);
		pagesToMap = kRamdiskModuleSize / PAGE_SIZE;
		if (kRamdiskModuleSize % PAGE_SIZE)
			pagesToMap++;
		printd(DEBUG_PAGING | DEBUG_DETAILED,"\tPAGING: Mapping virtual RAMDisk module (0x%016lx) to physical RAMDisk module (0x%016lx), %u pages in new page tables\n", kRamdiskModuleAddress, physAddrLookup, pagesToMap);
		paging_map_pages(pml4v, (uintptr_t)kRamdiskModuleAddress, physAddrLookup, pagesToMap, PAGE_PRESENT | PAGE_WRITE);
	}

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

	// Retro-map pass for lazy HHDM maintenance (see paging.h): everything the
	// allocator handed out BEFORE these tables existed (the paging page pool,
	// kMemoryStatus itself, anything else early boot grabbed) was reached
	// through Limine's full-HHDM tables until now. Walk the allocator's
	// ledger and HHDM-map every currently-allocated extent into the new
	// tables, so the "allocated <=> HHDM-mapped" invariant holds from the
	// moment we switch CR3. Free extents deliberately stay unmapped — that's
	// the tripwire. From here on, allocate/free maintain this incrementally.
	printd(DEBUG_PAGING | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "* PAGING: Retro-map allocated extents at HHDM\n");
	for (uint64_t cnt = 0; cnt < kMemoryStatusCurrentPtr; cnt++)
	{
		memory_status_t *entry = &kMemoryStatus[cnt];
		if (!entry->in_use || entry->length == 0 || entry->startAddress == 0)
			continue;
		uintptr_t first_page = entry->startAddress & PAGE_ADDRESS_MASK;
		uintptr_t end_page = (entry->startAddress + entry->length + PAGE_SIZE - 1) & PAGE_ADDRESS_MASK;
		printd(DEBUG_PAGING | DEBUG_DETAILED,"\tPAGING: HHDM retro-map 0x%016lx, %u pages\n", first_page, (end_page - first_page) / PAGE_SIZE);
		paging_map_pages(pml4v, first_page | kHHDMOffset, first_page,
		                 (end_page - first_page) / PAGE_SIZE, PAGE_PRESENT | PAGE_WRITE);
	}

	kKernelPML4 = (uintptr_t)pml4p;
	kKernelPML4v = (uintptr_t)pml4v;

	asm volatile ("cli\nmov cr3, %0\nsti" :: "r"(kKernelPML4) : "memory");

	// Limine's full-HHDM tables are gone; from now on the allocator maintains
	// HHDM mappings itself (paging_hhdm_map_range/paging_hhdm_unmap_range).
	kHHDMMaintenanceEnabled = true;
}
