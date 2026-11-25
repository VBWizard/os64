#ifndef OS64_ABI_STORAGE_H
#define OS64_ABI_STORAGE_H

#include <stdint.h>

typedef struct {
    uint64_t boot[2];
    uint64_t acquisition;
    uint64_t task;
} os64_porter_owner_t;

#define OS64_PORTER_ACQUIRE 0
#define OS64_PORTER_RELEASE 1
#define OS64_PORTER_BUSY 1
#define OS64_PORTER_UNSEEDED 2

#endif
