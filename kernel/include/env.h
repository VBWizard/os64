#ifndef ENV_H
#define ENV_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "CONFIG.h"
#include "spinlock.h"   // kTaskEnvLock — guards env-block swaps (see below)

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
// page_count starts at 1 and GROWS ON DEMAND (2026-08-14): when env_set
// refuses for lack of room, syscall_setenv grows the block via env_grow and
// retries. The ceiling is TASK_ENV_MAX_BYTES (task.h) — the fixed-VA window
// between TASK_ENV_VIRT and the exit trampoline — enforced at env_grow and
// backstopped by a panic at task_setup_entry's map site, so an oversized
// block can never be silently mapped over its neighbour (the argv blob
// learned this lesson the same week; see THE GUARD THAT DID NOT EXIST in
// task_create).
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
// (Compaction reclaims bytes, never pages: page_count never shrinks. A task
// that grew a big environment keeps the pages until it dies — growth is rare
// and bounded at 64KB, so a shrink path would be machinery without a client.)
bool        env_unset(envpage_t *env, const char *key);

// Allocate a LARGER copy of env — page_count doubles, capped at max_pages —
// and return it, or NULL if env is already at max_pages (the honest "your
// environment is full" refusal) or allocation fails. The caller owns the
// swap: remap the task's fixed window, exchange task->env, kfree the old
// block. Doubling (1→2→4→8→16) means a task pays at most four copies on its
// way to the 64KB ceiling.
envpage_t  *env_grow(const envpage_t *env, uint32_t max_pages);

// Serializes the two operations that can observe a task's env block MID-SWAP
// now that growth can move it: syscall_setenv's grow-and-exchange, and
// env_inherit's copy at spawn (a sibling thread of the parent could grow the
// parent's env out from under the memcpy — use-after-free, not just a torn
// read). Every reader that follows task->env to a block that might be
// growing takes this lock. Userland readers are untouched: they walk their
// own mapping at TASK_ENV_VIRT, which swaps atomically at the remap.
extern spinlock_t kTaskEnvLock;

#endif
