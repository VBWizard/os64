#ifndef WAY_INTERNAL_H
#define WAY_INTERNAL_H

// The seam between libway's two halves.

#include "way/way.h"

// Sets session->status.
void way_say(way_session_t *session, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Whether an address can be offered inside single quotes as a command.
bool way_shell_quotable(const char *address);

// ── The cache's store (cache.c), for way_fetch_whole ────────────────────

// One kept reply: the address asked for, the address it came from, what
// it was, and what it said about keeping it.
typedef struct {
    char url[OS64_FETCH_URL_MAX];
    char final[OS64_FETCH_URL_MAX];
    int32_t status;
    char content_type[HTTP_TYPE_MAX];
    char charset[HTTP_CHARSET_MAX];
    way_keep_t keep;
} way_entry_t;

// The entry for `url`, and its body (os64_malloc'd, the caller's), when
// there is a whole one of at most `cap` bytes. False for none.
bool way_cache_find(way_cache_t *cache, const char *url, way_entry_t *e, uint8_t **body,
                    size_t *len, size_t cap);
// Keeps an entry, replacing any for the same address. False when it was
// not kept: turned off, too large, or the disk said no.
bool way_cache_put(way_cache_t *cache, const way_entry_t *e, const uint8_t *body, size_t len);
// Counts a reply served from the store.
void way_cache_count(way_cache_t *cache, way_served_t how);

#endif
