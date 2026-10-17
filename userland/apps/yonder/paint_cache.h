#ifndef YONDER_PAINT_CACHE_H
#define YONDER_PAINT_CACHE_H

// The paint cache's store (YONDER_PERF.md § perf-2), private to the
// painter: entries of bytes, found by a key the painter makes, kept most
// recently used first, the least recently used let go to stay within the
// budget. What a key means and what the bytes are is the painter's.

#include "paint.h"

// A paint begins: entries used from here until the next begin are this
// paint's, and making another never lets one of them go.
void paint_cache_begin(yonder_paint_cache_t *c);
uint64_t paint_cache_hash(const uint8_t *key, uint32_t klen);
// The bytes kept under `key`, made this paint's, or NULL when none are.
void *paint_cache_find(yonder_paint_cache_t *c, const uint8_t *key, uint32_t klen, uint64_t hash);
// The bytes kept under `key` when this paint has found or made them
// already, else NULL: a second look, not counted as a hit.
void *paint_cache_held(yonder_paint_cache_t *c, const uint8_t *key, uint32_t klen, uint64_t hash);
// Room for `bytes` kept under `key` (which no entry holds), for the caller
// to fill before it asks the cache anything else; NULL, and counted, when
// there is no room without letting go of one of this paint's entries, or
// no memory.
void *paint_cache_make(yonder_paint_cache_t *c, const uint8_t *key, uint32_t klen, uint64_t hash,
                       size_t bytes);
// A picture too wide for a band was worked out without the cache.
void paint_cache_too_wide(yonder_paint_cache_t *c);

#endif
