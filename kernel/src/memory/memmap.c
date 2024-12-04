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
	printd(DEBUG_MEMMAP,"MEMMAP: Parsing memory map ... \n");
	for (uint64_t entry = 0; entry < kMemMapEntryCount; entry++)
	{
		kTotalMemory += kMemMap[entry]->length;
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
			kKernelExecutablePageCount = kMemMap[entry]->length % PAGE_SIZE;
		}
		if (entry == kMemMapEntryCount - 1)
			kMaxPhysicalAddress = kMemMap[entry]->base + kMemMap[entry]->length;
	}
	printd(DEBUG_MEMMAP, "MEMMAP: Parsing done\n");
	printd(DEBUG_MEMMAP, "MEMMAP: Usable memory: %Lu\n", kAvailableMemory);
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