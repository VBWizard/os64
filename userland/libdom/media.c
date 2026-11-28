#include "internal.h"
#include "os64/fmt.h"
#include <math.h>

/* THE MEDIA ROW (AUDIO_YONDER.md § The element in libdom, § The element's
 * states): HTMLMediaElement and HTMLAudioElement over the binding's media
 * prototypes, MediaError, TimeRanges, an always-empty TextTrackList, the
 * Audio factory, and the handlers of the media events. A <video> is a
 * media element too, playing sound and no picture: its own members and
 * interface are video.c's (§ Y.4).
 *
 * HTML's half of the element lives here and the sound lives with the host
 * (dom/media.h). Each media element the page has is a RECORD: one for each
 * <audio> and <video> in the document, found by the media turn's walk, and one for
 * each element a script reaches through a media member or gives a src,
 * which is how an element no document holds (`new Audio(url)`) gets one. The record holds
 * its node, so a playing element the page dropped keeps playing, as the
 * standard has it, and lives until the binding does.
 *
 * The record keeps the element's state as HTML names it (networkState,
 * readyState, paused, seeking, ended, the volume and muted, the rates, the
 * official playback position, the error), the play() promises pending,
 * and what the turn compares to see a change (the src it last acted on,
 * the facts it last heard). The turn moves the states by what the host's
 * facts say and queues the events the standard's steps fire, each a task
 * of its own, in order, and the play() promises a step settles go with
 * the event's task: a `playing` task resolves the promises pending when
 * it was queued, as "notify about playing" does. The tasks run through
 * os64_dom_media_fire, one at a time.
 *
 * What it leaves out: the audio and video track lists, any text track
 * (textTracks is always empty, and there is no addTextTrack()), the media
 * controller, a playback rate other than 1 (kept and reported; the host
 * plays at 1, DEBTS.md), getStartDate, the per-candidate `error` events at
 * <source> elements (a failed candidate moves to the next one silently),
 * and an element that is in no document, was never given a src and was
 * never reached through a media member (one made with innerHTML holding
 * its <source> children, say): it has no record, and loads once it has
 * one. */

enum { N_EMPTY, N_IDLE, N_LOADING, N_NO_SOURCE };
enum { R_NOTHING, R_METADATA, R_CURRENT, R_FUTURE, R_ENOUGH };
/* What a task does with the promises it took. */
enum { SETTLE_NONE, SETTLE_RESOLVE, SETTLE_ABORT, SETTLE_NOT_SUPPORTED };

typedef struct { JSValue promise, resolve, reject; } DMediaPromise;

typedef struct DMediaElement DMediaElement;

/* A task on the media elements' one queue: an element's event (`media`),
 * or one of Media Source Extensions' (mse.c, through d_media_queue_plain
 * and d_media_queue_work): an event at a plain target, or an append's or
 * removal's work, which a SourceBuffer may cancel before it runs. */
struct DMediaTask {
    struct DMediaTask *next;
    DMediaElement *media;           /* the element's task; NULL for MSE's */
    JSValue wrapper;                /* the element's, while queued (HOLDERS) */
    const char *type;               /* the event, NULL to only settle */
    uint8_t settle;
    uint8_t clocked;                /* its CLOCKED_* bit, 0 for an event of a change */
    DMediaPromise *promises;        /* the task's own */
    uint32_t promise_count;
    JSValue object;                 /* MSE's: the target's object, held while queued; JS_UNDEFINED else */
    DValue *plain;                  /* MSE's event: the plain target's entry in `object` */
    void (*run)(os64_dom_t *dom, void *work);       /* MSE's work, instead of an event */
    void (*left)(void *work, DMediaTask *task);     /* hears the work task leave the queue */
    void *work;
    bool cancelled;                 /* leaves the queue without running */
};

/* The events the element fires on a clock while it plays or loads, not at
 * a change of state: `timeupdate` every 250 ms of play and `progress` every
 * 350 ms of arrival. HTML asks for them only "if the handler is keeping
 * up", so one undelivered of each per element is the most ever queued
 * (queue_clocked): a page slower than the clock hears one when it gets
 * there, not a backlog. The event carries nothing of its time: the
 * handler reads currentTime and buffered when it runs, which is then. */
enum { CLOCKED_TIMEUPDATE = 1, CLOCKED_PROGRESS = 2 };

struct DMediaElement {
    DMediaElement *next;
    const os64_html_node_t *node;   /* held */
    void *session;                  /* the host's, NULL with none */
    char *src_seen;                 /* the src attribute as last acted on; NULL when absent */
    bool src_looked;                /* the record has looked at its src */
    bool loop_seen;                 /* the loop attribute as the host was last told */
    bool all_seen;                  /* wants_all as the host was last told */
    char *current_src;              /* currentSrc; NULL is "" */
    uint8_t network, ready;
    bool paused, seeking, autoplaying, connected, seen;
    bool select_pending;            /* resource selection is due at the next turn */
    bool deferred;                  /* preload=none: selected, waiting for play() or load() */
    bool show_poster;               /* HTML's show poster flag: set at each load, cleared by a play or a seek */
    bool wanted;                    /* play() or load() asked: preload=none no longer defers */
    bool dormant;                   /* gave its session back; play() or a seek wakes it (session_wake) */
    bool from_children;             /* the resource is a <source>'s */
    bool awaiting_source;           /* every <source> so far failed: a new one resumes at `candidate` */
    uint32_t candidate;             /* which <source> is being tried */
    bool loadeddata;                /* loadeddata fired for this load */
    bool ended;                     /* it has ended playback */
    uint64_t played_seq;            /* when it last began to play (DMediaElements.play_seq), 0 never */
    bool error_seen;                /* the session's error has been acted on */
    bool settings_applied;
    double volume, rate, default_rate;
    bool muted, preserves_pitch;
    double position;                /* the official playback position */
    double default_start;           /* a seek asked for before metadata */
    double duration;                /* NaN until known */
    uint32_t video_width, video_height; /* the picture's size as shown, 0 until metadata (a video's) */
    double played_end;              /* the furthest heard, for played */
    double seek_to;
    uint64_t seek_generation;
    uint64_t last_timeupdate_ms, last_progress_ms;
    /* The clocked events (CLOCKED_*) queued and not yet delivered: one of
     * each at most (queue_clocked). */
    uint8_t clocked_queued;
    double last_arrived;
    int error;
    char message[96];
    JSValue error_object;           /* the MediaError, held once made */
    JSValue text_tracks;            /* the TextTrackList, made once, kept for the page */
    JSValue fed;                    /* the MediaSource attached (held while it is), JS_UNDEFINED with none */
    DMediaPromise *pending;
    uint32_t pending_count, pending_cap;
};

struct DMediaElements {
    DMediaElement *records;
    DMediaTask *head, *tail;
    uint64_t play_seq;              /* the stamp's source: which element began to play last */
    uint64_t version;               /* the tree version the walk last saw */
    bool walked;
    DValue *then;                   /* Promise.prototype.then, taken at install */
    JSClassID error_class, ranges_class, tracks_class;
    DMutationConsumer mutations;    /* src set or changed: src_changed */
};

static JSClassID media_error_slot, time_ranges_slot, text_track_list_slot;

static bool is_media(const os64_html_node_t *node)
{
    return node != NULL && node->kind == OS64_HTML_ELEMENT && node->ns == OS64_HTML_NS_HTML &&
        (node->tag == OS64_HTML_TAG_AUDIO || node->tag == OS64_HTML_TAG_VIDEO);
}

static char *copy_text(os64_dom_t *dom, const char *text)
{
    if (text == NULL) return NULL;
    size_t n = os64_strlen(text);
    char *out = d_alloc(dom, n + 1);
    if (out != NULL) os64_memcpy(out, text, n + 1);
    return out;
}

static bool same_text(const char *a, const char *b)
{
    return a == NULL || b == NULL ? a == b : os64_streq(a, b);
}

/* ── The host ───────────────────────────────────────────────────────── */

void os64_dom_set_media(os64_dom_t *dom, const os64_dom_media_provider_t *provider)
{
    if (dom && !dom->closed && !dom->departure_finishing && !dom->departure_finished)
        dom->media_provider = provider ? *provider : (os64_dom_media_provider_t){0};
}

void os64_dom_set_fed(os64_dom_t *dom, const os64_dom_fed_provider_t *provider)
{
    if (dom && !dom->closed && !dom->departure_finishing && !dom->departure_finished)
        dom->fed_provider = provider ? *provider : (os64_dom_fed_provider_t){0};
}

static double gain_of(const DMediaElement *m) { return m->muted ? 0 : m->volume; }

static bool loops(const DMediaElement *m) { return os64_html_attr(m->node, "loop") != NULL; }

/* Whether the whole resource is wanted before play() asks: preload auto
 * (or empty), or autoplay, which preload is a hint to (HTML § 4.8.11.3). */
static bool wants_all(const DMediaElement *m)
{
    const os64_html_attr_t *preload = os64_html_attr(m->node, "preload");
    return (preload != NULL && (preload->value[0] == '\0' || os64_streq_nocase(preload->value, "auto"))) ||
           os64_html_attr(m->node, "autoplay") != NULL;
}

static void tell_settings(os64_dom_t *dom, DMediaElement *m)
{
    m->loop_seen = loops(m);
    m->all_seen = wants_all(m);
    if (m->session != NULL && dom->media_provider.settings != NULL)
        dom->media_provider.settings(dom->media_provider.opaque, m->session, gain_of(m), m->muted, m->loop_seen,
                                     m->all_seen);
}

/* The session given back. An attached MediaSource's is the fed
 * source's to end: the detach (mse.c), which closes it, runs whenever the
 * element lets its resource go (a load, the dedicated media source failure
 * steps, the page leaving). */
static void session_close(os64_dom_t *dom, DMediaElement *m)
{
    if (!JS_IsUndefined(m->fed)) {
        JSValue source = m->fed;
        m->fed = JS_UNDEFINED;
        m->session = NULL;
        d_mse_detach(dom, source);
        JS_FreeValueRT(dom->engine, source);
        return;
    }
    if (m->session != NULL && dom->media_provider.close != NULL)
        dom->media_provider.close(dom->media_provider.opaque, m->session);
    m->session = NULL;
}

/* ── Records and tasks ──────────────────────────────────────────────── */

static DMediaElement *record_of(os64_dom_t *dom, const os64_html_node_t *node, bool make)
{
    struct DMediaElements *s = dom->media_elements;
    if (s == NULL || !is_media(node)) return NULL;
    for (DMediaElement *m = s->records; m != NULL; m = m->next)
        if (m->node == node) return m;
    if (!make) return NULL;
    DMediaElement *m = d_alloc(dom, sizeof(*m));
    if (m == NULL) return NULL;
    os64_html_hold(dom->document, node);
    m->node = node;
    m->paused = true;
    m->show_poster = true;
    m->autoplaying = true;
    m->volume = 1;
    m->rate = m->default_rate = 1;
    m->preserves_pitch = true;
    /* HTML: muted starts as the content attribute says when the element is
     * made; after that the two are apart (defaultMuted reflects it). */
    m->muted = os64_html_attr(node, "muted") != NULL;
    m->duration = NAN;
    m->error_object = JS_UNDEFINED;
    m->text_tracks = JS_UNDEFINED;
    m->fed = JS_UNDEFINED;
    m->next = s->records;
    s->records = m;
    return m;
}

static void promises_release(os64_dom_t *dom, DMediaPromise *p, uint32_t count);

static void append_task(os64_dom_t *dom, DMediaTask *t)
{
    struct DMediaElements *s = dom->media_elements;
    if (s->tail != NULL) s->tail->next = t;
    else s->head = t;
    s->tail = t;
}

/* A media task for `m`: its event, and with `settle` the promises pending
 * now, which the task settles after the event. Out of memory, the event
 * is lost and the promises are left pending for the next settling step. */
static void queue(os64_dom_t *dom, DMediaElement *m, const char *type, uint8_t settle)
{
    DMediaTask *t = d_alloc(dom, sizeof(*t));
    if (t == NULL) return;
    t->media = m;
    t->wrapper = d_holder_take(dom, m->node);
    t->object = JS_UNDEFINED;
    t->type = type;
    t->settle = settle;
    if (settle != SETTLE_NONE) {
        t->promises = m->pending;
        t->promise_count = m->pending_count;
        m->pending = NULL;
        m->pending_count = m->pending_cap = 0;
    }
    append_task(dom, t);
}

/* A clocked event (CLOCKED_*) for `m`, unless one of that kind is queued
 * and not yet delivered. */
static void queue_clocked(os64_dom_t *dom, DMediaElement *m, const char *type, uint8_t bit)
{
    if (m->clocked_queued & bit) return;
    DMediaTask *tail = dom->media_elements->tail;
    queue(dom, m, type, SETTLE_NONE);
    if (dom->media_elements->tail != tail) {
        dom->media_elements->tail->clocked = bit;
        m->clocked_queued |= bit;
    }
}


DMediaTask *d_media_queue_plain(os64_dom_t *dom, JSValueConst object, DValue *target, const char *type)
{
    if (dom->media_elements == NULL) return NULL;
    DMediaTask *t = d_alloc(dom, sizeof(*t));
    if (t == NULL) return NULL;
    t->wrapper = JS_UNDEFINED;
    t->object = JS_DupValueRT(dom->engine, object);
    t->plain = target;
    t->type = type;
    append_task(dom, t);
    return t;
}

DMediaTask *d_media_queue_work(os64_dom_t *dom, JSValueConst object, void (*run)(os64_dom_t *dom, void *work),
                               void (*left)(void *work, DMediaTask *task), void *work)
{
    if (dom->media_elements == NULL) return NULL;
    DMediaTask *t = d_alloc(dom, sizeof(*t));
    if (t == NULL) return NULL;
    t->wrapper = JS_UNDEFINED;
    t->object = JS_DupValueRT(dom->engine, object);
    t->run = run;
    t->left = left;
    t->work = work;
    append_task(dom, t);
    return t;
}

void d_media_task_cancel(DMediaTask *task)
{
    if (task != NULL) task->cancelled = true;
}

/* A task leaves the queue, delivered or dropped: its element may queue
 * another of its clocked kind, and a work task's owner forgets it. */
static void task_leaves(DMediaTask *t)
{
    if (t->clocked != 0 && t->media != NULL) t->media->clocked_queued &= (uint8_t)~t->clocked;
    if (t->left != NULL) t->left(t->work, t);
    t->left = NULL;
}

/* A task's own values let go, the task freed. */
static void task_free(os64_dom_t *dom, DMediaTask *t)
{
    promises_release(dom, t->promises, t->promise_count);
    JS_FreeValueRT(dom->engine, t->wrapper);
    JS_FreeValueRT(dom->engine, t->object);
    d_free(dom, t);
}

static void promises_release(os64_dom_t *dom, DMediaPromise *p, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        JS_FreeValueRT(dom->engine, p[i].promise);
        JS_FreeValueRT(dom->engine, p[i].resolve);
        JS_FreeValueRT(dom->engine, p[i].reject);
    }
    d_free(dom, p);
}

/* HTML's "notify about playing": a task that fires playing and resolves
 * the promises pending now. */
static void notify_playing(os64_dom_t *dom, DMediaElement *m)
{
    queue(dom, m, "playing", SETTLE_RESOLVE);
}

/* HTML's internal pause steps. */
static void internal_pause(os64_dom_t *dom, DMediaElement *m)
{
    m->autoplaying = false;
    if (m->paused) return;
    m->paused = true;
    queue(dom, m, "timeupdate", SETTLE_NONE);
    queue(dom, m, "pause", SETTLE_ABORT);
    if (m->session != NULL) {
        if (dom->media_provider.facts != NULL) {
            os64_dom_media_facts_t f = {0};
            dom->media_provider.facts(dom->media_provider.opaque, m->session, &f);
            if (!m->seeking) m->position = f.position;
        }
        if (dom->media_provider.pause != NULL) dom->media_provider.pause(dom->media_provider.opaque, m->session);
    }
}

/* Whether the page may play sound now (§ Autoplay): a muted element may
 * play whenever; one with sound once the person has interacted with the
 * page. */
static bool allowed(os64_dom_t *dom, const DMediaElement *m)
{
    return m->muted ||
        (dom->media_provider.activated != NULL && dom->media_provider.activated(dom->media_provider.opaque));
}

static void session_wake(os64_dom_t *dom, DMediaElement *m);

/* HTML's seek steps; a resource the host says cannot seek is not sought
 * (HTML aborts a seek outside the seekable ranges). A fed element's
 * seekable range is its MediaSource's (d_mse_seekable), not the host's
 * fact: with none the seek is not made, and a target outside it is moved
 * to its nearest end. */
static void seek(os64_dom_t *dom, DMediaElement *m, double t)
{
    session_wake(dom, m);
    if (!JS_IsUndefined(m->fed)) {
        os64_dom_fed_range_t range;
        if (d_mse_seekable(dom, m->fed, &range) == 0) return;
        if (t < range.start) t = range.start;
        if (t > range.end) t = range.end;
    } else if (m->session != NULL && dom->media_provider.facts != NULL) {
        os64_dom_media_facts_t f = {0};
        dom->media_provider.facts(dom->media_provider.opaque, m->session, &f);
        if (f.probed && !f.seekable) return;
    }
    if (isfinite(m->duration) && t > m->duration) t = m->duration;
    if (!(t > 0)) t = 0;
    m->seeking = true;
    m->seek_to = t;
    m->position = t;
    m->ended = false;
    m->show_poster = false;
    m->seek_generation++;
    queue(dom, m, "seeking", SETTLE_NONE);
    if (m->session != NULL && dom->media_provider.seek != NULL)
        dom->media_provider.seek(dom->media_provider.opaque, m->session, t, m->seek_generation);
}

/* HTML's media element load algorithm, the steps before resource
 * selection. With `ctx` (a script's call) the promises of the element's
 * tasks still queued are settled now, in their order, and the tasks
 * dropped, as the standard does; from the turn they run as queued. */
static void media_load(os64_dom_t *dom, JSContext *ctx, DMediaElement *m)
{
    struct DMediaElements *s = dom->media_elements;
    /* Whichever way the load came (load(), the src setter, the turn), it
     * has acted on the src attribute as it stands, so the turn does not
     * take that same src for a change and load a second time. */
    const os64_html_attr_t *src = os64_html_attr(m->node, "src");
    char *copy = copy_text(dom, src != NULL ? src->value : NULL);
    if (src == NULL || copy != NULL) {
        d_free(dom, m->src_seen);
        m->src_seen = copy;
        m->src_looked = true;
    }
    m->select_pending = false;
    m->deferred = false;
    m->dormant = false;
    m->awaiting_source = false;
    m->candidate = 0;
    if (ctx != NULL) {
        DMediaTask **link = &s->head, *last = NULL;
        while (*link != NULL) {
            DMediaTask *t = *link;
            if (t->media != m) { last = t; link = &t->next; continue; }
            *link = t->next;
            task_leaves(t);
            for (uint32_t i = 0; i < t->promise_count; i++) {
                DMediaPromise *p = &t->promises[i];
                JSValue arg = JS_UNDEFINED;
                if (t->settle != SETTLE_RESOLVE) {
                    JSValue e = d_error(ctx, t->settle == SETTLE_ABORT ? "AbortError" : "NotSupportedError",
                                        t->settle == SETTLE_ABORT ? "The play() request was interrupted"
                                                                  : "The element has no supported source");
                    arg = JS_GetException(ctx);
                    JS_FreeValue(ctx, e);
                }
                JSValue r = JS_Call(ctx, t->settle == SETTLE_RESOLVE ? p->resolve : p->reject, JS_UNDEFINED, 1, &arg);
                JS_FreeValue(ctx, r);
                JS_FreeValue(ctx, arg);
            }
            promises_release(dom, t->promises, t->promise_count);
            JS_FreeValueRT(dom->engine, t->wrapper);
            d_free(dom, t);
        }
        s->tail = last;
    }
    if (m->network == N_LOADING || m->network == N_IDLE) queue(dom, m, "abort", SETTLE_NONE);
    if (m->network != N_EMPTY) {
        queue(dom, m, "emptied", SETTLE_NONE);
        session_close(dom, m);
        m->ready = R_NOTHING;
        if (!m->paused) {
            m->paused = true;
            queue(dom, m, NULL, SETTLE_ABORT);
        }
        m->seeking = false;
        if (m->position != 0) queue(dom, m, "timeupdate", SETTLE_NONE);
        m->position = 0;
        m->duration = NAN;
    }
    /* The poster again for the new resource, unless the element plays: a
     * play() before the turn's first look at src leaves it unpaused with
     * nothing to pause (network still empty), and the poster must not come
     * back over its picture. */
    m->show_poster = m->paused;
    m->played_end = 0;              /* played is the resource's, and this is another */
    d_free(dom, m->current_src);    /* none is selected until the selection says (currentSrc "") */
    m->current_src = NULL;
    m->loadeddata = false;
    m->ended = false;
    m->video_width = m->video_height = 0;   /* HTML: 0 until HAVE_METADATA */
    m->error_seen = false;
    m->settings_applied = false;
    m->default_start = 0;
    m->rate = m->default_rate;
    m->error = 0;
    m->message[0] = '\0';
    if (!JS_IsUndefined(m->error_object)) {
        JS_FreeValueRT(dom->engine, m->error_object);
        m->error_object = JS_UNDEFINED;
    }
    m->autoplaying = true;
    m->network = N_NO_SOURCE;
    m->select_pending = true;
}

/* ── Resource selection ─────────────────────────────────────────────── */

/* The `index`th <source> child that names a source this host may play:
 * a src, and a type canPlayType does not answer "" for. */
static const os64_html_node_t *candidate_at(os64_dom_t *dom, const os64_html_node_t *node, uint32_t index)
{
    for (const os64_html_node_t *c = node->first_child; c != NULL; c = c->next) {
        if (c->kind != OS64_HTML_ELEMENT || c->ns != OS64_HTML_NS_HTML || c->tag != OS64_HTML_TAG_SOURCE) continue;
        const os64_html_attr_t *src = os64_html_attr(c, "src");
        if (src == NULL || src->value[0] == '\0') continue;
        const os64_html_attr_t *type = os64_html_attr(c, "type");
        if (type != NULL && (dom->media_provider.can_play == NULL ||
                             dom->media_provider.can_play(dom->media_provider.opaque, type->value) == 0))
            continue;
        if (index-- == 0) return c;
    }
    return NULL;
}

static bool has_source_child(const os64_html_node_t *node)
{
    for (const os64_html_node_t *c = node->first_child; c != NULL; c = c->next)
        if (c->kind == OS64_HTML_ELEMENT && c->ns == OS64_HTML_NS_HTML && c->tag == OS64_HTML_TAG_SOURCE) return true;
    return false;
}

static void set_message(DMediaElement *m, const char *text)
{
    os64_strcopy(m->message, sizeof(m->message), text != NULL ? text : "");
}

static os64_dom_media_cors_t cors_of(const DMediaElement *m)
{
    const os64_html_attr_t *cors = os64_html_attr(m->node, "crossorigin");
    return cors == NULL ? OS64_DOM_MEDIA_NO_CORS
        : os64_streq_nocase(cors->value, "use-credentials") ? OS64_DOM_MEDIA_CORS_CREDENTIALS
        : OS64_DOM_MEDIA_CORS_ANONYMOUS;
}

/* A DORMANT element gave its session back (step: ended or failed, paused,
 * in no document), which is what lets a page play `new Audio(x)` a
 * thousand times without the host keeping a thousand decoders. play() or
 * a seek wakes it: a new session for the resource it had, the whole of it
 * wanted. One that will not open is the element's network error. */
static void session_wake(os64_dom_t *dom, DMediaElement *m)
{
    if (!m->dormant)
        return;
    m->dormant = false;
    m->session = dom->media_provider.open != NULL && m->current_src != NULL
        ? dom->media_provider.open(dom->media_provider.opaque, m->node, m->current_src, cors_of(m),
                                   OS64_DOM_MEDIA_PRELOAD_AUTO)
        : NULL;
    if (m->session == NULL) {
        m->error = OS64_DOM_MEDIA_ERR_NETWORK;
        set_message(m, "The sound could not be loaded again");
        if (dom->media_provider.failed != NULL && m->current_src != NULL)
            dom->media_provider.failed(dom->media_provider.opaque, m->node, NULL, m->current_src, m->error, m->message);
        m->error_seen = true;
        queue(dom, m, "error", SETTLE_NONE);
        return;
    }
    m->error = 0;
    m->message[0] = '\0';
    m->error_seen = false;
    if (!JS_IsUndefined(m->error_object)) {
        JS_FreeValueRT(dom->engine, m->error_object);
        m->error_object = JS_UNDEFINED;
    }
    tell_settings(dom, m);
}

/* The dedicated media source failure steps (a src that cannot play), or,
 * for a <source>, on to the next candidate. */
static void select_resource(os64_dom_t *dom, DMediaElement *m);

static void source_failed(os64_dom_t *dom, DMediaElement *m, const char *why)
{
    session_close(dom, m);
    if (m->from_children) {
        /* The next candidate's session is new: its error is unseen. */
        m->error_seen = false;
        m->candidate++;
        select_resource(dom, m);
        return;
    }
    m->error = OS64_DOM_MEDIA_ERR_SRC_NOT_SUPPORTED;
    set_message(m, why);
    m->network = N_NO_SOURCE;
    queue(dom, m, "error", SETTLE_NOT_SUPPORTED);
}

static void select_resource(os64_dom_t *dom, DMediaElement *m)
{
    m->select_pending = false;
    const os64_html_attr_t *src = os64_html_attr(m->node, "src");
    const char *reference = NULL;
    if (src != NULL) {
        m->from_children = false;
        reference = src->value;
    } else {
        const os64_html_node_t *c = candidate_at(dom, m->node, m->candidate);
        if (c == NULL) {
            /* No source (or none left): the element waits for one, with no
             * error of its own, as the standard's children mode does; a
             * <source> added later resumes it from this candidate (step). */
            m->network = m->from_children ? N_NO_SOURCE : N_EMPTY;
            m->awaiting_source = m->from_children;
            m->from_children = false;
            return;
        }
        m->from_children = true;
        reference = os64_html_attr(c, "src")->value;
    }
    /* loadstart once per selection: not again for the next candidate, nor
     * for a load preload=none deferred (left NETWORK_IDLE) going on. */
    if ((m->candidate == 0 || !m->from_children) && m->network != N_IDLE) {
        m->network = N_LOADING;
        queue(dom, m, "loadstart", SETTLE_NONE);
    }
    /* Resolved as a link's href is (os64_dom_resolve): one rule for every
     * address on the page. */
    d_free(dom, m->current_src);
    m->current_src = NULL;
    char *url = reference[0] != '\0' ? d_alloc(dom, OS64_DOM_URL_MAX) : NULL;
    if (url != NULL &&
        os64_dom_resolve(dom->document, dom->url != NULL ? dom->url : "about:blank", reference, url, OS64_DOM_URL_MAX))
        m->current_src = copy_text(dom, url);
    d_free(dom, url);
    if (m->current_src == NULL) {
        if (dom->media_provider.failed != NULL)
            dom->media_provider.failed(dom->media_provider.opaque, m->node, NULL, reference,
                                       OS64_DOM_MEDIA_ERR_SRC_NOT_SUPPORTED, "The address does not resolve");
        source_failed(dom, m, "The address does not resolve");
        return;
    }
    /* A MediaSource's object URL: the resource fetch's local mode, the
     * source attached (MSE § 3.15.1). One that will not attach (not
     * closed, attached elsewhere, or no host) cannot be fetched at all. */
    JSValue source = d_mse_lookup(dom, m->current_src);
    if (!JS_IsUndefined(source)) {
        void *session = NULL;
        if (!d_mse_attach(dom, source, m, m->node, &session)) {
            if (dom->media_provider.failed != NULL)
                dom->media_provider.failed(dom->media_provider.opaque, m->node, NULL, m->current_src,
                                           OS64_DOM_MEDIA_ERR_SRC_NOT_SUPPORTED, "The MediaSource cannot be attached");
            source_failed(dom, m, "The MediaSource cannot be attached");
            return;
        }
        m->fed = JS_DupValueRT(dom->engine, source);
        m->session = session;
        m->network = N_LOADING;
        tell_settings(dom, m);
        m->settings_applied = true;
        if (!m->paused && dom->media_provider.play != NULL)
            dom->media_provider.play(dom->media_provider.opaque, m->session);
        return;
    }
    const os64_html_attr_t *preload = os64_html_attr(m->node, "preload");
    bool none = preload != NULL && os64_streq_nocase(preload->value, "none");
    if (none && !m->wanted && m->paused && os64_html_attr(m->node, "autoplay") == NULL) {
        m->network = N_IDLE;
        m->deferred = true;
        queue(dom, m, "suspend", SETTLE_NONE);
        return;
    }
    m->network = N_LOADING;
    bool all = wants_all(m);
    m->session = dom->media_provider.open != NULL
        ? dom->media_provider.open(dom->media_provider.opaque, m->node, m->current_src, cors_of(m),
                                   all ? OS64_DOM_MEDIA_PRELOAD_AUTO : OS64_DOM_MEDIA_PRELOAD_METADATA)
        : NULL;
    if (m->session == NULL) {
        if (dom->media_provider.failed != NULL)
            dom->media_provider.failed(dom->media_provider.opaque, m->node, NULL, m->current_src,
                                       OS64_DOM_MEDIA_ERR_SRC_NOT_SUPPORTED, "The sound could not be loaded");
        source_failed(dom, m, "The sound could not be loaded");
        return;
    }
    tell_settings(dom, m);
    m->settings_applied = true;
    if (!m->paused && dom->media_provider.play != NULL)
        dom->media_provider.play(dom->media_provider.opaque, m->session);
}

/* ── The turn ───────────────────────────────────────────────────────── */

static void ready_to(os64_dom_t *dom, DMediaElement *m, uint8_t want)
{
    uint8_t old = m->ready;
    if (want == old) return;
    m->ready = want;
    if (old < R_CURRENT && want >= R_CURRENT && !m->loadeddata) {
        m->loadeddata = true;
        queue(dom, m, "loadeddata", SETTLE_NONE);
    }
    if (old >= R_FUTURE && want <= R_CURRENT && !m->paused && !m->ended) {
        queue(dom, m, "timeupdate", SETTLE_NONE);
        queue(dom, m, "waiting", SETTLE_NONE);
    }
    if (old <= R_CURRENT && want >= R_FUTURE) {
        queue(dom, m, "canplay", SETTLE_NONE);
        if (!m->paused) notify_playing(dom, m);
    }
    if (old < R_ENOUGH && want == R_ENOUGH) {
        /* HTML's autoplay: the element starts by itself once it can play
         * through, if it may (§ Autoplay). */
        if (m->autoplaying && m->paused && os64_html_attr(m->node, "autoplay") != NULL && allowed(dom, m)) {
            m->paused = false;
            m->show_poster = false;
            m->played_seq = ++dom->media_elements->play_seq;
            m->autoplaying = false;
            queue(dom, m, "play", SETTLE_NONE);
            notify_playing(dom, m);
            if (m->session != NULL && dom->media_provider.play != NULL)
                dom->media_provider.play(dom->media_provider.opaque, m->session);
        }
        queue(dom, m, "canplaythrough", SETTLE_NONE);
    }
}

/* HAVE_METADATA from HAVE_NOTHING: the duration (a fed element's is its
 * MediaSource's, already set), the picture's size, loadedmetadata. */
static void metadata_reached(os64_dom_t *dom, DMediaElement *m, const os64_dom_media_facts_t *f, bool fed)
{
    if (!fed) {
        m->duration = f->duration;
        queue(dom, m, "durationchange", SETTLE_NONE);
    }
    /* A video's size is set and resize queued before HAVE_METADATA's
     * loadedmetadata (HTML § 4.8.9); again at each later change. */
    if (f->picture && f->width > 0 && f->height > 0) {
        m->video_width = f->width;
        m->video_height = f->height;
        queue(dom, m, "resize", SETTLE_NONE);
    }
    m->ready = R_METADATA;
    queue(dom, m, "loadedmetadata", SETTLE_NONE);
    if (m->default_start > 0) seek(dom, m, m->default_start);
}

static void fold_facts(os64_dom_t *dom, DMediaElement *m)
{
    if (m->session == NULL || dom->media_provider.facts == NULL) return;
    os64_dom_media_facts_t f = {0};
    dom->media_provider.facts(dom->media_provider.opaque, m->session, &f);
    if (m->session == NULL) return;
    uint64_t now = d_host_now_ms(dom);
    /* A fed element (an attached MediaSource): its metadata is every
     * SourceBuffer's first initialization segment, which libdom saw
     * (MSE § 5.5.7 step 10), and its duration is the MediaSource's, set by
     * the duration change (d_media_fed_duration). */
    bool fed = !JS_IsUndefined(m->fed);
    if (fed) {
        f.probed = d_mse_probed(dom, m->fed);
        f.duration = m->duration;
    }
    if (f.error != 0 && !m->error_seen) {
        /* A decode error in play ends a fed element's source with a decode
         * error (the end of stream algorithm's MSE half); the element's
         * half is the code below, as for any session. */
        if (fed && f.error == OS64_DOM_MEDIA_ERR_DECODE) d_mse_decode_error(dom, m->fed);
        m->error_seen = true;
        f.message[sizeof(f.message) - 1] = '\0';
        if (dom->media_provider.failed != NULL)
            dom->media_provider.failed(dom->media_provider.opaque, m->node, m->session, m->current_src,
                                       m->ready == R_NOTHING ? OS64_DOM_MEDIA_ERR_SRC_NOT_SUPPORTED : f.error,
                                       f.message);
        if (m->ready == R_NOTHING) {
            source_failed(dom, m, f.message);
            return;
        }
        m->error = f.error;
        set_message(m, f.message);
        m->network = N_IDLE;
        queue(dom, m, "error", SETTLE_NONE);
    }
    if (f.probed && m->ready == R_NOTHING) {
        metadata_reached(dom, m, &f, fed);
    } else if (!fed && f.probed && !isnan(f.duration) && !(f.duration == m->duration)) {
        m->duration = f.duration;
        queue(dom, m, "durationchange", SETTLE_NONE);
    }
    if (m->ready >= R_METADATA && f.picture && f.width > 0 && f.height > 0 &&
        (f.width != m->video_width || f.height != m->video_height)) {
        m->video_width = f.width;
        m->video_height = f.height;
        queue(dom, m, "resize", SETTLE_NONE);
    }
    if (m->network == N_LOADING) {
        if (f.arrived != m->last_arrived && now - m->last_progress_ms >= 350) {
            m->last_arrived = f.arrived;
            m->last_progress_ms = now;
            queue_clocked(dom, m, "progress", CLOCKED_PROGRESS);
        }
        if (f.loaded) {
            if (f.arrived != m->last_arrived || m->last_progress_ms == 0) queue(dom, m, "progress", SETTLE_NONE);
            m->last_arrived = f.arrived;
            m->network = N_IDLE;
            queue(dom, m, "suspend", SETTLE_NONE);
        }
    }
    if (m->seeking && f.seeked == m->seek_generation) {
        /* Where the host landed, which is where it was when it could not
         * move. */
        m->seeking = false;
        m->position = f.position;
        queue(dom, m, "timeupdate", SETTLE_NONE);
        queue(dom, m, "seeked", SETTLE_NONE);
        m->last_timeupdate_ms = now;
    }
    if (m->ready >= R_METADATA) {
        uint8_t want = f.enough ? R_ENOUGH : f.future ? R_FUTURE : f.current ? R_CURRENT : R_METADATA;
        /* The host cannot sound it now (the device another program's, or
         * its mix full): the element waits, as it would for a slow network
         * (§ When the stream is busy). */
        if (f.busy && !m->paused && want > R_CURRENT) want = R_CURRENT;
        ready_to(dom, m, want);
    }
    if (!m->paused && !m->seeking) {
        m->position = f.position;
        if (f.position > m->played_end) m->played_end = f.position;
        if ((f.sounding || f.running) && now - m->last_timeupdate_ms >= 250) {
            m->last_timeupdate_ms = now;
            queue_clocked(dom, m, "timeupdate", CLOCKED_TIMEUPDATE);
        }
    }
    /* The end with loop on: the host loops by itself when loop was on as
     * its decoder reached the end; one turned on after that is HTML's own
     * loop step, a seek to the start. An end a failure made (the sound ran
     * out at the error) is no loop point: it ends the element, or a loop
     * would replay the same failure forever. */
    bool loop_point = loops(m) && m->error == 0;
    if (f.ended && !m->ended && !m->seeking && loop_point) seek(dom, m, 0);
    if (f.ended && !m->ended && !m->seeking && !loop_point) {
        m->ended = true;
        if (isfinite(m->duration)) m->position = m->duration;
        queue(dom, m, "timeupdate", SETTLE_NONE);
        if (!m->paused) {
            /* Paused for the host too: a seek back must not sound until
             * play() is asked again. */
            m->paused = true;
            queue(dom, m, "pause", SETTLE_ABORT);
            if (dom->media_provider.pause != NULL) dom->media_provider.pause(dom->media_provider.opaque, m->session);
        }
        queue(dom, m, "ended", SETTLE_NONE);
    }
}

/* The walk, once per tree version: a record for each <audio> and <video> in the
 * document, and the removal steps for one that has left it. */
static void walk(os64_dom_t *dom)
{
    struct DMediaElements *s = dom->media_elements;
    for (DMediaElement *m = s->records; m != NULL; m = m->next) m->seen = false;
    const os64_html_node_t *root = dom->document->document;
    for (const os64_html_node_t *at = root; at != NULL; at = os64_html_shadow_including_next(root, at))
        if (is_media(at)) {
            DMediaElement *m = record_of(dom, at, true);
            if (m != NULL) m->seen = true;
        }
    for (DMediaElement *m = s->records; m != NULL; m = m->next) {
        /* HTML: a media element removed from its document pauses. */
        if (m->connected && !m->seen) internal_pause(dom, m);
        m->connected = m->seen;
    }
}

static void step(os64_dom_t *dom, DMediaElement *m)
{
    const os64_html_attr_t *src = os64_html_attr(m->node, "src");
    const char *now = src != NULL ? src->value : NULL;
    if (!m->src_looked || !same_text(now, m->src_seen)) {
        bool first = !m->src_looked;
        m->src_looked = true;
        char *copy = copy_text(dom, now);
        if (now == NULL || copy != NULL) {
            d_free(dom, m->src_seen);
            m->src_seen = copy;
            /* HTML: setting or changing src loads; removing it does not.
             * The first look at a parsed element's src is the load the
             * parser started, arriving late: a seek a script asked before
             * it (default_start) is the script's, as it would have been
             * had the load come first. */
            double start = m->default_start;
            if (now != NULL) media_load(dom, NULL, m);
            if (first) m->default_start = start;
        }
    }
    if (m->network == N_EMPTY && !m->select_pending && src == NULL && has_source_child(m->node))
        m->select_pending = true;
    if (m->awaiting_source && !m->select_pending && src == NULL && candidate_at(dom, m->node, m->candidate) != NULL) {
        m->awaiting_source = false;
        m->select_pending = true;
    }
    /* preload set to none deferred the load; set to anything else since,
     * it goes on (play() and load() end the wait their own way). */
    if (m->deferred && !m->select_pending) {
        const os64_html_attr_t *preload = os64_html_attr(m->node, "preload");
        if (preload == NULL || !os64_streq_nocase(preload->value, "none") || wants_all(m)) {
            m->deferred = false;
            m->select_pending = true;
        }
    }
    if (m->select_pending) select_resource(dom, m);
    if (m->session != NULL && (loops(m) != m->loop_seen || wants_all(m) != m->all_seen)) tell_settings(dom, m);
    fold_facts(dom, m);
    /* Done with, for now: ended or failed, paused, and in no document. Its
     * session goes back to the host (session_wake). Not a fed session:
     * closing it is the MediaSource's detach, which the page sees, and
     * nothing could open it again. */
    if (m->session != NULL && JS_IsUndefined(m->fed) && m->paused && !m->connected && !m->seeking &&
        (m->ended || m->error != 0)) {
        session_close(dom, m);
        m->dormant = true;
    }
}

bool os64_dom_media_turn(os64_dom_t *dom)
{
    if (!d_delivery_active(dom) || !dom->options.modern || dom->media_elements == NULL) return false;
    struct DMediaElements *s = dom->media_elements;
    if (!d_provider_enter(dom)) return false;
    uint64_t version = os64_html_version(dom->document);
    if (!s->walked || version != s->version) {
        walk(dom);
        s->version = version;
        s->walked = true;
    }
    for (DMediaElement *m = s->records; m != NULL; m = m->next) step(dom, m);
    d_mse_turn(dom);
    d_provider_leave(dom);
    return s->head != NULL;
}

bool os64_dom_media_due(const os64_dom_t *dom)
{
    return d_delivery_active(dom) && dom->media_elements != NULL && dom->media_elements->head != NULL;
}

/* ── The tasks ──────────────────────────────────────────────────────── */

/* A promise reaction that swallows the rejection: an unawaited play() a
 * pause interrupted is no unhandled rejection (as webanimation.c marks
 * its own). */
static JSValue swallow(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)ctx; (void)self; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

static void mark_handled(os64_dom_t *dom, JSContext *ctx, JSValueConst promise)
{
    if (dom->media_elements->then == NULL) return;
    JSValue noop = JS_NewCFunction(ctx, swallow, "", 1);
    if (JS_IsException(noop)) return;
    JSValue args[2] = {JS_UNDEFINED, noop};
    JSValue r = JS_Call(ctx, dom->media_elements->then->value, promise, 2, args);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, noop);
}

static void settle(os64_dom_t *dom, JSContext *ctx, DMediaTask *t)
{
    for (uint32_t i = 0; i < t->promise_count; i++) {
        DMediaPromise *p = &t->promises[i];
        JSValue arg = JS_UNDEFINED;
        if (t->settle != SETTLE_RESOLVE) {
            JSValue e = t->settle == SETTLE_ABORT
                ? d_error(ctx, "AbortError", "The play() request was interrupted")
                : d_error(ctx, "NotSupportedError", "The element has no supported source");
            arg = JS_GetException(ctx);
            JS_FreeValue(ctx, e);
            if (t->settle == SETTLE_ABORT) mark_handled(dom, ctx, p->promise);
        }
        JSValue r = JS_Call(ctx, t->settle == SETTLE_RESOLVE ? p->resolve : p->reject, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, arg);
    }
}

bool os64_dom_media_fire(os64_dom_t *dom, os64_js_outcome_t *outcome)
{
    if (outcome != NULL) os64_memset(outcome, 0, sizeof(*outcome));
    struct DMediaElements *s = d_delivery_active(dom) ? dom->media_elements : NULL;
    if (outcome == NULL || s == NULL || s->head == NULL) return false;
    DMediaTask *t = s->head;
    s->head = t->next;
    if (s->head == NULL) s->tail = NULL;
    /* MSE's work runs as the task (a provider call and what it queues),
     * with no script of its own; a cancelled one leaves without running. */
    if (t->run != NULL || t->cancelled) {
        void (*run)(os64_dom_t *, void *) = t->cancelled ? NULL : t->run;
        void *work = t->work;
        task_leaves(t);
        if (run != NULL && d_provider_enter(dom)) {
            run(dom, work);
            d_provider_leave(dom);
        }
        task_free(dom, t);
        return true;
    }
    task_leaves(t);
    char name[OS64_JS_SOURCE_NAME_CAP];
    os64_snprintf(name, sizeof(name), "%.*s#media-%s", (int)(sizeof(name) - 40),
                  dom->url != NULL ? dom->url : "about:blank", t->type != NULL ? t->type : "settle");
    if (os64_js_realm_task_begin(dom->realm, OS64_JS_ABI_ID, name, outcome) == OS64_JS_OK) {
        os64_js_outcome_t access;
        JSContext *ctx = os64_js_realm_context(dom->realm, OS64_JS_ABI_ID, &access);
        if (ctx == NULL) d_outcome_fold(outcome, &access);
        if (ctx != NULL && t->type != NULL) {
            JSValue event = d_event_new(dom, ctx, t->type, false, false, true);
            if (JS_IsException(event)) {
                JSValue error = JS_GetException(ctx);
                JS_FreeValue(ctx, error);
            } else {
                if (t->plain != NULL) d_dispatch_plain_task(dom, ctx, event, t->plain, outcome);
                else d_dispatch_node_task(dom, ctx, event, t->media->node, outcome);
                JS_FreeValue(ctx, event);
            }
        }
        if (ctx != NULL && d_delivery_active(dom)) settle(dom, ctx, t);
        d_task_finish(dom, outcome);
    }
    d_task_close(dom, name, outcome);
    task_free(dom, t);
    return true;
}

/* ── Departure and teardown ─────────────────────────────────────────── */

static void tasks_drop(os64_dom_t *dom)
{
    struct DMediaElements *s = dom->media_elements;
    while (s->head != NULL) {
        DMediaTask *t = s->head;
        s->head = t->next;
        task_leaves(t);
        task_free(dom, t);
    }
    s->tail = NULL;
}

void d_media_elements_cancel(os64_dom_t *dom)
{
    if (dom->media_elements == NULL) return;
    for (DMediaElement *m = dom->media_elements->records; m != NULL; m = m->next) session_close(dom, m);
    tasks_drop(dom);
}

void d_media_elements_drain(os64_dom_t *dom)
{
    if (dom->media_elements == NULL) return;
    d_media_elements_cancel(dom);
    for (DMediaElement *m = dom->media_elements->records; m != NULL; m = m->next) {
        promises_release(dom, m->pending, m->pending_count);
        m->pending = NULL;
        m->pending_count = m->pending_cap = 0;
        JS_FreeValueRT(dom->engine, m->error_object);
        m->error_object = JS_UNDEFINED;
        JS_FreeValueRT(dom->engine, m->text_tracks);
        m->text_tracks = JS_UNDEFINED;
    }
    d_mse_drain(dom);
}

void d_media_elements_free(os64_dom_t *dom)
{
    struct DMediaElements *s = dom->media_elements;
    if (s == NULL) return;
    while (s->records != NULL) {
        DMediaElement *m = s->records;
        s->records = m->next;
        d_free(dom, m->src_seen);
        d_free(dom, m->current_src);
        d_free(dom, m->pending);
        os64_html_release(dom->document, m->node);
        d_free(dom, m);
    }
    for (DMediaTask *t = s->head; t != NULL;) {
        DMediaTask *next = t->next;
        d_free(dom, t->promises);
        d_free(dom, t);
        t = next;
    }
    d_free(dom, s);
    dom->media_elements = NULL;
    d_mse_free(dom);
}

/* ── MediaError and TimeRanges ──────────────────────────────────────── */

typedef struct { int code; char message[96]; } DMediaError;
/* A TimeRanges: normalized ranges (sorted, disjoint), a static copy of
 * what the member answered when it was read. */
typedef struct { uint32_t count; os64_dom_fed_range_t range[]; } DTimeRanges;

static void opaque_free(JSRuntime *rt, JSValue value)
{
    JSClassID id = JS_GetClassID(value);
    js_free_rt(rt, JS_GetOpaque(value, id));
}

static JSValue error_object(os64_dom_t *dom, JSContext *ctx, DMediaElement *m)
{
    if (m->error == 0) return JS_NULL;
    if (JS_IsUndefined(m->error_object)) {
        JSValue o = JS_NewObjectClass(ctx, (int)dom->media_elements->error_class);
        if (JS_IsException(o)) return o;
        DMediaError *e = js_mallocz(ctx, sizeof(*e));
        if (e == NULL) { JS_FreeValue(ctx, o); return JS_EXCEPTION; }
        e->code = m->error;
        os64_strcopy(e->message, sizeof(e->message), m->message);
        JS_SetOpaque(o, e);
        m->error_object = o;
    }
    return JS_DupValue(ctx, m->error_object);
}

static JSValue ranges_of(os64_dom_t *dom, JSContext *ctx, const os64_dom_fed_range_t *ranges, uint32_t count)
{
    JSValue o = JS_NewObjectClass(ctx, (int)dom->media_elements->ranges_class);
    if (JS_IsException(o)) return o;
    DTimeRanges *r = js_mallocz(ctx, sizeof(*r) + sizeof(r->range[0]) * count);
    if (r == NULL) { JS_FreeValue(ctx, o); return JS_EXCEPTION; }
    r->count = count;
    if (count != 0) os64_memcpy(r->range, ranges, sizeof(r->range[0]) * count);
    JS_SetOpaque(o, r);
    return o;
}

JSValue d_media_ranges(os64_dom_t *dom, JSContext *ctx, const os64_dom_fed_range_t *ranges, uint32_t count)
{
    return ranges_of(dom, ctx, ranges, count);
}

static JSValue ranges_object(os64_dom_t *dom, JSContext *ctx, uint32_t count, double start, double end)
{
    os64_dom_fed_range_t one = {start, end};
    return ranges_of(dom, ctx, &one, count != 0 ? 1 : 0);
}

/* A media element's TextTrackList (HTML § 4.8.11.11.1), always empty:
 * yonder reads no text tracks from a resource, and addTextTrack() is the
 * video arc's (AUDIO_YONDER.md's tracks row), so nothing is ever added and
 * the binding never fires its change, addtrack or removetrack events
 * (a page may dispatch them itself, as at any EventTarget). It is still an
 * EventTarget a player can listen on, with its entry in the object (A
 * PLAIN EVENT TARGET, internal.h), and the same object on every read: the
 * element's record holds it until the page's drain. */
typedef struct {
    DValue target;              /* first: the plain target's entry, in the object */
} DTrackList;

static void tracks_finalizer(JSRuntime *rt, JSValue value)
{
    DTrackList *t = JS_GetOpaque(value, os64_js_class_id(&text_track_list_slot));
    if (t == NULL) return;
    d_event_target_finalize(rt, &t->target);
    js_free_rt(rt, t);
}

static void tracks_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DTrackList *t = JS_GetOpaque(value, os64_js_class_id(&text_track_list_slot));
    if (t != NULL) d_event_target_mark(rt, &t->target, mark);
}

static JSValue tracks_object(os64_dom_t *dom, JSContext *ctx, DMediaElement *m)
{
    if (JS_IsUndefined(m->text_tracks)) {
        JSValue o = JS_NewObjectClass(ctx, (int)dom->media_elements->tracks_class);
        if (JS_IsException(o)) return o;
        DTrackList *t = js_mallocz(ctx, sizeof(*t));
        if (t == NULL) { JS_FreeValue(ctx, o); return JS_EXCEPTION; }
        d_event_target_init(dom, &t->target, o);
        JS_SetOpaque(o, t);
        m->text_tracks = o;
    }
    return JS_DupValue(ctx, m->text_tracks);
}

enum { E_CODE, E_MESSAGE, T_LENGTH, T_START, T_END, L_LENGTH, L_BY_ID };

static JSValue small_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL || dom->media_elements == NULL) return JS_EXCEPTION;
    if (magic == E_CODE || magic == E_MESSAGE) {
        DMediaError *e = JS_GetOpaque(self, dom->media_elements->error_class);
        if (e == NULL) return JS_ThrowTypeError(ctx, "Illegal invocation");
        return magic == E_CODE ? JS_NewInt32(ctx, e->code) : JS_NewString(ctx, e->message);
    }
    if (magic == L_LENGTH || magic == L_BY_ID) {
        if (JS_GetOpaque(self, dom->media_elements->tracks_class) == NULL)
            return JS_ThrowTypeError(ctx, "Illegal invocation");
        if (magic == L_LENGTH) return JS_NewUint32(ctx, 0);
        if (argc < 1) return JS_ThrowTypeError(ctx, "getTrackById needs an id");
        /* The id is converted, as WebIDL's DOMString is, and matches nothing. */
        JSValue id = JS_ToString(ctx, argv[0]);
        if (JS_IsException(id)) return id;
        JS_FreeValue(ctx, id);
        return JS_NULL;
    }
    DTimeRanges *r = JS_GetOpaque(self, dom->media_elements->ranges_class);
    if (r == NULL) return JS_ThrowTypeError(ctx, "Illegal invocation");
    if (magic == T_LENGTH) return JS_NewUint32(ctx, r->count);
    uint32_t index;
    if (JS_ToUint32(ctx, &index, argc > 0 ? argv[0] : JS_UNDEFINED) < 0) return JS_EXCEPTION;
    if (index >= r->count) return d_error(ctx, "IndexSizeError", "There is no range at that index");
    return JS_NewFloat64(ctx, magic == T_START ? r->range[index].start : r->range[index].end);
}

/* ── HTMLMediaElement's members ─────────────────────────────────────── */

enum {
    M_ERROR, M_SRC, M_CURRENT_SRC, M_CROSS_ORIGIN, M_NETWORK, M_PRELOAD, M_BUFFERED, M_LOAD, M_CAN_PLAY, M_READY,
    M_SEEKING, M_CURRENT_TIME, M_FAST_SEEK, M_DURATION, M_PAUSED, M_DEFAULT_RATE, M_RATE, M_PITCH, M_PLAYED,
    M_SEEKABLE, M_ENDED, M_AUTOPLAY, M_LOOP, M_PLAY, M_PAUSE, M_CONTROLS, M_VOLUME, M_MUTED, M_DEFAULT_MUTED,
    M_TEXT_TRACKS,
};

static const DReflection kSrc = {"src", "src", K_URL, 0, NULL, NULL, NULL, 0};
static const DReflection kBooleans[] = {
    {"autoplay", "autoplay", K_BOOLEAN, 0, NULL, NULL, NULL, 0},
    {"loop", "loop", K_BOOLEAN, 0, NULL, NULL, NULL, 0},
    {"controls", "controls", K_BOOLEAN, 0, NULL, NULL, NULL, 0},
    {"defaultMuted", "muted", K_BOOLEAN, 0, NULL, NULL, NULL, 0},
};

/* The src attribute set or changed, by any route (the property, setAttribute,
 * the Audio factory): HTML loads at once (§ 4.8.11.2), so a script that
 * sets it and calls play() next plays the new resource, and one reading
 * readyState sees HAVE_NOTHING. A media element no document holds gets
 * its record here too. Removing src loads nothing. */
static void src_changed(os64_dom_t *dom, const os64_html_change_t *record)
{
    if (record->kind != OS64_HTML_CHANGE_ATTR || record->attr_ns != NULL || record->attr_local == NULL ||
        !os64_streq(record->attr_local, "src") || !is_media(record->target) ||
        os64_html_attr(record->target, "src") == NULL || dom->media_elements == NULL)
        return;
    DMediaElement *m = record_of(dom, record->target, true);
    if (m != NULL && d_provider_enter(dom)) {
        media_load(dom, NULL, m);
        d_provider_leave(dom);
    }
}

static double fresh_position(os64_dom_t *dom, const DMediaElement *m)
{
    if (m->seeking) return m->seek_to;
    /* Before metadata a seek is the default playback start position,
     * which HTML's getter answers (so `currentTime += 1` adds to it). */
    if (m->ready == R_NOTHING) return m->default_start;
    if (m->paused || m->ended || m->session == NULL || dom->media_provider.facts == NULL) return m->position;
    os64_dom_media_facts_t f = {0};
    dom->media_provider.facts(dom->media_provider.opaque, m->session, &f);
    return f.position;
}

static JSValue promise_new(JSContext *ctx, DMediaPromise *out)
{
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return promise;
    *out = (DMediaPromise){JS_DupValue(ctx, promise), resolving[0], resolving[1]};
    return promise;
}

static JSValue rejected(JSContext *ctx, const char *name, const char *message)
{
    DMediaPromise p;
    JSValue promise = promise_new(ctx, &p);
    if (JS_IsException(promise)) return promise;
    JSValue e = d_error(ctx, name, message);
    JSValue reason = JS_GetException(ctx);
    JS_FreeValue(ctx, e);
    JSValue r = JS_Call(ctx, p.reject, JS_UNDEFINED, 1, &reason);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, reason);
    JS_FreeValue(ctx, p.promise);
    JS_FreeValue(ctx, p.resolve);
    JS_FreeValue(ctx, p.reject);
    return promise;
}

static JSValue play(os64_dom_t *dom, JSContext *ctx, DMediaElement *m)
{
    if (!allowed(dom, m))
        return rejected(ctx, "NotAllowedError",
                        "play() failed because the user didn't interact with the document first");
    if (m->error == OS64_DOM_MEDIA_ERR_SRC_NOT_SUPPORTED)
        return rejected(ctx, "NotSupportedError", "The element has no supported source");
    if (m->pending_count == m->pending_cap) {
        uint32_t cap = m->pending_cap != 0 ? m->pending_cap * 2 : 4;
        DMediaPromise *grown = d_alloc(dom, sizeof(*grown) * cap);
        if (grown == NULL) return d_error(ctx, "QuotaExceededError", "No room for another play()");
        if (m->pending_count != 0) os64_memcpy(grown, m->pending, sizeof(*grown) * m->pending_count);
        d_free(dom, m->pending);
        m->pending = grown;
        m->pending_cap = cap;
    }
    JSValue promise = promise_new(ctx, &m->pending[m->pending_count]);
    if (JS_IsException(promise)) return promise;
    m->pending_count++;
    /* HTML's internal play steps. */
    m->wanted = true;
    session_wake(dom, m);
    if (m->network == N_EMPTY) m->select_pending = true;
    if (m->deferred) {
        m->deferred = false;
        m->select_pending = true;
    }
    if (m->ended) seek(dom, m, 0);
    if (m->paused) {
        m->paused = false;
        m->show_poster = false;
        m->played_seq = ++dom->media_elements->play_seq;
        queue(dom, m, "play", SETTLE_NONE);
        if (m->ready <= R_CURRENT) queue(dom, m, "waiting", SETTLE_NONE);
        else notify_playing(dom, m);
    } else if (m->ready >= R_FUTURE) {
        queue(dom, m, NULL, SETTLE_RESOLVE);
    }
    m->autoplaying = false;
    if (m->session != NULL && dom->media_provider.play != NULL)
        dom->media_provider.play(dom->media_provider.opaque, m->session);
    return promise;
}

/* The members a script sets and the controls set the same way: a seek
 * (before metadata, the default start position), pause(), volume and
 * muted. Each queues its events and tells the host. */
static void time_set(os64_dom_t *dom, DMediaElement *m, double t)
{
    if (m->ready == R_NOTHING) {
        m->default_start = t;
        return;
    }
    if (d_provider_enter(dom)) {
        seek(dom, m, t);
        d_provider_leave(dom);
    }
}

static void pause_asked(os64_dom_t *dom, DMediaElement *m)
{
    if (m->network == N_EMPTY) m->select_pending = true;
    if (d_provider_enter(dom)) {
        internal_pause(dom, m);
        d_provider_leave(dom);
    }
}

static void volume_set(os64_dom_t *dom, DMediaElement *m, double v)
{
    if (v == m->volume) return;
    m->volume = v;
    queue(dom, m, "volumechange", SETTLE_NONE);
    if (d_provider_enter(dom)) {
        tell_settings(dom, m);
        d_provider_leave(dom);
    }
}

static void muted_set(os64_dom_t *dom, DMediaElement *m, bool on)
{
    if (on == m->muted) return;
    m->muted = on;
    queue(dom, m, "volumechange", SETTLE_NONE);
    if (d_provider_enter(dom)) {
        /* Muted is what let it play without the person: unmuted while it
         * plays and still without them, it pauses, as other browsers pause
         * it, rather than sound unasked (§ Autoplay). */
        if (!m->muted && !m->paused && !allowed(dom, m)) internal_pause(dom, m);
        tell_settings(dom, m);
        d_provider_leave(dom);
    }
}

/* The receiver's record: `this` must be a media element. */
static DMediaElement *receiver(os64_dom_t **dom, JSContext *ctx, JSValueConst self)
{
    const os64_html_node_t *node = d_node(*dom, ctx, self);
    if (node == NULL) return NULL;
    if (!is_media(node)) {
        JS_ThrowTypeError(ctx, "Illegal invocation");
        return NULL;
    }
    *dom = d_owner(*dom, node);
    DMediaElement *m = record_of(*dom, node, true);
    if (m == NULL) d_error(ctx, "QuotaExceededError", "No room for the media element's state");
    return m;
}

static JSValue media_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    DMediaElement *m = receiver(&dom, ctx, self);
    if (m == NULL) return JS_EXCEPTION;
    if (dom->media_elements == NULL) return JS_ThrowTypeError(ctx, "Illegal invocation");
    switch (magic) {
    case M_ERROR: return error_object(dom, ctx, m);
    case M_TEXT_TRACKS: return tracks_object(dom, ctx, m);
    case M_SRC: {
        if (!set) return d_reflection_get(dom, ctx, m->node, &kSrc);
        /* The attribute's change loads (src_changed). */
        return d_reflection_set(dom, ctx, m->node, &kSrc, value);
    }
    case M_CURRENT_SRC: return JS_NewString(ctx, m->current_src != NULL ? m->current_src : "");
    case M_CROSS_ORIGIN: {
        if (set) {
            if (JS_IsNull(value)) {
                int64_t status = d_set_attr(dom, m->node, "crossorigin", NULL, 0, true);
                return status < 0 ? d_html_error(ctx, status) : JS_UNDEFINED;
            }
            DString text = {0};
            if (!d_string(dom, ctx, value, &text)) return JS_EXCEPTION;
            int64_t status = d_set_attr(dom, m->node, "crossorigin", text.data != NULL ? text.data : "", text.length,
                                        false);
            d_string_free(dom, &text);
            return status < 0 ? d_html_error(ctx, status) : JS_UNDEFINED;
        }
        const os64_html_attr_t *cors = os64_html_attr(m->node, "crossorigin");
        if (cors == NULL) return JS_NULL;
        return JS_NewString(ctx, os64_streq_nocase(cors->value, "use-credentials") ? "use-credentials" : "anonymous");
    }
    case M_NETWORK: return JS_NewInt32(ctx, m->network);
    case M_PRELOAD: {
        if (set) {
            DString text = {0};
            if (!d_string(dom, ctx, value, &text)) return JS_EXCEPTION;
            int64_t status = d_set_attr(dom, m->node, "preload", text.data != NULL ? text.data : "", text.length, false);
            d_string_free(dom, &text);
            return status < 0 ? d_html_error(ctx, status) : JS_UNDEFINED;
        }
        /* An enumerated attribute (HTML § 4.8.11.3): "" is auto, and a
         * missing or unknown value is metadata, the state HTML suggests. */
        const os64_html_attr_t *preload = os64_html_attr(m->node, "preload");
        const char *answer = "metadata";
        if (preload != NULL) {
            if (preload->value[0] == '\0' || os64_streq_nocase(preload->value, "auto")) answer = "auto";
            else if (os64_streq_nocase(preload->value, "none")) answer = "none";
        }
        return JS_NewString(ctx, answer);
    }
    case M_BUFFERED: {
        if (!JS_IsUndefined(m->fed)) {
            os64_dom_fed_range_t ranges[D_RANGES_MAX];
            uint32_t n = d_mse_buffered(dom, m->fed, ranges, D_RANGES_MAX);
            return ranges_of(dom, ctx, ranges, n);
        }
        os64_dom_media_facts_t f = {0};
        if (m->session != NULL && dom->media_provider.facts != NULL)
            dom->media_provider.facts(dom->media_provider.opaque, m->session, &f);
        double end = f.arrived, start = f.arrived_from > 0 ? f.arrived_from : 0;
        if (isfinite(m->duration) && end > m->duration) end = m->duration;
        return ranges_object(dom, ctx, end > start ? 1 : 0, end > start ? start : 0, end > start ? end : 0);
    }
    case M_LOAD:
        m->wanted = true;
        if (d_provider_enter(dom)) {
            media_load(dom, ctx, m);
            d_provider_leave(dom);
        }
        return JS_UNDEFINED;
    case M_CAN_PLAY: {
        DString text = {0};
        if (!d_string(dom, ctx, value, &text)) return JS_EXCEPTION;
        int answer = 0;
        if (text.data != NULL && dom->media_provider.can_play != NULL && d_provider_enter(dom)) {
            answer = dom->media_provider.can_play(dom->media_provider.opaque, text.data);
            d_provider_leave(dom);
        }
        d_string_free(dom, &text);
        return JS_NewString(ctx, answer == 2 ? "probably" : answer == 1 ? "maybe" : "");
    }
    case M_READY: return JS_NewInt32(ctx, m->ready);
    case M_SEEKING: return JS_NewBool(ctx, m->seeking);
    case M_CURRENT_TIME:
    case M_FAST_SEEK: {
        if (!set && magic == M_CURRENT_TIME) {
            if (!d_provider_enter(dom)) return JS_NewFloat64(ctx, m->position);
            double t = fresh_position(dom, m);
            d_provider_leave(dom);
            return JS_NewFloat64(ctx, t);
        }
        double t;
        if (JS_ToFloat64(ctx, &t, value) < 0) return JS_EXCEPTION;
        if (!isfinite(t)) return JS_ThrowTypeError(ctx, "The provided double value is non-finite");
        time_set(dom, m, t);
        return JS_UNDEFINED;
    }
    case M_DURATION: return JS_NewFloat64(ctx, m->duration);
    case M_PAUSED: return JS_NewBool(ctx, m->paused);
    case M_DEFAULT_RATE:
    case M_RATE: {
        double *rate = magic == M_RATE ? &m->rate : &m->default_rate;
        if (!set) return JS_NewFloat64(ctx, *rate);
        double r;
        if (JS_ToFloat64(ctx, &r, value) < 0) return JS_EXCEPTION;
        if (!isfinite(r)) return JS_ThrowTypeError(ctx, "The provided double value is non-finite");
        if (r != *rate) {
            *rate = r;
            queue(dom, m, "ratechange", SETTLE_NONE);
        }
        return JS_UNDEFINED;
    }
    case M_PITCH: {
        if (!set) return JS_NewBool(ctx, m->preserves_pitch);
        int on = JS_ToBool(ctx, value);
        if (on < 0) return JS_EXCEPTION;
        m->preserves_pitch = on != 0;
        return JS_UNDEFINED;
    }
    case M_PLAYED: return ranges_object(dom, ctx, m->played_end > 0 ? 1 : 0, 0, m->played_end);
    case M_SEEKABLE: {
        if (!JS_IsUndefined(m->fed)) {
            os64_dom_fed_range_t range;
            uint32_t n = d_mse_seekable(dom, m->fed, &range);
            return ranges_of(dom, ctx, &range, n);
        }
        os64_dom_media_facts_t f = {0};
        if (m->session != NULL && dom->media_provider.facts != NULL)
            dom->media_provider.facts(dom->media_provider.opaque, m->session, &f);
        bool any = f.seekable && m->ready >= R_METADATA && isfinite(m->duration) && m->duration > 0;
        return ranges_object(dom, ctx, any ? 1 : 0, 0, any ? m->duration : 0);
    }
    case M_ENDED: return JS_NewBool(ctx, m->ended && !loops(m));
    case M_AUTOPLAY: case M_LOOP: case M_CONTROLS: case M_DEFAULT_MUTED: {
        const DReflection *row = &kBooleans[magic == M_AUTOPLAY ? 0 : magic == M_LOOP ? 1 : magic == M_CONTROLS ? 2 : 3];
        if (!set) return d_reflection_get(dom, ctx, m->node, row);
        JSValue r = d_reflection_set(dom, ctx, m->node, row, value);
        if (!JS_IsException(r) && magic == M_LOOP && d_provider_enter(dom)) {
            tell_settings(dom, m);
            d_provider_leave(dom);
        }
        return r;
    }
    case M_PLAY: {
        if (!d_provider_enter(dom)) return rejected(ctx, "AbortError", "The document is going away");
        JSValue promise = play(dom, ctx, m);
        d_provider_leave(dom);
        return promise;
    }
    case M_PAUSE:
        pause_asked(dom, m);
        return JS_UNDEFINED;
    case M_VOLUME: {
        if (!set) return JS_NewFloat64(ctx, m->volume);
        double v;
        if (JS_ToFloat64(ctx, &v, value) < 0) return JS_EXCEPTION;
        if (!isfinite(v)) return JS_ThrowTypeError(ctx, "The provided double value is non-finite");
        if (v < 0 || v > 1) return d_error(ctx, "IndexSizeError", "The volume is outside the range [0, 1]");
        volume_set(dom, m, v);
        return JS_UNDEFINED;
    }
    case M_MUTED: {
        if (!set) return JS_NewBool(ctx, m->muted);
        int on = JS_ToBool(ctx, value);
        if (on < 0) return JS_EXCEPTION;
        muted_set(dom, m, on != 0);
        return JS_UNDEFINED;
    }
    default: return JS_ThrowTypeError(ctx, "Unknown media member");
    }
}

/* ── The controls (AUDIO_YONDER.md § The controls) ──────────────────── */

bool os64_dom_media_view(os64_dom_t *dom, const os64_html_node_t *node, os64_dom_media_view_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    out->duration = NAN;
    if (dom == NULL || !d_delivery_active(dom) || !dom->options.modern) return false;
    dom = d_owner(dom, node);
    DMediaElement *m = record_of(dom, node, false);
    if (m == NULL) return false;
    os64_dom_media_facts_t f = {0};
    /* A fed element's buffered and seekable are its MediaSource's, never
     * the session's facts (dom/media.h § THE FED SOURCE). */
    bool fed = !JS_IsUndefined(m->fed);
    os64_dom_fed_range_t fed_buffered[D_RANGES_MAX], fed_seekable;
    uint32_t fed_buffered_count = 0, fed_seekable_count = 0;
    if (d_provider_enter(dom)) {
        out->position = fresh_position(dom, m);
        if (m->session != NULL && dom->media_provider.facts != NULL)
            dom->media_provider.facts(dom->media_provider.opaque, m->session, &f);
        if (fed) {
            fed_buffered_count = d_mse_buffered(dom, m->fed, fed_buffered, D_RANGES_MAX);
            fed_seekable_count = d_mse_seekable(dom, m->fed, &fed_seekable);
        }
        d_provider_leave(dom);
    } else {
        out->position = m->position;
    }
    if (fed) {
        /* The bar's arrived span: the first buffered range that ends
         * after the position (the one it is in, or the next past a gap),
         * else the last one. */
        f.arrived = f.arrived_from = 0;
        for (uint32_t i = 0; i < fed_buffered_count; i++) {
            f.arrived_from = fed_buffered[i].start;
            f.arrived = fed_buffered[i].end;
            if (out->position < fed_buffered[i].end) break;
        }
        f.seekable = fed_seekable_count != 0;
    }
    out->paused = m->paused;
    out->ended = m->ended && !loops(m);
    out->waiting = !m->paused && (m->ready <= R_CURRENT || f.busy);
    out->busy = f.busy;
    out->duration = m->duration;
    out->arrived = isfinite(m->duration) && f.arrived > m->duration ? m->duration : f.arrived;
    out->arrived_from = f.arrived_from > 0 && f.arrived_from < out->arrived ? f.arrived_from : 0;
    out->seekable = f.seekable && m->ready >= R_METADATA && isfinite(m->duration) && m->duration > 0;
    out->volume = m->volume;
    out->muted = m->muted;
    out->error = m->error;
    out->show_poster = m->show_poster;
    return true;
}

bool os64_dom_media_control(os64_dom_t *dom, const os64_html_node_t *node, os64_dom_media_control_t what,
                            double value, os64_js_outcome_t *outcome)
{
    os64_memset(outcome, 0, sizeof(*outcome));
    if (dom == NULL || !d_delivery_active(dom) || !dom->options.modern || !is_media(node)) return false;
    dom = d_owner(dom, node);
    if (dom->media_elements == NULL) return false;
    DMediaElement *m = record_of(dom, node, true);
    if (m == NULL) return false;
    switch (what) {
    case OS64_DOM_MEDIA_PAUSE: pause_asked(dom, m); return true;
    case OS64_DOM_MEDIA_SEEK:
        if (isfinite(value)) time_set(dom, m, value < 0 ? 0 : value);
        return true;
    case OS64_DOM_MEDIA_VOLUME:
        if (isfinite(value)) volume_set(dom, m, value < 0 ? 0 : value > 1 ? 1 : value);
        return true;
    case OS64_DOM_MEDIA_MUTE: muted_set(dom, m, value != 0); return true;
    case OS64_DOM_MEDIA_PLAY: break;
    }
    /* play() makes a promise, which needs the realm: a task of its own, as
     * a script's click handler calling it would be. Its rejection is the
     * control's to see (the element's state), never a page's unhandled one. */
    char name[OS64_JS_SOURCE_NAME_CAP];
    os64_snprintf(name, sizeof(name), "%.*s#media-controls", (int)(sizeof(name) - 40),
                  dom->url != NULL ? dom->url : "about:blank");
    if (os64_js_realm_task_begin(dom->realm, OS64_JS_ABI_ID, name, outcome) == OS64_JS_OK) {
        os64_js_outcome_t access;
        JSContext *ctx = os64_js_realm_context(dom->realm, OS64_JS_ABI_ID, &access);
        if (ctx == NULL) d_outcome_fold(outcome, &access);
        if (ctx != NULL && d_provider_enter(dom)) {
            JSValue promise = play(dom, ctx, m);
            d_provider_leave(dom);
            if (JS_IsException(promise)) {
                JSValue error = JS_GetException(ctx);
                JS_FreeValue(ctx, error);
            } else {
                mark_handled(dom, ctx, promise);
                JS_FreeValue(ctx, promise);
            }
        }
        d_task_finish(dom, outcome);
    }
    d_task_close(dom, name, outcome);
    return true;
}

/* ── For the media session (media_session.c) ────────────────────────── */
/* Every element the page has made counts, in its document or not (a player
 * that plays through `new Audio()` is still the page's sound, and HTML
 * pauses an element that leaves its document), WHEN IT CAN BE HEARD: not
 * muted, a volume above 0, no error. A muted clip looping on a news page,
 * or an element whose source failed, must not take the keys from a sound
 * a person is listening to, and other browsers give it none. */

static bool audible(const DMediaElement *m)
{
    return !m->muted && m->volume > 0 && m->error == 0;
}

void d_media_video_size(os64_dom_t *dom, const os64_html_node_t *node, uint32_t *width, uint32_t *height)
{
    DMediaElement *m = record_of(dom, node, false);
    *width = m != NULL && m->ready >= R_METADATA ? m->video_width : 0;
    *height = m != NULL && m->ready >= R_METADATA ? m->video_height : 0;
}

void d_media_quality(os64_dom_t *dom, const os64_html_node_t *node, uint64_t *shown, uint64_t *dropped)
{
    *shown = *dropped = 0;
    DMediaElement *m = record_of(dom, node, false);
    if (m == NULL || m->session == NULL || dom->media_provider.facts == NULL || !d_provider_enter(dom)) return;
    os64_dom_media_facts_t f = {0};
    dom->media_provider.facts(dom->media_provider.opaque, m->session, &f);
    d_provider_leave(dom);
    /* The total counts the dropped too (media-playback-quality § 2). */
    *shown = f.frames_shown + f.frames_dropped;
    *dropped = f.frames_dropped;
}

/* ── For Media Source Extensions (mse.c) ────────────────────────────── */

/* HTML's duration change: durationchange, and a position past the new end
 * sought back to it. */
void d_media_fed_duration(os64_dom_t *dom, void *media, double duration)
{
    DMediaElement *m = media;
    if (m == NULL || duration == m->duration || (isnan(duration) && isnan(m->duration))) return;
    m->duration = duration;
    queue(dom, m, "durationchange", SETTLE_NONE);
    if (m->ready >= R_METADATA && (m->seeking ? m->seek_to : m->position) > duration) seek(dom, m, duration);
}

/* The end of stream algorithm's element steps (MSE § 3.15.7). With
 * nothing yet (HAVE_NOTHING), the dedicated media source failure steps,
 * which a network error ("cannot be fetched at all") and a decode error
 * ("unsupported format") both are, and the source detached as Chromium
 * does: from src, MediaError 4 and `error` queued and then the detach
 * (its NoneSupported, then CloseMediaSource); from a <source> child, the
 * detach and the next child (source_failed; Chromium closes the source
 * as the next child loads). After that, a network error is the connection
 * interrupted and a decode error the media data corrupted, and the source
 * stays attached until the element lets its resource go. Either detach
 * may drop the last reference to the MediaSource and its SourceBuffers'
 * parents, and the next child is selected before this returns. */
void d_media_fed_failed(os64_dom_t *dom, void *media, bool decode)
{
    DMediaElement *m = media;
    if (m == NULL || m->error != 0) return;
    m->error_seen = true;
    if (m->ready == R_NOTHING) {
        const char *why = decode ? "The media data is not in a supported format" : "The media data could not be fetched";
        if (dom->media_provider.failed != NULL)
            dom->media_provider.failed(dom->media_provider.opaque, m->node, m->session, m->current_src,
                                       OS64_DOM_MEDIA_ERR_SRC_NOT_SUPPORTED, why);
        if (m->from_children) {
            source_failed(dom, m, why);
            return;
        }
        m->error = OS64_DOM_MEDIA_ERR_SRC_NOT_SUPPORTED;
        set_message(m, why);
        m->network = N_NO_SOURCE;
        queue(dom, m, "error", SETTLE_NOT_SUPPORTED);
        session_close(dom, m);
        return;
    }
    m->error = decode ? OS64_DOM_MEDIA_ERR_DECODE : OS64_DOM_MEDIA_ERR_NETWORK;
    set_message(m, decode ? "The media data is corrupted" : "The connection was interrupted");
    m->network = N_IDLE;
    queue(dom, m, "error", SETTLE_NONE);
    if (dom->media_provider.failed != NULL)
        dom->media_provider.failed(dom->media_provider.opaque, m->node, m->session, m->current_src, m->error,
                                   m->message);
}

void d_media_fed_metadata(os64_dom_t *dom, void *media)
{
    DMediaElement *m = media;
    if (m == NULL || m->ready != R_NOTHING || m->session == NULL) return;
    os64_dom_media_facts_t f = {0};
    if (dom->media_provider.facts != NULL) dom->media_provider.facts(dom->media_provider.opaque, m->session, &f);
    metadata_reached(dom, m, &f, true);
}

void d_media_fed_reinit(os64_dom_t *dom, void *media)
{
    DMediaElement *m = media;
    if (m != NULL && m->ready > R_CURRENT) ready_to(dom, m, R_METADATA);
}

void d_media_fed_removed(os64_dom_t *dom, void *media, double start, double end)
{
    DMediaElement *m = media;
    if (m == NULL || m->ready <= R_METADATA) return;
    double at = m->seeking ? m->seek_to : m->position;
    if (at >= start && at < end) ready_to(dom, m, R_METADATA);
}

bool d_media_fed_has_error(const void *media)
{
    const DMediaElement *m = media;
    return m != NULL && m->error != 0;
}

void d_media_session_facts(os64_dom_t *dom, bool *playing, bool *paused_after_play, bool *played)
{
    *playing = *paused_after_play = *played = false;
    if (dom->media_elements == NULL) return;
    for (DMediaElement *m = dom->media_elements->records; m != NULL; m = m->next) {
        if (!audible(m)) continue;
        *playing = *playing || !m->paused;
        *played = *played || m->played_seq != 0;
        *paused_after_play = *paused_after_play || (m->paused && m->played_seq != 0);
    }
}

void d_media_session_default(os64_dom_t *dom, JSContext *ctx, bool play_one)
{
    if (dom->media_elements == NULL) return;
    if (!play_one) {
        for (DMediaElement *m = dom->media_elements->records; m != NULL; m = m->next)
            if (!m->paused && audible(m)) pause_asked(dom, m);
        return;
    }
    DMediaElement *last = NULL;
    for (DMediaElement *m = dom->media_elements->records; m != NULL; m = m->next)
        if (audible(m) && m->played_seq != 0 && (last == NULL || m->played_seq > last->played_seq)) last = m;
    if (last == NULL || !last->paused || !d_provider_enter(dom)) return;
    /* As a control's play: its rejection is the element's state, not the
     * page's to handle. */
    JSValue promise = play(dom, ctx, last);
    d_provider_leave(dom);
    if (JS_IsException(promise)) {
        JSValue error = JS_GetException(ctx);
        JS_FreeValue(ctx, error);
    } else {
        mark_handled(dom, ctx, promise);
        JS_FreeValue(ctx, promise);
    }
}

/* ── The Audio factory (HTML § 4.8.10, a legacy factory function) ───── */

/* `new Audio(src)`: an audio element of the window's document, preload
 * auto, with src set when one is given (which loads it). Its prototype is
 * new.target's, which for Audio itself is HTMLAudioElement's. */
static JSValue audio_factory(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return JS_EXCEPTION;
    DString src = {0};
    bool has_src = argc > 0 && !JS_IsUndefined(argv[0]);
    JSValue result = JS_EXCEPTION;
    if (!has_src || d_string(dom, ctx, argv[0], &src)) {
        int64_t status = 0;
        os64_html_node_t *audio = os64_html_create_element(dom->document, OS64_HTML_NS_HTML, "audio", &status);
        if (audio != NULL) status = d_set_attr(dom, audio, "preload", "auto", 4, false);
        if (audio != NULL && status >= 0 && has_src)
            status = d_set_attr(dom, audio, "src", src.data != NULL ? src.data : "", src.length, false);
        if (audio != NULL && status >= 0) {
            /* Its src attribute's change gave it a record and loaded it
             * (src_changed), though no document holds it. */
            result = d_wrap_constructed(dom, ctx, audio, prototype);
        } else {
            result = d_html_error(ctx, status);
        }
    }
    d_string_free(dom, &src);
    JS_FreeValue(ctx, prototype);
    return result;
}

/* ── The install ────────────────────────────────────────────────────── */

static int constants(JSContext *ctx, JSValueConst target, const char *const *names, int count, int first)
{
    for (int i = 0; i < count; i++)
        if (JS_DefinePropertyValueStr(ctx, target, names[i], JS_NewInt32(ctx, first + i), JS_PROP_ENUMERABLE) < 0)
            return -1;
    return 0;
}

int d_media_elements_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    struct DMediaElements *s = d_alloc(dom, sizeof(*s));
    if (s == NULL) return -1;
    dom->media_elements = s;
    JSValueConst media = dom->prototypes[D_PROTO_MEDIA]->value;
    JSValueConst audio = dom->prototypes[D_PROTO_AUDIO]->value;
    /* Promise.prototype.then as the engine has it before any page ran. */
    JSValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
    JSValue promise_proto = JS_IsException(promise_ctor) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, promise_ctor, "prototype");
    JS_FreeValue(ctx, promise_ctor);
    if (JS_IsException(promise_proto)) return -1;
    s->then = d_intrinsic(dom, ctx, promise_proto, "then");
    JS_FreeValue(ctx, promise_proto);
    if (s->then == NULL) return -1;

    static const struct { const char *name; int magic; bool writable; } kAttributes[] = {
        {"error", M_ERROR, false}, {"src", M_SRC, true}, {"currentSrc", M_CURRENT_SRC, false},
        {"crossOrigin", M_CROSS_ORIGIN, true}, {"networkState", M_NETWORK, false}, {"preload", M_PRELOAD, true},
        {"buffered", M_BUFFERED, false}, {"readyState", M_READY, false}, {"seeking", M_SEEKING, false},
        {"currentTime", M_CURRENT_TIME, true}, {"duration", M_DURATION, false}, {"paused", M_PAUSED, false},
        {"defaultPlaybackRate", M_DEFAULT_RATE, true}, {"playbackRate", M_RATE, true},
        {"preservesPitch", M_PITCH, true}, {"played", M_PLAYED, false}, {"seekable", M_SEEKABLE, false},
        {"ended", M_ENDED, false}, {"autoplay", M_AUTOPLAY, true}, {"loop", M_LOOP, true},
        {"controls", M_CONTROLS, true}, {"volume", M_VOLUME, true}, {"muted", M_MUTED, true},
        {"defaultMuted", M_DEFAULT_MUTED, true}, {"textTracks", M_TEXT_TRACKS, false},
    };
    for (size_t i = 0; i < sizeof(kAttributes) / sizeof(kAttributes[0]); i++)
        if (d_accessor(dom, ctx, media, kAttributes[i].name, media_member, kAttributes[i].magic,
                       kAttributes[i].writable) < 0)
            return -1;
    static const struct { const char *name; int magic, argc; } kMethods[] = {
        {"load", M_LOAD, 0}, {"canPlayType", M_CAN_PLAY, 1}, {"fastSeek", M_FAST_SEEK, 1},
        {"play", M_PLAY, 0}, {"pause", M_PAUSE, 0},
    };
    for (size_t i = 0; i < sizeof(kMethods) / sizeof(kMethods[0]); i++)
        if (d_method_named(dom, ctx, media, kMethods[i].name, media_member, kMethods[i].argc, kMethods[i].magic) < 0)
            return -1;
    static const char *const kNetwork[] = {"NETWORK_EMPTY", "NETWORK_IDLE", "NETWORK_LOADING", "NETWORK_NO_SOURCE"};
    static const char *const kReady[] = {"HAVE_NOTHING", "HAVE_METADATA", "HAVE_CURRENT_DATA", "HAVE_FUTURE_DATA",
                                         "HAVE_ENOUGH_DATA"};
    if (constants(ctx, media, kNetwork, 4, 0) < 0 || constants(ctx, media, kReady, 5, 0) < 0) return -1;

    /* The interfaces, over the interfaces row's HTMLElement. */
    JSValue parent = JS_GetPropertyStr(ctx, global, "HTMLElement");
    if (JS_IsException(parent)) return -1;
    JSValue media_interface = d_interface_new(ctx, "HTMLMediaElement", 0, NULL, media, parent);
    JS_FreeValue(ctx, parent);
    if (JS_IsException(media_interface)) return -1;
    if (constants(ctx, media_interface, kNetwork, 4, 0) < 0 || constants(ctx, media_interface, kReady, 5, 0) < 0) {
        JS_FreeValue(ctx, media_interface);
        return -1;
    }
    JSValue audio_interface = d_interface_new(ctx, "HTMLAudioElement", 0, NULL, audio, media_interface);
    /* Each define takes its value whatever it answers, so the second
     * interface is let go here if the first define fails. */
    if (JS_DefinePropertyValueStr(ctx, global, "HTMLMediaElement", media_interface,
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(ctx, audio_interface);
        return -1;
    }
    if (JS_IsException(audio_interface) ||
        JS_DefinePropertyValueStr(ctx, global, "HTMLAudioElement", audio_interface,
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;

    /* MediaError and TimeRanges, objects only the binding makes. */
    static const JSClassDef error_class = {.class_name = "MediaError", .finalizer = opaque_free};
    static const JSClassDef ranges_class = {.class_name = "TimeRanges", .finalizer = opaque_free};
    s->error_class = os64_js_class_id(&media_error_slot);
    s->ranges_class = os64_js_class_id(&time_ranges_slot);
    JSValue error_proto, ranges_proto;
    if (d_interface_class(dom, ctx, global, s->error_class, &error_class, &error_proto) < 0 ||
        d_interface_class(dom, ctx, global, s->ranges_class, &ranges_class, &ranges_proto) < 0)
        return -1;
    static const char *const kErrors[] = {"MEDIA_ERR_ABORTED", "MEDIA_ERR_NETWORK", "MEDIA_ERR_DECODE",
                                          "MEDIA_ERR_SRC_NOT_SUPPORTED"};
    JSValue error_interface = JS_GetPropertyStr(ctx, global, "MediaError");
    if (JS_IsException(error_interface)) return -1;
    int result = constants(ctx, error_interface, kErrors, 4, 1);
    JS_FreeValue(ctx, error_interface);
    if (result < 0 || constants(ctx, error_proto, kErrors, 4, 1) < 0 ||
        d_accessor(dom, ctx, error_proto, "code", small_member, E_CODE, false) < 0 ||
        d_accessor(dom, ctx, error_proto, "message", small_member, E_MESSAGE, false) < 0 ||
        d_accessor(dom, ctx, ranges_proto, "length", small_member, T_LENGTH, false) < 0 ||
        d_method_named(dom, ctx, ranges_proto, "start", small_member, 1, T_START) < 0 ||
        d_method_named(dom, ctx, ranges_proto, "end", small_member, 1, T_END) < 0)
        return -1;

    /* TextTrackList (tracks_object): an EventTarget with its three
     * handlers, a length, getTrackById, and an array's iterator. */
    static const JSClassDef tracks_class = {
        .class_name = "TextTrackList", .finalizer = tracks_finalizer, .gc_mark = tracks_mark
    };
    s->tracks_class = os64_js_class_id(&text_track_list_slot);
    JSValue tracks_proto;
    if (d_interface_class(dom, ctx, global, s->tracks_class, &tracks_class, &tracks_proto) < 0 ||
        d_event_target(dom, ctx, tracks_proto, s->tracks_class, NULL) < 0 ||
        d_event_handler_named(dom, ctx, tracks_proto, "change") < 0 ||
        d_event_handler_named(dom, ctx, tracks_proto, "addtrack") < 0 ||
        d_event_handler_named(dom, ctx, tracks_proto, "removetrack") < 0 ||
        d_accessor(dom, ctx, tracks_proto, "length", small_member, L_LENGTH, false) < 0 ||
        d_method_named(dom, ctx, tracks_proto, "getTrackById", small_member, 1, L_BY_ID) < 0 ||
        d_iterable(ctx, tracks_proto, false) < 0)
        return -1;

    /* Audio: its `prototype` is HTMLAudioElement's, whose `constructor`
     * stays HTMLAudioElement (WebIDL § 3.7.2, a legacy factory function). */
    JSValue factory = JS_NewCFunction2(ctx, audio_factory, "Audio", 0, JS_CFUNC_constructor, 0);
    if (JS_IsException(factory)) return -1;
    if (JS_DefinePropertyValueStr(ctx, factory, "prototype", JS_DupValue(ctx, audio), 0) < 0) {
        JS_FreeValue(ctx, factory);
        return -1;
    }
    if (JS_DefinePropertyValueStr(ctx, global, "Audio", factory, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;

    /* The media events' handlers, where GlobalEventHandlers puts them; and
     * onerror on the media element's own prototype, the one element the
     * binding fires error at (window and Document have theirs from the
     * report row, and no other element has one). */
    JSValueConst holders[] = {global, dom->prototypes[D_PROTO_ELEMENT]->value, dom->prototypes[D_PROTO_DOCUMENT]->value};
    for (int kind = D_EVENT_LOADSTART; kind <= D_EVENT_VOLUMECHANGE; kind++)
        for (size_t i = 0; i < sizeof(holders) / sizeof(holders[0]); i++)
            if (d_event_handler(dom, ctx, holders[i], kind) < 0) return -1;
    if (d_event_handler(dom, ctx, media, D_EVENT_ERROR) < 0) return -1;
    d_mutation_register(dom, &s->mutations, src_changed);
    if (dom->options.media_source && d_mse_install(dom, ctx, global) < 0) return -1;
    return d_video_install(dom, ctx, global);
}
