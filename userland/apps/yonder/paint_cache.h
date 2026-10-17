#ifndef YONDER_PAINT_CACHE_H
#define YONDER_PAINT_CACHE_H

// The paint cache's store (YONDER_PERF.md § perf-2), private to the
// painter: entries of bytes, found by a key the painter makes, kept most
// recently used first, the least recently used let go to stay within the
// budget — never one the last two paints used. What a key means and what
// the bytes are is the painter's.

#include "paint.h"

// A paint begins: entries used from here until the next begin are this
// paint's, and making another never lets one of them, or one the paint
// before used, go.
void paint_cache_begin(yonder_paint_cache_t *c);
// FNV-1a over `n` bytes, from `h` (a key's hash so far; paint_cache_hash
// starts one), so a key's common part is hashed once.
uint64_t paint_cache_hash_from(uint64_t h, const uint8_t *key, uint32_t n);
uint64_t paint_cache_hash(const uint8_t *key, uint32_t n);
// The bytes kept under `key`, now this paint's, or NULL when none are.
void *paint_cache_find(yonder_paint_cache_t *c, const uint8_t *key, uint32_t klen, uint64_t hash);
// What an entry of `bytes` under a key `klen` long costs the budget.
size_t paint_cache_cost(uint32_t klen, size_t bytes);
// Whether `cost` more fits, in what is free and what may be let go.
bool paint_cache_room(const yonder_paint_cache_t *c, size_t cost);
// Room for `bytes` kept under `key` (which no entry holds), this paint's,
// for the caller to fill; NULL when it does not fit (ask paint_cache_room
// first) or there is no memory.
void *paint_cache_make(yonder_paint_cache_t *c, const uint8_t *key, uint32_t klen, uint64_t hash,
                       size_t bytes);
// An entry this paint found or made and cannot use after all: no paint's,
// and the first to be let go.
void paint_cache_release(yonder_paint_cache_t *c, void *bytes);
// A picture worked out without the cache: its blocks did not fit, or found
// no memory.
void paint_cache_refused(yonder_paint_cache_t *c, bool memory);

#endif
