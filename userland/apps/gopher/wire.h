#ifndef GOPHER_WIRE_H
#define GOPHER_WIRE_H

// wire.h — the whole of RFC 1436 (1991) that a client needs: an address, an
// item line, and the three ways an answer ends.
//
// THE PROTOCOL IS FOUR SENTENCES. The client connects, sends a selector and
// CRLF, and reads until the server hangs up. A menu is lines of
// `<type><display>\t<selector>\t<host>\t<port>` ended by a line holding one
// period. A text file is its lines, ended the same way, with any line that
// begins with a period doubled so the terminator cannot be forged. A binary
// is its bytes and NOTHING else — no terminator, no length, no header.
//
// THE THIRD SENTENCE IS THE TRAP. Nothing in a gopher response says how it
// is framed; the type character on the item that LED here is the only thing
// that does. Follow a `9` expecting a period and the client hangs on a
// server that has already said everything it has to say. That is why
// `gopher_framing_for` exists as its own function rather than as an `if` in
// the reader: the decision is made once, from the type, before a byte is
// read.
//
// WHY PARSING IS SEPARATE FROM THE SOCKET, the lesson http.h paid for first:
// every reader below takes its bytes from a `gopher_source_fn`, never from a
// handle. On os64 that is one os64_read_for of a dialed connection; in
// tools/test_gopher_host.sh it is a memory buffer handing out one byte at a
// time, then two, then seventeen — because a stream parser's bugs live where
// a token straddles two reads, and a parser only a network can drive is a
// parser nobody drives across that boundary.
//
// THE GRAMMAR IS NOT HERE. `os64_url_parse` (os64/url.h) reads
// `scheme://host:port/path` for this client and for os64get both, because
// RFC 1738 specified that syntax once for http and gopher as siblings — with
// gopher's own inventor as one of its three authors — and because the rules
// it enforces have security edges that must not exist in two copies. What is
// left here is what is gopher's alone: the TYPE character glued to the front
// of the path, the percent-decoding the library deliberately does not do,
// and the TAB that separates a search query from its selector.
//
// THERE ARE NO RELATIVE LINKS IN GOPHERSPACE, which is why no reference
// resolution reaches this file. Every menu item carries its own host, port
// and selector — a menu on one machine links straight into another with no
// base, no `..`, nothing to resolve against. A gopher address is parsed from
// what a person TYPED; after that, addresses arrive whole.
//
// WHAT IS DELIBERATELY ABSENT: Gopher+ (the 1993 extension — attribute
// blocks, `+INFO`, alternate views; nothing serves it that does not also
// serve plain gopher), and CSO (type 2), whose phone books died with the
// campus directories they indexed. Both are shown in a menu and neither is
// followed. See DEBTS.md.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os64/url.h"       // the shared grammar; GOPHER_HOST_MAX is its host

// A selector is a path the server chose, so it is bounded by what a server
// can sensibly name rather than by any rule in the protocol (RFC 1436 states
// no limit at all). A display name is what fits on a wide screen twice over.
// The host is the library's, spelled as its constant so the two cannot drift.
#define GOPHER_HOST_MAX      OS64_URL_HOST_MAX
#define GOPHER_SELECTOR_MAX  1024
#define GOPHER_DISPLAY_MAX   512
#define GOPHER_QUERY_MAX     256

// A line longer than this is TRUNCATED at the cap and the rest skipped to
// the newline, rather than refused: an over-long display name is a server
// being sloppy, and losing the tail of one line is a better answer than
// losing the menu it is in. The read is still bounded — the skipped tail is
// counted against the response's own ceiling, so a peer that sends one
// endless line is cut off by GOPHER_RESPONSE_MAX and not by patience.
#define GOPHER_LINE_MAX      4096

// The read buffer, and the ceiling on a menu or text answer held in memory.
// A binary is streamed to a file and is bounded by the disk instead.
#define GOPHER_BUF_SIZE      8192
#define GOPHER_RESPONSE_MAX  (8u * 1024u * 1024u)

// ── The type character ──────────────────────────────────────────────────
//
// RFC 1436 defines 0-9, '+' and 'T'; the world added 'i', 'h', 'd' and 's'
// without ever writing them down, and they are as universal now as the
// standard ones. What matters to a client is not the list but the QUESTION
// each type answers: how is the answer framed, and may this item be
// followed at all.

typedef enum {
    GOPHER_FRAMING_MENU = 0,   // lines, dot-terminated, parsed as items
    GOPHER_FRAMING_TEXT,       // lines, dot-terminated, dot-unstuffed
    GOPHER_FRAMING_BINARY,     // bytes to the close; no terminator exists
} gopher_framing_t;

// How an answer of this type is framed. An UNKNOWN type has no answer here
// and none is guessed — see gopher_type_followable.
gopher_framing_t gopher_framing_for(char type);

// May a client fetch this item at all? False for the types that are not a
// fetch (`i` says something, `3` is the server complaining, `8`/`T` are
// terminal sessions, `2` is a phone book) and false for every type this
// client does not know — because guessing what a type means is how you
// download a thing that is not what it said it was.
bool gopher_type_followable(char type);

// What this type IS, in a word, for the column a menu shows. Every type gets
// a name including the ones nothing follows, because "you may not click
// this" is a worse answer than "this is a telnet session".
const char *gopher_type_name(char type);

// ── An address ──────────────────────────────────────────────────────────

typedef struct {
    char     host[GOPHER_HOST_MAX];
    char     selector[GOPHER_SELECTOR_MAX];
    char     query[GOPHER_QUERY_MAX];   // a type-7 search; empty otherwise
    uint16_t port;
    char     type;
} gopher_addr_t;

typedef enum {
    GOPHER_URL_OK = 0,
    GOPHER_URL_SCHEME,      // a URL, but not gopher
    GOPHER_URL_NO_HOST,
    GOPHER_URL_HOST_CHARS,  // userinfo, an IPv6 literal, or bytes a host cannot hold
    GOPHER_URL_PATH_CHARS,  // a byte in the selector that would paint, not address
    GOPHER_URL_ESCAPE,      // a `%` that is not followed by two hex digits
    GOPHER_URL_PORT,
    GOPHER_URL_TOO_LONG,
} gopher_url_result_t;

// Take a `gopher://host[:port][/<type><selector>]` apart (RFC 4266), or a
// bare `host[:port]`.
//
// A BARE HOST IS A GOPHER HOST — that guess is made HERE and not in the
// library, because "an address has a scheme" is grammar and "a bare word
// means gopher" is a policy only this program is entitled to. os64get makes
// the opposite guess about the same shape (a valet file name) out of the
// same OS64_URL_NOT_A_URL, which is exactly why the library refuses to guess
// for either of them.
//
// THE TYPE CHARACTER IS THE FIRST BYTE OF THE PATH, not a field of its own:
// `/1/archives` is type '1', selector `/archives`. An empty path is the
// server's root menu — type '1', empty selector — which is what the library
// normalising an empty path to "/" already spells.
//
// THE DECODING IS OURS AND SO IS ITS DUTY. `os64_url_parse` hands back a RAW
// path, because on an HTTP wire percent escapes stay encoded and decoding
// them there would corrupt a request. A gopher selector is the opposite: it
// is delivered to the server decoded, and `%09` is how a search query is
// written down. So this function decodes — and then checks AGAIN for control
// bytes, because `%00` and `%1B` are bytes that were not in the text the
// library judged. A '?' is NOT a query separator; a TAB is (RFC 4266 §2.2),
// and a question mark is an ordinary byte in somebody's selector.
gopher_url_result_t gopher_url_parse(const char *text, gopher_addr_t *out);

// The refusal in words — os64_dial_reason's shape, and for its reason: a
// program should be able to say something a person can act on.
const char *gopher_url_reason(gopher_url_result_t rc);

// Write an address back out as a `gopher://` URL. The inverse of the parse,
// so the client can show where it is and a person can write it down.
// Returns the length it wanted, strlcpy-style: >= cap means truncated.
size_t gopher_url_text(const gopher_addr_t *addr, char *out, size_t cap);

// The bytes that ask for `addr`: the selector, a TAB and the query when
// there is one, and CRLF. CRLF and not a bare LF — 1991 protocols mean it,
// and a server written to the letter of the RFC is entitled to wait for the
// second byte. Returns the length written, or 0 if it would not fit.
size_t gopher_request(const gopher_addr_t *addr, char *out, size_t cap);

// ── An item line ────────────────────────────────────────────────────────

typedef enum {
    GOPHER_ITEM_OK = 0,      // a link: `addr` says where
    GOPHER_ITEM_INFO,        // an `i` line: `display` is all there is
    GOPHER_ITEM_MALFORMED,   // not enough fields, or a port that is not one
    GOPHER_ITEM_REFUSED,     // a byte in it would paint rather than say
} gopher_item_result_t;

typedef struct {
    gopher_item_result_t result;
    char                 type;
    char                 display[GOPHER_DISPLAY_MAX];
    gopher_addr_t        addr;
    bool                 followable;   // result OK *and* the type is one
} gopher_item_t;

// Parse one menu line (with its terminator already stripped).
//
// A MENU IS A STRANGER'S BYTES ON YOUR TERMINAL, and os64's terminal learned
// to obey escape sequences the day before this client was written. So the
// refusal is HERE, once, where the line is parsed — not at each print, which
// is the arrangement that eventually misses a print. A display name, a
// selector or a host carrying a byte below 0x20 or a DEL is GOPHER_ITEM_
// REFUSED: shown as a refusal, never followed, never painted.
//
// High bytes are NOT controls and are kept. A menu written in Latin-1 by
// somebody in 1994 is not an attack, and the renderer's font decides what a
// byte over 0x7F looks like.
gopher_item_result_t gopher_item_parse(const char *line, gopher_item_t *out);

// ── Reading an answer ───────────────────────────────────────────────────

// Bytes from somewhere. Returns >0 read, 0 at end of input, <0 on error —
// os64_read's contract, so the os64 implementation is a one-line forward.
typedef int64_t (*gopher_source_fn)(void *ctx, void *buf, size_t cap);

typedef struct {
    gopher_source_fn source;
    void            *ctx;
    char             buf[GOPHER_BUF_SIZE];
    size_t           len;
    size_t           pos;
    bool             eof;         // the source is finished
    bool             terminated;  // a lone '.' ended the answer properly
    bool             truncated;   // a line hit GOPHER_LINE_MAX, or the ceiling did
    bool             failed;      // the source returned an error
    uint64_t         bytes;       // read from the source, ceiling included
} gopher_stream_t;

void gopher_stream_init(gopher_stream_t *s, gopher_source_fn source, void *ctx);

// One line, terminator stripped, NUL-terminated. Returns 1 for a line, 0 at
// the end of the answer, and <0 for a source that failed.
//
// END means one of three things, and the caller can tell them apart from the
// flags: `terminated` — the period arrived and the answer is whole;
// otherwise the connection simply ended, which is common enough in the wild
// that it is not an error; and `failed`, which is.
//
// `unstuff` undoes RFC 1436's transparency rule (a leading period was
// doubled). True for a text file, false for a menu — servers stuff text and
// nobody stuffs menus, and unstuffing a menu would eat the type character
// off any item whose type is '.'.
int gopher_stream_line(gopher_stream_t *s, char *out, size_t cap, bool unstuff);

// Raw bytes, for a binary answer: read to the close, no framing, no
// terminator. Same returns as the source.
int64_t gopher_stream_raw(gopher_stream_t *s, void *out, size_t cap);

#endif // GOPHER_WIRE_H
