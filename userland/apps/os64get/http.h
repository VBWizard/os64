#ifndef OS64GET_HTTP_H
#define OS64GET_HTTP_H

// http.h — enough of HTTP/1.0 (RFC 1945) to ask a stranger on port 80 for a
// file and know whether what came back is one.
//
// WHY IT IS ITS OWN FILE. os64get speaks two dialects now: the valet's
// (`GET <name>\n`, one length, one CRC, RTL8125.md's 1971 shape) and the
// world's. They share the .part-then-rename discipline and nothing else, so
// they are kept apart here rather than braided through one function. The
// browser campaign's later rungs — a gopher client, the browser itself —
// will want this machinery too; BROWSER.md rules that the shared library is
// a LATER slice with its own review, so what lives here is the SEAM, cut
// where it will eventually be sawn through, and not the library.
//
// WHY PARSING IS SEPARATE FROM THE SOCKET. Every function below that reads
// takes its bytes from an `http_source_fn`, never from a handle. On os64 the
// source is one os64_read of a dialed connection; in tools/test_http_host.sh
// it is a memory buffer handing out one byte at a time, then two, then
// seventeen — because a stream parser's bugs live exactly where a token
// straddles two reads, and a parser that can only be driven by a real
// network is a parser nobody drives across those boundaries.
//
// DELIBERATELY NOT HERE, each a rung of BROWSER.md's ladder or a ruling of
// its own: chunked transfer-encoding, redirects, content codings,
// authentication, cookies, keep-alive, IPv6 literals. What is missing is
// REFUSED BY NAME rather than mis-read: a response this code cannot honestly
// turn into a file must never become a file.
//
// TLS IS NOT HERE EITHER AND NEVER WILL BE — os64 borrows a TLS when its day
// comes, because thirty years of side-channel and oracle attacks teach no
// kernel lessons (BROWSER.md's first ruling). What this file DOES know is
// that an https URL is a perfectly ordinary address which some other machine
// may be willing to fetch on our behalf: `http_url_parse` reads both schemes
// and `http_request` can address a proxy, and the decision about whether a
// given https URL is reachable belongs to the caller, which is the only
// layer that knows whether a proxy is configured.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os64/resolve.h"   // OS64_RESOLVE_NAME_MAX — DNS's own ceiling

// A header line longer than this is DROPPED, not fatal. The headers this
// code acts on are all short (a length, a coding name, a URL), so dropping a
// two-kilobyte one costs nothing an answer depends on — while refusing the
// whole response over somebody's enormous Set-Cookie would cost the fetch.
#define HTTP_LINE_MAX      2048
// ...but the DROPPING is bounded, or a peer that talks forever and never
// sends a newline is read forever. HTTP_HEAD_MAX is the whole head's budget
// AND, spent down line by line, each line's — because a cap checked only
// between lines is a cap the endless line never returns to be measured by.
// HTTP_HEADERS_MAX bounds the other shape of the same attack: short lines,
// endlessly many. Both refuse loudly.
#define HTTP_HEAD_MAX      65536
#define HTTP_HEADERS_MAX   128

#define HTTP_HOST_MAX      (OS64_RESOLVE_NAME_MAX + 1)
#define HTTP_PATH_MAX      1024
#define HTTP_SCHEME_MAX    16
#define HTTP_REASON_MAX    64
#define HTTP_TOKEN_MAX     32     // a coding name: "chunked", "gzip", "identity"

// The read buffer. Big enough that a whole response head usually arrives in
// one os64_read, and the leftover tail of that read is where the body starts
// — which is why the stream, not the caller, owns it (see http_stream_read).
#define HTTP_BUF_SIZE      8192

// ── A URL, taken apart ──────────────────────────────────────────────────

typedef struct {
    char     scheme[HTTP_SCHEME_MAX];   // lowercased; filled even when refused
    char     host[HTTP_HOST_MAX];       // no port, no userinfo, no brackets
    char     path[HTTP_PATH_MAX];       // always starts '/'; query kept, fragment dropped
    uint16_t port;                      // the scheme's default (80/443) unless the URL said so
    bool     explicitPort;              // did it? (decides the Host: header's shape)
} http_url_t;

typedef enum {
    HTTP_URL_OK = 0,
    HTTP_URL_NOT_A_URL,   // no "scheme://" — this operand names a valet file
    HTTP_URL_SCHEME,      // a URL, but neither http nor https
    HTTP_URL_NO_HOST,
    HTTP_URL_HOST_CHARS,  // userinfo, an IPv6 literal, or bytes a host cannot hold
    HTTP_URL_PATH_CHARS,  // a space or a control byte: the request-splitting shape
    HTTP_URL_PORT,
    HTTP_URL_TOO_LONG
} http_url_result_t;

// Take `url` apart. HTTP_URL_NOT_A_URL is not a complaint: it is how the
// caller learns the operand was a bare name meant for the valet, which is
// why it is a result rather than an error.
http_url_result_t http_url_parse(const char *url, http_url_t *out);

// The refusal in words, one vocabulary for every caller (os64_dial_reason's
// shape, and for its reason: a program should be able to print something a
// person can act on without a debugger). HTTP_URL_SCHEME's sentence names no
// scheme — the caller holds the parsed one and owes https:// a better
// sentence than a generic refusal.
const char *http_url_reason(http_url_result_t rc);

// Build the request. HTTP/1.0 on purpose: a server may not answer a 1.0
// client with chunked encoding, so asking in 1.0 is what keeps this rung of
// the ladder honestly self-contained. `Host:` is 1.1's header, sent anyway
// because name-based virtual hosting means a request without it reaches
// whatever the address answers with by default — usually not the page asked
// for.
//
// `absoluteForm` puts the WHOLE URL in the request line instead of the path,
// which is what a proxy needs to know which origin the request is for (RFC
// 7230 §5.3.2, and the CERN proxy's shape since 1994). Returns false only if
// the request will not fit.
bool http_request(char *out, size_t cap, const http_url_t *url, bool absoluteForm);

// Write a parsed URL back out as text — the inverse of http_url_parse for
// everything the parse kept, so a fragment and a redundant default port are
// gone by construction.
bool http_url_render(const http_url_t *url, char *out, size_t cap);


// Spell a `Location:` as a WHOLE address, given the URL it arrived from.
// Handles the two forms a redirect actually uses: an absolute URL (copied
// through) and an absolute path (joined to the origin that just answered).
// Returns false for a genuinely relative reference (`page.html`, `../up`) —
// resolving those is RFC 3986's full algorithm and belongs to the increment
// that follows redirects for real; saying "I cannot spell this one" is the
// honest answer until then, and a better one than a plausible guess.
//
// This exists for a DIAGNOSTIC: a message telling someone to fetch an
// address owes them one they can actually type.
bool http_url_absolute(const http_url_t *base, const char *location,
                       char *out, size_t cap);
// ── A stream of bytes that arrives in whatever pieces it likes ──────────

// Fill `buf` with up to `cap` bytes: > 0 got some, 0 the peer is done, < 0
// the connection broke. os64_read's contract exactly, so the os64 source is
// a two-line shim.
typedef int64_t (*http_source_fn)(void *ctx, void *buf, size_t cap);

typedef struct {
    http_source_fn source;
    void          *ctx;
    uint8_t        buf[HTTP_BUF_SIZE];
    size_t         have;      // bytes in buf
    size_t         next;      // how many of them are spent
    bool           ended;     // the source answered 0
    bool           failed;    // the source answered < 0
} http_stream_t;

void http_stream_init(http_stream_t *s, http_source_fn source, void *ctx);

// ── The answer ──────────────────────────────────────────────────────────

typedef struct {
    int32_t  status;                          // 200, 404, 301...
    char     reason[HTTP_REASON_MAX];         // the server's own words
    bool     hasLength;
    uint64_t length;                          // valid only when hasLength
    char     transferEncoding[HTTP_TOKEN_MAX];// "" = none said
    char     contentEncoding[HTTP_TOKEN_MAX]; // "" = identity, the only one this speaks
    char     location[HTTP_LINE_MAX];         // "" = none said
} http_response_t;

typedef enum {
    HTTP_HEAD_OK = 0,
    HTTP_HEAD_SOURCE,     // the connection died before the head was whole
    HTTP_HEAD_STATUS,     // the first line is not an HTTP status line
    HTTP_HEAD_SYNTAX,     // a header line is not "Name: value"
    HTTP_HEAD_TOO_MUCH,   // more head than this will read
    HTTP_HEAD_CONFLICT    // two different answers to one question
} http_head_result_t;

// Read the status line and every header, stopping on the blank line that
// ends them. On return the stream is positioned at the first byte of the
// body — including any body bytes that arrived in the same read as the head.
http_head_result_t http_head_read(http_stream_t *s, http_response_t *out);

const char *http_head_reason(http_head_result_t rc);

// Body bytes, in whatever quantity is to hand: > 0 got some, 0 the peer is
// done, < 0 the connection broke. Serves what the head-read over-read first,
// so no byte of the body is ever lost to the header buffer — a stream has no
// message boundaries, and this is where that fact is paid for.
int64_t http_stream_read(http_stream_t *s, void *out, size_t cap);

#endif // OS64GET_HTTP_H
