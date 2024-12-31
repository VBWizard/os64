#ifndef KMALLOC_H
#define KMALLOC_H

#include <stdint.h>
#include <stddef.h>

#define KMALLOC_CLEAR_FREED_POINTERS

void *kmalloc_aligned(uint64_t length);
void *kmalloc(uint64_t length);
void *kmalloc_dma(uint64_t length);
void *kmalloc_dma32_address(uint32_t address, uint64_t length);
void kfree(void *address);
#endif