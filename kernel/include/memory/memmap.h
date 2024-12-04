#ifndef MEMMAP_H
#define MEMMAP_H
#include "limine.h"

typedef struct limine_memmap_entry limine_memmap_entry_t;

extern limine_memmap_entry_t** kMemMap;
extern uint64_t kMaxPhysicalAddress;
extern uint64_t kMemMapEntryCount;
extern uint64_t kAvailableMemory;
void memmap_init(limine_memmap_entry_t **entries, uint64_t entryCount);
uint64_t getLowestAvailableMemoryAddress(uint64_t startAddress);
#endif