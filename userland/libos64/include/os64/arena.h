#ifndef OS64_ARENA_H
#define OS64_ARENA_H

#include <stddef.h>

// Reusable, bulk-lifetime storage backed by the libos64 heap. Pointers stay
// stable across growth and expire together at reset or destroy. Do not pass
// them to os64_free/realloc. An arena has no individual free or rewind.
// Callers must synchronize access to a shared arena, including its readers
// before reset/destroy; separate arenas can be used by separate threads.
typedef struct os64_arena os64_arena_t;

typedef struct {
    size_t used_bytes;       // Live payload plus alignment padding since reset.
    size_t reserved_bytes;   // Requested heap bytes, including arena/chunk headers.
    size_t chunk_count;
} os64_arena_stats_t;

// Create the handle; chunks are allocated lazily. initial_capacity is the
// preferred first chunk payload size (0 selects 4096). Regular chunks grow
// geometrically up to 256 KiB, or the initial capacity if that is larger.
// max_bytes limits reserved_bytes (0 is unlimited); it excludes malloc's own
// overhead/page rounding. Growth can use a smaller chunk near the limit.
// If the preferred chunk fails, retry once at the request's size plus any
// alignment slack, without changing the regular growth target.
// NULL on allocation failure or a limit too small for the handle.
os64_arena_t *os64_arena_create(size_t initial_capacity, size_t max_bytes);

// Allocate uninitialized storage, aligned to 16 bytes by default. The aligned
// form accepts nonzero power-of-two alignments. Size 0, invalid alignment,
// overflow, a NULL arena, exhausted budget or heap failure returns NULL and
// leaves the arena unchanged. Growth does not move existing allocations.
void *os64_arena_alloc(os64_arena_t *arena, size_t size);
void *os64_arena_alloc_aligned(os64_arena_t *arena, size_t size, size_t alignment);

// Zero count*size bytes, including when reusing dirty storage after reset.
// Zero count/size and multiplication overflow return NULL without allocation.
void *os64_arena_calloc(os64_arena_t *arena, size_t count, size_t size);

// Copy a NUL-terminated string, including its terminator. NULL input returns
// NULL; an empty string produces a one-byte string with normal alignment.
char *os64_arena_strdup(os64_arena_t *arena, const char *string);

// Invalidate allocations and make retained chunks reusable. Does not clear
// bytes or release storage to the heap. NULL is a no-op. Cost: O(chunk_count).
void os64_arena_reset(os64_arena_t *arena);

// Release chunks and handle to the heap. NULL is a no-op.
void os64_arena_destroy(os64_arena_t *arena);

// A NULL arena produces zero statistics. No internal locking.
os64_arena_stats_t os64_arena_stats(const os64_arena_t *arena);

#endif
