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
// most `cap` bytes with the NUL. When they do not all fit, the HttpOnly
// ones are kept first and then the rest, the newest first within each, as
// many as fit (jar.c's pick_to_send says why), still written in that
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

// ── Kept across restarts (YONDER_HONEY_DO.md § 4) ───────────────────────
//
// A FILE OF THE JAR is Netscape's cookies.txt, as curl and wget write it
// (jar.c § Kept across restarts has the format). The jar does no I/O: a
// face reads the file and hands the text in, and asks for the text to
// write. Two windows share one file by READ-MERGE-WRITE: each change to a
// cookie a file keeps (one with an expiry, or one set in such a cookie's
// place, or one let go) is numbered, and a face writing the file reads it
// into a jar of its own, lays this jar's changes over it
// (way_jar_overlay), writes that, tells this jar which changes the file has
// now heard (way_jar_saved), and has this jar follow the file
// (way_jar_follow). What another window wrote stands except where this one
// changed the same cookie (its name, domain, path and host-only flag)
// since, because its change is the newer; what another window let go,
// this one lets go when it next follows the file.

// The longest line a file's reader takes: a cookie's name and value, its
// path and domain at their longest, and the rest of the line.
#define WAY_JAR_LINE_MAX (WAY_COOKIE_BYTES_MAX + OS64_URL_PATH_MAX + OS64_URL_HOST_MAX + 64)

// The jar's cookies a file keeps — every one with an expiry not yet passed
// at `now`, but one with a byte in a field that would break its line — as
// the file's text, oldest first, under three comment lines that say what
// the file is. os64_malloc'd, `*len` its length; NULL on no memory.
char *way_jar_text(way_jar_t *jar, int64_t now, size_t *len);

// What way_jar_read made of a file: lines read as cookies, lines refused
// (malformed, longer than WAY_JAR_LINE_MAX, or refused by the rules a
// Set-Cookie is held to), and whether memory ran out on the way. A
// comment, a blank line, a session line and an expired one are none of
// these.
typedef struct {
    int32_t read, refused;
    bool short_of_memory;
} way_jar_read_t;

// A file's text into the jar, each line held to the rules a Set-Cookie
// is (prefixes, domains, sizes) and the jar's limits, a malformed line
// skipped. A cookie the jar has a change to of its own that no file has
// heard is left as the jar has it. A cookie read replaces its namesake
// and keeps its age; one new to the jar is the least recently used. What
// is read is no change a file has still to hear. `report` may be NULL.
void way_jar_read(way_jar_t *jar, const char *text, size_t len, int64_t now, way_jar_read_t *report);

// The same, with the text as the whole of what a file keeps: after it, a
// cookie with an expiry that the jar has no change to of its own and no
// line named is let go (another window let it go, or cleared the file).
// Nothing is let go when memory ran out on the way, nor while the jar
// refuses cookies. Session cookies are the jar's alone and stay.
void way_jar_follow(way_jar_t *jar, const char *text, size_t len, int64_t now, way_jar_read_t *report);

// The number of the jar's latest change a file has to hear, 0 for none
// since it was made. A face compares it with the number it last saved.
uint64_t way_jar_changes(way_jar_t *jar);

// `from`'s changes numbered `through` and below, laid over `to` (a face's
// own jar, read from the file): each cookie set in `from` replaces its
// namesake in `to`, or removes it when it is a session cookie now (or is
// expired at `now`), and each one `from` let go is removed. `to` must not
// be `from`.
void way_jar_overlay(way_jar_t *to, way_jar_t *from, uint64_t through, int64_t now);

// A file has heard the jar's changes numbered `through` and below.
void way_jar_saved(way_jar_t *jar, uint64_t through);

// Every cookie, and every change a file had still to hear, let go. A
// face empties the file itself.
void way_jar_clear(way_jar_t *jar);

// `cookies = off`: while `refuse` is true the jar hears no cookie and
// sends none. What it holds stays until it is cleared.
void way_jar_refuse(way_jar_t *jar, bool refuse);

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
