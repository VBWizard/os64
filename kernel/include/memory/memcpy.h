#ifndef MEMCPY_H
#define MEMCPY_H

#include <stdint.h>
#include <stddef.h>
#include "CONFIG.h"

void *memcpy(void *dest1, const void *src1, size_t len);
void memmove(void *dest, const void *src, size_t n);

#endif
