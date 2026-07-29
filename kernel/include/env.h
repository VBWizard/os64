#ifndef ENV_H
#define ENV_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "CONFIG.h"

// Flat key/val environment stored in one or more contiguous pages.
//
// THE LAYOUT IS ABI (since os64_getenv landed in libos64): the struct lives
// in abi/include/os64/env.h, shared with userland exactly like the syscall
// numbers — the kernel maps this block read-only into every task and hands
// it to main() as the third argument, so both sides must walk the same
// bytes. envpage_t is the kernel's traditional name for it; keep using it.
//
// data[] holds alternating null-terminated strings:
//   key0\0val0\0key1\0val1\0...
// data_end is the byte offset of the next free byte (i.e., data[data_end] is unused).
// The region data[data_end..end-of-last-page] is zeroed and available.
//
// page_count is currently always 1.  Multi-page expansion is reserved for
// when a process needs more than ~4 KB of environment data.
#include "os64/env.h"
typedef os64_env_block_t envpage_t;

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

// Remove key (compacting the block).  Returns true whether or not the key
// existed — unsetting the absent is success, not error (idempotent, the
// way every shell's `unset` has behaved since Bourne).
bool        env_unset(envpage_t *env, const char *key);

#endif
