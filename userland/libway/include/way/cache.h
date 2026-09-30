#ifndef WAY_CACHE_H
#define WAY_CACHE_H

// The browser's cache (CACHE.md): the replies a browser may use again,
// kept on disk, one file each, and the rules of RFC 9111 that say when.
// One per browser, like the jar, shared by every fetch it makes on any
// thread, and by every other browser that opens the same directory. It is
// reached through way_fetch_whole (way.h); what is here is the store, what
// a person is shown of it, and the rules, which are pure: a stored reply's
// facts and a clock in, a verdict out, so a harness drives them with no
// network and no disk.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fetch/fetch.h"

#pragma GCC visibility push(default)

// ── The rules ───────────────────────────────────────────────────────────

// Heuristic freshness (§ 4.2.2) is a tenth of how long a reply had gone
// unmodified when it was sent, and no more than this: a bound of our own.
#define WAY_KEEP_HEURISTIC_MAX (7 * 24 * 60 * 60)

// What a reply said about keeping it, read. Times are seconds since 1970
// (UTC); -1 is "not said". `stored` is this machine's clock when the reply
// arrived, or when a 304 last confirmed it.
typedef struct {
    int64_t stored;
    int64_t date, expires, last_modified;
    int64_t age;                        // the Age it arrived with; 0 when none
    int64_t max_age;                    // -1 when none
    bool no_cache;                      // or Pragma: no-cache with no Cache-Control
    bool must_revalidate;
    char etag[OS64_FETCH_KEEP_FIELD];
    char last_modified_text[OS64_FETCH_KEEP_FIELD];
} way_keep_t;

// Reads a reply's fields as it arrives at `now`. False when they forbid
// keeping it: `no-store`, a `Vary` on anything but Accept-Encoding, or a
// field too long to have been read whole.
bool way_keep_read(const os64_fetch_keep_t *fields, int64_t now, way_keep_t *out);

// How long it is fresh for (§ 4.2.1), and how old it is at `now` (§ 4.2.3),
// in seconds.
int64_t way_keep_lifetime(const way_keep_t *k);
int64_t way_keep_age(const way_keep_t *k, int64_t now);

// Whether it may be used at `now` without asking the server.
bool way_keep_fresh(const way_keep_t *k, int64_t now);

// The request lines that ask whether it changed — If-None-Match for an
// ETag, else If-Modified-Since — as "Name: value\r\n", within `cap`. The
// length written; 0 when it has no validator, and the server can only be
// asked for it whole.
size_t way_keep_conditions(const way_keep_t *k, char *out, size_t cap);

// A 304's fields laid over the stored ones at `now` (§ 4.3.4): what it
// sent replaces what was kept, and the clock of its freshness restarts.
// False when the 304 forbids keeping it any longer.
bool way_keep_confirm(way_keep_t *k, const os64_fetch_keep_t *fields, int64_t now);

// Whether a stale reply may be used while the server cannot be reached at
// all (§ 4.2.4): not when it asked to be checked every time.
bool way_keep_stale_ok(const way_keep_t *k);

// ── The store ───────────────────────────────────────────────────────────

typedef struct way_cache way_cache_t;

#define WAY_CACHE_DIR "/var/cache/yonder"
#define WAY_CACHE_MB_DEFAULT 256

// The cache in `dir`, which is made if it is not there, holding at most
// `cap` bytes. NULL on no memory. A directory that cannot be made or
// written is a cache that keeps nothing, and says so in its stats.
way_cache_t *way_cache_open(const char *dir, uint64_t cap);
void way_cache_close(way_cache_t *cache);

// Whether fetches use it; a cache turned off keeps what it has.
void way_cache_enable(way_cache_t *cache, bool on);
bool way_cache_enabled(way_cache_t *cache);

// What a person is shown: what the directory holds (counted when asked,
// so another browser's entries count too), and what this one did.
typedef struct {
    bool usable;                        // the directory is there and writable
    bool enabled;
    int32_t entries;
    uint64_t bytes, cap;
    uint64_t hits;                      // served without asking
    uint64_t confirmed;                 // served after a 304
    uint64_t stale;                     // served because the server could not be reached
    uint64_t stored;
} way_cache_stats_t;
void way_cache_stats(way_cache_t *cache, way_cache_stats_t *out);
const char *way_cache_dir(way_cache_t *cache);

// Removes every entry. How many went.
int32_t way_cache_clear(way_cache_t *cache);

#pragma GCC visibility pop

#endif
