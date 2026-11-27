#ifndef YONDER_MEDIA_H
#define YONDER_MEDIA_H

// media.h — the sound behind a page's media elements (AUDIO_YONDER.md
// § The pipeline, § The audio thread and the mixer). libdom keeps HTML's
// half of each element and asks this for the rest through its media
// provider (dom/media.h), which yonder.c builds over these calls.
//
// A SESSION is one element's resource: the window opens it, plays,
// pauses, seeks and closes it, and reads its facts; everything slow
// happens on threads of its own. Every session is libmedia's player
// (os64/media_player.h) over the session's bytes, which the window drives:
// for an <audio> the sound alone, for a <video> the sound and the picture,
// which the window asks for at each frame of the display. For any address
// but a file: one, a LOADER thread runs the fetch job, its body streamed
// down a pipe to the player's decoding thread. Its sound is a voice of
// yonder's one output (os64/media_output.h): libmedia's audio thread owns
// /dev/audio and mixes every session that plays.

#include <stdbool.h>
#include <stdint.h>

#include "dom/media.h"
#include "os64/media_player.h"
#include "way/cache.h"
#include "way/jar.h"
#include "way/origin.h"

typedef struct yonder_media yonder_media_t;

// What every media request is made with, all of it yonder's, borrowed
// until yonder_media_stop: the browser's cookies, preflight cache and HTTP
// cache, and how a thread tells the window it has news (rings the window's
// media bell; any thread may call it). The agent is each request's own
// (yonder_media_request_t).
typedef struct {
    way_jar_t *jar;
    way_preflight_t *preflight;
    way_cache_t *cache;
    void (*ring)(void);
} yonder_media_env_t;

// The resource: its absolute address (http, https, data:, blob: or file:),
// the page that asks (its address, which is its origin and the Referer,
// and its serial, which names an opaque origin), and the element's
// crossorigin; the browser's agent and preset lines as they are chosen
// when it opens (Settings can change them), borrowed for the session's
// life as a script's request borrows them; and a blob: address's entry,
// taken by the caller (BLOB_URLS.md) and borrowed for the call: the
// session holds its own, and NULL is none. `picture`: the element is a
// <video>, whose player decodes the picture too. `element` ("video 2") and
// `named` (the address as the page's record shows it) are what a failure
// is recorded by (yonder_media_failure_take), copied; NULL takes the
// element's kind and the address.
typedef struct {
    const char *url;
    const char *page_url;
    uint64_t page;
    os64_dom_media_cors_t cors;
    const char *agent;
    const way_identity_t *identity;
    struct os64_dom_blob *blob;
    bool picture;
    const char *element;
    const char *named;
} yonder_media_request_t;

// Once, before the first open; and once as yonder leaves, after the pages
// are gone and before anything in the environment is freed: every session
// is stopped, and it returns once every thread has let go of the sessions
// and the environment.
void yonder_media_start(const yonder_media_env_t *env);
void yonder_media_stop(void);

// The types of sound yonder plays, as a list libway sorts a reply by
// (way_leg_t.sounds), made at yonder_media_start and unchanged after, so
// any thread may read it.
const char *yonder_media_sounds(void);

// NULL when it cannot begin (no memory, no thread, or the program's sound
// output could not be made): the element's network error. Window thread,
// as are all of these.
yonder_media_t *yonder_media_open(const yonder_media_request_t *request);
// The session ends: its threads are told to stop and the window frees it
// once they have (yonder_media_reap). The pointer is dead on return.
void yonder_media_close(yonder_media_t *m);
void yonder_media_play(yonder_media_t *m);
void yonder_media_pause(yonder_media_t *m);
void yonder_media_seek(yonder_media_t *m, double seconds, uint64_t generation);
void yonder_media_settings(yonder_media_t *m, double gain, bool muted, bool loop);
void yonder_media_facts(yonder_media_t *m, os64_dom_media_facts_t *out);
// Once a session has failed (its facts' `error`), its FAILED media line
// once, into `out`: its address, then media_bytes.h's words for what it
// failed with — `code` the MediaError the page was handed, which libdom
// decides (dom/media.h's `failed`), 0 for the session's own — and what was
// heard of the fetch and from libmedia. False before it has failed, and
// after the line was taken.
bool yonder_media_failure_take(yonder_media_t *m, int code, char *out, size_t cap);
// A <video>'s picture to show at `blank` (wall microseconds, the blank a
// publish now is seen at), `period` the display's: its player's
// (os64_media_player_frame, whose rule the pixels keep). False with `out`
// empty for an <audio>, or with no picture yet.
bool yonder_media_frame(yonder_media_t *m, int64_t blank, int64_t period, os64_media_player_frame_t *out);
// The size a <video>'s pictures are converted to (the fitted rectangle,
// device pixels). Nothing for an <audio>.
void yonder_media_size(yonder_media_t *m, uint32_t width, uint32_t height);
// A session's player's own facts (its pace, clock and counts), for the
// record's reading; false for one with no player yet.
bool yonder_media_player_facts(yonder_media_t *m, os64_media_player_facts_t *out);
// The `i`th open session's line for the record's reading, on how its length
// travelled: its source (file, network or blob, the size, whether ranges
// were answered, the fetches begun), the player's length and when it was set, whether a
// `look` followed, the duration told to libdom and when, and when libdom
// last read it. False past the last session. Window.
bool yonder_media_report(uint32_t i, char *out, size_t cap);
// A time for the record: seconds to the millisecond, "unknown" for NaN,
// "no end" for infinity (libos64's printf has no floating point).
size_t yonder_media_seconds(double s, char *out, size_t cap);
// A <video>'s player given a plane of `win` (os64/media_player.h § A
// plane), from a thread of its own: the ask waits for the plane to be made,
// the window does not. Asked once, or again once the last ask's thread is
// done (for a plane that ended; the player refuses any other). The verdict
// is the player's facts' `plane_state`, read at the window's next turn.
// False when not asked (an <audio>, no player yet, an ask still running,
// closed) or when its thread could not be started.
bool yonder_media_plane_ask(yonder_media_t *m, int64_t win);
// os64_media_player_plane_want, for a <video> with a player.
void yonder_media_plane_want(yonder_media_t *m, bool want);
// os64_media_player_plane_gap_take, for a <video> with a player; 0 without.
int64_t yonder_media_plane_gap_take(yonder_media_t *m);
// The picture its player's pace last picked, pinned until
// yonder_media_shown_done (os64_media_player_shown). False, `out` empty,
// with none. Every view goes back before the session's close.
bool yonder_media_shown(yonder_media_t *m, os64_media_player_view_t *out);
void yonder_media_shown_done(yonder_media_t *m, os64_media_player_view_t *view);
// canPlayType's answer (0, 1 or 2; dom/media.h), libmedia's.
int yonder_media_can_play(const char *type);
// The window's turn: frees the closed sessions whose threads have ended.
void yonder_media_reap(void);

#endif
