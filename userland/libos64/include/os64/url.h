#ifndef OS64_URL_H
#define OS64_URL_H

// url.h — what an address IS, for every program that takes one.
//
// THE GRAMMAR WAS DESIGNED ONCE, FOR TWO PROTOCOLS, AND THIS PUTS IT BACK
// TOGETHER. RFC 1738 (December 1994, "Uniform Resource Locators") has three
// authors: Tim Berners-Lee, Larry Masinter, and Mark McCahill — who led the
// team at the University of Minnesota that built Gopher. The common syntax
// `scheme://host:port/path` was written for http and gopher AS SIBLINGS by
// the people who made both. That they ended up with separate parsers in this
// tree is an accident of which one os64 learned first.
//
// WHAT IS HERE IS GRAMMAR. WHAT IS NOT HERE IS POLICY. This file answers
// "what is this address": which scheme was named, which host, which port if
// any, which path. It does NOT answer "may I go there", "what does this
// scheme default its port to", or "what does this path MEAN" — those differ
// per protocol and per machine, and a library that knew them would have to
// be edited every time a caller learned a new one. So there is no scheme
// table in here: a scheme is validated as a scheme and handed back, and the
// caller says whether it is one it serves.
//
// WHY IT IS SHARED, which is a hazard argument and not a tidiness one. Four
// of the decisions below have a security edge:
//
//   - the host alphabet, which is what stops a '@', a '[' or a space from
//     reaching the resolver or a request line;
//   - the colon searched from the RIGHT, so `user@host` and `[::1]` are
//     refused as host bytes instead of mistaken for a port;
//   - the host folded and the path never folded (a DNS name is
//     case-insensitive by RFC 4343; a path is a name on somebody else's
//     filesystem, where /Case and /case are two files);
//   - `host:` with nothing after it refused rather than defaulted.
//
// Two copies of a rule with an edge on it drift, and the drift is invisible
// until the looser copy is the one an attacker reaches. One copy cannot.
//
// WHAT A CALLER STILL OWES. A path arrives RAW — percent escapes are not
// decoded, because on an HTTP wire they stay encoded and decoding them here
// would corrupt the request. A caller that DOES decode (gopher's selector
// carries `%09` for a search query) inherits the duty to re-check what
// decoding produced: `%00` and `%1B` are control bytes that were not in the
// text this parser judged.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os64/resolve.h"   // OS64_RESOLVE_NAME_MAX — DNS's own ceiling

#define OS64_URL_SCHEME_MAX 16
#define OS64_URL_HOST_MAX   (OS64_RESOLVE_NAME_MAX + 1)
#define OS64_URL_PATH_MAX   1024

typedef struct {
    char     scheme[OS64_URL_SCHEME_MAX];   // lowercased; spelled with scheme bytes
    char     host[OS64_URL_HOST_MAX];       // lowercased; no port, userinfo or brackets
    char     path[OS64_URL_PATH_MAX];       // always starts '/', raw, fragment dropped
    // ZERO MEANS THE URL NAMED NO PORT, and the caller supplies its scheme's
    // default. That is one fact in one field: a separate "did it say?" flag
    // would be the same answer written twice, and two ways to spell it is
    // two ways for them to disagree.
    uint16_t port;
} os64_url_t;

typedef enum {
    OS64_URL_OK = 0,
    OS64_URL_NOT_A_URL,   // no "scheme://" — the operand means something else
    OS64_URL_NO_HOST,
    OS64_URL_HOST_CHARS,  // userinfo, an IPv6 literal, or bytes a host cannot hold
    OS64_URL_PATH_CHARS,  // a space or a control byte: the request-splitting shape
    OS64_URL_PORT,
    OS64_URL_TOO_LONG,
} os64_url_result_t;

// Take `text` apart.
//
// OS64_URL_NOT_A_URL IS NOT A COMPLAINT. It is how a caller learns the
// operand had no scheme, and what that means is the caller's to decide:
// os64get reads it as a valet file name, gopher reads a bare `floodgap.com`
// as a gopher host. "An address has a scheme" is grammar and lives here; "a
// bare word means X" is a guess, and a guess belongs to whoever is entitled
// to make it.
os64_url_result_t os64_url_parse(const char *text, os64_url_t *out);

// The refusal in words — os64_dial_reason's shape, and for its reason: a
// program should be able to tell a person what went wrong without a
// debugger. A caller refusing a scheme it does not serve owes a better
// sentence than any of these, because it is the only layer that knows which
// schemes it serves.
const char *os64_url_reason(os64_url_result_t rc);

#endif // OS64_URL_H
