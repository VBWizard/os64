#ifndef YONDER_COOKIES_H
#define YONDER_COOKIES_H

// THE COOKIE FILE (YONDER_HONEY_DO.md § 4): the browser's jar kept across
// restarts as ONE file for every site, `<profile>/cookies`, in Netscape's
// cookies.txt (way/jar.h § Kept across restarts). It is a file of logins:
// it lives under the profile and nowhere else, and says so in its first
// lines.
//
// `cookies = on | session | off` in yonder.conf. ON, the default, reads
// the file at start and writes it after changes; SESSION keeps the jar in
// memory only, and empties the file as yonder leaves and as a SESSION
// yonder starts (Firefox's "delete cookies when closed": switching back to
// ON before then loses nothing); OFF sends no cookie, keeps none, and
// empties the file at once.
//
// THE WRITE IS COALESCED: a change asks for a write, and the loop makes
// it at its turn (yonder_cookies_turn), at most one in
// YONDER_COOKIES_WRITE_MS, so a page that sets a cookie in a loop costs a
// write a second, not one a cookie. What is owed when yonder leaves is
// written then (yonder_cookies_flush).
//
// TWO WINDOWS, ONE FILE: each write reads the file as it stands, lays this
// window's changes over it and publishes the result by temp and rename,
// reading again (up to three times in all) when another window published
// between the read and the rename; this window then follows what it wrote
// (way_jar_follow). A window also follows the file whenever another has
// changed it, looked at before each navigation (yonder_cookies_look), so a
// sign-in in one window is the other's at its next page. So is Clear
// cookies, except that a change the other has not yet written (one made in
// the last YONDER_COOKIES_WRITE_MS, or YONDER_COOKIES_RETRY_MS after a
// failed write) is written back into the emptied file (BROWSER_DEBTS.md),
// and its open page sends the cleared cookies until it leaves. The file's
// change is told by its size and modification time, as page storage's is:
// two writes of one size within one second of the clock read alike, and
// the next change is seen.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "way/jar.h"

#define YONDER_COOKIES_FILE "cookies"
#define YONDER_COOKIES_WRITE_MS 1000u
// After a write that failed, the next is tried this much later: a disk
// that refused once is not asked again every second.
#define YONDER_COOKIES_RETRY_MS 10000u

typedef enum {
    YONDER_COOKIES_ON = 0,
    YONDER_COOKIES_SESSION,
    YONDER_COOKIES_OFF,
} yonder_cookies_mode_t;

// `on`, `session` or `off`, without case: false for anything else.
bool yonder_cookies_mode_of(const char *value, yonder_cookies_mode_t *out);
// The word yonder.conf spells `mode` with.
const char *yonder_cookies_mode_word(yonder_cookies_mode_t mode);

typedef struct yonder_cookies yonder_cookies_t;

// The file for `jar` under the profile folder `profile` (a full path,
// copied), in `mode`: ON follows the file now, SESSION empties it, OFF
// refuses cookies and empties it. NULL on no memory, and with a NULL jar.
yonder_cookies_t *yonder_cookies_new(way_jar_t *jar, const char *profile, yonder_cookies_mode_t mode);
// Lets it go; the jar is the caller's. Writes nothing (yonder_cookies_flush).
void yonder_cookies_free(yonder_cookies_t *k);

yonder_cookies_mode_t yonder_cookies_mode(const yonder_cookies_t *k);
// Settings' switch: to OFF, the jar refuses cookies and is emptied, and so
// is the file; from OFF, it hears and sends again; to ON, it follows the
// file and the changes it made meanwhile are owed to it.
void yonder_cookies_set_mode(yonder_cookies_t *k, yonder_cookies_mode_t mode);

// When the next write is due, on yonder_now_ms's clock: YONDER_NEVER when
// none is owed. A due moment already past is the loop's to meet at once.
uint64_t yonder_cookies_due(const yonder_cookies_t *k);
// The loop's turn: the write, when one is owed and due at `now_ms`.
void yonder_cookies_turn(yonder_cookies_t *k, uint64_t now_ms);
// As yonder leaves: whatever is owed, written now, due or not; in SESSION,
// the file emptied.
void yonder_cookies_flush(yonder_cookies_t *k);
// The file followed when another window has changed it since this one
// last read or wrote it (ON only).
void yonder_cookies_look(yonder_cookies_t *k);

// Settings' "Clear cookies": the jar emptied and the file too, whatever
// the mode. Answers how many cookies went, negative when the file could
// not be emptied (the jar is, all the same).
int64_t yonder_cookies_clear(yonder_cookies_t *k);

// Why the file is not kept, the latest answer: NULL while nothing has gone
// wrong. For the status line.
const char *yonder_cookies_problem(const yonder_cookies_t *k);

// The file's full path, "" when the profile gives none.
const char *yonder_cookies_path(const yonder_cookies_t *k);

#endif
