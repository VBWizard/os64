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
#define PAGE_GLOBAL       (1ULL << 8)    // Global page
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
void paging_map_page(pt_entry_t *pml4, uint64_t virtual_address, uint64_t physical_address, uint64_t flags);

void paging_map_pages(
	pt_entry_t* pml4,
	uint64_t virtual_address,
	uint64_t physical_address,
	uint64_t page_count,
	uint64_t flags);

void paging_unmap_page(pt_entry_t *pml4, uint64_t virtual_address);
void paging_unmap_pages(pt_entry_t *pml4, uint64_t virtual_address, size_t length);
uintptr_t paging_walk_paging_table_keep_flags(pt_entry_t* pml4, uint64_t virtual_address, bool keepPageFlags);
uintptr_t paging_walk_paging_table(pt_entry_t* pml4, uint64_t virtual_address);
void validatePagingHierarchy(uintptr_t address);
void init_os64_paging_tables();
void paging_map_kernel_into_pml4(uintptr_t* pml4v);
uintptr_t get_paging_table_page();
uintptr_t get_paging_table_pageV();

#endif // PAGING_H