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
    double arrived_from;    /* where those begin (buffered's start): a seek far ahead fetches from there */
    bool loaded;            /* every byte has arrived */
    bool current;           /* decoded sound for the position waits: HAVE_CURRENT_DATA */
    bool future;            /* more than that: HAVE_FUTURE_DATA */
    bool enough;            /* as much as the host holds: HAVE_ENOUGH_DATA */
    bool busy;              /* play was asked, and the sound device is another program's */
    bool sounding;          /* its sound is going to the device: the position moves */
    bool running;           /* its clock runs (sounding, or a picture's clock with no sound heard):
                             * the position moves, and timeupdate follows it */
    bool picture;           /* it has a picture (a video's), read once probed */
    uint32_t width, height; /* the picture's size as shown (videoWidth and videoHeight), 0 with none;
                             * it may change while it plays */
    bool ended;             /* it played to its end (with loop off) and all of it was heard */
    double position;        /* where in the media the ear is, seconds */
    uint64_t seeked;        /* the last seek's generation the host has landed */
    int error;              /* 0, or a MediaError code below; for a fed session (below) a
                             * MEDIA_ERR_DECODE here ends its MediaSource with a decode error */
    char message[96];       /* MediaError.message: why, or "" */
    uint64_t frames_shown;  /* pictures shown since the session opened (getVideoPlaybackQuality) */
    uint64_t frames_dropped; /* pictures dropped since then, before or after their decode */
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
     * at all, which the element reports as MEDIA_ERR_SRC_NOT_SUPPORTED when
     * its resource is being selected, and as MEDIA_ERR_NETWORK when a
     * dormant element wakes for it again (`failed` is told either way). */
    void *(*open)(void *opaque, const os64_html_node_t *node, const char *url, os64_dom_media_cors_t cors,
                  os64_dom_media_preload_t preload);
    void (*close)(void *opaque, void *session);
    /* Sound when it can (busy waits, a slow network waits), from where it
     * stands; and stop sounding, keeping the place. */
    void (*play)(void *opaque, void *session);
    void (*pause)(void *opaque, void *session);
    /* Move to `seconds`; facts.seeked is `generation` once it has. */
    void (*seek)(void *opaque, void *session, double seconds, uint64_t generation);
    /* The gain the element's volume and muted make (0 to 1), whether it is
     * muted (a gain of 0 with muted off is a silent sound, still playing),
     * loop, and whether the whole resource is wanted now (preload became
     * auto, or autoplay was set): a host that reads less before play may
     * read on. */
    void (*settings)(void *opaque, void *session, double gain, bool muted, bool loop, bool all);
    void (*facts)(void *opaque, void *session, os64_dom_media_facts_t *out);
    /* Whether the person has interacted with this page (HTML's sticky
     * activation), which play() with sound needs (§ Autoplay). */
    bool (*activated)(void *opaque);
    /* canPlayType's answer for a MIME type with its parameters: 0 for "",
     * 1 for "maybe", 2 for "probably". */
    int (*can_play)(void *opaque, const char *type);
    /* The element failed, told once a failure: `session` the one whose
     * facts said so (NULL when there was none: its address did not resolve
     * against the page, or `open` answered NULL), `reference` the address
     * (as the page wrote it when it did not resolve), and `code` and `why`
     * the MediaError the page is handed, which for a session that failed
     * before its length was known is MEDIA_ERR_SRC_NOT_SUPPORTED whatever
     * its facts said (a <source> candidate's failure moves on to the next
     * and hands the page no error, and is told all the same). NULL for
     * none. */
    void (*failed)(void *opaque, const os64_html_node_t *node, void *session, const char *reference, int code,
                   const char *why);
} os64_dom_media_provider_t;

/* THE FED SOURCE (MEDIA_SOURCE.md § 7, Media Source Extensions): a
 * MediaSource attached to a media element feeds it the bytes a page
 * appends. libdom keeps the objects, their states, their events and their
 * order, and the element's states; the host keeps the FED SESSION it
 * opens for the element, which is also a session of the media provider
 * above (play, pause, seek, settings and facts take it; `close` here, not
 * the media provider's, ends it), and per SourceBuffer a BUFFER: its
 * parser's input, its track buffers and its quota. Every call is made on
 * the window thread, returns at once and enters no script; every time is
 * in seconds, converted to and from the stream's own ticks by the host. */
typedef enum {
    OS64_DOM_FED_DONE,      /* every byte taken was parsed */
    OS64_DOM_FED_PENDING,   /* the parse runs elsewhere: `poll` again from the media turn */
    OS64_DOM_FED_ERROR,     /* the byte stream broke a rule: the append error (the parser reset) */
} os64_dom_fed_status_t;

typedef enum { OS64_DOM_FED_SEGMENTS, OS64_DOM_FED_SEQUENCE } os64_dom_fed_mode_t;

/* What a parse (or a reset, or a removal) did to a buffer. */
typedef struct {
    os64_dom_fed_status_t status;
    uint32_t inits;         /* initialization segments completed by this parse */
    double init_duration;   /* the last one's duration; NaN when it gives none (an mvhd
                             * duration of 0 means unknown: NaN) */
    bool audio, video;      /* the tracks the buffer's first initialization segment holds */
    bool frames;            /* coded frames were added */
    bool parsing_media;     /* the parser stopped inside a media segment (PARSING_MEDIA_SEGMENT) */
    double group_end;       /* the coded frame group's end, NaN when unset */
    double highest_pts;     /* the highest presentation time of any frame the buffer holds; NaN none */
    double timestamp_offset; /* the buffer's timestampOffset as it stands now (sequence mode moves it) */
    char message[96];       /* why, with OS64_DOM_FED_ERROR */
} os64_dom_fed_result_t;

typedef struct { double start, end; } os64_dom_fed_range_t;

typedef struct {
    void *opaque;
    /* A fed session for `node`, attaching; NULL refuses (the element's
     * "cannot be fetched at all"). */
    void *(*open)(void *opaque, const os64_html_node_t *node);
    /* The detach: the session and every buffer it holds are gone. */
    void (*close)(void *opaque, void *session);
    /* A buffer for a type isTypeSupported answered yes to; NULL refuses
     * (QuotaExceededError). */
    void *(*add)(void *opaque, void *session, const char *type);
    void (*remove_buffer)(void *opaque, void *session, void *buffer);
    /* Coded frame eviction before an append of `incoming` bytes; false
     * when the buffer would still not hold them (QuotaExceededError). */
    bool (*evict)(void *opaque, void *session, void *buffer, size_t incoming);
    /* The append's bytes copied into the parser's input, the one copy;
     * false for no memory. */
    bool (*take)(void *opaque, void *session, void *buffer, const uint8_t *bytes, size_t length);
    /* The segment parser loop over what was taken, frames into the track
     * buffers; and the same answer again while a parse is PENDING. */
    void (*parse)(void *opaque, void *session, void *buffer, os64_dom_fed_result_t *out);
    void (*poll)(void *opaque, void *session, void *buffer, os64_dom_fed_result_t *out);
    /* The reset parser state algorithm (complete coded frames of a media
     * segment processed, the rest of the input dropped). */
    void (*reset)(void *opaque, void *session, void *buffer, os64_dom_fed_result_t *out);
    /* Coded frame removal of [start, end). */
    void (*remove)(void *opaque, void *session, void *buffer, double start, double end, os64_dom_fed_result_t *out);
    /* The buffer's mode, timestampOffset and append window, for the frames
     * that follow. `offset_assigned` says the page assigned timestampOffset
     * (at any value, the same one included): in sequence mode the next
     * coded frame group starts at it. A change of mode to sequence starts
     * the next group at the last group's end; libdom says nothing more
     * of it than the mode. */
    void (*set)(void *opaque, void *session, void *buffer, os64_dom_fed_mode_t mode, double timestamp_offset,
                bool offset_assigned, double window_start, double window_end);
    /* The buffer's ranges, up to `cap`, and their count: its tracks'
     * ranges intersected, and with `ended` (the MediaSource has ended) each
     * track's last range first reaching the largest end of any of its
     * tracks (the buffered attribute's steps); and in *highest_end that
     * largest end. */
    uint32_t (*ranges)(void *opaque, void *session, void *buffer, bool ended, os64_dom_fed_range_t *out,
                       uint32_t cap, double *highest_end);
    /* The MediaSource's duration, after each change. */
    void (*duration)(void *opaque, void *session, double seconds);
    /* endOfStream() without an error: nothing more is coming. */
    void (*end)(void *opaque, void *session);
} os64_dom_fed_provider_t;

/* What a media element's controls show (AUDIO_YONDER.md § The controls),
 * as its members would answer now. */
typedef struct {
    bool paused;            /* paused */
    bool ended;             /* ended */
    bool waiting;           /* playing, but nothing to hear yet: loading, or busy */
    bool busy;              /* play was asked and the sound device is another program's */
    bool seekable;          /* seekable has a range, so a seek bar may be dragged */
    bool muted;             /* muted */
    double position;        /* currentTime, seconds */
    double duration;        /* duration: NaN unknown, INFINITY for no end */
    double arrived;         /* buffered's end, seconds */
    double arrived_from;    /* buffered's start, seconds */
    double volume;          /* volume, 0 to 1 */
    int error;              /* error's code, or 0 */
    bool show_poster;       /* HTML's show poster flag: a video shows its poster, not its picture */
} os64_dom_media_view_t;

/* What a control asks of its element: what a script would by play(),
 * pause(), setting currentTime (value: seconds), volume (0 to 1) or muted
 * (value nonzero). */
typedef enum {
    OS64_DOM_MEDIA_PLAY,
    OS64_DOM_MEDIA_PAUSE,
    OS64_DOM_MEDIA_SEEK,
    OS64_DOM_MEDIA_VOLUME,
    OS64_DOM_MEDIA_MUTE,
} os64_dom_media_control_t;

/* THE MEDIA SESSION (AUDIO_YONDER.md § Y.6): what the window says of its
 * sound, for the media keys, and a press done on the page. */
typedef enum {
    OS64_DOM_MEDIA_SESSION_STOPPED,
    OS64_DOM_MEDIA_SESSION_PAUSED,
    OS64_DOM_MEDIA_SESSION_PLAYING,
} os64_dom_media_session_state_t;

typedef struct {
    os64_dom_media_session_state_t state;
    /* The page has something to say: an action handler set, or an audible
     * element that has played. */
    bool wanted;
    /* Which presses the page answers itself: bit n for action n below. */
    uint32_t handled;
} os64_dom_media_session_t;

typedef enum {
    OS64_DOM_MEDIA_SESSION_PLAY,
    OS64_DOM_MEDIA_SESSION_PAUSE,
    OS64_DOM_MEDIA_SESSION_NEXT,
    OS64_DOM_MEDIA_SESSION_PREVIOUS,
    OS64_DOM_MEDIA_SESSION_STOP,
} os64_dom_media_session_action_t;

#pragma GCC visibility push(default)
/* Install or replace (NULL removes) the host, outside callbacks. A media
 * element on a page with none plays nothing: canPlayType answers "", and a
 * load fails as a network error does. */
void os64_dom_set_media(os64_dom_t *dom, const os64_dom_media_provider_t *provider);
/* Install or replace (NULL removes) the fed source's host, outside
 * callbacks, in a binding made with the media_source switch (dom.h), which
 * is what puts MediaSource on window. With none, a MediaSource never opens
 * (its element fails as a source that cannot be fetched) and
 * isTypeSupported answers no. */
void os64_dom_set_fed(os64_dom_t *dom, const os64_dom_fed_provider_t *provider);
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
/* The controls' side, owner thread, between tasks. The view of `node`'s
 * element (zeroed, duration NaN, and false while it has no state: no
 * member has been reached and no turn has found it). */
bool os64_dom_media_view(os64_dom_t *dom, const os64_html_node_t *node, os64_dom_media_view_t *out);
/* A person's use of `node`'s controls, done as the element's member would
 * be done by script, so the page hears the same events: a play is a host
 * task (`outcome` its verdict), and its promise's rejection is the
 * element's state, not the page's to handle. Activation is the host's to
 * have noted first (the press that reached the control). False when the
 * element cannot be reached. */
bool os64_dom_media_control(os64_dom_t *dom, const os64_html_node_t *node, os64_dom_media_control_t what,
                            double value, os64_js_outcome_t *outcome);
/* The media session's side, owner thread, between tasks. The window's
 * state, over the page's AUDIBLE media elements (in its document or not;
 * not muted, a volume, no error): PLAYING when one is not paused, or
 * navigator.mediaSession.playbackState is "playing" and one has played;
 * else PAUSED when one that played is paused, or playbackState is
 * "paused" and one has played; else STOPPED. A page with no session (the
 * Modern Web off) answers STOPPED, wanting none. False only when the
 * binding cannot answer now (recovering, or no longer active). */
bool os64_dom_media_session_state(os64_dom_t *dom, os64_dom_media_session_t *out);
/* A media key pressed, as a host task (`outcome` its verdict): the page's
 * handler for the action, given {action}; with none, the browser's own
 * answer: play plays the audible element that played most recently, pause
 * and stop pause every audible element that plays, next and previous do
 * nothing. Activation
 * is the host's to have noted first (a press is a person's). Only the top
 * document's session is asked. False when there is no session. */
bool os64_dom_media_session_action(os64_dom_t *dom, os64_dom_media_session_action_t action,
                                   os64_js_outcome_t *outcome);
#pragma GCC visibility pop
#endif
