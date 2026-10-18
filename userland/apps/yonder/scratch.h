#ifndef YONDER_SCRATCH_H
#define YONDER_SCRATCH_H

// The paint's scratch (YONDER_PERF.md § perf-5): buffers a paint borrows
// and gives back, kept from one paint to the next. A buffer of 128 KB or
// more is a region of its own in libos64's heap, mapped on malloc and
// unmapped on free, so taking and freeing one every paint costs a map, a
// zeroed page per page touched, an unmap and a TLB shootdown every paint.
// Keeping it costs none of those.
//
// A buffer is lent DIRTY: whoever takes one writes each byte before reading
// it. No lock: only the window's main thread paints.

#include <stddef.h>
#include <stdint.h>

typedef struct yonder_scratch yonder_scratch_t;

// Below this a request is not pooled: the heap carves it from its pools
// without the kernel, so freeing it costs nothing worth saving.
#define YONDER_SCRATCH_FLOOR (64u * 1024u)
// Buffers a scratch can hold, lent or idle.
#define YONDER_SCRATCH_SLOTS 16
// Paints a buffer may sit idle before it is freed.
#define YONDER_SCRATCH_IDLE_PAINTS 120
// The most the idle buffers may hold in all; one given back past it is
// freed, not kept.
#define YONDER_SCRATCH_KEEP ((size_t)64 * 1024 * 1024)

typedef struct {
    uint32_t lent, idle;            // slots, now
    size_t idle_bytes;              // what the idle buffers hold, now
    uint64_t made, reused;          // pooled takes that allocated, and that did not
    uint64_t unpooled;              // takes that went straight to os64_malloc
} yonder_scratch_counts_t;

// NULL when there is no memory. Every call below takes a NULL scratch too,
// and then each take is os64_malloc and each give os64_free.
yonder_scratch_t *yonder_scratch_new(void);
void yonder_scratch_free(yonder_scratch_t *s);

// At least `bytes`, its contents whatever they were, or NULL when there is
// no memory.
void *yonder_scratch_take(yonder_scratch_t *s, size_t bytes);
// Back from whoever took it; NULL is nothing to give.
void yonder_scratch_give(yonder_scratch_t *s, void *p);
// A paint is over: a buffer idle for YONDER_SCRATCH_IDLE_PAINTS paints is
// freed.
void yonder_scratch_paint_end(yonder_scratch_t *s);
// Another page is shown: every idle buffer is freed.
void yonder_scratch_drop(yonder_scratch_t *s);

yonder_scratch_counts_t yonder_scratch_counts(const yonder_scratch_t *s);

#endif
