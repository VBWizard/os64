#ifndef FETCH_FETCH_H
#define FETCH_FETCH_H

// fetch.h — a URL in, the page's bytes out. LIBFETCH.md is the design
// record; this file is the contract every fetcher on os64 shares.
//
// THE SHAPE: `open` dials, follows redirects and reads the head, and it
// BLOCKS — a fetch is a thread's work. `read` hands back body bytes with the
// chunking and the gzip taken off. `close` releases everything. One object
// per fetch, no static state anywhere in the library, so twenty fetches on
// twenty threads share not one byte.
//
// POLICY ABOUT THE WIRE IS THE LIBRARY'S; POLICY ABOUT THIS MACHINE IS THE
// CALLER'S. The library knows what an HTTP reply means, where a Location
// points, which redirects are safe to follow by default, what the
// environment's proxy settings say, and how long to wait for a silent
// peer. The caller knows whether a 404 is a failure (a browser shows the
// page; a downloader refuses it), whether a person may follow a downgrade,
// and what to say about any of it — which is why nothing in here prints.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os64/url.h"
#include "fetch/http.h"
#include "gzip/gzip.h"
#include "tls/tls.h"

typedef struct os64_fetch os64_fetch_t;   // opaque; one fetch, one object

// ── Why a fetch stopped ─────────────────────────────────────────────────
//
// Every value names a cause a caller can act on; `os64_fetch_detail` holds
// the numbers behind it and `os64_fetch_reason` one sentence about it.
typedef enum {
    OS64_FETCH_OK = 0,             // open: head read; read: the body ended WHOLE
    OS64_FETCH_BAD_URL,            // the address does not parse (detail.url says why)
    OS64_FETCH_UNSUPPORTED_SCHEME, // parses, but not http or https (detail.scheme)
    OS64_FETCH_DIAL_FAILED,        // no road to the peer (detail.dial, os64_dial_reason)
    OS64_FETCH_PROXY_BAD,          // the proxy setting that would carry it is unusable (detail.why)
    OS64_FETCH_TLS_FAILED,         // handshake, verification, or the trust store (detail.tls*, store*)
    OS64_FETCH_REQUEST_FAILED,     // could not send the request, or it does not fit (detail.why)
    OS64_FETCH_BAD_HEAD,           // the reply is not HTTP (detail.head), or its chunk framing
                                   // stopped being HTTP mid-body (detail.body)
    OS64_FETCH_UNSUPPORTED,        // a framing, coding, or 101 this code cannot honestly undo
    OS64_FETCH_SILENT,             // the idle deadline passed (detail.silent_after_body)
    OS64_FETCH_REDIRECT_STOPPED,   // a hop verdict said stop; the head is the redirect's
    OS64_FETCH_TOO_MANY_HOPS,      // past max_hops; the head is the last redirect's
    OS64_FETCH_CUT,                // the peer closed before the framing was satisfied
    OS64_FETCH_BROKE,              // the connection failed mid-body
    OS64_FETCH_CORRUPT,            // the gzip coding did not decode (detail.gzip)
    OS64_FETCH_LIMIT,              // max_body, or the gzip expansion caps
    OS64_FETCH_INTERRUPTED,        // the caller's predicate said so. (A wait that a signal
                                   // interrupts is RETRIED under the same deadline — a caught
                                   // SIGWINCH must not abort a page load — so the predicate
                                   // is the only way to say "stop"; that is why it exists.)
    OS64_FETCH_NO_MEMORY,
} os64_fetch_status_t;

const char *os64_fetch_status_name(os64_fetch_status_t status);

// ── Redirects ───────────────────────────────────────────────────────────

// What a hop IS, worked out by the library before anybody decides anything:
// the Location resolved against the address that answered, parsed, and
// judged. WHOLE is a target with a road to it; every other kind names what
// is wrong with it. The default verdict follows WHOLE and stops on the rest.
typedef enum {
    OS64_FETCH_HOP_WHOLE = 0,   // a whole, fetchable, different address, and a proxy or a direct road
    OS64_FETCH_HOP_NONE,        // the redirect does not say where to
    OS64_FETCH_HOP_TOO_LONG,    // the address it spells is longer than this will hold
    OS64_FETCH_HOP_UNUSABLE,    // it spells something that is not a usable address (detail: parse)
    OS64_FETCH_HOP_SCHEME,      // an address of a kind this library does not fetch (mailto:, ftp://)
    OS64_FETCH_HOP_DOWNGRADE,   // an HTTPS origin redirected to unencrypted HTTP
    OS64_FETCH_HOP_PROXY,       // the proxy setting that would carry it is unusable (why)
    OS64_FETCH_HOP_SELF,        // it points back at the address that just answered
} os64_fetch_hop_kind_t;

// The caller's say. DEFAULT = the library's policy (follow WHOLE, stop on
// the rest, within max_hops). FOLLOW on a DOWNGRADE or a SELF overrides the
// policy — a person at a keyboard may choose what a script may not; FOLLOW
// on a hop with no address to follow (NONE, TOO_LONG, UNUSABLE, SCHEME,
// PROXY) is STOP, because there is nowhere to go. STOP ends the fetch with
// the redirect as its final head, body readable.
typedef enum {
    OS64_FETCH_HOP_DEFAULT = 0,
    OS64_FETCH_HOP_FOLLOW,
    OS64_FETCH_HOP_STOP,
} os64_fetch_verdict_t;

#define OS64_FETCH_URL_MAX HTTP_URL_TEXT_MAX
#define OS64_FETCH_WHY_MAX 200

typedef struct {
    os64_fetch_hop_kind_t kind;
    int32_t  status;                  // the 3xx that sent us
    char     reason[HTTP_REASON_MAX]; // ...and its own words for it, "" if none
    uint32_t number;                  // this would be hop N (1 = the first redirect)
    char     whole[OS64_FETCH_URL_MAX];   // the target spelled out; "" for NONE and TOO_LONG
    os64_url_t target;                // WHOLE / DOWNGRADE / SELF: parsed, port filled in from the scheme
    os64_url_result_t parse;          // UNUSABLE: what the parser objected to
    bool     via_proxy;               // WHOLE: who carries the next hop
    char     proxy_host[OS64_URL_HOST_MAX];
    uint16_t proxy_port;
    bool     from_via_proxy;          // ...and who carried the hop that answered: a caller
    char     from_proxy_host[OS64_URL_HOST_MAX];   // warning about a proxied first leg needs
    uint16_t from_proxy_port;         // it even when the final head went direct
    char     why[OS64_FETCH_WHY_MAX]; // PROXY: what is wrong with the setting
} os64_fetch_hop_t;

// ── What came back ──────────────────────────────────────────────────────

// Valid from the moment `open` returns with a head (any status whose head
// was read: OK, REDIRECT_STOPPED, TOO_MANY_HOPS) until `close`. Every
// string is storage inside the fetch object — nothing here outlives it.
typedef struct {
    int32_t  status;                          // 200, 404, 301...
    char     reason[HTTP_REASON_MAX];         // the server's own words, "" if none
    char     content_type[HTTP_TYPE_MAX];     // lowercased media type, "" if not said
    char     charset[HTTP_CHARSET_MAX];       // lowercased charset parameter, "" if not said
    bool     has_length;
    uint64_t length;                          // the WIRE's count — gzip output is larger
    char     encoding[HTTP_TOKEN_MAX];        // the content coding the wire used; "" = identity
    bool     has_location;                    // a Location line was seen, even an empty one
    const char *location;                     // valid until close
    os64_url_t url;                           // where the body came from, port filled in
    char     url_text[OS64_FETCH_URL_MAX];    // the same, spelled — the browser's base URL
    uint32_t hops;                            // redirects followed to get here
    bool     encrypted;                       // https, and not through a proxy
    bool     via_proxy;                       // the facts behind "this is not end-to-end"
    char     proxy_host[OS64_URL_HOST_MAX];
    uint16_t proxy_port;
} os64_fetch_head_t;

// Counters a caller reads between reads, for a meter or a status line.
typedef struct {
    uint64_t wire;        // body bytes the framing handed over
    uint64_t produced;    // bytes handed to the caller (after gzip)
    uint64_t length;      // the promised wire length, when has_length
    bool     has_length;
} os64_fetch_progress_t;

// The numbers behind a status. Only the fields the status names are
// meaningful; the rest are whatever the fetch left there.
typedef struct {
    os64_url_result_t url;                    // BAD_URL
    char     scheme[OS64_URL_SCHEME_MAX];     // UNSUPPORTED_SCHEME: which one
    int64_t  dial;                            // DIAL_FAILED: os64_dial's answer
    bool     dial_was_proxy;                  // DIAL_FAILED / TLS_FAILED: the peer was the proxy, not the origin
    char     dial_host[OS64_URL_HOST_MAX];    // DIAL_FAILED / TLS_FAILED: the peer asked
    uint16_t dial_port;
    uint32_t dial_hop;                        // DIAL_FAILED / TLS_FAILED: 0 = the typed address, else which hop
    os64_tls_status_t tls;                    // TLS_FAILED
    os64_tls_policy_reason_t tls_policy;
    int      tls_engine;
    bool     store_failed;                    // TLS_FAILED: the trust store, not the handshake
    os64_tls_store_status_t store;
    os64_tls_store_report_t store_report;
    http_head_result_t head;                  // BAD_HEAD / UNSUPPORTED (head-shaped)
    http_body_result_t body;                  // CUT / BROKE / BAD_HEAD (body-shaped)
    os64_gzip_status_t gzip;                  // CORRUPT / LIMIT / UNSUPPORTED (coding-shaped)
    uint64_t gzip_limit;                      // LIMIT: the cap the decoder was given
    char     unsupported[HTTP_TOKEN_MAX];     // UNSUPPORTED: the coding or framing named
    os64_fetch_hop_t hop;                     // REDIRECT_STOPPED / TOO_MANY_HOPS
    char     why[OS64_FETCH_WHY_MAX];         // PROXY_BAD / REQUEST_FAILED: the sentence
    bool     silent_after_body;               // SILENT: mid-body (true) or before the head
} os64_fetch_detail_t;

// ── Options ─────────────────────────────────────────────────────────────

// The gzip expansion caps: a reply that claims gzip may not expand past
// RATIO_MAX times its wire length (with a FLOOR so a tiny reply can still
// unpack to something) nor past OUTPUT_MAX at all. A chunked or close-framed
// gzip body, whose wire length is unknown in advance, gets OUTPUT_MAX.
#define OS64_FETCH_GZIP_OUTPUT_FLOOR (1ull * 1024ull * 1024ull)
#define OS64_FETCH_GZIP_OUTPUT_MAX   (16ull * 1024ull * 1024ull)
#define OS64_FETCH_GZIP_RATIO_MAX    100ull

#define OS64_FETCH_HOPS_DEFAULT 5
#define OS64_FETCH_AGENT_MAX    128
#define OS64_FETCH_ACCEPT_MAX   128
#define OS64_FETCH_EXTRA_MAX    1024

typedef struct {
    // Request headers, each optional (NULL = not sent). They are COPIED at
    // open, so the caller's strings need not outlive the call. Every byte
    // is judged by http.h's field-byte rule and a bad one is REQUEST_FAILED.
    const char *user_agent;
    const char *accept;
    const char *extra_headers;     // "Name: value\r\n" lines, already terminated
    // The trust store, for https. Caller-owned when given and never freed
    // here. NULL = the library loads the store on the first https hop and
    // frees it at close (the "trust once per invocation" rule of #89).
    os64_tls_trust *trust;
    uint64_t max_body;             // decoded bytes; 0 = no cap
    uint32_t idle_ms;              // 0 = 30 seconds
    uint32_t max_hops;             // 0 = OS64_FETCH_HOPS_DEFAULT
    bool     no_proxy;             // ignore the environment's proxy settings
    // Fires on EVERY hop, followed or not, with the facts worked out. NULL =
    // the default verdict throughout.
    os64_fetch_verdict_t (*on_hop)(void *ctx, const os64_fetch_hop_t *hop);
    // Asked before every wait and after every interrupted one; a yes ends
    // the fetch as INTERRUPTED. NULL = never (interrupted waits are retried).
    bool (*cancelled)(void *ctx);
    void *ctx;
} os64_fetch_options_t;

// ── The calls ───────────────────────────────────────────────────────────

// Dial, follow redirects, read the head. Returns NULL for no memory ONLY;
// every other outcome is an object whose status says what happened, so the
// caller has the detail to explain it. Whatever this returns that is not
// NULL, close it.
os64_fetch_t *os64_fetch_open(const char *url, const os64_fetch_options_t *opt);

// The LAST outcome: open's until the first read, then each read's.
os64_fetch_status_t os64_fetch_status(const os64_fetch_t *f);
const os64_fetch_detail_t *os64_fetch_detail(const os64_fetch_t *f);

// NULL until a head was read (see os64_fetch_head_t).
const os64_fetch_head_t *os64_fetch_head(const os64_fetch_t *f);

// Body bytes, chunking and gzip removed: > 0 got some; 0 the body is over,
// and `status` says whether it ENDED (OK: every promised byte arrived) or
// was merely stopped (CUT); < 0 it cannot go on, and `status` says why.
// A close-framed body cannot tell ENDED from stopped and reports OK. Once
// it has answered 0 or < 0 it answers the same thing again. Never hands
// back a byte past max_body: the cap is refused BEFORE the byte that
// would cross it is produced. A zero `cap` is the one exception to "< 0
// is final": it is refused with -1 and nothing changes — status, state,
// the next read — because 0 would have read as a body that ended whole.
int64_t os64_fetch_read(os64_fetch_t *f, void *buf, size_t cap);

const os64_fetch_progress_t *os64_fetch_progress(const os64_fetch_t *f);

// One sentence about the last outcome, in the library's words — for the
// caller that has nothing more specific to say. Storage inside the object.
const char *os64_fetch_reason(os64_fetch_t *f);

// Release the connection (a bounded TLS closure if the body ended whole),
// the decoder, the store if this loaded it, and the object. Idempotent on
// the connection; the object is gone.
void os64_fetch_close(os64_fetch_t *f);

#endif // FETCH_FETCH_H
