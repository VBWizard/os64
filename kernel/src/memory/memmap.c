#include "limine.h"
#include "paging.h"
#include "video.h"
#include "serial_logging.h"
#include "memmap.h"
#include "strcpy.h"

uint64_t kTotalMemory = 0, kAvailableMemory = 0;
uint64_t kMemMapEntryCount;
limine_memmap_entry_t** kMemMap;
uint64_t kKernelExecutableStartAddress=0;
uint64_t kKernelExecutablePageCount=0;
uint64_t kMaxPhysicalAddress = 0;
limine_memmap_entry_t* kRemapMemoryEntries[50];
int kRemapMemoryEntryCount=0;

char* determine_memory_type(uint64_t memTypeID, char *memoryType)
{
	switch (memTypeID)
	{
		case 0:
			strcpy(memoryType,"usable");
			break;
		case 1:
			strcpy(memoryType,"reserved");
			break;
		case 2:
			strcpy(memoryType,"ACPI reclaimable");
			break;
		case 3:
			strcpy(memoryType,"ACPI NVS");
			break;
		case 4:
			strcpy(memoryType,"bad memory");
			break;
		case 5:
			strcpy(memoryType,"bootloader reclaimable");
			break;
		case 6:
			strcpy(memoryType,"kernel and modules");
			break;
		case 7:
			strcpy(memoryType,"framebuffer");
			break;
		default:
			strcpy(memoryType,"unknown");
			break;
	}
	return memoryType;
}

void calculateAvailableMemory()
{
	char memType[100];
	kMaxPhysicalAddress = 0;
	kAvailableMemory = 0;
	kTotalMemory = 0;
	printd(DEBUG_MEMMAP,"MEMMAP: Parsing memory map ... \n");
	for (uint64_t entry = 0; entry < kMemMapEntryCount; entry++)
	{
		// kTotalMemory = installed physical RAM (what SYSCALL_MEMORY reports
		// as .total). Only DRAM-backed entry types count: RESERVED is mostly
		// MMIO address space (ECAM, PCI hole, LAPIC), FRAMEBUFFER is a device
		// BAR, BAD is broken silicon — none of them are RAM you paid for.
		// Summing every entry told an 8GB QEMU guest it had 20GB "total";
		// free(1) caught it on its first run (2026-07-27).
		switch (kMemMap[entry]->type)
		{
			case LIMINE_MEMMAP_USABLE:
			case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
			case LIMINE_MEMMAP_ACPI_NVS:
			case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
			case LIMINE_MEMMAP_KERNEL_AND_MODULES:
				kTotalMemory += kMemMap[entry]->length;
				break;
			default:
				break;
		}
		//CLR 11/24/2024 - Removed claiming LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE as usable memory
		if (kMemMap[entry]->type == LIMINE_MEMMAP_USABLE)
		{
			kAvailableMemory += kMemMap[entry]->length;
			printd(DEBUG_MEMMAP,"\t %u: 0x%016Lx for 0x%016Lx bytes (type %u - usable)\n", entry, kMemMap[entry]->base, kMemMap[entry]->length, kMemMap[entry]->type);
		}
		else
		{
			printd(DEBUG_MEMMAP,"\t %u: 0x%016Lx for 0x%016Lx bytes (type %u - %s)\n", 
				entry, 
				kMemMap[entry]->base, 
				kMemMap[entry]->length, 
				kMemMap[entry]->type,
				determine_memory_type(kMemMap[entry]->type, memType)
				);
		}
		if (kMemMap[entry]->type == LIMINE_MEMMAP_KERNEL_AND_MODULES || kMemMap[entry]->type == LIMINE_MEMMAP_ACPI_NVS || kMemMap[entry]->type == LIMINE_MEMMAP_FRAMEBUFFER)
			kRemapMemoryEntries[kRemapMemoryEntryCount++]=kMemMap[entry];
		if (kMemMap[entry]->type == LIMINE_MEMMAP_KERNEL_AND_MODULES)
		{
			kKernelExecutableStartAddress = kMemMap[entry]->base;
			// Was `% PAGE_SIZE` — a modulo where a divide belongs, so this
			// held the REMAINDER bytes (0 for an aligned kernel), not the page
			// count. No consumers yet, which is the only reason it never bit.
			kKernelExecutablePageCount = kMemMap[entry]->length / PAGE_SIZE;
		}
		if (kMemMap[entry]->type == LIMINE_MEMMAP_USABLE)
			kMaxPhysicalAddress = kMemMap[entry]->base + kMemMap[entry]->length;
	}
	printd(DEBUG_MEMMAP, "MEMMAP: Parsing done\n");
	printd(DEBUG_MEMMAP, "MEMMAP: Usable memory: %Lu\n", kAvailableMemory);
	printd(DEBUG_MEMMAP, "MEMMAP: Calculated max physical address: 0x%016lx\n",kMaxPhysicalAddress);
}

uint64_t getLowestAvailableMemoryAddress(uint64_t startAddress)
{

	for (uint64_t i = 0; i < kMemMapEntryCount; i++)
	{
		//CLR 11/24/2024 - Removed claiming LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE as usable memory
		if ((kMemMap[i]->type == LIMINE_MEMMAP_USABLE) && kMemMap[i]->base >= startAddress)
			return kMemMap[i]->base;
	}
memmap_broken_loop:
	goto memmap_broken_loop;	
}

//uint64_t getHighestNotAvailableMemoryAddress()

void memmap_init(limine_memmap_entry_t **entries, uint64_t entryCount)
{
	kMemMap = entries;
	kMemMapEntryCount = entryCount;
	calculateAvailableMemory();
}