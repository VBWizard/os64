#ifndef MEMORY_ARENA_H
#define MEMORY_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/// Arena allocator for fast, bulk-freeable allocations.
/// All allocations from an arena are freed together when the arena is destroyed.
/// Individual allocations cannot be freed - this is by design.

typedef struct arena {
    uint8_t *buffer;      // Base of allocated buffer
    size_t capacity;      // Total capacity in bytes
    size_t offset;        // Current allocation offset (next free byte)
} arena_t;

/// @brief Create a new arena with the specified capacity
/// @param capacity Size in bytes for the arena buffer
/// @return Pointer to new arena, or NULL on failure
arena_t *arena_create(size_t capacity);

/// @brief Allocate memory from the arena (unaligned)
/// @param arena The arena to allocate from
/// @param size Number of bytes to allocate
/// @return Pointer to allocated memory, or NULL if arena is exhausted
void *arena_alloc(arena_t *arena, size_t size);

/// @brief Allocate aligned memory from the arena
/// @param arena The arena to allocate from
/// @param size Number of bytes to allocate
/// @param alignment Required alignment (must be power of 2)
/// @return Pointer to aligned allocated memory, or NULL if arena is exhausted
void *arena_alloc_aligned(arena_t *arena, size_t size, size_t alignment);

/// @brief Reset the arena to reuse its memory (all previous allocations become invalid)
/// @param arena The arena to reset
void arena_reset(arena_t *arena);

/// @brief Destroy the arena and free all its memory
/// @param arena The arena to destroy
void arena_destroy(arena_t *arena);

/// @brief Get remaining capacity in the arena
/// @param arena The arena to query
/// @return Number of bytes still available
size_t arena_remaining(arena_t *arena);

#endif // MEMORY_ARENA_H
