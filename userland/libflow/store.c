// store.c — the arena and the node map every pass keeps its records in.

#include "internal.h"

struct FBlock {
    FBlock *next;
    size_t used, cap;
    // Records are laid down after the header, maximally aligned.
    _Alignas(16) unsigned char data[];
};

// The host harness builds with a block of one byte, so every record is its
// own allocation and the allocation sweep can fail each one in turn.
#ifndef F_BLOCK_BYTES
#define F_BLOCK_BYTES (64u * 1024u)
#endif

void *f_arena_alloc(FArena *arena, size_t size)
{
    size = (size + 15u) & ~(size_t)15u;
    FBlock *b = arena->blocks;
    if (b == NULL || b->cap - b->used < size) {
        size_t cap = size > F_BLOCK_BYTES ? size : F_BLOCK_BYTES;
        b = os64_malloc(sizeof(FBlock) + cap);
        if (b == NULL)
            return NULL;
        b->next = arena->blocks;
        b->used = 0;
        b->cap = cap;
        arena->blocks = b;
    }
    void *at = b->data + b->used;
    b->used += size;
    os64_memset(at, 0, size);
    return at;
}

void f_arena_free(FArena *arena)
{
    FBlock *b = arena->blocks;
    while (b != NULL) {
        FBlock *next = b->next;
        os64_free(b);
        b = next;
    }
    arena->blocks = NULL;
}

static size_t hash_ptr(const void *p)
{
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (size_t)x;
}

static bool map_grow(FMap *map)
{
    size_t cap = map->cap != 0 ? map->cap * 2 : 64;
    const void **keys = os64_calloc(cap, sizeof(*keys));
    void **vals = os64_calloc(cap, sizeof(*vals));
    if (keys == NULL || vals == NULL) {
        os64_free(keys);
        os64_free(vals);
        return false;
    }
    for (size_t i = 0; i < map->cap; i++) {
        if (map->keys[i] == NULL)
            continue;
        size_t at = hash_ptr(map->keys[i]) & (cap - 1);
        while (keys[at] != NULL)
            at = (at + 1) & (cap - 1);
        keys[at] = map->keys[i];
        vals[at] = map->vals[i];
    }
    os64_free(map->keys);
    os64_free(map->vals);
    map->keys = keys;
    map->vals = vals;
    map->cap = cap;
    return true;
}

bool f_map_put(FMap *map, const void *key, void *val)
{
    // Kept under three quarters full, so a probe always ends.
    if ((map->count + 1) * 4 > map->cap * 3 && !map_grow(map))
        return false;
    size_t at = hash_ptr(key) & (map->cap - 1);
    while (map->keys[at] != NULL && map->keys[at] != key)
        at = (at + 1) & (map->cap - 1);
    if (map->keys[at] == NULL)
        map->count++;
    map->keys[at] = key;
    map->vals[at] = val;
    return true;
}

void *f_map_get(const FMap *map, const void *key)
{
    if (map->cap == 0 || key == NULL)
        return NULL;
    size_t at = hash_ptr(key) & (map->cap - 1);
    while (map->keys[at] != NULL) {
        if (map->keys[at] == key)
            return map->vals[at];
        at = (at + 1) & (map->cap - 1);
    }
    return NULL;
}

void f_map_free(FMap *map)
{
    os64_free(map->keys);
    os64_free(map->vals);
    map->keys = NULL;
    map->vals = NULL;
    map->cap = map->count = 0;
}
