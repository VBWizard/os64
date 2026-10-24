#ifndef WAY_ORIGIN_H
#define WAY_ORIGIN_H

// Origins and the CORS protocol (MODERN_WEB.md § Slice 2, the origins and
// CORS part; the Fetch Standard, sections cited at each rule): what a page
// may ask of another origin, and what of the answer it may see; and what
// every request says about who is asking (Fetch Metadata, below).
//
// PURE. Nothing here dials, reads a clock or knows libfetch's types: a URL,
// a request's bookkeeping and a response's headers in, a verdict out, so
// tools/test_way_origin_host.sh drives every rule with no network. The fetch
// job (yonder/fetch_job.c) asks these questions hop by hop and does the I/O;
// libdom asks the header questions when a page builds a request. Each rule
// is ONE function, so a ruling changes one place.
//
// A HEADER BLOCK is "Name: value\r\n" lines, in order, NUL-free: the shape
// libfetch's extra_headers already takes. A name is a token and a value
// holds no CR, LF or NUL and does not begin or end with a space or tab
// (way_header_value_ok), so a line can only be read one way.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os64/url.h"

#pragma GCC visibility push(default)

// ── Origins (HTML § origin; URL § origin) ──────────────────────────────

// "scheme://host:port" at its longest.
#define WAY_ORIGIN_TEXT_MAX (OS64_URL_SCHEME_MAX + OS64_URL_HOST_MAX + 16)

// A TUPLE (scheme, host, port) or OPAQUE. An opaque origin is the same
// origin as itself and nothing else, so it carries an identity: two opaque
// origins are the same when both have the same NON-ZERO `id`. Zero is an
// origin equal to nothing at all, the answer for a URL that is not a page's.
typedef struct {
    bool opaque;
    uint64_t id;
    char scheme[OS64_URL_SCHEME_MAX];       // lowercased
    char host[OS64_URL_HOST_MAX];           // lowercased
    uint16_t port;                          // the effective port: the scheme's default filled in
} way_origin_t;

// The origin of `url`: a tuple for the special schemes but file (ftp, http,
// https, ws, wss), a blob: URL's inner http(s) origin, else opaque with
// identity `opaque_id` (0 for "equal to nothing"). Out of memory reads as
// opaque, the answer that grants nothing.
void way_origin_of(const char *url, uint64_t opaque_id, way_origin_t *out);
// The same, for an address already parsed (libfetch hands a hop's so).
void way_origin_of_url(const os64_url_t *url, uint64_t opaque_id, way_origin_t *out);
bool way_origin_same(const way_origin_t *a, const way_origin_t *b);
// "Same site" until the public-suffix list lands (DEBTS): the same scheme and
// the same host, ports aside. FINER than the real partition, so every answer
// it changes is the restrictive one; never "the last two labels", which would
// make evil.co.uk and bank.co.uk one site.
bool way_origin_same_site(const way_origin_t *a, const way_origin_t *b);
// "null" for an opaque origin; "scheme://host" with ":port" only when it is
// not the scheme's default. False when it does not fit in `cap`.
bool way_origin_serialize(const way_origin_t *o, char *out, size_t cap);

// Whether a page of this origin may not fetch plain http (Mixed Content
// § 4.3 "prohibits mixed security contexts"): its origin is https.
bool way_origin_prohibits_mixed(const way_origin_t *page);
// Whether `url` is potentially trustworthy (Secure Contexts' "Is url
// potentially trustworthy?"): about:blank and about:srcdoc, https, wss,
// data:, or an http host in 127.0.0.0/8 written as four decimal parts. The
// NAME localhost is not, since os64 cannot promise it resolves to
// loopback. The browser's one judgement of it: whatever asks whether an
// address is trustworthy asks here, and keeps no copy. (A fetch of
// about:blank is not mixed content, as Mixed Content says; the main fetch
// refuses it all the same, as it refuses every scheme but http(s) and
// data:.)
bool way_url_trustworthy(const char *url);
// The same judgement of a tuple origin (Secure Contexts' "Is origin
// potentially trustworthy?"): https, wss, or a loopback host written as
// way_url_trustworthy reads one. An opaque origin is not.
bool way_origin_trustworthy(const way_origin_t *o);
// Whether a fetch of `url` from `page` is blocked as mixed content: the page
// prohibits it and the URL is not potentially trustworthy.
bool way_mixed_blocked(const way_origin_t *page, const char *url);
// Fetch § 2.9: an http(s) URL on a port the standard blocks, or one whose
// port cannot be read at all.
bool way_port_bad(const char *url);

// ── Methods (Fetch § 2.2.1) ─────────────────────────────────────────────

#define WAY_METHOD_MAX 64

bool way_method_ok(const char *method, size_t len);         // a token
bool way_method_forbidden(const char *method, size_t len);  // CONNECT, TRACE, TRACK
bool way_method_safelisted(const char *method);             // GET, HEAD, POST, case-sensitive
// Upper-cases DELETE, GET, HEAD, OPTIONS, POST and PUT in place, matched
// without case; every other method keeps its spelling.
void way_method_normalize(char *method);

// ── Headers (Fetch § 2.2.2) ─────────────────────────────────────────────

bool way_header_name_ok(const char *name, size_t len);      // a token
// A header value: no NUL, CR or LF, no leading or trailing space or tab.
bool way_header_value_ok(const char *value, size_t len);
// The span of `value` with HTTP white space (space, tab, CR, LF) taken off
// both ends: § 2.2.2 "normalize".
void way_header_value_normalize(const char **value, size_t *len);

bool way_header_forbidden_request(const char *name, size_t nlen, const char *value, size_t vlen);
bool way_header_forbidden_response(const char *name, size_t nlen);
bool way_header_cors_safelisted(const char *name, size_t nlen, const char *value, size_t vlen);
bool way_header_no_cors_safelisted(const char *name, size_t nlen, const char *value, size_t vlen);
bool way_header_no_cors_safelisted_name(const char *name, size_t nlen);
bool way_header_privileged_no_cors(const char *name, size_t nlen);  // Range
bool way_header_request_body(const char *name, size_t nlen);        // Content-{Encoding,Language,Location,Type}
bool way_header_non_wildcard(const char *name, size_t nlen);        // Authorization

// One line of a header block, read in place.
typedef struct {
    const char *name, *value;
    size_t name_len, value_len;
} way_header_t;

// The line at `*at` of `block` (`len` bytes), and `*at` moved past it.
// False at the end, or at a line that is not "Name: value\r\n".
bool way_header_next(const char *block, size_t len, size_t *at, way_header_t *h);

// The CORS-unsafe request-header names of an author header block, lowercased,
// deduplicated, sorted bytewise and joined by "," (§ 4.8's spelling), into
// `out` (NULL counts them). The count of names; -1 when they do not fit in
// `cap`, or memory ran out, which a caller reads as "there are some".
int32_t way_cors_unsafe_names(const char *block, size_t len, char *out, size_t cap);

// The most elements a response's Access-Control-Allow-Methods, -Allow-
// Headers or -Expose-Headers is read for; a longer list is judged as one
// that does not parse (no grant, nothing exposed).
#define WAY_CORS_LIST_MAX 1024

// ── MIME types (MIME Sniffing § 4.4, the strict parser) ────────────────

#define WAY_MIME_ESSENCE_MAX 128
#define WAY_MIME_CHARSET_MAX 64

// `value` parsed as a MIME type: its essence ("type/subtype", lowercased)
// and its charset parameter (as written, "" when none or too long to keep).
// False when it is not a MIME type, or its essence does not fit.
bool way_mime_parse(const char *value, size_t len, char *essence, size_t ecap, char *charset,
                    size_t ccap);

// Fetch § 2.2.2 "extract a MIME type" over a Content-Type's combined value
// (its lines joined with ", "): split as "get, decode, and split" splits,
// so a comma in a quoted value is the value's; each part that parses and
// is not */* replaces the type, a new essence resetting the charset and
// the same essence keeping the earlier charset when it names none. The
// result's essence and charset ("" for none, as way_mime_parse gives it).
// False when no part is a MIME type.
bool way_mime_extract(const char *value, size_t len, char *essence, size_t ecap, char *charset, size_t ccap);
// The same algorithm's MIME type serialized whole (way_mime_serialize's
// rules), every parameter of the part it chose kept, a carried charset
// added: what blob() types a Blob and where formData() finds a boundary.
// False when no part is a MIME type or the result does not fit.
bool way_mime_extract_serialized(const char *value, size_t len, char *out, size_t cap);

// `value` parsed as a MIME type and serialized (MIME Sniffing § 4.6): the
// essence, then every parameter that parses, the first of each name, its
// name lowercased, its value quoted unless it is a token. False when it is
// not a MIME type, it is longer than WAY_MIME_SERIALIZE_IN_MAX, or the
// result does not fit `cap` (WAY_MIME_SERIALIZED_MAX always does).
#define WAY_MIME_SERIALIZE_IN_MAX 512
#define WAY_MIME_SERIALIZED_MAX (WAY_MIME_ESSENCE_MAX + 2 * WAY_MIME_SERIALIZE_IN_MAX + 16)
bool way_mime_serialize(const char *value, size_t len, char *out, size_t cap);
// The same, with the charset parameter's value replaced by `charset` where
// the type has one (XHR's send(): "set contentTypeRecord's
// parameters["charset"] to "UTF-8"", then serialize); a type without one is
// serialized as it is.
bool way_mime_serialize_charset(const char *value, size_t len, const char *charset, char *out, size_t cap);

// ── A request, hop by hop (Fetch § 4.1 main fetch) ─────────────────────

typedef enum { WAY_MODE_CORS = 0, WAY_MODE_SAME_ORIGIN, WAY_MODE_NO_CORS } way_mode_t;
typedef enum { WAY_CREDENTIALS_SAME_ORIGIN = 0, WAY_CREDENTIALS_OMIT, WAY_CREDENTIALS_INCLUDE } way_credentials_t;
typedef enum { WAY_REDIRECT_FOLLOW = 0, WAY_REDIRECT_ERROR, WAY_REDIRECT_MANUAL } way_redirect_t;
typedef enum { WAY_TAINT_BASIC = 0, WAY_TAINT_CORS, WAY_TAINT_OPAQUE } way_tainting_t;
typedef enum { WAY_RTAINT_SAME_ORIGIN = 0, WAY_RTAINT_SAME_SITE, WAY_RTAINT_CROSS_SITE } way_rtaint_t;
// Referrer Policy § 3; the default is strict-origin-when-cross-origin.
typedef enum {
    WAY_POLICY_STRICT_ORIGIN_WHEN_CROSS_ORIGIN = 0, WAY_POLICY_NO_REFERRER,
    WAY_POLICY_NO_REFERRER_WHEN_DOWNGRADE, WAY_POLICY_SAME_ORIGIN, WAY_POLICY_ORIGIN,
    WAY_POLICY_STRICT_ORIGIN, WAY_POLICY_ORIGIN_WHEN_CROSS_ORIGIN, WAY_POLICY_UNSAFE_URL,
} way_policy_t;

// A request's facts and the fetch's bookkeeping about it. The caller fills
// the first group and calls way_request_start; the rest is this file's.
typedef struct {
    way_origin_t origin;                    // the page's, made once with the page
    way_mode_t mode;
    way_credentials_t credentials;
    way_redirect_t redirect;
    way_policy_t policy;
    bool preflight_flag;                    // use-CORS-preflight (an XHR upload listener)
    char method[WAY_METHOD_MAX];            // normalized
    char *headers;                          // the author headers, a block the redirect
    size_t headers_len;                     //   steps edit in place
    bool body;                              // there is a request body

    way_tainting_t tainting;
    way_rtaint_t rtaint;
    way_origin_t current;                   // the origin of the URL list's last URL
    uint32_t urls;                          // the URL list's length
} way_request_t;

// The URL list begins with `url`. Tainting basic, redirect-taint same-origin.
void way_request_start(way_request_t *r, const char *url);

typedef enum {
    WAY_HOP_REFUSED = 0,    // a network error; `why` says which rule
    WAY_HOP_BASIC,          // same origin: no CORS
    WAY_HOP_DATA,           // a data: URL, answered without the wire
    WAY_HOP_OPAQUE,         // no-cors to another origin
    WAY_HOP_CORS,           // the CORS protocol
} way_hop_t;

typedef enum {
    WAY_REFUSE_NONE = 0,
    WAY_REFUSE_PORT,        // § 2.9
    WAY_REFUSE_MIXED,       // mixed content
    WAY_REFUSE_SAME_ORIGIN, // mode same-origin met another origin
    WAY_REFUSE_NO_CORS,     // mode no-cors with a redirect mode but follow
    WAY_REFUSE_SCHEME,      // not http, https or data
    WAY_REFUSE_URL,         // http(s), but an address os64's URL grammar refuses
} way_refusal_t;

const char *way_refusal_name(way_refusal_t why);

// Main fetch for the URL list's last URL, `url`: the response tainting it
// sets, or the rule that refuses it.
way_hop_t way_main_fetch(way_request_t *r, const char *url, way_refusal_t *why);

// § 4.1's preflight branch: a CORS hop whose request is not simple (the
// use-CORS-preflight flag, a method that is not safelisted, or a CORS-unsafe
// header). Whether one is SENT is the cache's question (way_preflight_needed).
bool way_preflight_capable(const way_request_t *r);

// § 4.6: whether this hop sends cookies and hears Set-Cookie.
bool way_credentials_included(const way_request_t *r);

// § 2.2.5 "serializing a request origin": the page's origin, or "null" once
// the redirect-taint is not same-origin.
bool way_request_origin(const way_request_t *r, char *out, size_t cap);

// § 3.2: the Origin header's value for a hop to `url`. False when the hop
// sends none (or it does not fit).
bool way_origin_header(const way_request_t *r, const char *url, char *out, size_t cap);

// Referrer Policy § 8.3: the Referer a hop to `url` carries from the page at
// `referrer` under the request's policy. False for none.
bool way_referrer_for(const way_request_t *r, const char *referrer, const char *url, char *out,
                      size_t cap);

// ── Who is asking, and for what ─────────────────────────────────────────
//
// A browser's request says more than its User-Agent. Some lines describe
// the REQUEST, and every browser computes them the same way: Fetch
// Metadata (the W3C's Fetch Metadata Request Headers: Sec-Fetch-Site,
// -Mode, -Dest and -User) and, on a navigation, Upgrade-Insecure-Requests.
// Those are computed here from facts the request holds. Other lines
// describe the BROWSER, the same on every request it makes: the client
// hints and the Accept-Language a face's agent preset carries
// (way_identity_t). One call writes both, per hop, because both depend on
// where the hop goes: Fetch Metadata goes only to a potentially trustworthy
// URL, and so do the client hints, which are a secure-context feature.

// Sec-Fetch-Dest: what the response will be used as.
typedef enum {
    WAY_DEST_EMPTY = 0,     // fetch(), XMLHttpRequest, and every preflight
    WAY_DEST_DOCUMENT,      // a navigation: its mode is navigate
    WAY_DEST_SCRIPT,
    WAY_DEST_STYLE,
    WAY_DEST_IMAGE,
    WAY_DEST_FONT,
} way_dest_t;

// Sec-Fetch-Site: the request's origin against every URL of its URL list
// so far, the furthest of them; NONE for a navigation the browser's own
// controls started, which stays NONE through its redirects.
typedef enum {
    WAY_SITE_SAME_ORIGIN = 0,
    WAY_SITE_SAME_SITE,
    WAY_SITE_CROSS_SITE,
    WAY_SITE_NONE,
} way_site_t;

// Who started a navigation, which decides its Sec-Fetch-Site and whether
// it carries Sec-Fetch-User.
typedef enum {
    // The browser's own controls: an address typed or given to it, Back,
    // Forward, Reload. Site NONE (Fetch Metadata § 2.3: "explicitly caused
    // by a user's interaction with the user agent"), and a person's.
    WAY_BY_BROWSER = 0,
    // A person, on the page: a link followed, a form sent. The page's site,
    // and a person's.
    WAY_BY_PERSON,
    // The page itself: a refresh, a script. The page's site, and nobody's.
    WAY_BY_PAGE,
} way_initiator_t;

// One request's facts, carried across its hops. Made by one of the two
// calls below, then stepped by way_metadata_hop at every hop before its
// lines are written.
typedef struct {
    way_origin_t origin;    // the request's: the page it is made from
    way_site_t site;
    way_dest_t dest;
    way_mode_t mode;        // unread for a document, whose mode is navigate
    bool activation;        // a person started it: Sec-Fetch-User, on a navigation
    bool preflight;         // a CORS preflight: no client hints go with it
} way_metadata_t;

// A navigation, started by `by` from the page whose origin is `from`
// (unread for WAY_BY_BROWSER).
void way_metadata_navigation(way_metadata_t *m, way_initiator_t by, const way_origin_t *from);
// A page's own request: a picture, a sheet, a script, a font, or a script's
// fetch, made from a page of origin `page`.
void way_metadata_request(way_metadata_t *m, const way_origin_t *page, way_dest_t dest,
                          way_mode_t mode);
// The preflight for the request `m` describes, as it stands: mode cors,
// destination empty, its site the request's.
way_metadata_t way_metadata_preflight(const way_metadata_t *m);
// `to` joins the URL list: the site only moves away from same-origin.
// Asking again for the same `to` changes nothing, so a hop that is
// composed twice is described once.
void way_metadata_hop(way_metadata_t *m, const way_origin_t *to);

// What a face's agent preset says on every request beside its User-Agent,
// verbatim as that browser sends it. Either may be NULL; a typed agent has
// none of it.
typedef struct {
    // The client hints, as whole "Name: value\r\n" lines in the browser's
    // order, sent only to a potentially trustworthy URL and never with a
    // preflight, as Chrome sends them.
    const char *hints;
    const char *language;   // Accept-Language's value, on every request
} way_identity_t;

// The most a hop's identity lines may take: Chrome's are about 300 bytes,
// and the rest is room for a brand list that grows. libfetch's per-hop cap
// (OS64_FETCH_HOP_FIELDS_MAX) holds this much on top of the cookies'
// kilobyte.
#define WAY_IDENTITY_MAX 512

// A hop to `to`'s lines, in this order: the client hints,
// Upgrade-Insecure-Requests (a document's, on every hop), Sec-Fetch-Site,
// -Mode, -User and -Dest (a trustworthy `to` only), and Accept-Language
// unless `language_given` (the request already holds one a page set:
// Fetch appends the browser's only when there is none). `id` may be NULL.
// False when they do not fit in `cap`, with `out` empty.
bool way_identity_lines(const way_identity_t *id, const way_metadata_t *m, const way_origin_t *to,
                        bool language_given, char *out, size_t cap);

// ── A response's headers, for the judgements ────────────────────────────

// What came back. `headers` is a header block in wire order. `omitted`
// names, one "name\r\n" line each, the header lines libfetch heard but could
// not read for their length: a CORS header with such a line is UNREADABLE,
// and judged the restrictive way.
typedef struct {
    int32_t status;
    const char *headers;
    size_t headers_len;
    const char *omitted;
    size_t omitted_len;
} way_response_t;

// § 4.10 CORS check.
bool way_cors_check(const way_request_t *r, const way_response_t *resp);

// ── The preflight (§ 4.8) ───────────────────────────────────────────────

// Access-Control-Request-Method and, when there are unsafe names,
// Access-Control-Request-Headers, as a header block. False when it does
// not fit.
bool way_preflight_lines(const way_request_t *r, char *out, size_t cap);

// The preflight's answer judged for the actual request. True when it may
// go; `*max_age` is then how many seconds the grants may be kept (§ 4.8:
// 5 when not said, at most WAY_PREFLIGHT_AGE_MAX, 0 when unreadable).
bool way_preflight_judge(const way_request_t *r, const way_response_t *resp, uint32_t *max_age);

// ── The preflight cache (§ 4.9) ─────────────────────────────────────────
//
// One per browser, beside the jar, shared by the fetch jobs on every thread.
// Keyed on the partition (the page's site), the serialized request origin,
// the URL, whether the request was credentialed, and a method or a header
// name. Times are the caller's monotonic milliseconds.

#define WAY_PREFLIGHT_AGE_MAX     7200   // seconds: Chrome's two hours
#define WAY_PREFLIGHT_ENTRIES_MAX 256

typedef struct way_preflight way_preflight_t;

way_preflight_t *way_preflight_new(void);   // NULL on no memory
void way_preflight_free(way_preflight_t *cache);

// § 4.4: whether a preflight must be SENT for this capable hop to `url` at
// `now_ms` (a NULL cache answers yes).
bool way_preflight_needed(way_preflight_t *cache, const way_request_t *r, const char *url,
                          uint64_t now_ms);
// § 4.8's last steps: the grants of a judged answer kept for `max_age`
// seconds. A grant that cannot be stored (no memory) is simply not kept.
void way_preflight_store(way_preflight_t *cache, const way_request_t *r, const char *url,
                         const way_response_t *resp, uint32_t max_age, uint64_t now_ms);
// § 4.1 "clear cache entries": every entry for this request's partition,
// origin and `url`.
void way_preflight_clear(way_preflight_t *cache, const way_request_t *r, const char *url);
// Entries held, the expired among them until next asked.
int32_t way_preflight_count(way_preflight_t *cache);

// ── Redirects (§ 4.5) ───────────────────────────────────────────────────

#define WAY_REDIRECTS_MAX 20

typedef enum {
    WAY_FOLLOW_GO = 0,      // the request is rewritten for the next hop
    WAY_FOLLOW_REFUSED,     // a network error
} way_follow_t;

// Whether a status is a redirect status (301, 302, 303, 307, 308).
bool way_redirect_status(int32_t status);

// The redirect from the URL list's last URL, `from`, to `to` (absolute), on
// `status`: the count, the scheme, the downgrade refusal, the method and
// body rewrite with the request-body headers, the Authorization strip, and
// `to` appended to the URL list. `*drop_body` says the body went.
way_follow_t way_redirect_follow(way_request_t *r, int32_t status, const char *from, const char *to,
                                 bool *drop_body);

// ── The response a page sees (§ 2.2.6, § 4.1) ───────────────────────────

typedef enum {
    WAY_TYPE_BASIC = 0, WAY_TYPE_CORS, WAY_TYPE_OPAQUE, WAY_TYPE_OPAQUEREDIRECT, WAY_TYPE_ERROR,
    WAY_TYPE_DEFAULT,       // made by a page's own `new Response`
} way_type_t;

const char *way_type_name(way_type_t type);

// The type the request's tainting gives a response that is not a redirect
// stopped by mode manual.
way_type_t way_response_type(const way_request_t *r);

// The response's header block as the page may see it: everything but
// Set-Cookie for basic, the safelisted and exposed names for cors, nothing
// for the rest. The length written; SIZE_MAX when it does not fit in `cap`.
size_t way_response_headers(const way_request_t *r, way_type_t type, const way_response_t *resp,
                            char *out, size_t cap);

// Whether the page's response has a body at all: not for opaque, opaque-
// redirect or error, not for a HEAD, not for a null body status.
bool way_response_has_body(const way_request_t *r, way_type_t type, int32_t status);

#pragma GCC visibility pop

#endif
