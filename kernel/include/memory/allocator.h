#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define RESERVED_PAGES 9

typedef struct memory_status_s
{
	uint64_t startAddress;
	uint64_t length;
	bool in_use;

} memory_status_t;

extern uint64_t kMemoryStatusCurrentPtr;
extern memory_status_t *kMemoryStatus;

bool physical_page_is_allocated_on(uintptr_t physical_page_start);
uint64_t allocate_memory_at_address(uint64_t address, uint64_t requested_length, bool use_address);
uint64_t allocate_memory_aligned(uint64_t requested_length);
uint64_t allocate_memory(uint64_t requested_length);
bool merge_freed_block(uint64_t freedIndex);
void compact_memory_array();
uint64_t free_memory(uint64_t address);
void allocator_init();

#endif