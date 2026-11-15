#ifndef YONDER_MEDIA_H
#define YONDER_MEDIA_H

// media.h — the sound behind a page's media elements (AUDIO_YONDER.md
// § The pipeline, § The audio thread and the mixer). libdom keeps HTML's
// half of each element and asks this for the rest through its media
// provider (dom/media.h), which yonder.c builds over these calls.
//
// A SESSION is one element's resource: the window opens it, plays,
// pauses, seeks and closes it, and reads its facts; everything slow
// happens on threads of its own. Each session has a DECODER thread
// (libmedia: the bytes, the decode, the PCM queue) and, for any address
// but a file: one, a LOADER thread (the fetch job, its body streamed down
// a pipe to the decoder). One AUDIO thread for the whole of yonder owns
// /dev/audio and writes one session's sound at a time.

#include <stdbool.h>
#include <stdint.h>

#include "dom/media.h"
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

// The resource: its absolute address (http, https, data: or file:), the
// page that asks (its address, which is its origin and the Referer, and
// its serial, which names an opaque origin), and the element's
// crossorigin; and the browser's agent and preset lines as they are
// chosen when it opens (Settings can change them), borrowed for the
// session's life as a script's request borrows them.
typedef struct {
    const char *url;
    const char *page_url;
    uint64_t page;
    os64_dom_media_cors_t cors;
    const char *agent;
    const way_identity_t *identity;
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

// NULL when it cannot begin (no memory, no thread): the element's network
// error. Window thread, as are all of these.
yonder_media_t *yonder_media_open(const yonder_media_request_t *request);
// The session ends: its threads are told to stop and the window frees it
// once they have (yonder_media_reap). The pointer is dead on return.
void yonder_media_close(yonder_media_t *m);
void yonder_media_play(yonder_media_t *m);
void yonder_media_pause(yonder_media_t *m);
void yonder_media_seek(yonder_media_t *m, double seconds, uint64_t generation);
void yonder_media_settings(yonder_media_t *m, double gain, bool loop);
void yonder_media_facts(yonder_media_t *m, os64_dom_media_facts_t *out);
// canPlayType's answer (0, 1 or 2; dom/media.h), libmedia's.
int yonder_media_can_play(const char *type);
// The window's turn: frees the closed sessions whose threads have ended.
void yonder_media_reap(void);

#endif
