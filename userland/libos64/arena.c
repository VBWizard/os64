#include "os64/arena.h"
#include "os64/mem.h"
#include "os64/str.h"
#include <stdint.h>

#define ARENA_ALIGNMENT 16u
#define ARENA_INITIAL_CAPACITY 4096u
#define ARENA_GROWTH_LIMIT (256u * 1024u)

typedef struct arena_chunk {
    struct arena_chunk *next;
    size_t capacity;
    size_t used;
    _Alignas(ARENA_ALIGNMENT) unsigned char data[];
} arena_chunk_t;

struct os64_arena {
    arena_chunk_t *chunks;
    arena_chunk_t *tail;
    arena_chunk_t *current;
    size_t next_capacity;
    size_t max_bytes;
    os64_arena_stats_t stats;
};

os64_arena_t *os64_arena_create(size_t initial_capacity, size_t max_bytes)
{
    if (max_bytes && max_bytes < sizeof(os64_arena_t))
        return NULL;
    os64_arena_t *arena = os64_malloc(sizeof(*arena));
    if (!arena)
        return NULL;
    *arena = (os64_arena_t){
        .next_capacity = initial_capacity ? initial_capacity : ARENA_INITIAL_CAPACITY,
        .max_bytes = max_bytes ? max_bytes : SIZE_MAX,
        .stats.reserved_bytes = sizeof(*arena),
    };
    return arena;
}

static void *chunk_alloc(os64_arena_t *arena, arena_chunk_t *chunk,
                         size_t size, size_t alignment)
{
    uintptr_t address = (uintptr_t)(chunk->data + chunk->used);
    size_t padding = (size_t)(-(uintptr_t)address & (alignment - 1));
    size_t remaining = chunk->capacity - chunk->used;
    if (padding > remaining || size > remaining - padding)
        return NULL;
    void *out = chunk->data + chunk->used + padding;
    chunk->used += padding + size;
    arena->stats.used_bytes += padding + size;
    arena->current = chunk;
    return out;
}

void *os64_arena_alloc_aligned(os64_arena_t *arena, size_t size, size_t alignment)
{
    if (!arena || !size || !alignment || (alignment & (alignment - 1)))
        return NULL;
    if (size > SIZE_MAX - (alignment - 1))
        return NULL;

    if (arena->current) {
        void *out = chunk_alloc(arena, arena->current, size, alignment);
        if (out)
            return out;
    }
    // Search retained chunks as well as the hot chunk. After a reset, a
    // different allocation order can still reuse the previous frame's space.
    for (arena_chunk_t *chunk = arena->chunks; chunk; chunk = chunk->next) {
        if (chunk == arena->current)
            continue;
        void *out = chunk_alloc(arena, chunk, size, alignment);
        if (out)
            return out;
    }

    // Heap alignment also aligns data. Use a conservative padding bound for
    // larger alignments; the next heap address is not known before allocation.
    size_t need = size + (alignment > ARENA_ALIGNMENT ? alignment - 1 : 0);
    size_t available = arena->max_bytes - arena->stats.reserved_bytes;
    if (available <= sizeof(arena_chunk_t) || need > available - sizeof(arena_chunk_t))
        return NULL;
    size_t capacity = arena->next_capacity > need ? arena->next_capacity : need;
    if (capacity > available - sizeof(arena_chunk_t))
        capacity = available - sizeof(arena_chunk_t);
    arena_chunk_t *chunk = os64_malloc(sizeof(*chunk) + capacity);
    // The growth target is a preference: a smaller block may still fit in
    // the heap when the preferred chunk cannot be supplied. Retry once.
    if (!chunk && capacity > need) {
        capacity = need;
        chunk = os64_malloc(sizeof(*chunk) + capacity);
    }
    if (!chunk)
        return NULL;

    chunk->capacity = capacity;
    chunk->used = 0;
    chunk->next = NULL;
    if (arena->tail)
        arena->tail->next = chunk;
    else
        arena->chunks = chunk;
    arena->tail = chunk;
    arena->stats.reserved_bytes += sizeof(*chunk) + capacity;
    arena->stats.chunk_count++;
    // Oversized requests get their own sufficiently large chunk without
    // inflating the growth policy for subsequent ordinary requests.
    if (need <= arena->next_capacity && capacity >= arena->next_capacity &&
        arena->next_capacity < ARENA_GROWTH_LIMIT) {
        arena->next_capacity = arena->next_capacity > ARENA_GROWTH_LIMIT / 2
            ? ARENA_GROWTH_LIMIT : arena->next_capacity * 2;
    }
    return chunk_alloc(arena, chunk, size, alignment);
}

void *os64_arena_alloc(os64_arena_t *arena, size_t size)
{
    return os64_arena_alloc_aligned(arena, size, ARENA_ALIGNMENT);
}

void *os64_arena_calloc(os64_arena_t *arena, size_t count, size_t size)
{
    if (!count || !size || count > SIZE_MAX / size)
        return NULL;
    void *out = os64_arena_alloc(arena, count * size);
    if (out)
        os64_memset(out, 0, count * size);
    return out;
}

char *os64_arena_strdup(os64_arena_t *arena, const char *string)
{
    if (!arena || !string)
        return NULL;
    size_t length = os64_strlen(string);
    if (length == SIZE_MAX)
        return NULL;
    char *out = os64_arena_alloc(arena, length + 1);
    if (out)
        os64_memcpy(out, string, length + 1);
    return out;
}

void os64_arena_reset(os64_arena_t *arena)
{
    if (!arena)
        return;
    for (arena_chunk_t *chunk = arena->chunks; chunk; chunk = chunk->next)
        chunk->used = 0;
    arena->current = arena->chunks;
    arena->stats.used_bytes = 0;
}

void os64_arena_destroy(os64_arena_t *arena)
{
    if (!arena)
        return;
    arena_chunk_t *chunk = arena->chunks;
    while (chunk) {
        arena_chunk_t *next = chunk->next;
        os64_free(chunk);
        chunk = next;
    }
    os64_free(arena);
}

os64_arena_stats_t os64_arena_stats(const os64_arena_t *arena)
{
    return arena ? arena->stats : (os64_arena_stats_t){0};
}
