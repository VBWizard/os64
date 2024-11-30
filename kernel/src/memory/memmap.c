#include "limine.h"
#include "paging.h"
#include "video.h"
#include "serial_logging.h"

uint64_t kTotalMemory = 0, kAvailableMemory = 0;
uint64_t kMemMapEntryCount;
limine_memmap_entry_t** kMemMap;
uint64_t kKernelExecutableStartAddress=0;
uint64_t kKernelExecutablePageCount=0;

void calculateAvailableMemory()
{
	printd(DEBUG_MEMMAP,"MEMMAP: Parsing memory map ... \n");
	for (uint64_t entry = 0; entry < kMemMapEntryCount; entry++)
	{
		kTotalMemory += kMemMap[entry]->length;
		//CLR 11/24/2024 - Removed claiming LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE as usable memory
		if (kMemMap[entry]->type == LIMINE_MEMMAP_USABLE)
		{
			kAvailableMemory += kMemMap[entry]->length;
			printd(DEBUG_MEMMAP,"\t %u: 0x%016Lx for 0x%016Lx bytes (type %u)\n", entry, kMemMap[entry]->base, kMemMap[entry]->length, kMemMap[entry]->type);
		}
		else
			printd(DEBUG_MEMMAP,"\t %u: 0x%016Lx for 0x%016Lx bytes (type %u - unusable)\n", entry, kMemMap[entry]->base, kMemMap[entry]->length, kMemMap[entry]->type);

		if (kMemMap[entry]->type == LIMINE_MEMMAP_KERNEL_AND_MODULES)
		{
			kKernelExecutableStartAddress = kMemMap[entry]->base;
			kKernelExecutablePageCount = kMemMap[entry]->length % PAGE_SIZE;
		}
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

void memmap_init(limine_memmap_entry_t **entries, uint64_t entryCount)
{
	kMemMap = entries;
	kMemMapEntryCount = entryCount;
	calculateAvailableMemory();
}