#include "memory/arena.h"
#include "memory/kmalloc.h"
#include "memory/memset.h"
#include "serial_logging.h"
#include "CONFIG.h"

arena_t *arena_create(size_t capacity)
{
    if (capacity == 0) {
        printd(DEBUG_KMALLOC, "arena_create: Invalid capacity 0\n");
        return NULL;
    }

    // Allocate the arena structure itself
    arena_t *arena = kmalloc(sizeof(arena_t));
    if (arena == NULL) {
        printd(DEBUG_KMALLOC, "arena_create: Failed to allocate arena structure\n");
        return NULL;
    }

    // Allocate the arena buffer
    arena->buffer = kmalloc(capacity);
    if (arena->buffer == NULL) {
        printd(DEBUG_KMALLOC, "arena_create: Failed to allocate arena buffer of %lu bytes\n", capacity);
        kfree(arena);
        return NULL;
    }

    // Zero the buffer for safety
    memset(arena->buffer, 0, capacity);

    arena->capacity = capacity;
    arena->offset = 0;

    printd(DEBUG_KMALLOC, "arena_create: Created arena at 0x%lx, buffer=0x%lx, capacity=%lu\n",
           (uintptr_t)arena, (uintptr_t)arena->buffer, capacity);

    return arena;
}

void *arena_alloc(arena_t *arena, size_t size)
{
    if (arena == NULL || size == 0) {
        return NULL;
    }

    // Check if we have enough space
    if (arena->offset + size > arena->capacity) {
        printd(DEBUG_KMALLOC, "arena_alloc: Arena exhausted, requested %lu, available %lu\n",
               size, arena->capacity - arena->offset);
        return NULL;
    }

    // Bump allocate
    void *ptr = arena->buffer + arena->offset;
    arena->offset += size;

    printd(DEBUG_KMALLOC | DEBUG_DETAILED, "arena_alloc: Allocated %lu bytes at 0x%lx, offset now %lu/%lu\n",
           size, (uintptr_t)ptr, arena->offset, arena->capacity);

    return ptr;
}

void *arena_alloc_aligned(arena_t *arena, size_t size, size_t alignment)
{
    if (arena == NULL || size == 0 || alignment == 0) {
        return NULL;
    }

    // Alignment must be power of 2
    if ((alignment & (alignment - 1)) != 0) {
        printd(DEBUG_KMALLOC, "arena_alloc_aligned: Alignment %lu is not a power of 2\n", alignment);
        return NULL;
    }

    // Calculate aligned offset
    uintptr_t current = (uintptr_t)(arena->buffer + arena->offset);
    uintptr_t aligned = (current + alignment - 1) & ~(alignment - 1);
    size_t padding = aligned - current;

    // Check if we have enough space (including padding)
    if (arena->offset + padding + size > arena->capacity) {
        printd(DEBUG_KMALLOC, "arena_alloc_aligned: Arena exhausted, requested %lu (with %lu padding), available %lu\n",
               size, padding, arena->capacity - arena->offset);
        return NULL;
    }

    // Bump allocate with alignment
    arena->offset += padding + size;
    void *ptr = (void *)aligned;

    printd(DEBUG_KMALLOC | DEBUG_DETAILED, "arena_alloc_aligned: Allocated %lu bytes (alignment=%lu, padding=%lu) at 0x%lx, offset now %lu/%lu\n",
           size, alignment, padding, (uintptr_t)ptr, arena->offset, arena->capacity);

    return ptr;
}

void arena_reset(arena_t *arena)
{
    if (arena == NULL) {
        return;
    }

    printd(DEBUG_KMALLOC, "arena_reset: Resetting arena at 0x%lx, was at offset %lu\n",
           (uintptr_t)arena, arena->offset);

    // Zero the used portion for safety (prevent use-after-reset bugs from appearing to work)
    memset(arena->buffer, 0, arena->offset);

    arena->offset = 0;
}

void arena_destroy(arena_t *arena)
{
    if (arena == NULL) {
        return;
    }

    printd(DEBUG_KMALLOC, "arena_destroy: Destroying arena at 0x%lx, capacity=%lu, used=%lu\n",
           (uintptr_t)arena, arena->capacity, arena->offset);

    // Free the buffer
    if (arena->buffer != NULL) {
        kfree(arena->buffer);
    }

    // Free the arena structure
    kfree(arena);
}

size_t arena_remaining(arena_t *arena)
{
    if (arena == NULL) {
        return 0;
    }

    return arena->capacity - arena->offset;
}
