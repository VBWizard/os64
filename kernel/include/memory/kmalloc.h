#ifndef KMALLOC_H
#define KMALLOC_H

#include <stdint.h>
#include <stddef.h>

#define KMALLOC_CLEAR_FREED_POINTERS

void *kmalloc_aligned(uint64_t length);
void *kmalloc(uint64_t length);
// DMA allocation: page-aligned, zeroed, and the caller gets BOTH addresses —
// the HHDM virtual pointer (returned; what the KERNEL reads and writes) and
// the physical address (*phys_out; what the DEVICE is programmed with). The
// two are never the same number again: the identity-mapped era ended
// 2026-08-19 (see kmalloc_dma's comment for the burial notice). phys_out may
// be NULL if a call site genuinely never programs hardware with the address.
// Free with plain kfree(the returned pointer) — it is an ordinary HHDM
// pointer now, which is the point.
void *kmalloc_dma(uint64_t length, uintptr_t *phys_out);
void kfree(void *address);
#endif