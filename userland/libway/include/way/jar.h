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

// The browsers' limits: past one, the expired go first and then the
// cookie used longest ago (RFC 6265 §5.3 step 12).
#define WAY_COOKIE_BYTES_MAX   4096   // a cookie's name and value together
#define WAY_COOKIES_PER_DOMAIN 180
#define WAY_COOKIES_MAX        3000

// THE MOST ONE REQUEST'S COOKIE LINE TAKES, "Cookie: " through its CR LF
// and a NUL. RFC 6265 § 6.1 asks a browser to KEEP at least 4096 bytes a
// cookie and 50 a domain, which the limits above do; it sets no figure for
// SENDING them, and fifty such cookies are 200 KB no server reads. The
// servers set the figure: Apache's LimitRequestFieldSize and one of
// nginx's large_client_header_buffers are each 8 KB by default, a field
// longer than that is answered 400 ("Request Header Or Cookie Too Large"),
// so a line past this logs nobody in on them either. 8192 keeps the field
// itself (CR LF and NUL aside) at 8189 bytes, inside Apache's 8190.
#define WAY_COOKIE_LINE_MAX    8192

// The most a hop's Referer line takes, "Referer: " through its CR LF and a
// NUL: Chrome and Firefox send an address of at most 4096 bytes there.
#define WAY_REFERER_LINE_MAX   (4096 + 16)

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
// most `cap` bytes with the NUL. When they do not all fit, the newest that
// fit are the ones written (jar.c's pick_newest says why), still in that
// order. Answers the length written; `*left_out` (may be NULL) is how many
// would have gone and did not fit.
size_t way_jar_cookies(way_jar_t *jar, const os64_url_t *to, bool encrypted, int64_t now,
                       char *out, size_t cap, int32_t *left_out);

// The same two for a PAGE'S SCRIPT (document.cookie), RFC 6265's "non-HTTP
// API": it reads the cookies a request to `to` would carry less the
// HttpOnly ones (`left_out` as above), and a cookie it sets may not be
// HttpOnly nor have the name, domain and path of an HttpOnly one, host-only
// or not. way.h's way_script_cookie(s) is the page-level door.
void way_jar_script_hear(way_jar_t *jar, const os64_url_t *from, bool encrypted, const char *value,
                         size_t len, int64_t now);
size_t way_jar_script_cookies(way_jar_t *jar, const os64_url_t *to, bool encrypted, int64_t now,
                              char *out, size_t cap, int32_t *left_out);

// How many cookies the jar holds, expired ones included until the jar is
// next asked or told.
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
// first — a login depends on them, not on a Referer — in at most
// WAY_COOKIE_LINE_MAX of `cap`, and the Referer goes only if it fits after
// them. `referrer` may be NULL or "". Answers how many cookies were left
// off for want of room, which a face puts on the page's record: a site
// that sees fewer cookies than it set may see a stranger.
int32_t way_hop_headers(way_jar_t *jar, const char *referrer, const os64_url_t *to,
                        bool encrypted, int64_t now, char *out, size_t cap);

// THE COOKIES A LOAD LEFT OUT, for the page's record: the most any one of
// its requests left out for want of room, and the host that request went
// to. Zeroed, it holds none.
typedef struct {
    int32_t left;
    char host[OS64_URL_HOST_MAX];
} way_cookies_short_t;

// A request to `to` left `left` cookies out (way_hop_headers' answer): kept
// when it is more than `s` holds. Nothing for 0.
void way_cookies_short_note(way_cookies_short_t *s, const os64_url_t *to, int32_t left);

#pragma GCC visibility pop

#endif
