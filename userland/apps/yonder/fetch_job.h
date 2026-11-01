#ifndef YONDER_FETCH_JOB_H
#define YONDER_FETCH_JOB_H

// A script's request (fetch() and XMLHttpRequest's),
// run by the Fetch standard's rules: ONE path, whether it runs on a pool
// worker or, for a synchronous XHR, inline on the window thread. The job
// drives the redirect loop itself, one libfetch fetch per hop, and asks
// libway's origin.c every question the standard asks of a hop (MODERN_WEB.md
// § Slice 2's origins and CORS): the tainting, the preflight, the Origin
// header, whether cookies go and Set-Cookie is heard, the Authorization
// strip, the CORS check, and the Fetch Metadata the hop carries beside the
// agent's preset lines. What it hands back is the FILTERED response: a
// page never receives a header or a byte it may not see. A network error's
// `why` is the page record's (yonder's request_record); the answer the
// binding is handed carries none, and the page's TypeError says only
// "Failed to fetch".

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bytecode.h"
#include "fetch/fetch.h"
#include "jobs.h"
#include "way/cache.h"
#include "way/jar.h"
#include "way/origin.h"

// A body's cap (fetch's max_body), and a response head's: the standard's
// headers are kept whole or the fetch fails and says so.
#define YONDER_FETCH_BODY_MAX ((uint64_t)16 * 1024 * 1024)
#define YONDER_FETCH_HEAD_MAX ((size_t)64 * 1024)
// A data: URL is answered without the wire, and its text is bounded so a
// script's string cannot become an allocation past the body's cap: base64
// of the cap, and a little room for its type.
#define YONDER_FETCH_DATA_URL_MAX ((size_t)(YONDER_FETCH_BODY_MAX / 3 * 4 + 4096))
// A job's peak: the body, the head and the request's own bytes beside it.
#define YONDER_FETCH_RESERVE ((size_t)YONDER_FETCH_BODY_MAX * 2 + ((size_t)4 << 20))
// A module's job's: its compiled form read beside them.
#define YONDER_FETCH_MODULE_RESERVE (YONDER_FETCH_RESERVE + SCRIPT_BYTECODE_MAX)

typedef struct {
    uint32_t kind;                  // YONDER_JOB_FETCH
    uint64_t page;                  // the page it belongs to (the script host's serial)
    uint32_t token;                 // which of that page's requests
    const char *agent;              // the browser's, read-only for the run
    const way_identity_t *identity; // the agent's preset lines, NULL for none
    char *url;                      // os64_malloc'd: the request's address, fragment and all
    // The request's facts (way/origin.h): the page's origin, mode,
    // credentials, redirect mode, referrer policy, the use-CORS-preflight
    // flag and the method. Its `headers` is the job's own block below, and
    // its bookkeeping is the run's.
    way_request_t request;
    char *headers;                  // os64_malloc'd author header block, NUL-terminated
    size_t headers_len;
    uint8_t *body;                  // os64_malloc'd request body, or NULL
    size_t body_len;
    // The page the request is made from, which its Referer names under its
    // policy; "" for none (`referrer: ""`).
    char referrer[OS64_FETCH_URL_MAX];
    uint64_t max_body;              // 0: YONDER_FETCH_BODY_MAX
    way_jar_t *jar;                 // the browser's cookies; NULL keeps none and sends none
    way_preflight_t *preflight;     // the browser's preflight cache; NULL asks every time
    // The browser's HTTP cache (SCRIPT_CACHE.md), NULL to keep and read
    // nothing, and how this request uses it.
    way_cache_t *cache;
    // The page's mode, which the run makes what the request's own lines
    // say (a precondition, or Cache-Control no-store/no-cache).
    way_cache_mode_t cache_mode;
    // A module script's fetch: its compiled form is read beside the entry
    // that holds its body (BYTECODE_CACHE.md § 9).
    bool module;
    // When the window handed it to the pool, on os64_micros' clock; 0
    // when it was not (a synchronous request runs where it is made).
    int64_t submitted_us;
} yonder_fetch_job_t;

typedef struct {
    bool ok;                        // false: a network error, which the page sees as a TypeError
    // Why it is a network error, in the page record's words: a refusal of
    // origin.c's, the CORS check, the fetch's own reason.
    char why[256];
    bool mixed;                     // refused as mixed content (the page record says so)
    way_type_t type;
    int32_t status;                 // 0 for opaque, opaqueredirect and error
    char status_text[HTTP_REASON_MAX];
    char *url;                      // os64_malloc'd: the response's URL without its fragment ("" for opaque)
    bool redirected;
    uint32_t hops;                  // redirects followed: each was a dial, and the record counts them
    char *headers;                  // os64_malloc'd filtered header block
    size_t headers_len;
    bool has_body;                  // false: the body is null
    uint8_t *body;                  // os64_malloc'd
    size_t body_len;
    way_cookies_short_t cookies_short;  // what its hops left out for want of room (way/jar.h)
    // How long the fetch took, from the job's start to its answer, in
    // microseconds; -1 when the clock could not say. The page record's
    // number for what a request cost. Beside it, how long the job waited
    // for a worker (-1: not known), and when its answer was ready, on
    // os64_micros' clock (-1: not known), from which the record says how
    // long the answer then waited for the window.
    int64_t elapsed_us;
    int64_t queued_us;
    int64_t finished_us;
    // Where the answer came from: the network, or the cache, fresh or
    // after a 304 (way_served_t; the page record says which).
    way_served_t served;
    // A module's compiled form, when the cache holds the body it answered
    // with: the blob kept for that body (NULL for none) and where a compile
    // of it is kept. NULL for any other answer.
    yonder_bytecode_t *bytecode;
} yonder_fetched_t;

/* Native image consumer: applies the same hop/CORS checks but retains an
 * opaque body's bytes for decoding. Its product must not enter script-response
 * delivery. type still carries the origin-clean verdict (opaque taints). */
int64_t yonder_fetch_image_run(void *job,bool (*cancelled)(void *),void *ctx,void **out);
int64_t yonder_fetch_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out);
// Frees the job and the product (either may be NULL).
void yonder_fetch_release(void *job, void *product);
// Frees a product alone.
void yonder_fetched_free(yonder_fetched_t *got);

// Fetch § 6's data: URL processor, pure for the host's tests: the body
// (os64_malloc'd) and its Content-Type. False when the URL is not one, its
// base64 is not, or the body would pass `max_body`.
bool yonder_fetch_data_url(const char *url, uint64_t max_body, uint8_t **body, size_t *len,
                           char *type, size_t type_cap);

#endif
