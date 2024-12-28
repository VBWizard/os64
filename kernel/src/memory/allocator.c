#include "CONFIG.h"
#include "allocator.h"
#include "memmap.h"
#include "paging.h"
#include "memset.h"
#include "serial_logging.h"
#include "panic.h"
#include "memcpy.h"

memory_status_t *kMemoryStatus;
//Points to the next available kernel status - increment AFTER use
uint64_t kMemoryStatusCurrentPtr = 0;
uintptr_t memoryBaseAddress;

//NOTE: Will return the passed address if it is already page aligned
static inline uintptr_t round_up_to_nearest_page(uintptr_t addr) {
    return (addr + 0xFFF) & ~0xFFF;
}

void compact_memory_array() {
    size_t writeIndex = 0; // Where the next valid entry will be written
	printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "allocator: Compacting memory status array\n");

    for (size_t i = 0; i < kMemoryStatusCurrentPtr; i++) 
	{
		//If the current entry is in use
        if (kMemoryStatus[i].length > 0) 
		{
            //And the "write to" index isn't the same as the current entry
            if (i != writeIndex) 
			{
			// Copy the valid entry to the "write to" index
                kMemoryStatus[writeIndex] = kMemoryStatus[i];
            }
			//Increment the "write to" index regardless
            writeIndex++;
        }
    }

	printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "\tallocator: Clearing out compacted entries\n");
    // Clear remaining entries after the last valid index
    for (size_t i = writeIndex; i < kMemoryStatusCurrentPtr; i++) {
        kMemoryStatus[i].startAddress = 0;
        kMemoryStatus[i].length = 0;
        kMemoryStatus[i].in_use = false;
    }
	kMemoryStatusCurrentPtr=writeIndex;
}

bool merge_freed_block(uint64_t freedIndex) {
    memory_status_t *freedBlock = &kMemoryStatus[freedIndex];

    // Scan for a parent block to merge into
	memory_status_t* ours=&kMemoryStatus[freedIndex];
    printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "allocator: Looking for an entry to merge ours at index %u, address 0x%016lx, with\n", freedIndex, ours->startAddress);
	for (size_t idx = 0; idx < kMemoryStatusCurrentPtr; idx++) {
        if (idx == freedIndex) continue; // Skip the block being freed

        memory_status_t *candidate = &kMemoryStatus[idx];
		if (candidate->startAddress == 0x0) continue;
        // Check if the candidate is free and contiguous
        if (!candidate->in_use && candidate->length > 0) {
            // Merge freed block into candidate (preceding)
            if (candidate->startAddress + candidate->length == freedBlock->startAddress) {
				printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "\tallocator: found a p candidate with start=0x%016lx, length=0x%016lx, in_use=%u\n", 
						candidate->startAddress, candidate->length, candidate->in_use);
                candidate->length += freedBlock->length;

                // Invalidate the freed block
                freedBlock->startAddress = 0;
                freedBlock->length = 0;
                freedBlock->in_use = false;
				printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "\tallocator: Merged with candidate: start=0x%016lx, length=0x%016lx, in_use=%u\n", 
						candidate->startAddress, candidate->length, candidate->in_use);
                return true;
            }

            // Merge candidate into freed block (following)
            if (freedBlock->startAddress + freedBlock->length == candidate->startAddress) {
				printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "\tallocator: found an f candidate with start=0x%016lx, length=0x%016lx, in_use=%u\n", 
						candidate->startAddress, candidate->length, candidate->in_use);
                candidate->length += freedBlock->length;
				//The found candidate followed our block to be freed, so its new address becomes our block's
				candidate->startAddress = freedBlock->startAddress;

                // Invalidate the freed block
                freedBlock->startAddress = 0;
                freedBlock->length = 0;
                freedBlock->in_use = false;
				printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "\tallocator: Merged with candidate: start=0x%016lx, length=0x%016lx, in_use=%u\n", 
						candidate->startAddress, candidate->length, candidate->in_use);
				return true;
            }
        }
    }
	printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "\t allocator: Did not find a candidate to merge with\n");
	return false;
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
		//Don't allow page 0 to be allocated!!!
		if (kMemoryStatus[cnt].startAddress > 0 && kMemoryStatus[cnt].in_use == false && 
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

uint64_t get_status_index_for_requested_address(uint64_t address,uint64_t requested_length, bool in_use)
{
	for (uint64_t cnt = 0; cnt < kMemoryStatusCurrentPtr; cnt++)
	{
		if ( (kMemoryStatus[cnt].startAddress <= address && kMemoryStatus[cnt].startAddress + kMemoryStatus[cnt].length > address) &&
			kMemoryStatus[cnt].in_use == in_use &&
			kMemoryStatus[cnt].length >= requested_length
		)
			return cnt;
	}
	panic("get_status_index_for_requested_address: Can't find the index!!! :-(\n");
	return 0;
}

memory_status_t* get_status_entry_for_requested_address(uint64_t address,uint64_t requested_length, bool in_use)
{
	for (uint64_t cnt = 0; cnt < kMemoryStatusCurrentPtr; cnt++)
	{
		if ( (kMemoryStatus[cnt].startAddress <= address && kMemoryStatus[cnt].startAddress + kMemoryStatus[cnt].length > address) &&
			kMemoryStatus[cnt].in_use == in_use &&
			kMemoryStatus[cnt].length >= requested_length
		)
			return &kMemoryStatus[cnt];
	}
	panic("get_status_entry_for_requested_address: Can't find the index!!! :-(\n");
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
	uint64_t aligned_start;
	uint64_t aligned_length = 0;

	if (requested_length >= 200000000)
	{
		int a = 0;
		a+=1;
	}

	//Find the appropriate memory status page
	if (!use_address)
	{
		//When a specific address is NOT requested, internally align request to 8 bytes since our architecture is 64-bit
		// Align to the next multiple of 8
		requested_length = (requested_length + 7) & ~((size_t)7);
		memaddr = get_status_entry_for_first_available_address(requested_length, page_aligned);
		if (memaddr == NULL)
			__asm__("cli\nhlt\n");
	}
	else
	{
		memaddr = get_status_entry_for_requested_address(requested_address, requested_length, false);
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
		uint64_t true_start = memaddr->startAddress;
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

		//Create an entry for the memory being utilized
		//NOTE that even if an aligned address was requested, the new entry will start with the unaligned start address. 
		//The address RETURNED will be the aligned address
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
	printd(DEBUG_ALLOCATOR, "allocate_memory_at_address_internal: Allocated 0x%08x bytes at phys address 0x%08x (%s - %s)\n", aligned_length, use_address?requested_address:aligned_start,
			use_address?"requested address":"",page_aligned?"aligned":"");
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
	printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "allocator: Freeing memory at 0x%016lx\n", address);
	uint64_t statusIdx = get_status_index_for_requested_address(address, 0, true);
	memory_status_t *status_entry = &kMemoryStatus[statusIdx];
	if (status_entry != NULL)
	{
		printd(DEBUG_ALLOCATOR | DEBUG_DETAILED, "allocator: Found block to free, address = 0x%016lx, length=0x%016lx\n", status_entry->startAddress, status_entry->length);
		status_entry->in_use = false;
		//TODO: Fix this.  It isn't working because some addresses are NOT offset by the HHDM
//		//Memory should still be mapped so we can clear it out safely
//		memset((void*)(status_entry->startAddress + kHHDMOffset), 0xFE, status_entry->length);
		return statusIdx;
	}
	panic("ALLOCATOR: Did not find kMemoryStatus entry to mark not in use, address was: 0x%016lx\n",address);
	return 0xFFFFFFFF;
}

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
	kMemoryStatus = (memory_status_t*)(memoryBaseAddress + kHHDMOffset);

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

	//Officially allocate our allocator entries and map them, along with the reverved pages where we created our initial paging pages
	uint64_t size = sizeof(memory_status_t);
	uint64_t allocSize = size*INITIAL_MEMORY_STATUS_COUNT;
	uint64_t newAddress = allocate_memory(allocSize) | kHHDMOffset;
	uint64_t mapSize = allocSize/PAGE_SIZE;
	if (allocSize%PAGE_SIZE)
		mapSize++;
	//paging_map_pages((pt_entry_t*)kKernelPML4v, newAddress, newAddress - kHHDMOffset, mapSize, PAGE_PRESENT | PAGE_WRITE);
	memcpy((void*)newAddress, kMemoryStatus, kMemoryStatusCurrentPtr * size);
	kMemoryStatus = (memory_status_t*)newAddress;
}