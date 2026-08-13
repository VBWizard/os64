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
    arena->next = NULL;

    printd(DEBUG_KMALLOC, "arena_create: Created arena at 0x%lx, buffer=0x%lx, capacity=%lu\n",
           (uintptr_t)arena, (uintptr_t)arena->buffer, capacity);

    return arena;
}

/// @brief Chain a fresh buffer when the current one can't satisfy `need`.
///
/// The head arena_t stays the caller's handle: the FULL block's bookkeeping
/// moves onto the `next` chain (where only arena_destroy ever looks), and the
/// head gets a new buffer at least `need` bytes big — doubling-by-default via
/// max(capacity, need), so a task that keeps growing pays O(log n) kmallocs,
/// not one per page.
static bool arena_grow(arena_t *arena, size_t need)
{
    arena_t *retired = kmalloc(sizeof(arena_t));
    if (retired == NULL) {
        printd(DEBUG_KMALLOC, "arena_grow: Failed to allocate chain record\n");
        return false;
    }

    size_t newCapacity = (arena->capacity > need) ? arena->capacity : need;
    uint8_t *newBuffer = kmalloc(newCapacity);
    if (newBuffer == NULL) {
        printd(DEBUG_KMALLOC, "arena_grow: Failed to allocate %lu-byte growth buffer\n", newCapacity);
        kfree(retired);
        return false;
    }
    memset(newBuffer, 0, newCapacity);

    // The full block's identity moves to the chain; the head becomes the
    // fresh block. Order matters to nobody (arena calls are caller-serialized
    // — one task's tables are built by one thread at a time) but keeping the
    // head consistent at every line costs nothing.
    retired->buffer   = arena->buffer;
    retired->capacity = arena->capacity;
    retired->offset   = arena->offset;
    retired->next     = arena->next;

    arena->buffer   = newBuffer;
    arena->capacity = newCapacity;
    arena->offset   = 0;
    arena->next     = retired;

    printd(DEBUG_KMALLOC, "arena_grow: Arena 0x%lx grew by %lu bytes (new hot buffer 0x%lx)\n",
           (uintptr_t)arena, newCapacity, (uintptr_t)newBuffer);
    return true;
}

void *arena_alloc(arena_t *arena, size_t size)
{
    if (arena == NULL || size == 0) {
        return NULL;
    }

    // Grow rather than fail — NULL now means kmalloc itself is out, not that
    // the birth-time capacity guess was wrong.
    if (arena->offset + size > arena->capacity) {
        if (!arena_grow(arena, size)) {
            printd(DEBUG_KMALLOC, "arena_alloc: Arena exhausted and could not grow, requested %lu\n", size);
            return NULL;
        }
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

    // Grow rather than fail (see arena_alloc). Growth is sized for the worst
    // case (size + full alignment slack), and the padding is recomputed
    // against the FRESH buffer — the old buffer's residue means nothing now.
    if (arena->offset + padding + size > arena->capacity) {
        if (!arena_grow(arena, size + alignment)) {
            printd(DEBUG_KMALLOC, "arena_alloc_aligned: Arena exhausted and could not grow, requested %lu\n", size);
            return NULL;
        }
        current = (uintptr_t)(arena->buffer + arena->offset);
        aligned = (current + alignment - 1) & ~(alignment - 1);
        padding = aligned - current;
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

    // Reset means "back to birth": the growth chain's blocks are as invalid
    // as every other allocation, so return them rather than orphan them.
    arena_t *block = arena->next;
    arena->next = NULL;
    while (block != NULL) {
        arena_t *next = block->next;
        if (block->buffer != NULL) {
            kfree(block->buffer);
        }
        kfree(block);
        block = next;
    }
}

void arena_destroy(arena_t *arena)
{
    if (arena == NULL) {
        return;
    }

    printd(DEBUG_KMALLOC, "arena_destroy: Destroying arena at 0x%lx, capacity=%lu, used=%lu\n",
           (uintptr_t)arena, arena->capacity, arena->offset);

    // Free the hot buffer, then every retired block on the growth chain.
    // For kmalloc-backed buffers this is also where the lazy HHDM unmaps
    // them — which is the paging-arena design's tripwire: anything that
    // touches a dead task's page tables after this line faults loudly.
    if (arena->buffer != NULL) {
        kfree(arena->buffer);
    }
    arena_t *block = arena->next;
    while (block != NULL) {
        arena_t *next = block->next;
        if (block->buffer != NULL) {
            kfree(block->buffer);
        }
        kfree(block);
        block = next;
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
