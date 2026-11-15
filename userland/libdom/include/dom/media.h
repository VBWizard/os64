#ifndef OS64_DOM_MEDIA_H
#define OS64_DOM_MEDIA_H
#include <dom/dom.h>

/* THE MEDIA ELEMENT'S HOST (AUDIO_YONDER.md § The element in libdom).
 * libdom keeps HTML's half of <audio>: the element's states, its events,
 * play()'s promises, the resource selection over src and <source>. The
 * host keeps the sound: it fetches, decodes and plays what libdom opens,
 * and libdom asks it what has happened (facts) from its media turn. No
 * call below enters script, and every one is made on the window thread.
 *
 * A SESSION is one resource loading or loaded for one element, the host's
 * pointer, from open until close. libdom closes every session it opened:
 * when the element loads another resource, and at departure. */

/* What the host says of a session now. */
typedef struct {
    bool probed;            /* the length and form are known: HAVE_METADATA */
    double duration;        /* seconds; INFINITY for no known end; read once probed */
    bool seekable;          /* a seek is honoured */
    double arrived;         /* seconds of the media whose bytes have arrived (buffered's end) */
    bool loaded;            /* every byte has arrived */
    bool current;           /* decoded sound for the position waits: HAVE_CURRENT_DATA */
    bool future;            /* more than that: HAVE_FUTURE_DATA */
    bool enough;            /* as much as the host holds: HAVE_ENOUGH_DATA */
    bool busy;              /* play was asked, and the sound device is another program's */
    bool sounding;          /* its sound is going to the device: the position moves */
    bool ended;             /* it played to its end (with loop off) and all of it was heard */
    double position;        /* where in the media the ear is, seconds */
    uint64_t seeked;        /* the last seek's generation the host has landed */
    int error;              /* 0, or a MediaError code below */
    char message[96];       /* MediaError.message: why, or "" */
} os64_dom_media_facts_t;

/* MediaError's codes (HTML § 4.8.11.1). */
#define OS64_DOM_MEDIA_ERR_ABORTED 1
#define OS64_DOM_MEDIA_ERR_NETWORK 2
#define OS64_DOM_MEDIA_ERR_DECODE 3
#define OS64_DOM_MEDIA_ERR_SRC_NOT_SUPPORTED 4

/* The element's crossorigin attribute, as its request's mode and
 * credentials take it. */
typedef enum {
    OS64_DOM_MEDIA_NO_CORS,           /* no attribute: no-cors */
    OS64_DOM_MEDIA_CORS_ANONYMOUS,    /* cors, credentials same-origin */
    OS64_DOM_MEDIA_CORS_CREDENTIALS,  /* cors, credentials include */
} os64_dom_media_cors_t;

/* How much the host fetches before play is asked for (the preload
 * attribute, HTML's hint): the length and form only, or the whole. */
typedef enum { OS64_DOM_MEDIA_PRELOAD_METADATA, OS64_DOM_MEDIA_PRELOAD_AUTO } os64_dom_media_preload_t;

typedef struct {
    void *opaque;
    /* Begin loading `url` (absolute) for `node`; NULL when it cannot begin
     * at all, which the element reports as a network error. */
    void *(*open)(void *opaque, const os64_html_node_t *node, const char *url, os64_dom_media_cors_t cors,
                  os64_dom_media_preload_t preload);
    void (*close)(void *opaque, void *session);
    /* Sound when it can (busy waits, a slow network waits), from where it
     * stands; and stop sounding, keeping the place. */
    void (*play)(void *opaque, void *session);
    void (*pause)(void *opaque, void *session);
    /* Move to `seconds`; facts.seeked is `generation` once it has. */
    void (*seek)(void *opaque, void *session, double seconds, uint64_t generation);
    /* The gain the element's volume and muted make (0 to 1), loop, and
     * whether the whole resource is wanted now (preload became auto, or
     * autoplay was set): a session opened for its metadata reads on. */
    void (*settings)(void *opaque, void *session, double gain, bool loop, bool all);
    void (*facts)(void *opaque, void *session, os64_dom_media_facts_t *out);
    /* Whether the person has interacted with this page (HTML's sticky
     * activation), which play() with sound needs (§ Autoplay). */
    bool (*activated)(void *opaque);
    /* canPlayType's answer for a MIME type with its parameters: 0 for "",
     * 1 for "maybe", 2 for "probably". */
    int (*can_play)(void *opaque, const char *type);
} os64_dom_media_provider_t;

#pragma GCC visibility push(default)
/* Install or replace (NULL removes) the host, outside callbacks. A media
 * element on a page with none plays nothing: canPlayType answers "", and a
 * load fails as a network error does. */
void os64_dom_set_media(os64_dom_t *dom, const os64_dom_media_provider_t *provider);
/* The media turn, owner thread, between tasks, no script: each media
 * element brought up to date with its tree (inserted, removed, src
 * changed) and with its session's facts, its events queued. True when a
 * media task waits for os64_dom_media_fire. */
bool os64_dom_media_turn(os64_dom_t *dom);
/* One queued media task (one event, and the play() promises it settles),
 * as one host task; false with nothing queued. */
bool os64_dom_media_fire(os64_dom_t *dom, os64_js_outcome_t *outcome);
/* Whether a media task waits for os64_dom_media_fire. The turn itself is
 * the host's to call whenever a session has news, and at least every
 * 250 ms while one sounds (timeupdate's pace). */
bool os64_dom_media_due(const os64_dom_t *dom);
#pragma GCC visibility pop
#endif
