/* resample.c, built OUTSIDE_SPEEX, asks <stdlib.h> for three names. They
 * are libos64's allocator. */
#ifndef OS64_SOUND_STDLIB_H
#define OS64_SOUND_STDLIB_H

#include <stddef.h>

void *os64_calloc(size_t count, size_t size);
void *os64_realloc(void *ptr, size_t size);
void  os64_free(void *ptr);

#define calloc  os64_calloc
#define realloc os64_realloc
#define free    os64_free

#endif
