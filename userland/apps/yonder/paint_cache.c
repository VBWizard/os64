// paint_cache.c — the paint cache's store (paint_cache.h): a hash table to
// find an entry by its key and a list to find the least recently used.

#include "paint_cache.h"
#include "os64/mem.h"
#include "os64/str.h"

typedef struct Entry {
    struct Entry *chain;            // the next in its bucket
    struct Entry *newer, *older;    // the list, most recently used first
    uint64_t hash;
    uint64_t stamp;                 // the paint that last used it; 0 for none
    size_t size;                    // all of it, as the budget counts
    uint32_t klen;
    uint32_t pad;
    // Then the bytes, 8-aligned, then the key.
} Entry;

_Static_assert(sizeof(Entry) % 8 == 0, "an entry's bytes must start 8-aligned");

#define BUCKETS_FIRST 256

struct yonder_paint_cache {
    size_t budget, held;
    Entry **buckets;
    uint32_t nbuckets;              // a power of two, or 0 before the first entry
    uint64_t entries;
    uint64_t stamp;                 // this paint's; the first paint's is 1
    Entry *newest, *oldest;
    yonder_paint_cache_counts_t counts;
};

static uint8_t *bytes_of(Entry *e)
{
    return (uint8_t *)(e + 1);
}

static uint8_t *key_of(Entry *e)
{
    return bytes_of(e) + (e->size - sizeof(Entry) - e->klen);
}

// Used by this paint or the one before it: never let go for room.
static bool kept_now(const yonder_paint_cache_t *c, const Entry *e)
{
    return e->stamp != 0 && e->stamp + 1 >= c->stamp;
}

size_t yonder_paint_cache_budget(uint32_t width, uint32_t height, uint64_t memory)
{
    // Four bytes a pixel, the widest an entry holds (a tile's; a mask's is
    // one). The pixels are compared with the ceiling before they are
    // multiplied out, which no pair of 32-bit sides can then overflow.
    const uint64_t per_pixel = 4u * YONDER_PAINT_CACHE_SCREENS;
    uint64_t pixels = (uint64_t)width * height;
    uint64_t want = pixels > YONDER_PAINT_CACHE_CEILING / per_pixel ? YONDER_PAINT_CACHE_CEILING
                                                                     : pixels * per_pixel;
    uint64_t share = memory / YONDER_PAINT_CACHE_SHARE;
    if (want > share)
        want = share;
    return want < YONDER_PAINT_CACHE_FLOOR ? YONDER_PAINT_CACHE_FLOOR : (size_t)want;
}

yonder_paint_cache_t *yonder_paint_cache_new(size_t budget)
{
    yonder_paint_cache_t *c = os64_malloc(sizeof(*c));
    if (c != NULL) {
        os64_memset(c, 0, sizeof(*c));
        c->budget = budget;
    }
    return c;
}

static void unlink_list(yonder_paint_cache_t *c, Entry *e)
{
    if (e->newer != NULL)
        e->newer->older = e->older;
    else
        c->newest = e->older;
    if (e->older != NULL)
        e->older->newer = e->newer;
    else
        c->oldest = e->newer;
    e->newer = e->older = NULL;
}

static void push_newest(yonder_paint_cache_t *c, Entry *e)
{
    e->older = c->newest;
    e->newer = NULL;
    if (c->newest != NULL)
        c->newest->newer = e;
    else
        c->oldest = e;
    c->newest = e;
}

static void push_oldest(yonder_paint_cache_t *c, Entry *e)
{
    e->newer = c->oldest;
    e->older = NULL;
    if (c->oldest != NULL)
        c->oldest->older = e;
    else
        c->newest = e;
    c->oldest = e;
}

static void let_go(yonder_paint_cache_t *c, Entry *e)
{
    Entry **at = &c->buckets[e->hash & (c->nbuckets - 1)];
    while (*at != e)
        at = &(*at)->chain;
    *at = e->chain;
    unlink_list(c, e);
    c->held -= e->size;
    c->entries--;
    os64_free(e);
}

void yonder_paint_cache_empty(yonder_paint_cache_t *c)
{
    if (c == NULL)
        return;
    while (c->oldest != NULL)
        let_go(c, c->oldest);
    os64_free(c->buckets);
    c->buckets = NULL;
    c->nbuckets = 0;
}

void yonder_paint_cache_free(yonder_paint_cache_t *c)
{
    yonder_paint_cache_empty(c);
    os64_free(c);
}

yonder_paint_cache_counts_t yonder_paint_cache_counts(const yonder_paint_cache_t *c)
{
    yonder_paint_cache_counts_t n = {0};
    if (c != NULL) {
        n = c->counts;
        n.bytes = c->held;
        n.entries = c->entries;
        n.budget = c->budget;
    }
    return n;
}

void yonder_paint_cache_counts_restart(yonder_paint_cache_t *c)
{
    if (c != NULL)
        os64_memset(&c->counts, 0, sizeof(c->counts));
}

void paint_cache_begin(yonder_paint_cache_t *c)
{
    c->stamp++;
}

uint64_t paint_cache_hash_from(uint64_t h, const uint8_t *key, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        h = (h ^ key[i]) * 0x100000001b3ull;
    return h;
}

uint64_t paint_cache_hash(const uint8_t *key, uint32_t n)
{
    return paint_cache_hash_from(0xcbf29ce484222325ull, key, n);
}

void *paint_cache_find(yonder_paint_cache_t *c, const uint8_t *key, uint32_t klen, uint64_t hash)
{
    if (c->nbuckets == 0)
        return NULL;
    for (Entry *e = c->buckets[hash & (c->nbuckets - 1)]; e != NULL; e = e->chain) {
        if (e->hash != hash || e->klen != klen || os64_memcmp(key_of(e), key, klen) != 0)
            continue;
        e->stamp = c->stamp;
        unlink_list(c, e);
        push_newest(c, e);
        c->counts.hits++;
        return bytes_of(e);
    }
    return NULL;
}

size_t paint_cache_cost(uint32_t klen, size_t bytes)
{
    return sizeof(Entry) + ((bytes + 7) & ~(size_t)7) + klen;
}

bool paint_cache_room(const yonder_paint_cache_t *c, size_t cost)
{
    if (cost > c->budget)
        return false;
    size_t free = c->budget - c->held;
    for (const Entry *e = c->oldest; free < cost && e != NULL && !kept_now(c, e); e = e->newer)
        free += e->size;
    return free >= cost;
}

// Twice the buckets once there are as many entries as buckets. A table
// that cannot grow keeps its chains longer, which costs time, not answers.
static bool buckets_fit(yonder_paint_cache_t *c)
{
    if (c->nbuckets != 0 && c->entries < c->nbuckets)
        return true;
    uint32_t n = c->nbuckets == 0 ? BUCKETS_FIRST : c->nbuckets * 2;
    Entry **b = os64_malloc((size_t)n * sizeof(*b));
    if (b == NULL)
        return c->nbuckets != 0;
    os64_memset(b, 0, (size_t)n * sizeof(*b));
    for (uint32_t i = 0; i < c->nbuckets; i++) {
        Entry *e = c->buckets[i];
        while (e != NULL) {
            Entry *next = e->chain;
            e->chain = b[e->hash & (n - 1)];
            b[e->hash & (n - 1)] = e;
            e = next;
        }
    }
    os64_free(c->buckets);
    c->buckets = b;
    c->nbuckets = n;
    return true;
}

void *paint_cache_make(yonder_paint_cache_t *c, const uint8_t *key, uint32_t klen, uint64_t hash,
                       size_t bytes)
{
    size_t size = paint_cache_cost(klen, bytes);
    if (!paint_cache_room(c, size))
        return NULL;
    while (c->held + size > c->budget) {
        let_go(c, c->oldest);
        c->counts.evicted++;
    }
    if (!buckets_fit(c))
        return NULL;
    Entry *e = os64_malloc(size);
    // Memory refused below the budget: what neither this paint nor the one
    // before it uses is let go, oldest first, asking again after each, so
    // a cache that cannot grow gives its memory back instead of pinning it.
    while (e == NULL && c->oldest != NULL && !kept_now(c, c->oldest)) {
        let_go(c, c->oldest);
        c->counts.evicted++;
        e = os64_malloc(size);
    }
    if (e == NULL)
        return NULL;
    os64_memset(e, 0, sizeof(*e));
    e->hash = hash;
    e->stamp = c->stamp;
    e->size = size;
    e->klen = klen;
    os64_memcpy(key_of(e), key, klen);
    e->chain = c->buckets[hash & (c->nbuckets - 1)];
    c->buckets[hash & (c->nbuckets - 1)] = e;
    push_newest(c, e);
    c->held += size;
    c->entries++;
    c->counts.made++;
    return bytes_of(e);
}

void paint_cache_release(yonder_paint_cache_t *c, void *bytes)
{
    Entry *e = (Entry *)bytes - 1;
    e->stamp = 0;
    unlink_list(c, e);
    push_oldest(c, e);
}

void paint_cache_refused(yonder_paint_cache_t *c, bool memory)
{
    if (memory)
        c->counts.no_memory++;
    else
        c->counts.no_room++;
}
