#ifndef KMALLOC_H
#define KMALLOC_H

#include <stdint.h>
#include <stddef.h>

void *kmalloc_aligned(uint64_t length);
void *kmalloc(uint64_t length);
#endif