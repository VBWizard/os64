#ifndef WAY_JAR_H
#define WAY_JAR_H

// The cookie jar (YONDER.md § Y3b): RFC 6265 as the browsers ship it, one
// per browser, shared by every fetch the browser makes, on whatever thread.
// It knows no clock and no network: hearing a cookie and choosing the ones
// to send each carry the address, whether the connection is encrypted, and
// the time, in seconds since 1970 (UTC).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os64/url.h"

#pragma GCC visibility push(default)

// The browsers' limits: past one, the oldest cookie goes.
#define WAY_COOKIE_BYTES_MAX   4096   // a cookie's name and value together
#define WAY_COOKIES_PER_DOMAIN 180
#define WAY_COOKIES_MAX        3000

typedef struct way_jar way_jar_t;

// NULL on no memory.
way_jar_t *way_jar_new(void);
void way_jar_free(way_jar_t *jar);

// One Set-Cookie VALUE (the line after "Set-Cookie:"), heard from `from`
// over a connection that was or was not encrypted, at `now`. A cookie the
// rules refuse is dropped without a word: a server gets no answer about
// its cookies, and a person has nothing to do about one.
void way_jar_hear(way_jar_t *jar, const os64_url_t *from, bool encrypted, const char *value,
                  size_t len, int64_t now);

// The cookies a request to `to` carries, as a Cookie header's value
// ("a=1; b=2"), longest path first and then oldest, whole cookies only, at
// most `cap` bytes with the NUL. Answers the length written; `*left_out`
// (may be NULL) is how many would have gone and did not fit.
size_t way_jar_cookies(way_jar_t *jar, const os64_url_t *to, bool encrypted, int64_t now,
                       char *out, size_t cap, int32_t *left_out);

// How many cookies the jar holds, expired ones included until next asked.
int32_t way_jar_count(way_jar_t *jar);

// RFC 6265 §5.1.1: a cookie date, read the forgiving way. False when it is
// not one.
bool way_cookie_date(const char *text, size_t len, int64_t *epoch);

// The Referer a request to `to` carries from the page at `from`, by the
// browsers' default policy, strict-origin-when-cross-origin: the page's
// whole address (no fragment, no user) to the same scheme, host and port;
// its origin to anywhere else; nothing when an https page's request goes
// to http, or when `from` is not an http or https page. False for nothing.
bool way_referrer(const char *from, const os64_url_t *to, char *out, size_t cap);

// A hop's Cookie and Referer lines, as libfetch's headers_for wants them
// ("Name: value\r\n", NUL-terminated, within `cap`). The cookies come
// first — a login depends on them, not on a Referer — and the Referer
// goes only if it fits after them. `referrer` may be NULL or "". Answers
// how many cookies were left off for want of room.
int32_t way_hop_headers(way_jar_t *jar, const char *referrer, const os64_url_t *to,
                        bool encrypted, int64_t now, char *out, size_t cap);

#pragma GCC visibility pop

#endif
