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

void update_existing_status_entry(memory_status_t* entry, uint64_t address, uint64_t length, bool in_use)
{
	entry->startAddress = address;
	entry->length = length;
	entry->in_use = in_use;
}

memory_status_t* make_new_status_entry(uint64_t address, uint64_t length, bool in_use)
{
	kMemoryStatus[kMemoryStatusCurrentPtr].startAddress = address;
	kMemoryStatus[kMemoryStatusCurrentPtr].length = length;
	kMemoryStatus[kMemoryStatusCurrentPtr].in_use = in_use;
	kMemoryStatusCurrentPtr++;
	return &kMemoryStatus[kMemoryStatusCurrentPtr-1];
}

uint64_t allocate_memory_at_address_internal(uint64_t requested_address, uint64_t requested_length, bool use_address, bool page_aligned)
{
	memory_status_t* memaddr;
	uint64_t retVal = 0;
	uint64_t found_block_original_length = 0;
	uint64_t block_before_length = 0;
	//Find the appropriate memory status page
	if (!use_address)
	{
		memaddr = get_status_entry_for_first_available_address(requested_length, page_aligned);
		if (memaddr == NULL)
			__asm__("cli\nhlt\n");
	}
	else
	{
		memaddr = get_status_entry_for_requested_address(requested_address, page_aligned, false);
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
		found_block_original_length = memaddr->length;
		//Create a new memory_status for the memory we're assigning.
		memory_status_t* block_before_requested;

		//The starting address for the new Status entry, aligned if necessary.  THIS IS ONLY USED AS A RETURN VALUE FROM THIS METHOD
		uint64_t aligned_start;
		uint64_t true_start = memaddr->startAddress;
		uint64_t aligned_length = 0;
		if (page_aligned)
		{
			aligned_start = round_up_to_nearest_page(memaddr->startAddress);
			//Count the extra bytes between the aligned start and the real start, and add the requested length
			aligned_length = aligned_start - memaddr->startAddress + requested_length;
		}
		else
		{
			aligned_start = memaddr->startAddress;
			aligned_length = requested_length;
		}
		uint64_t aligned_end = true_start + aligned_length;

		memory_status_t* new_entry = make_new_status_entry(
							  use_address?requested_address:
							  	memaddr->startAddress, 
							  aligned_length, 
							  true);
		//If a specific address was requested and there was memory before the requested address, make a block from its starting address to the requested address - 1
		if (use_address && memaddr->startAddress != requested_address)
		{
			block_before_length = requested_address - memaddr->startAddress;
			block_before_requested = make_new_status_entry(memaddr->startAddress, requested_address - memaddr->startAddress, false);
		}

		//Fixup the exiting entry to just point to what's left after the allocated memory
		//If a specific address was requested, make the current status point to the address after the requested memory
		if (use_address)
			update_existing_status_entry(memaddr, requested_address + requested_length, found_block_original_length - block_before_length - requested_length, false);
		else
			//Otherwise just update the starting address and length of the existing address to point to after the allocated memory
			update_existing_status_entry(memaddr, 
			                             //The end of the created block which is the new startAddress of the existing block
										 aligned_end,
										 memaddr->length - aligned_length,
										 false
										 );
		retVal = use_address?requested_address:aligned_start;

	}
	int a = 0;
	if (retVal == 0x7b000)
		a += 1;
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
uint64_t free_memory(uint64_t address)
{
	memory_status_t *status_entry = get_status_entry_for_requested_address(address, 0, true);
	if (status_entry != NULL)
	{
		status_entry->in_use = false;
		//Memory should still be mapped so we can clear it out safely
		memset((void*)(status_entry->startAddress + kHHDMOffset), 0, status_entry->length);
		return status_entry->length;
	}
	return 0;
}

#define RESERVED_PAGES 9
void allocator_init()
{

	//uint64_t allocate_size = sizeof(memory_status_t) * INITIAL_MEMORY_STATUS_COUNT;
	//uint64_t page_count = round_up_to_nearest_page(allocate_size) / PAGE_SIZE;
	
	//Get the lowest available address above or equal to 0x1000 (don't include the zero page)
	//NOTE: First pages went to paging structures
	memoryBaseAddress = getLowestAvailableMemoryAddress(0x1000) + (RESERVED_PAGES * PAGE_SIZE);

	//Update the kernel page tables for the memory used by kMemoryStatus
	//paging_map_pages((pt_entry_t*)kKernelPML4v, memoryBaseAddress, memoryBaseAddress, page_count, PAGE_PRESENT | PAGE_WRITE);

	//Create an allocator entry for kMemoryStatus which is MAX_MEMORY_STATUS_COUNT entries long
	//kMemoryStatus = (memory_status_t*)allocate_memory(allocate_size);
	kMemoryStatus = (memory_status_t*)(0x9000 + kHHDMOffset);

	//Parse the memory map into the newly created kMemoryStatus
	for (uint64_t cnt=0;cnt<kMemMapEntryCount;cnt++)
	{
		if (kMemMap[cnt]->type == LIMINE_MEMMAP_USABLE)
		{
			kMemoryStatus[kMemoryStatusCurrentPtr].startAddress = kMemMap[cnt]->base;
			kMemoryStatus[kMemoryStatusCurrentPtr].length = kMemMap[cnt]->length;
			//CLR 11/24/2024 - Removed claiming LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE as usable memory
			kMemoryStatus[kMemoryStatusCurrentPtr].in_use = kMemMap[cnt]->type != LIMINE_MEMMAP_USABLE;  //If the type isn't 0/5 then the area is in use
			kMemoryStatusCurrentPtr++;
		}
	}
		kMemoryStatus[0].startAddress = memoryBaseAddress;
		kMemoryStatus[0].length = kMemoryStatus[0].length - (RESERVED_PAGES * PAGE_SIZE);
}