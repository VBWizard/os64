#ifndef OS64GET_HTTP_H
#define OS64GET_HTTP_H

// http.h — enough of HTTP/1.1 (RFC 9112) to ask a stranger on port 80 for a
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
// its own: content codings, transfer codings other than chunked,
// authentication, cookies, keep-alive, IPv6 literals. What is missing is
// REFUSED BY NAME rather than mis-read: a response this code cannot honestly
// turn into a file must never become a file.
//
// WHAT A REDIRECT MEANS IS NOT HERE EITHER, and that split is on purpose:
// this file answers "where does that Location point", which is arithmetic on
// addresses (http_url_absolute), and the caller answers "and may I go
// there", which depends on the machine — whether a proxy is configured, how
// many hops have been spent, whether the answer points back at itself.
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

// A header line longer than this is DROPPED rather than fatal — but only
// after the line's NAME has been read out of the truncated prefix and found
// to be one nothing depends on. Refusing every over-long line would cost the
// fetch over somebody's enormous Set-Cookie, which os64get does not even
// read; dropping one blindly would let a server hide `Transfer-Encoding:
// chunked` behind three kilobytes of legal whitespace and get raw chunk
// framing published as the file. See overlong_verdict: the three headers
// that decide how a body is framed and coded refuse the reply instead.
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

// The longest address http_url_absolute can spell: a base's scheme and host,
// with a whole Location header joined onto its path. Every buffer an
// absolute-form address is written into is this size, so the ones that must
// agree cannot drift apart.
#define HTTP_URL_TEXT_MAX  (HTTP_SCHEME_MAX + HTTP_HOST_MAX + HTTP_PATH_MAX + HTTP_LINE_MAX + 16)

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

// Build the request. HTTP/1.1, and the version is a promise about what the
// reply may look like: a 1.1 client must read chunked framing, because a 1.1
// server may answer any request with it (a 1.0 client is owed a length or a
// close, which is what this asked for until http_body_read learned chunks).
// `Connection: close` is sent because keep-alive is not spoken here — one
// request, one connection, and a reply framed by neither length nor chunks
// still ends at the close. `Host:` because name-based virtual hosting means a
// request without it reaches whatever the address answers with by default —
// usually not the page asked for.
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


// Spell a `Location:` as a WHOLE address, given the URL it arrived from —
// RFC 3986 §5.2's reference resolution, which is what a redirect header is
// written in. Every form a server actually sends resolves: an absolute URL
// (`http://other/x`), a scheme-relative one (`//cdn/x`), an absolute path
// (`/login`), a path relative to the page (`page.html`, `../up/`), a
// query-only reference (`?page=2`) and a fragment-only one (which names the
// page it came from). `.` and `..` are resolved away where a merge with the
// base's path created them.
//
// A reference that names its OWN scheme is copied through untouched, even
// one this program could never fetch (`mailto:`, `ftp://`) — what the
// address IS and whether to go there are different questions, and the second
// belongs to the caller.
//
// Returns false only when the answer will not fit in `cap` or the reference
// is empty. An empty `Location` names no address at all: RFC 3986 would read
// it as "the page you already have", and a redirect to the page you already
// have is a server that has lost its place.
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
    bool     hasLocation;                     // a Location line was seen, even an empty one
    char     location[HTTP_LINE_MAX];         // valid only when hasLocation; "" = it named nowhere
} http_response_t;

typedef enum {
    HTTP_HEAD_OK = 0,
    HTTP_HEAD_SOURCE,     // the connection died before the head was whole
    HTTP_HEAD_STATUS,     // the first line is not an HTTP status line
    HTTP_HEAD_SYNTAX,     // a header line is not "Name: value"
    HTTP_HEAD_TOO_MUCH,   // more head than this will read
    HTTP_HEAD_CONFLICT,   // two different answers to one question
    HTTP_HEAD_FRAMING,    // a header deciding how to read the body was unreadable
    HTTP_HEAD_SWITCHED    // 101: the bytes that follow are not HTTP any more
} http_head_result_t;

// Read the status line and every header, stopping on the blank line that
// ends them. On return the stream is positioned at the first byte of the
// body — including any body bytes that arrived in the same read as the head.
http_head_result_t http_head_read(http_stream_t *s, http_response_t *out);

const char *http_head_reason(http_head_result_t rc);

// Raw bytes after the head, in whatever quantity is to hand: > 0 got some, 0
// the peer is done, < 0 the connection broke. Serves what the head-read
// over-read first, so no byte of the body is ever lost to the header buffer —
// a stream has no message boundaries, and this is where that fact is paid
// for. The body reader below is built on it; a caller wanting the BODY rather
// than the wire goes through that.
int64_t http_stream_read(http_stream_t *s, void *out, size_t cap);

// ── The body, with its framing taken off ────────────────────────────────
//
// HTTP has three ways of saying where a body ends, and a reply picks one:
// a Content-Length (count the bytes), chunked transfer coding (the body
// arrives as sized pieces and a zero-sized piece is the end — HTTP/1.1's
// answer for a page whose length is not known when its head is sent), or
// neither, in which case the close IS the end (HTTP/1.0's original framing,
// RFC 1945 §7.2.2). The first two can tell a complete body from a cut one;
// the third cannot, which is the whole reason the other two were invented.

typedef enum {
    HTTP_FRAMING_CLOSE = 0,
    HTTP_FRAMING_LENGTH,
    HTTP_FRAMING_CHUNKED
} http_framing_t;

typedef enum {
    HTTP_BODY_OPEN = 0,   // still reading
    HTTP_BODY_DONE,       // the framing was satisfied: every byte promised arrived
    HTTP_BODY_CUT,        // the peer closed before it had
    HTTP_BODY_BROKE,      // the connection failed
    HTTP_BODY_SYNTAX,     // chunk framing that is not chunk framing
    HTTP_BODY_TOO_MUCH    // a chunk-size line or trailer section beyond the caps
} http_body_result_t;

typedef struct {
    http_stream_t     *s;
    http_framing_t     framing;
    http_body_result_t result;
    uint64_t           delivered;   // body bytes handed to the caller so far
    uint64_t           remaining;   // LENGTH: of the body; CHUNKED: of this chunk
    uint8_t            state;       // CHUNKED's position (private to http.c)
    size_t             trailerUsed; // CHUNKED: trailer bytes read, against HTTP_HEAD_MAX
    size_t             trailers;    // CHUNKED: trailer lines read, against HTTP_HEADERS_MAX
} http_body_t;

// Decide the framing from the head and position the reader at the body.
// Returns false when the reply names a transfer coding this code cannot undo
// (anything but chunked or identity) — the caller refuses that by name, and
// nothing is read. A Content-Length beside a transfer coding never reaches
// here; http_head_read calls it a CONFLICT.
bool http_body_open(http_body_t *b, http_stream_t *s, const http_response_t *reply);

// Body bytes, framing removed: > 0 got some; 0 the body is over, and
// `b->result` says whether it ended (DONE) or was merely stopped (CUT); < 0
// it cannot go on, and `b->result` says why (BROKE, SYNTAX, TOO_MUCH). Never
// hands back a byte of chunk framing, and never returns 0 for a chunked body
// before its terminating chunk and trailer section have been read — so a
// caller that sees 0 with DONE has a WHOLE file.
int64_t http_body_read(http_body_t *b, void *out, size_t cap);

const char *http_body_reason(http_body_result_t rc);

#endif // OS64GET_HTTP_H
