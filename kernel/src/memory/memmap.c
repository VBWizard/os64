#include "limine.h"
#include "paging.h"
#include "video.h"

uint64_t kTotalMemory = 0, kAvailableMemory = 0;
uint64_t kMemMapEntryCount;
limine_memmap_entry_t** kMemMap;
uint64_t kKernelExecutableStartAddress=0;
uint64_t kKernelExecutablePageCount=0;
void calculateAvailableMemory()
{
	for (uint64_t i = 0; i < kMemMapEntryCount; i++)
	{
		kTotalMemory += kMemMap[i]->length;
		if (kMemMap[i]->type == LIMINE_MEMMAP_USABLE ||kMemMap[i]->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
		{
			kAvailableMemory += kMemMap[i]->length;
			printf("\t 0x%x for 0x%x bytes (type 0x%x)\n", kMemMap[i]->base, kMemMap[i]->length, kMemMap[i]->type);
		}
		if (kMemMap[i]->type == LIMINE_MEMMAP_KERNEL_AND_MODULES)
		{
			kKernelExecutableStartAddress = kMemMap[i]->base;
			kKernelExecutablePageCount = kMemMap[i]->length % PAGE_SIZE;
		}
	}
}

uint64_t getLowestAvailableMemoryAddress(uint64_t startAddress)
{

	for (uint64_t i = 0; i < kMemMapEntryCount; i++)
	{
		if ((kMemMap[i]->type == LIMINE_MEMMAP_USABLE ||kMemMap[i]->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) && kMemMap[i]->base >= startAddress)
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