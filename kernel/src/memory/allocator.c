#include "CONFIG.h"
#include "allocator.h"
#include "memmap.h"
#include "paging.h"
#include "memset.h"

memory_status_t *kMemoryStatus;
//Points to the next available kernel status
uint64_t kMemoryStatusCurrentPtr = 0;
uintptr_t memoryBaseAddress;

//NOTE: Will return the passed address if it is already page aligned
static inline uintptr_t round_up_to_nearest_page(uintptr_t addr) {
    return (addr + 0xFFF) & ~0xFFF;
}

//Identify whether any statuses allocate on the page passed.
bool physical_page_is_allocated_on(uintptr_t physical_page)
{
	for (uint64_t cnt=0;cnt<kMemoryStatusCurrentPtr;cnt++)
		if ((kMemoryStatus[cnt].startAddress & 0xFFFFFFFFFFFFF000) == physical_page && kMemoryStatus[cnt].in_use == true)
			return true;
	return false;
}

/// @brief Get the address of the first block of memory found that is greater than or equal to the length passed
/// @param requestedLength 
/// @param aligned 
/// @return 
memory_status_t* get_status_entry_for_first_available_address(uint64_t requested_length, bool page_aligned)
{
	for (uint64_t cnt = 0; cnt < kMemoryStatusCurrentPtr; cnt++)
	{
		if (kMemoryStatus[cnt].in_use == false && 
		//Either the requested block doesn't need to be aligned and the current status' size is big enough
			( 
				(page_aligned == false && kMemoryStatus[cnt].length >= requested_length)
				||  ( round_up_to_nearest_page(requested_length) < kMemoryStatus[cnt].length)
			)
		)
		{
			return &kMemoryStatus[cnt];
		}
	}
	return NULL;
}

memory_status_t* get_status_entry_for_requested_address(uint64_t address,uint64_t requested_length, bool in_use)
{
	for (uint64_t cnt = 0; cnt < kMemoryStatusCurrentPtr; cnt++)
	{
		if ( (kMemoryStatus[cnt].startAddress <= address && kMemoryStatus[cnt].startAddress + kMemoryStatus[cnt].length >= address) &&
			kMemoryStatus[cnt].in_use == in_use &&
			kMemoryStatus[cnt].length >= requested_length
		)
			return &kMemoryStatus[cnt];
	}
	return NULL;
}

uint64_t allocate_memory_at_address_internal(uint64_t address, uint64_t requested_length, bool use_address, bool page_aligned)
{
	memory_status_t* memaddr;
	uint64_t retVal = 0;

	//Find the appropriate memory status page
	if (!use_address)
	{
		memaddr = get_status_entry_for_first_available_address(requested_length, page_aligned);
		if (memaddr == NULL)
			__asm__("cli\nhlt\n");
	}
	else
	{
		memaddr = get_status_entry_for_requested_address(address, page_aligned, false);
		if ( memaddr == NULL)
			__asm__("cli\nhlt\n");
	}
	if (memaddr->length < requested_length)
		retVal = 0;
	else if (memaddr->length == requested_length)
	{
		memaddr->in_use = true;
		retVal = memaddr->startAddress;
	}
	else //memory available is > requested memory
	{
		//Create a new memory_status for the memory we're assigning
		//Update memaddr to remove the memory we're assigning
		kMemoryStatus[kMemoryStatusCurrentPtr].startAddress = memaddr->startAddress;
		kMemoryStatus[kMemoryStatusCurrentPtr].in_use = true;

		if (page_aligned)
		{
			kMemoryStatus[kMemoryStatusCurrentPtr].length = round_up_to_nearest_page(requested_length);
			memaddr->startAddress += round_up_to_nearest_page(requested_length);
			memaddr->length -= round_up_to_nearest_page(requested_length);
			retVal = round_up_to_nearest_page(kMemoryStatus[kMemoryStatusCurrentPtr].startAddress);
		}
		else
		{
			kMemoryStatus[kMemoryStatusCurrentPtr].length = requested_length;
			memaddr->startAddress += requested_length;
			memaddr->length -= requested_length;
			retVal = kMemoryStatus[kMemoryStatusCurrentPtr].startAddress;
		}
		kMemoryStatusCurrentPtr++;
	}
	return retVal;
}

/// @brief Allocate memory, possibly at a specific address (not block aligned unless you pass a block aligned address)
/// @param address - The address of the requested memory range.  Pass 0 if no specific address is requested
/// @param requestedLength - The length of the requested memory range.  If 0 method returns 0
/// @param aligned - Should the starting address be aligned to a block based on system block size.
/// @return 
uint64_t allocate_memory_at_address(uint64_t address, uint64_t requested_length, bool use_address)
{
	return allocate_memory_at_address_internal(address, requested_length, use_address, false);
}

uint64_t allocate_memory_aligned(uint64_t requested_length)
{
	return allocate_memory_at_address_internal(0, requested_length, false, true);
}

//NOTE: Only the kernel can request unaligned memory.  User space allocations MUST be on a page boundry and be the full page
uint64_t allocate_memory(uint64_t requested_length)
{
	return allocate_memory_at_address_internal(0, requested_length, false, false);
}

//TODO: Coalesce adjacent memory blocks back together
int free_memory(uint64_t address)
{
	memory_status_t *status_entry = get_status_entry_for_requested_address(address, 0, true);
	if (status_entry != NULL)
	{
		status_entry->in_use = false;
		//Memory should still be mapped so we can clear it out safely
		memset((void*)(status_entry->startAddress + kHHDMOffset), 0, status_entry->length);
	}
	return status_entry->length;
}

void allocator_init()
{

	//uint64_t allocate_size = sizeof(memory_status_t) * INITIAL_MEMORY_STATUS_COUNT;
	//uint64_t page_count = round_up_to_nearest_page(allocate_size) / PAGE_SIZE;
	
	//Get the lowest available address above or equal to 0x1000 (don't include the zero page)
	//NOTE: First pages went to paging structures
	memoryBaseAddress = getLowestAvailableMemoryAddress(0x1000) + (9 * PAGE_SIZE);

	//Update the kernel page tables for the memory used by kMemoryStatus
	//paging_map_pages((pt_entry_t*)kKernelPML4v, memoryBaseAddress, memoryBaseAddress, page_count, PAGE_PRESENT | PAGE_WRITE);

	//Create an allocator entry for kMemoryStatus which is MAX_MEMORY_STATUS_COUNT entries long
	//kMemoryStatus = (memory_status_t*)allocate_memory(allocate_size);
	kMemoryStatus = (memory_status_t*)(0x9000 + kHHDMOffset);

	//Parse the memory map into the newly created kMemoryStatus
	for (uint64_t cnt=0;cnt<kMemMapEntryCount;cnt++)
	{
		kMemoryStatus[cnt].startAddress = kMemMap[cnt]->base;
		kMemoryStatus[cnt].length = kMemMap[cnt]->length;
		kMemoryStatus[cnt].in_use = kMemMap[cnt]->type != LIMINE_MEMMAP_USABLE && kMemMap[cnt]->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE;  //If the type isn't 0/5 then the area is in use
		if (cnt==0)
		{
			kMemoryStatus[cnt].startAddress = memoryBaseAddress;
			kMemoryStatus[cnt].length = kMemoryStatus[cnt].length - (9 * PAGE_SIZE);
		}
		kMemoryStatusCurrentPtr++;
	}

}