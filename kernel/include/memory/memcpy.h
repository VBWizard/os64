#ifndef MEMCPY_H
#define MEMCPY_H

#include <stdint.h>
#include <stddef.h>
#include "CONFIG.h"

// Defined in shared/copy_fill.c, the copy libos64 uses too. For memory, not
// device registers: they move bytes in whatever widths suit the length,
// string instructions among them, and write some bytes more than once, so
// a register is read and written at its own width instead.
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);

#endif
