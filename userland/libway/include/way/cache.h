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
#include "way/origin.h"

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
// False when the 304 forbids keeping it any longer. way_keep_confirm_varied
// is the same for an entry whose request's Vary'd values are recorded
// (way_keep_read_varied): a 304 repeating that Vary still confirms it.
bool way_keep_confirm(way_keep_t *k, const os64_fetch_keep_t *fields, int64_t now);
bool way_keep_confirm_varied(way_keep_t *k, const os64_fetch_keep_t *fields, int64_t now);

// Whether a stale reply may be used while the server cannot be reached at
// all (§ 4.2.4): not when it asked to be checked every time.
bool way_keep_stale_ok(const way_keep_t *k);

// What the redirects on the way to a reply said about keeping them, for
// the reply kept under the address that led through them: a permanent
// redirect is kept only as its own fields allow (§ 4), and an entry that
// stands for the whole chain is held to the strictest of them.
typedef struct {
    bool no_store;                      // one forbade keeping, or could not be read
    bool no_cache, must_revalidate;
    int64_t until;                      // when the shortest-lived explicit freshness
                                        // ends, this machine's clock; -1 when none said
} way_chain_t;

void way_chain_start(way_chain_t *chain);
// A redirect's fields, as they arrive at `now`. One that says nothing about
// freshness is kept by heuristic (§ 4.2.2: 301 and 308 may be), and limits
// nothing.
void way_chain_hop(way_chain_t *chain, const os64_fetch_keep_t *fields, int64_t now);
// The chain laid over the reply's own keep, at `now`. False when the chain
// forbids keeping it at all.
bool way_chain_apply(const way_chain_t *chain, way_keep_t *k, int64_t now);

// Where an answer came from: the network, or the cache (way_fetch_whole's
// and the fetch job's alike).
typedef enum {
    WAY_SERVED_NETWORK = 0,
    WAY_SERVED_KEPT,            // the cache, fresh: nothing went out
    WAY_SERVED_CONFIRMED,       // the cache, after the server answered 304
    WAY_SERVED_STALE,           // the cache, stale, because the server could not be reached
} way_served_t;

// ── What a script's request reads of a kept reply (SCRIPT_CACHE.md § 4) ─

// How a request uses the cache (Fetch's cache mode, fetch()'s `cache`), in
// the order the API lists them (SCRIPT_CACHE.md § 5): default — fresh
// served, stale asked again; no-store — neither read nor kept; reload — not
// read, kept; no-cache — always asked again; force-cache — served whatever
// its age; only-if-cached — served whatever its age, and a miss is an error.
typedef enum {
    WAY_CACHE_DEFAULT,
    WAY_CACHE_NO_STORE,
    WAY_CACHE_RELOAD,
    WAY_CACHE_NO_CACHE,
    WAY_CACHE_FORCE_CACHE,
    WAY_CACHE_ONLY_IF_CACHED,
} way_cache_mode_t;

// way_keep_read, keeping a reply that varies on any header but `*` (whose
// answer varies on things a request does not say): the caller records what
// the request said of each (way_vary_lines) and holds a later request to it
// (way_vary_matches).
bool way_keep_read_varied(const os64_fetch_keep_t *fields, int64_t now, way_keep_t *out);

// What a request sends for header `name` (`nlen` bytes, any case): a VALUE
// into `out` (one sent empty is a value, ""), NONE when it sends no such
// line, which a server can tell apart from an empty one, or UNKNOWN when
// the caller cannot say, which keeps nothing that varies on that header.
typedef enum { WAY_SAID_UNKNOWN, WAY_SAID_NONE, WAY_SAID_VALUE } way_said_t;
typedef way_said_t (*way_request_value_t)(void *ctx, const char *name, size_t nlen, char *out, size_t cap);

// A header line's value is at most this long here; one longer is a header
// nothing that varies on it is kept for.
#define WAY_VARY_VALUE_MAX 1024

// What the request said of each header `vary` (a response's Vary value)
// names, as lines into `out`: "name: =value\r\n" for one it sent, "name:
// -\r\n" for one it did not — but Accept-Encoding, which libfetch asks for
// the same way every time. The length written; SIZE_MAX when `vary` is `*`,
// or names a header `value_of` cannot say, or the lines do not fit.
size_t way_vary_lines(const char *vary, way_request_value_t value_of, void *ctx, char *out, size_t cap);
// Whether a request says, of each header `lines` (way_vary_lines') names,
// the value written there.
bool way_vary_matches(const char *lines, size_t len, way_request_value_t value_of, void *ctx);

// A response's header block as the cache keeps it: without Set-Cookie and
// Set-Cookie2, whose cookies were the jar's when the reply came and are no
// one's to hear again. The length written; SIZE_MAX when it does not fit.
size_t way_headers_storable(const char *block, size_t len, char *out, size_t cap);

// A kept header block with a 304's laid over it (RFC 9111 § 3.2): each
// header the 304 sends replaces the kept lines of that name, but the ones
// that frame the body (Content-Length, Content-Encoding,
// Transfer-Encoding), which describe the kept body, not the 304's; Set-Cookie
// is not kept from either. The length written; SIZE_MAX when it does not
// fit.
size_t way_headers_update(const char *kept, size_t klen, const char *update, size_t ulen, char *out,
                          size_t cap);

// ── The store ───────────────────────────────────────────────────────────

typedef struct way_cache way_cache_t;

#define WAY_CACHE_DIR "/var/cache/yonder"
#define WAY_CACHE_MB_DEFAULT 256
#define WAY_CACHE_MB_MAX 65536

// The cache in `dir`, which is made if it is not there, holding at most
// `cap` bytes — the directory's, shared with any other browser that opens
// it: each counts the directory again after writing a sixteenth of the cap,
// so it is over by no more than that much for each browser writing to it.
// NULL on no memory. A directory that cannot be made or written is a cache
// that keeps nothing, and says so in its stats.
way_cache_t *way_cache_open(const char *dir, uint64_t cap);
void way_cache_close(way_cache_t *cache);

// How much it may hold, from now on, fetches in flight included. Lowered
// below what it holds, the oldest entries go at once, on the caller's
// thread.
void way_cache_set_cap(way_cache_t *cache, uint64_t cap);

// Whether fetches use it; a cache turned off keeps what it has.
void way_cache_enable(way_cache_t *cache, bool on);
bool way_cache_enabled(way_cache_t *cache);

// What a person is shown: what the directory holds (counted when asked,
// so another browser's entries count too, and swept when that is over the
// cap), and what this one did.
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

// Removes every entry, and every companion. How many entries went.
int32_t way_cache_clear(way_cache_t *cache);

// ── Entries ─────────────────────────────────────────────────────────────
//
// What is kept is found by its KEY (SCRIPT_CACHE.md § 2-3): the partition
// of the page it was fetched for (way_partition_of: one site's page never
// finds what another's caused to be kept), whether the request sent
// credentials (an anonymous reply never answers a credentialed request, nor
// the reverse: each is another key, so another file), and the address
// asked for. A key's parts are checked against the ones inside, as the
// file's name is a hash. A page of an opaque origin keeps nothing: its
// partition names nothing outside this run.
typedef struct {
    const char *partition;
    bool credentials;
    const char *url;
} way_cache_key_t;

// One kept reply: its key's parts, the address it came from, what it was,
// and what it said about keeping it.
typedef struct {
    char partition[WAY_PARTITION_MAX];
    bool credentials;
    char url[OS64_FETCH_URL_MAX];
    char final[OS64_FETCH_URL_MAX];
    int32_t status;
    char content_type[HTTP_TYPE_MAX];
    char charset[HTTP_CHARSET_MAX];
    way_keep_t keep;
} way_entry_t;

// What a kept reply carries beside its body, each as received: the
// response's header lines ("Name: value\r\n", no Set-Cookie among them),
// the names of the lines too long to read ("name\r\n"), which are what the
// live CORS check reads (way_cors_check), and the request's own lines for
// each header the response's Vary names, which a later request's must
// equal. All empty for way_fetch_whole's replies, whose readers read none.
typedef struct {
    const char *headers;
    size_t headers_len;
    const char *omitted;
    size_t omitted_len;
    const char *vary;
    size_t vary_len;
} way_blocks_t;

// The entry `key` names, with its body of at most `cap` bytes, when there is
// a whole one. `*body` is os64_malloc'd and the caller's, and `blocks`, when
// not NULL, points into the same allocation, after the body. False for none.
bool way_cache_find(way_cache_t *cache, const way_cache_key_t *key, way_entry_t *e, uint8_t **body,
                    size_t *len, way_blocks_t *blocks, size_t cap);
// Keeps an entry (`blocks` may be NULL for none), replacing any for the same
// key. False when it was not kept: turned off, too large, a partition that
// names nothing outside this run, or the disk said no.
bool way_cache_put(way_cache_t *cache, const way_entry_t *e, const way_blocks_t *blocks,
                   const uint8_t *body, size_t len);
// Forgets the entry `key` names, and its companion, if there is one.
void way_cache_drop(way_cache_t *cache, const way_cache_key_t *key);

// ── Companions (BYTECODE_CACHE.md § 5) ──────────────────────────────────
//
// What a face derives from a kept body and keeps beside its entry —
// yonder's is a script's compiled form — under the entry's key and a
// TAG of the face's naming what it was derived from, so one derived from
// another body is a miss. It lives and dies with its entry: dropped with
// it, cleared with it, swept by the same oldest-first rule, counted against
// the same cap, and like an entry no larger than an eighth of it. A face
// reads one only for a body the cache holds (way_whole_t.kept).

// A tag is one line of text, shorter than this.
#define WAY_COMPANION_TAG_MAX 128

// Keeps `bytes` as the companion of `key`'s entry under `tag`, replacing
// any. False when it was not kept: turned off, too large, a tag or address
// that is not one line, or the disk said no.
bool way_cache_companion_put(way_cache_t *cache, const way_cache_key_t *key, const char *tag,
                             const uint8_t *bytes, size_t len);
// The companion of `key`'s entry, when it was kept under `tag` and is whole,
// of at most `cap` bytes: os64_malloc'd, the caller's. False for none.
bool way_cache_companion_get(way_cache_t *cache, const way_cache_key_t *key, const char *tag,
                             uint8_t **bytes, size_t *len, size_t cap);

#pragma GCC visibility pop

#endif
