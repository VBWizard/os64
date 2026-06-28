#ifndef ENV_H
#define ENV_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "CONFIG.h"

// Flat key/val environment stored in one or more contiguous pages.
//
// Memory layout (per page_count pages):
//
//   [ uint32_t page_count | uint32_t count | uint32_t data_end | char data[] ]
//   ^--- header (ENV_HEADER_SIZE bytes) ---^                    ^--- strings ---^
//
// data[] holds alternating null-terminated strings:
//   key0\0val0\0key1\0val1\0...
// data_end is the byte offset of the next free byte (i.e., data[data_end] is unused).
// The region data[data_end..end-of-last-page] is zeroed and available.
//
// page_count is currently always 1.  Multi-page expansion is reserved for
// when a process needs more than ~4 KB of environment data.
typedef struct {
    uint32_t page_count;   // number of pages allocated (always >= 1)
    uint32_t count;        // number of key/val pairs
    uint32_t data_end;     // offset into data[] of the next free byte
    char     data[];       // packed key\0val\0 pairs
} envpage_t;

#define ENV_HEADER_SIZE       offsetof(envpage_t, data)
#define ENV_DATA_CAPACITY(pg) ((uint32_t)((pg) * PAGE_SIZE - ENV_HEADER_SIZE))

// Allocate and initialise a fresh, empty environment (1 page).
envpage_t  *env_create(void);

// Allocate a copy of parent's environment.  The copy is independent —
// future writes to either env do not affect the other.
// (CoW optimisation — sharing the physical page until first write — is a
// future enhancement; for now we do a full copy on inherit.)
envpage_t  *env_inherit(const envpage_t *parent);

// Look up key.  Returns a pointer into env->data (valid as long as env is
// not modified), or NULL if the key is not present.
const char *env_get(const envpage_t *env, const char *key);

// Set key=val.  If the key already exists its value is replaced (the old
// entry is compacted out and the new pair appended).  Returns false if
// there is no room in the current page allocation.
bool        env_set(envpage_t *env, const char *key, const char *val);

#endif
