#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include "CONFIG.h"
#include "memmap.h"

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

// Initial paging table entry locations
#define PDPT_ADDRESS 0x2000
#define PD_ADDRESS   0x3000
#define KERNEL_PT_PAGES 32
#define PT_ADDRESS   0x4000
#define PT END PT_ADDRESS + (0x1000 * KERNEL_PT_PAGES) //Enough room to map 64MB of memory for the kernel

typedef uint64_t pt_entry_t;
typedef struct {
	pt_entry_t entries[512];
} page_table_t;

extern pt_entry_t kKernelPML4;
extern pt_entry_t kKernelPML4v;
extern uint64_t kHHDMOffset;

void paging_init(/*uint64_t kernel_physical, uint64_t kernel_virtual*/);
void paging_map_page(
	pt_entry_t* pml4,
	uint64_t virtual_address,
	uint64_t physical_address,
	uint64_t flags);

void paging_map_pages(
	pt_entry_t* pml4,
	uint64_t virtual_address,
	uint64_t physical_address,
	uint64_t page_count,
	uint64_t flags);

#endif // PAGING_H