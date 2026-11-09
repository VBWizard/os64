#ifndef MEMSET_H
#define MEMSET_H

#include <stdint.h>
#include <stddef.h>

// Defined in shared/copy_fill.c, the fill libos64 uses too. For memory, not
// device registers, for memcpy.h's reason.
void *memset(void *dst, int c, size_t n);

#endif
