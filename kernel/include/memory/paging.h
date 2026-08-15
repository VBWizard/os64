#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include <stddef.h>
#include "CONFIG.h"
#include "memmap.h"

#define PML4_SHIFT 39
#define PDPT_SHIFT 30
#define PD_SHIFT   21
#define PT_SHIFT   12

#define KERNEL_PAGE_COUNT 0x1000

// Page table entry flags
#define PAGE_PRESENT      (1ULL << 0)    // Page is present
#define PAGE_WRITE        (1ULL << 1)    // Writable
#define PAGE_USER         (1ULL << 2)    // User-accessible
#define PAGE_PWT          (1ULL << 3)    // Write-through caching
#define PAGE_PCD          (1ULL << 4)    // Cache disable
#define PAGE_ACCESSED     (1ULL << 5)    // Accessed
#define PAGE_DIRTY        (1ULL << 6)    // Dirty
#define PAGE_PAT_4K       (1ULL << 7)    // PAT high bit (4KB PTEs; 2MB pages use bit 12)
#define PAGE_GLOBAL       (1ULL << 8)    // Global page

// WRITE-COMBINING page type (2026-08-04, the console-speed slice): the PTE
// bits {PAT, PCD, PWT} select PAT entry 7, which pat_init_this_core()
// programs to WC on every core. The framebuffer is the customer: WC batches
// stores into burst writes — the memory type invented for framebuffers —
// where the old PCD (uncached) mapping paid full price per store (~30MB/s,
// the reason a 3MB scroll blit capped the console at ~10 lines/sec).
// DELIBERATE FAIL-SAFE: PAT entry 7's power-on default is UC-, so a core
// that somehow missed the MSR write sees exactly the old uncached behavior
// — the worst case of this feature is its own absence.
#define PAGE_WC           (PAGE_PAT_4K | PAGE_PCD | PAGE_PWT)
#define PAGE_NO_EXECUTE   (1ULL << 63)   // No-execute

#define PAGE_FLAGS_MASK 0xFFFUL
#define PAGE_ADDRESS_MASK  (~PAGE_FLAGS_MASK)

// Initial paging table entry locations
#define PDPT_ADDRESS 0x2000
#define PD_ADDRESS   0x3000
#define KERNEL_PT_PAGES 32
#define PT_ADDRESS   0x4000
#define PT END PT_ADDRESS + (0x1000 * KERNEL_PT_PAGES) //Enough room to map 64MB of memory for the kernel
#define RELOAD_CR3 __asm__ __volatile__("mov rax, cr3\n\t"          \
                                        "mov cr3, rax\n\t"          \
                                        ::: "rax");
#define PHYS_TO_VIRT(addr) ((pt_entry_t)(addr) + kHHDMOffset)
#define VIRT_TO_PHYS(addr) ((pt_entry_t)(addr) & 0x00000FFFFFFFFFFF)
typedef uint64_t pt_entry_t;
typedef struct {
	pt_entry_t entries[512];
} page_table_t;

extern pt_entry_t kKernelPML4;
extern pt_entry_t kKernelPML4v;
extern uint64_t kHHDMOffset;
//Pointer to the beginning of the pool of identity mapped pages that are allocated and mapped, to be used for page tables
extern uintptr_t kPagingPagesBaseAddressV;
extern uintptr_t kPagingPagesBaseAddressP;
extern uintptr_t kKernelPageMappings[KERNEL_PAGE_COUNT][2];
extern int kKernelPageMappingsCount;


void paging_init(/*uint64_t kernel_physical, uint64_t kernel_virtual*/);
void paging_map_page(pt_entry_t *pml4v, uint64_t virtual_address, uint64_t physical_address, uint64_t flags);

void paging_map_pages(
	pt_entry_t* pml4v,
	uint64_t virtual_address,
	uint64_t physical_address,
	uint64_t page_count,
	uint64_t flags);

void paging_unmap_page(pt_entry_t *pml4v, uint64_t virtual_address);
void paging_unmap_pages(pt_entry_t *pml4v, uint64_t virtual_address, size_t length);

// ---- Lazy HHDM maintenance (map-on-alloc / unmap-on-free) ----
// Physical memory is HHDM-mapped in the kernel page tables ONLY while the
// allocator considers it allocated. The allocator calls these two functions
// from its single alloc/free choke points; nothing else should. The payoff:
// `phys | kHHDMOffset` is guaranteed dereferenceable for ANY allocator-owned
// extent (no more "works on QEMU because the memory map happens to line up"),
// and touching freed or never-allocated RAM through the HHDM faults — a
// deliberate use-after-free/wild-pointer tripwire, in the spirit of Linux's
// DEBUG_PAGEALLOC, chosen over an eager Linux-style full direct map.
//
// False until init_os64_paging_tables() has built the real kernel tables and
// switched CR3 — before that, Limine's own full-HHDM tables are live and
// allocations made that early are retro-mapped during the table build.
extern volatile bool kHHDMMaintenanceEnabled;

/// @brief Map every page overlapping physical [phys_start, phys_start+length)
/// at its HHDM address in the kernel page tables. Idempotent (HHDM virt<->phys
/// is a fixed 1:1 relation, so remapping writes an identical PTE). No-op until
/// kHHDMMaintenanceEnabled.
void paging_hhdm_map_range(uintptr_t phys_start, uint64_t length);

/// @brief Unmap the HHDM mapping of every page FULLY CONTAINED in physical
/// [phys_start, phys_start+length). Boundary partial pages are deliberately
/// left mapped — unaligned (8-byte-granularity) kernel allocations can share
/// a page with live neighbouring extents, and interior pages are the only
/// ones this extent provably owns outright. Broadcasts a TLB-shootdown IPI
/// to the other cores when anything was actually unmapped.
void paging_hhdm_unmap_range(uintptr_t phys_start, uint64_t length);
uintptr_t paging_walk_paging_table_keep_flags(pt_entry_t* pml4, uint64_t virtual_address, bool keepPageFlags);

// The four-level post-mortem for one VA, and the leaf-table address a healthy
// caller records so a later failure can be reported as was/is. Both print or
// answer for whatever pml4 they are handed. See the essay in paging.c.
void paging_report_walk(pt_entry_t *pml4v, uint64_t va, const char *what);
uintptr_t paging_leaf_table_phys(pt_entry_t *pml4v, uint64_t va);
// The address OF the page table entry that maps `va` — what you aim a
// hardware watchpoint at to catch whoever rewrites a mapping.
uintptr_t paging_pte_address(pt_entry_t *pml4v, uint64_t va);
// The addresses of ALL FOUR entries that map `va` (PML4E, PDPTE, PDE, PTE):
// watch the whole chain, because the attack may land at any level.
int paging_walk_entry_addresses(pt_entry_t *pml4v, uint64_t va, uintptr_t out[4]);

// Mapping sentinels: register a kernel mapping that must never change, then
// check it at the phase boundaries of anything under suspicion. The first
// checkpoint to see the damage names the phase that caused it. See paging.c.
void paging_sentinel_add(uintptr_t va, const char *name);
void paging_sentinel_check(const char *where);
uintptr_t paging_walk_paging_table(pt_entry_t* pml4, uint64_t virtual_address);
void validatePagingHierarchy(uintptr_t address);
void init_os64_paging_tables();
void paging_map_kernel_into_pml4(uintptr_t* pml4v);
uintptr_t get_paging_table_page();
uintptr_t get_paging_table_pageV();
// The pool's odometer: pages the (never-refunding) bump pool has handed out.
// Monotone by construction; the SLOPE under a workload is the diagnostic.
uint64_t paging_pool_pages_used(void);
extern uint64_t kPagingPagesCount;

// ── Who funds a table page? (the paging-arena seam, 2026-08-12) ────────────
//
// Task page tables no longer come from the pool — they come from the owning
// task's tableArena and die with it (PAGING_ARENA.md is the charter). The
// paging layer doesn't know tasks, so it ASKS: given the pml4 a map call is
// building under, whose arena (if any) should fund new PDPT/PD/PT pages?
//
// IMPLEMENTED IN task.c — the task layer owns the pml4→arena knowledge:
//   kernel PML4            → NULL (the pool; kernel tables are eternal)
//   the CURRENT task's     → its tableArena (demand faults, mmap, threads)
//   a child being BUILT by
//   the current task       → the child's arena (task_create's bracket)
//   anything else          → NULL (pool — a safe leak, never a corruption)
//
// Called once per paging_map_page with the NORMALIZED (HHDM) pml4v.
struct arena;
struct arena *paging_table_arena_for(pt_entry_t *pml4v);

// Program PAT entry 7 = write-combining on THE CALLING CORE (IA32_PAT is
// per-core and the SDM wants all cores uniform). BSP calls it before the
// kernel page tables are built; each AP calls it during its own bring-up,
// before it can ever touch the WC-tagged framebuffer. See PAGE_WC above.
void pat_init_this_core(void);

#endif // PAGING_H
