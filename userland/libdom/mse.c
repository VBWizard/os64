#include "internal.h"
#include "os64/fmt.h"
#include <math.h>

/* MEDIA SOURCE EXTENSIONS (MSE_DOM.md; MEDIA_SOURCE.md § 7; W3C
 * media-source): the MediaSource, SourceBuffer and SourceBufferList a page
 * feeds a media element through, and the object URLs that name a
 * MediaSource.
 *
 * libdom keeps the objects, their states and their events; the host keeps
 * the fed session it opens when a MediaSource attaches and a buffer per
 * SourceBuffer (dom/media.h § THE FED SOURCE), and is asked from here. The
 * media element's side (its states, buffered, seekable, the attach in
 * resource selection and the detach whenever it lets its resource go) is
 * media.c's, which calls in through internal.h.
 *
 * EVERY EVENT IS A TASK on the media elements' one queue (d_media_queue_plain),
 * so MSE's events and the element's run in the order the standard's steps
 * queue them (`sourceended` before the element's `durationchange`, `error`
 * before `updateend`). An append's work and a removal's are tasks there
 * too (d_media_queue_work), between `updatestart` and `update`; the parse
 * runs inside the append's task (MEDIA_SOURCE.md § 9, "where an append is
 * parsed"), and a host whose parse runs elsewhere answers PENDING and is
 * polled from the media turn (d_mse_turn) until it is done. abort() and
 * removeSourceBuffer() cancel the queued task; a queued task holds its
 * target, so a dropped SourceBuffer still hears its `updateend`.
 *
 * A detach with an append in flight queues `abort` and `updateend` at the
 * SourceBuffer, and one with a removal in flight queues nothing, as
 * Chromium's SourceBuffer::RemovedFromMediaSource does (Firefox agrees on
 * the append); a revoked object URL leaves an attachment as it is.
 *
 * All of it exists only with the binding's media_source switch (dom.h),
 * which a host sets when it installs a fed source; without it the names
 * are absent, and createObjectURL takes Blobs alone.
 *
 * What it leaves out (DEBTS.md): a MediaSource in a worker and its
 * MediaSourceHandle, ManagedMediaSource, the AudioTrack and VideoTrack
 * lists (the element has neither), and changeType, which refuses every
 * type. isTypeSupported answers H.264 and AAC-LC in MP4 alone, and
 * nothing while no fed host is installed. */

enum { MS_CLOSED, MS_OPEN, MS_ENDED };
enum { END_NONE, END_NETWORK, END_DECODE };
/* A MediaSource holds one SourceBuffer of sound and one of picture (or
 * one carrying both): MEDIA_SOURCE.md § 0's v1. */
#define MSE_BUFFERS_MAX 2
/* Object URLs a page may name a MediaSource by at once. */
#define MSE_URLS_MAX 1024

typedef struct {
    DValue target;                  /* first: the plain target's entry, in the object */
    JSValue self;                   /* the object (not held: the struct is its opaque) */
    uint8_t ready;
    double duration;
    bool live;                      /* a live seekable range is set */
    double live_start, live_end;
    void *media;                    /* the element's record while attached */
    void *session;                  /* the host's fed session while attached */
    JSValue buffers[MSE_BUFFERS_MAX]; /* sourceBuffers, in order (held) */
    bool active[MSE_BUFFERS_MAX];   /* which of them are in activeSourceBuffers */
    uint32_t count;
    JSValue list, active_list;      /* sourceBuffers and activeSourceBuffers (held) */
} DMediaSource;

typedef struct {
    DValue target;
    JSValue self;
    JSValue parent;                 /* the MediaSource while in its sourceBuffers (held); JS_UNDEFINED removed */
    void *handle;                   /* the host's buffer; NULL once removed */
    os64_dom_fed_mode_t mode;
    bool updating;
    bool removing;                  /* the queued task is a removal */
    bool polling;                   /* the append's parse answered PENDING */
    bool first_init;                /* its first initialization segment has been received */
    bool parsing_media;             /* the append state is PARSING_MEDIA_SEGMENT */
    double offset, window_start, window_end;
    double highest_pts;             /* NaN with no frame */
    double remove_start, remove_end;
    DMediaTask *pending;            /* the queued APPEND or REMOVE, NULL with none */
    JSValue buffered;               /* the TimeRanges last answered (held), JS_UNDEFINED none */
    os64_dom_fed_range_t cached[D_RANGES_MAX];
    uint32_t cached_count;
} DSourceBuffer;

typedef struct {
    DValue target;
    JSValue source;                 /* the MediaSource it lists (held) */
    bool active;                    /* activeSourceBuffers, else sourceBuffers */
} DSourceBufferList;

typedef struct { char *url; JSValue source; } DMseUrl;
typedef struct { DSourceBuffer *buffer; JSValue object; } DMsePoll;

struct DMseState {
    JSClassID source_class, buffer_class, list_class;
    DMseUrl *urls;
    uint32_t url_count, url_cap;
    DMsePoll *polls;                /* SourceBuffers whose parse is PENDING (each held) */
    uint32_t poll_count, poll_cap;
};

static JSClassID media_source_slot, source_buffer_slot, source_buffer_list_slot;

static DMediaSource *source_of(os64_dom_t *dom, JSValueConst value)
{
    return dom->mse != NULL ? JS_GetOpaque(value, dom->mse->source_class) : NULL;
}

static DSourceBuffer *buffer_of(os64_dom_t *dom, JSValueConst value)
{
    return dom->mse != NULL ? JS_GetOpaque(value, dom->mse->buffer_class) : NULL;
}

/* ── isTypeSupported (MSE § 3.12; MSE_DOM.md § 7) ─────────────────── */

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

static bool token_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c != '\0' && os64_strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static bool hex_digit(char c, int *value)
{
    c = lower(c);
    if (c >= '0' && c <= '9') { *value = c - '0'; return true; }
    if (c >= 'a' && c <= 'f') { *value = c - 'a' + 10; return true; }
    return false;
}

static bool same_nocase(const char *a, size_t a_len, const char *b)
{
    size_t n = os64_strlen(b);
    if (a_len != n) return false;
    for (size_t i = 0; i < n; i++)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

/* One codec of a `codecs` list: 1 picture, 2 sound, 0 one v1 refuses.
 * H.264 as avc1 or avc3 with its profile (Baseline 42, Main 4D, High 64),
 * its constraints and a level up to 5.1; AAC-LC (mp4a.40.2). */
static int codec_kind(const char *c, size_t n)
{
    if (n == 11 && (same_nocase(c, 5, "avc1.") || same_nocase(c, 5, "avc3."))) {
        int d[6];
        for (int i = 0; i < 6; i++)
            if (!hex_digit(c[5 + i], &d[i])) return 0;
        int profile = d[0] * 16 + d[1], level = d[4] * 16 + d[5];
        if (profile != 0x42 && profile != 0x4D && profile != 0x64) return 0;
        return level > 0 && level <= 0x33 ? 1 : 0;
    }
    if (same_nocase(c, n, "mp4a.40.2")) return 2;
    return 0;
}

/* `type` as a MIME type with parameters: `type/subtype` then `;`-separated
 * `name=value` (quoted or bare); of the parameters only `codecs` is read,
 * as both other browsers read it. */
static bool type_supported(const char *type)
{
    const char *p = type;
    while (*p == ' ' || *p == '\t') p++;
    const char *major = p;
    while (token_char(*p)) p++;
    size_t major_len = (size_t)(p - major);
    if (major_len == 0 || *p++ != '/') return false;
    const char *minor = p;
    while (token_char(*p)) p++;
    size_t minor_len = (size_t)(p - minor);
    if (minor_len == 0 || !same_nocase(minor, minor_len, "mp4")) return false;
    bool video = same_nocase(major, major_len, "video");
    if (!video && !same_nocase(major, major_len, "audio")) return false;
    const char *codecs = NULL;
    size_t codecs_len = 0;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        if (*p++ != ';') return false;
        while (*p == ' ' || *p == '\t') p++;
        const char *name = p;
        while (token_char(*p)) p++;
        size_t name_len = (size_t)(p - name);
        if (name_len == 0 || *p++ != '=') return false;
        const char *value = p;
        size_t value_len;
        if (*p == '"') {
            value = ++p;
            while (*p != '\0' && *p != '"') p++;
            if (*p != '"') return false;
            value_len = (size_t)(p - value);
            p++;
        } else {
            while (token_char(*p) || *p == ',') p++;
            value_len = (size_t)(p - value);
        }
        if (same_nocase(name, name_len, "codecs")) {
            codecs = value;
            codecs_len = value_len;
        }
    }
    if (codecs == NULL) return false;
    int pictures = 0, sounds = 0;
    for (size_t at = 0; at <= codecs_len;) {
        size_t end = at;
        while (end < codecs_len && codecs[end] != ',') end++;
        size_t from = at, to = end;
        while (from < to && (codecs[from] == ' ' || codecs[from] == '\t')) from++;
        while (to > from && (codecs[to - 1] == ' ' || codecs[to - 1] == '\t')) to--;
        int kind = codec_kind(codecs + from, to - from);
        if (kind == 0) return false;
        if (kind == 1) pictures++;
        else sounds++;
        at = end + 1;
    }
    if (pictures > 1 || sounds > 1 || pictures + sounds == 0) return false;
    return video || pictures == 0;
}

/* ── The host ───────────────────────────────────────────────────────── */

static const os64_dom_fed_provider_t *host(os64_dom_t *dom) { return &dom->fed_provider; }

static DMediaSource *parent_of(os64_dom_t *dom, DSourceBuffer *b)
{
    return JS_IsUndefined(b->parent) ? NULL : source_of(dom, b->parent);
}

static void push_settings(os64_dom_t *dom, DSourceBuffer *b)
{
    DMediaSource *s = parent_of(dom, b);
    if (s != NULL && b->handle != NULL && host(dom)->set != NULL)
        host(dom)->set(host(dom)->opaque, s->session, b->handle, b->mode, b->offset, b->window_start, b->window_end);
}

/* The buffer's ranges (its tracks' intersected, as the host answers
 * them), the last one reaching the highest end when its source has ended
 * (the buffered getter's step 4); their count, and in *highest that end. */
static uint32_t buffer_ranges(os64_dom_t *dom, DMediaSource *s, DSourceBuffer *b, os64_dom_fed_range_t *out,
                              uint32_t cap, double *highest)
{
    *highest = 0;
    if (s == NULL || b->handle == NULL || host(dom)->ranges == NULL) return 0;
    double top = 0;
    uint32_t n = host(dom)->ranges(host(dom)->opaque, s->session, b->handle, out, cap, &top);
    if (n > cap) n = cap;
    if (isfinite(top) && top > 0) *highest = top;
    if (s->ready == MS_ENDED && n > 0 && *highest > out[n - 1].end) out[n - 1].end = *highest;
    return n;
}

/* The intersection of two normalized lists, into `out` (at most `cap`). */
static uint32_t intersect(const os64_dom_fed_range_t *a, uint32_t na, const os64_dom_fed_range_t *b, uint32_t nb,
                          os64_dom_fed_range_t *out, uint32_t cap)
{
    uint32_t n = 0, i = 0, j = 0;
    while (i < na && j < nb && n < cap) {
        double start = a[i].start > b[j].start ? a[i].start : b[j].start;
        double end = a[i].end < b[j].end ? a[i].end : b[j].end;
        if (start < end) out[n++] = (os64_dom_fed_range_t){start, end};
        if (a[i].end < b[j].end) i++;
        else j++;
    }
    return n;
}

static double highest_end_of(os64_dom_t *dom, DMediaSource *s)
{
    double top = 0;
    for (uint32_t i = 0; i < s->count; i++) {
        os64_dom_fed_range_t r[D_RANGES_MAX];
        double highest;
        (void)buffer_ranges(dom, s, buffer_of(dom, s->buffers[i]), r, D_RANGES_MAX, &highest);
        if (highest > top) top = highest;
    }
    return top;
}

/* ── Events and states ──────────────────────────────────────────────── */

static void source_event(os64_dom_t *dom, DMediaSource *s, const char *type)
{
    (void)d_media_queue_plain(dom, s->self, &s->target, type);
}

static void buffer_event(os64_dom_t *dom, DSourceBuffer *b, const char *type)
{
    (void)d_media_queue_plain(dom, b->self, &b->target, type);
}

static void list_event(os64_dom_t *dom, JSValueConst list, const char *type)
{
    DSourceBufferList *l = JS_GetOpaque(list, dom->mse->list_class);
    if (l != NULL) (void)d_media_queue_plain(dom, list, &l->target, type);
}

/* An operation on an ended source reopens it (§ 5's setters, prepare
 * append, remove()): "open", and `sourceopen` queued. */
static void reopen(os64_dom_t *dom, DMediaSource *s)
{
    if (s == NULL || s->ready != MS_ENDED) return;
    s->ready = MS_OPEN;
    source_event(dom, s, "sourceopen");
}

static void poll_forget(os64_dom_t *dom, DSourceBuffer *b)
{
    struct DMseState *m = dom->mse;
    if (!b->polling || m == NULL) return;
    b->polling = false;
    for (uint32_t i = 0; i < m->poll_count; i++)
        if (m->polls[i].buffer == b) {
            JS_FreeValueRT(dom->engine, m->polls[i].object);
            m->polls[i] = m->polls[--m->poll_count];
            return;
        }
}

/* The queued APPEND or REMOVE given up (abort(), removeSourceBuffer(), a
 * detach), and a PENDING parse no longer polled. */
static void work_cancel(os64_dom_t *dom, DSourceBuffer *b)
{
    d_media_task_cancel(b->pending);
    b->pending = NULL;
    poll_forget(dom, b);
}

/* MSE § 3.15.6. False (an InvalidStateError thrown when `ctx`) when the
 * value is below a frame a SourceBuffer holds. */
static bool duration_change(os64_dom_t *dom, JSContext *ctx, DMediaSource *s, double value)
{
    if (value == s->duration) return true;
    for (uint32_t i = 0; i < s->count; i++) {
        DSourceBuffer *b = buffer_of(dom, s->buffers[i]);
        if (b != NULL && !isnan(b->highest_pts) && value < b->highest_pts) {
            if (ctx != NULL) d_error(ctx, "InvalidStateError", "A SourceBuffer holds frames past that duration");
            return false;
        }
    }
    double top = highest_end_of(dom, s);
    if (value < top) value = top;
    s->duration = value;
    d_media_fed_duration(dom, s->media, value);
    if (s->session != NULL && host(dom)->duration != NULL)
        host(dom)->duration(host(dom)->opaque, s->session, value);
    return true;
}

/* MSE § 3.15.7. */
static void end_of_stream(os64_dom_t *dom, DMediaSource *s, int error)
{
    s->ready = MS_ENDED;
    source_event(dom, s, "sourceended");
    if (error == END_NONE) {
        (void)duration_change(dom, NULL, s, highest_end_of(dom, s));
        if (s->session != NULL && host(dom)->end != NULL) host(dom)->end(host(dom)->opaque, s->session);
        return;
    }
    d_media_fed_failed(dom, s->media, error == END_DECODE);
}

static void take_result(DSourceBuffer *b, const os64_dom_fed_result_t *r)
{
    if (isfinite(r->timestamp_offset)) b->offset = r->timestamp_offset;
    b->parsing_media = r->parsing_media;
    b->highest_pts = r->highest_pts;
}

/* MSE § 5.5.3. */
static void append_error(os64_dom_t *dom, DMediaSource *s, DSourceBuffer *b)
{
    if (b->handle != NULL && host(dom)->reset != NULL) {
        os64_dom_fed_result_t r = {.timestamp_offset = NAN, .highest_pts = NAN, .group_end = NAN};
        host(dom)->reset(host(dom)->opaque, s->session, b->handle, &r);
        take_result(b, &r);
    }
    b->updating = false;
    buffer_event(dom, b, "error");
    buffer_event(dom, b, "updateend");
    end_of_stream(dom, s, END_DECODE);
}

static uint32_t index_of(DMediaSource *s, DSourceBuffer *b)
{
    for (uint32_t i = 0; i < s->count; i++)
        if (JS_VALUE_GET_PTR(s->buffers[i]) == JS_VALUE_GET_PTR(b->self)) return i;
    return s->count;
}

/* What a parse did (the append's task, or a poll once it is no longer
 * PENDING): the initialization segment received (§ 5.5.7), the duration
 * past the group end (coded frame processing's step 5), and the end of
 * the buffer append (§ 5.5.5) or the append error. */
static void parsed(os64_dom_t *dom, DSourceBuffer *b, const os64_dom_fed_result_t *r)
{
    DMediaSource *s = parent_of(dom, b);
    if (s == NULL) return;
    if (r->status == OS64_DOM_FED_PENDING) {
        struct DMseState *m = dom->mse;
        if (b->polling) return;
        if (m->poll_count == m->poll_cap) {
            uint32_t cap = m->poll_cap != 0 ? m->poll_cap * 2 : 4;
            DMsePoll *grown = d_alloc(dom, sizeof(*grown) * cap);
            if (grown == NULL) {
                append_error(dom, s, b);
                return;
            }
            if (m->poll_count != 0) os64_memcpy(grown, m->polls, sizeof(*grown) * m->poll_count);
            d_free(dom, m->polls);
            m->polls = grown;
            m->poll_cap = cap;
        }
        m->polls[m->poll_count++] = (DMsePoll){b, JS_DupValueRT(dom->engine, b->self)};
        b->polling = true;
        return;
    }
    if (r->status == OS64_DOM_FED_ERROR) {
        append_error(dom, s, b);
        return;
    }
    take_result(b, r);
    if (r->inits > 0) {
        if (isnan(s->duration)) (void)duration_change(dom, NULL, s, isnan(r->init_duration) ? INFINITY : r->init_duration);
        if (!b->first_init) {
            b->first_init = true;
            uint32_t i = index_of(s, b);
            if ((r->audio || r->video) && i < s->count && !s->active[i]) {
                s->active[i] = true;
                list_event(dom, s->active_list, "addsourcebuffer");
                d_media_fed_reinit(dom, s->media);
            }
            /* § 5.5.7's last step: with every SourceBuffer initialized the
             * element has its metadata, inside this append. */
            if (d_mse_probed(dom, s->self)) d_media_fed_metadata(dom, s->media);
        }
    }
    /* Coded frame processing's step 5: frames past the duration raise it
     * to the group's end. */
    if (r->frames && !isnan(r->group_end) && (isnan(s->duration) || r->group_end > s->duration))
        (void)duration_change(dom, NULL, s, r->group_end);
    b->updating = false;
    buffer_event(dom, b, "update");
    buffer_event(dom, b, "updateend");
}

static void work_left(void *work, DMediaTask *task)
{
    DSourceBuffer *b = work;
    if (b->pending == task) b->pending = NULL;
}

static void append_run(os64_dom_t *dom, void *work)
{
    DSourceBuffer *b = work;
    b->pending = NULL;
    DMediaSource *s = parent_of(dom, b);
    if (s == NULL || b->handle == NULL || host(dom)->parse == NULL) return;
    os64_dom_fed_result_t r = {.timestamp_offset = NAN, .highest_pts = NAN, .group_end = NAN, .init_duration = NAN};
    host(dom)->parse(host(dom)->opaque, s->session, b->handle, &r);
    parsed(dom, b, &r);
}

static void remove_run(os64_dom_t *dom, void *work)
{
    DSourceBuffer *b = work;
    b->pending = NULL;
    DMediaSource *s = parent_of(dom, b);
    if (s == NULL || b->handle == NULL) return;
    if (host(dom)->remove != NULL) {
        os64_dom_fed_result_t r = {.timestamp_offset = NAN, .highest_pts = NAN, .group_end = NAN};
        host(dom)->remove(host(dom)->opaque, s->session, b->handle, b->remove_start, b->remove_end, &r);
        take_result(b, &r);
    }
    uint32_t i = index_of(s, b);
    if (i < s->count && s->active[i]) d_media_fed_removed(dom, s->media, b->remove_start, b->remove_end);
    b->updating = false;
    b->removing = false;
    buffer_event(dom, b, "update");
    buffer_event(dom, b, "updateend");
}

bool d_mse_turn(os64_dom_t *dom)
{
    struct DMseState *m = dom->mse;
    if (m == NULL || m->poll_count == 0 || host(dom)->poll == NULL) return false;
    bool any = false;
    for (uint32_t i = 0; i < m->poll_count;) {
        DSourceBuffer *b = m->polls[i].buffer;
        DMediaSource *s = parent_of(dom, b);
        os64_dom_fed_result_t r = {.status = OS64_DOM_FED_PENDING, .timestamp_offset = NAN, .highest_pts = NAN,
                                   .group_end = NAN, .init_duration = NAN};
        if (s != NULL && b->handle != NULL) host(dom)->poll(host(dom)->opaque, s->session, b->handle, &r);
        if (r.status == OS64_DOM_FED_PENDING) {
            i++;
            continue;
        }
        JSValue held = m->polls[i].object;
        m->polls[i] = m->polls[--m->poll_count];
        b->polling = false;
        parsed(dom, b, &r);
        JS_FreeValueRT(dom->engine, held);
        any = true;
    }
    return any;
}

/* ── MediaSource ────────────────────────────────────────────────────── */

enum {
    S_BUFFERS, S_ACTIVE, S_READY, S_DURATION, S_ADD, S_REMOVE, S_END, S_LIVE_SET, S_LIVE_CLEAR,
    S_IS_TYPE_SUPPORTED, S_WORKER,
};

static JSValue source_add(os64_dom_t *dom, JSContext *ctx, DMediaSource *s, int argc, JSValueConst *argv)
{
    const char *type = argc > 0 ? JS_ToCString(ctx, argv[0]) : JS_ToCString(ctx, JS_UNDEFINED);
    if (type == NULL) return JS_EXCEPTION;
    JSValue result;
    if (type[0] == '\0') result = JS_ThrowTypeError(ctx, "The type is empty");
    else if (!type_supported(type)) result = d_error(ctx, "NotSupportedError", "That type is not supported");
    else if (s->count >= MSE_BUFFERS_MAX) result = d_error(ctx, "QuotaExceededError", "This MediaSource holds no more SourceBuffers");
    else if (s->ready != MS_OPEN) result = d_error(ctx, "InvalidStateError", "The MediaSource is not open");
    else if (!d_provider_begin(dom, ctx)) result = JS_EXCEPTION;
    else {
        void *handle = host(dom)->add != NULL ? host(dom)->add(host(dom)->opaque, s->session, type) : NULL;
        d_provider_leave(dom);
        result = handle == NULL ? d_error(ctx, "QuotaExceededError", "No SourceBuffer of that type can be made now")
                                : JS_NewObjectClass(ctx, (int)dom->mse->buffer_class);
        if (handle != NULL && !JS_IsException(result)) {
            DSourceBuffer *b = js_mallocz(ctx, sizeof(*b));
            if (b == NULL) {
                JS_FreeValue(ctx, result);
                result = JS_EXCEPTION;
            } else {
                d_event_target_init(dom, &b->target, result);
                b->self = result;
                b->parent = JS_DupValue(ctx, s->self);
                b->handle = handle;
                b->mode = OS64_DOM_FED_SEGMENTS;
                b->window_end = INFINITY;
                b->highest_pts = NAN;
                b->buffered = JS_UNDEFINED;
                JS_SetOpaque(result, b);
                s->buffers[s->count] = JS_DupValue(ctx, result);
                s->active[s->count] = false;
                s->count++;
                list_event(dom, s->list, "addsourcebuffer");
            }
        }
        if (handle != NULL && JS_IsException(result) && host(dom)->remove_buffer != NULL && d_provider_enter(dom)) {
            host(dom)->remove_buffer(host(dom)->opaque, s->session, handle);
            d_provider_leave(dom);
        }
    }
    JS_FreeCString(ctx, type);
    return result;
}

/* removeSourceBuffer (§ 3.8). */
static JSValue source_remove(os64_dom_t *dom, JSContext *ctx, DMediaSource *s, int argc, JSValueConst *argv)
{
    DSourceBuffer *b = argc > 0 ? buffer_of(dom, argv[0]) : NULL;
    if (b == NULL) return JS_ThrowTypeError(ctx, "The argument is not a SourceBuffer");
    uint32_t i = index_of(s, b);
    if (i >= s->count) return d_error(ctx, "NotFoundError", "That SourceBuffer is not this MediaSource's");
    if (b->updating) {
        work_cancel(dom, b);
        b->updating = false;
        b->removing = false;
        buffer_event(dom, b, "abort");
        buffer_event(dom, b, "updateend");
    }
    if (s->active[i]) list_event(dom, s->active_list, "removesourcebuffer");
    JSValue held = s->buffers[i];
    for (uint32_t k = i + 1; k < s->count; k++) {
        s->buffers[k - 1] = s->buffers[k];
        s->active[k - 1] = s->active[k];
    }
    s->count--;
    list_event(dom, s->list, "removesourcebuffer");
    if (b->handle != NULL && host(dom)->remove_buffer != NULL && d_provider_begin(dom, ctx)) {
        host(dom)->remove_buffer(host(dom)->opaque, s->session, b->handle);
        d_provider_leave(dom);
    }
    b->handle = NULL;
    JS_FreeValue(ctx, b->parent);
    b->parent = JS_UNDEFINED;
    JS_FreeValue(ctx, held);
    return JS_UNDEFINED;
}

static bool any_updating(os64_dom_t *dom, DMediaSource *s)
{
    for (uint32_t i = 0; i < s->count; i++) {
        DSourceBuffer *b = buffer_of(dom, s->buffers[i]);
        if (b != NULL && b->updating) return true;
    }
    return false;
}

static JSValue source_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL || dom->mse == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    if (magic == S_IS_TYPE_SUPPORTED) {
        const char *type = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
        if (type == NULL) return JS_EXCEPTION;
        /* With no fed host nothing can play, and a player told yes would
         * choose MSE over the plain file it falls back to. */
        bool yes = host(dom)->open != NULL && type[0] != '\0' && type_supported(type);
        JS_FreeCString(ctx, type);
        return JS_NewBool(ctx, yes);
    }
    if (magic == S_WORKER) return JS_FALSE;
    DMediaSource *s = source_of(dom, self);
    if (s == NULL) return JS_ThrowTypeError(ctx, "Illegal invocation");
    switch (magic) {
    case S_BUFFERS: return JS_DupValue(ctx, s->list);
    case S_ACTIVE: return JS_DupValue(ctx, s->active_list);
    case S_READY: return JS_NewString(ctx, s->ready == MS_OPEN ? "open" : s->ready == MS_ENDED ? "ended" : "closed");
    case S_DURATION: {
        if (!set) return JS_NewFloat64(ctx, s->ready == MS_CLOSED ? NAN : s->duration);
        double value;
        if (JS_ToFloat64(ctx, &value, argc > 0 ? argv[0] : JS_UNDEFINED) < 0) return JS_EXCEPTION;
        if (isnan(value) || value < 0) return JS_ThrowTypeError(ctx, "The duration is negative or NaN");
        if (s->ready != MS_OPEN) return d_error(ctx, "InvalidStateError", "The MediaSource is not open");
        if (any_updating(dom, s)) return d_error(ctx, "InvalidStateError", "A SourceBuffer is updating");
        if (!d_provider_begin(dom, ctx)) return JS_EXCEPTION;
        bool ok = duration_change(dom, ctx, s, value);
        d_provider_leave(dom);
        return ok ? JS_UNDEFINED : JS_EXCEPTION;
    }
    case S_ADD: return source_add(dom, ctx, s, argc, argv);
    case S_REMOVE: return source_remove(dom, ctx, s, argc, argv);
    case S_END: {
        int error = END_NONE;
        if (argc > 0 && !JS_IsUndefined(argv[0])) {
            const char *text = JS_ToCString(ctx, argv[0]);
            if (text == NULL) return JS_EXCEPTION;
            error = os64_streq(text, "network") ? END_NETWORK : os64_streq(text, "decode") ? END_DECODE : -1;
            JS_FreeCString(ctx, text);
            if (error < 0) return JS_ThrowTypeError(ctx, "The error is neither \"network\" nor \"decode\"");
        }
        if (s->ready != MS_OPEN) return d_error(ctx, "InvalidStateError", "The MediaSource is not open");
        if (any_updating(dom, s)) return d_error(ctx, "InvalidStateError", "A SourceBuffer is updating");
        if (!d_provider_begin(dom, ctx)) return JS_EXCEPTION;
        end_of_stream(dom, s, error);
        d_provider_leave(dom);
        return JS_UNDEFINED;
    }
    case S_LIVE_SET: {
        double start, end;
        if (JS_ToFloat64(ctx, &start, argc > 0 ? argv[0] : JS_UNDEFINED) < 0 ||
            JS_ToFloat64(ctx, &end, argc > 1 ? argv[1] : JS_UNDEFINED) < 0)
            return JS_EXCEPTION;
        if (!isfinite(start) || !isfinite(end)) return JS_ThrowTypeError(ctx, "The range is not finite");
        if (s->ready != MS_OPEN) return d_error(ctx, "InvalidStateError", "The MediaSource is not open");
        if (start < 0 || start > end) return JS_ThrowTypeError(ctx, "The range is negative or backwards");
        s->live = true;
        s->live_start = start;
        s->live_end = end;
        return JS_UNDEFINED;
    }
    case S_LIVE_CLEAR:
        if (s->ready != MS_OPEN) return d_error(ctx, "InvalidStateError", "The MediaSource is not open");
        s->live = false;
        return JS_UNDEFINED;
    default: return JS_ThrowTypeError(ctx, "Unknown MediaSource member");
    }
}

static JSValue list_new(os64_dom_t *dom, JSContext *ctx, JSValueConst source, bool active)
{
    JSValue o = JS_NewObjectClass(ctx, (int)dom->mse->list_class);
    if (JS_IsException(o)) return o;
    DSourceBufferList *l = js_mallocz(ctx, sizeof(*l));
    if (l == NULL) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    d_event_target_init(dom, &l->target, o);
    l->source = JS_DupValue(ctx, source);
    l->active = active;
    JS_SetOpaque(o, l);
    return o;
}

static JSValue source_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL || dom->mse == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSClassID id = dom->mse->source_class;
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id) : JS_NewObjectClass(ctx, (int)id);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object)) return object;
    DMediaSource *s = js_mallocz(ctx, sizeof(*s));
    if (s == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    d_event_target_init(dom, &s->target, object);
    s->self = object;
    s->ready = MS_CLOSED;
    s->duration = NAN;
    s->list = s->active_list = JS_UNDEFINED;
    for (uint32_t i = 0; i < MSE_BUFFERS_MAX; i++) s->buffers[i] = JS_UNDEFINED;
    JS_SetOpaque(object, s);
    s->list = list_new(dom, ctx, object, false);
    s->active_list = JS_IsException(s->list) ? JS_EXCEPTION : list_new(dom, ctx, object, true);
    if (JS_IsException(s->list) || JS_IsException(s->active_list)) {
        if (JS_IsException(s->list)) s->list = JS_UNDEFINED;
        if (JS_IsException(s->active_list)) s->active_list = JS_UNDEFINED;
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
}

static void source_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DMediaSource *s = JS_GetOpaque(value, os64_js_class_id(&media_source_slot));
    if (s == NULL) return;
    d_event_target_mark(rt, &s->target, mark);
    for (uint32_t i = 0; i < s->count; i++) JS_MarkValue(rt, s->buffers[i], mark);
    JS_MarkValue(rt, s->list, mark);
    JS_MarkValue(rt, s->active_list, mark);
}

static void source_finalizer(JSRuntime *rt, JSValue value)
{
    DMediaSource *s = JS_GetOpaque(value, os64_js_class_id(&media_source_slot));
    if (s == NULL) return;
    d_event_target_finalize(rt, &s->target);
    for (uint32_t i = 0; i < s->count; i++) JS_FreeValueRT(rt, s->buffers[i]);
    JS_FreeValueRT(rt, s->list);
    JS_FreeValueRT(rt, s->active_list);
    js_free_rt(rt, s);
}

/* ── SourceBuffer ───────────────────────────────────────────────────── */

enum {
    B_MODE, B_UPDATING, B_BUFFERED, B_OFFSET, B_WINDOW_START, B_WINDOW_END, B_APPEND, B_ABORT, B_CHANGE_TYPE,
    B_REMOVE,
};

static JSValue removed_error(JSContext *ctx)
{
    return d_error(ctx, "InvalidStateError", "This SourceBuffer has been removed from its MediaSource");
}

/* appendBuffer (§ 5.2), prepare append (§ 5.5.4) included. */
static JSValue buffer_append(os64_dom_t *dom, JSContext *ctx, DSourceBuffer *b, int argc, JSValueConst *argv)
{
    const uint8_t *bytes = NULL;
    size_t length = 0;
    int is = d_buffer_source(dom, ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &bytes, &length);
    if (is < 0) return JS_EXCEPTION;
    if (is == 0) return JS_ThrowTypeError(ctx, "The argument is not a BufferSource");
    DMediaSource *s = parent_of(dom, b);
    if (s == NULL) return removed_error(ctx);
    if (b->updating) return d_error(ctx, "InvalidStateError", "The SourceBuffer is updating");
    if (d_media_fed_has_error(s->media)) return d_error(ctx, "InvalidStateError", "The media element has an error");
    if (!d_provider_begin(dom, ctx)) return JS_EXCEPTION;
    reopen(dom, s);
    bool room = b->handle != NULL && host(dom)->evict != NULL &&
                host(dom)->evict(host(dom)->opaque, s->session, b->handle, length);
    bool taken = room && host(dom)->take != NULL && host(dom)->take(host(dom)->opaque, s->session, b->handle, bytes, length);
    d_provider_leave(dom);
    if (!room) return d_error(ctx, "QuotaExceededError", "The SourceBuffer is full");
    if (!taken) return d_error(ctx, "QuotaExceededError", "No memory for the appended bytes");
    b->updating = true;
    buffer_event(dom, b, "updatestart");
    b->pending = d_media_queue_work(dom, b->self, append_run, work_left, b);
    if (b->pending == NULL) {
        /* No memory for the task: the append ends as an error would, its
         * bytes reset away. */
        if (d_provider_begin(dom, ctx)) {
            append_error(dom, s, b);
            d_provider_leave(dom);
        }
    }
    return JS_UNDEFINED;
}

/* abort() (§ 5.2). */
static JSValue buffer_abort(os64_dom_t *dom, JSContext *ctx, DSourceBuffer *b)
{
    DMediaSource *s = parent_of(dom, b);
    if (s == NULL) return removed_error(ctx);
    if (s->ready != MS_OPEN) return d_error(ctx, "InvalidStateError", "The MediaSource is not open");
    if (b->removing) return d_error(ctx, "InvalidStateError", "A removal is running");
    if (b->updating) {
        work_cancel(dom, b);
        b->updating = false;
        buffer_event(dom, b, "abort");
        buffer_event(dom, b, "updateend");
    }
    if (!d_provider_begin(dom, ctx)) return JS_EXCEPTION;
    if (b->handle != NULL && host(dom)->reset != NULL) {
        os64_dom_fed_result_t r = {.timestamp_offset = NAN, .highest_pts = NAN, .group_end = NAN};
        host(dom)->reset(host(dom)->opaque, s->session, b->handle, &r);
        take_result(b, &r);
    }
    b->window_start = 0;
    b->window_end = INFINITY;
    push_settings(dom, b);
    d_provider_leave(dom);
    return JS_UNDEFINED;
}

/* remove(start, end) (§ 5.2) and the range removal's synchronous half
 * (§ 5.5.6). */
static JSValue buffer_remove(os64_dom_t *dom, JSContext *ctx, DSourceBuffer *b, int argc, JSValueConst *argv)
{
    double start, end;
    if (JS_ToFloat64(ctx, &start, argc > 0 ? argv[0] : JS_UNDEFINED) < 0 ||
        JS_ToFloat64(ctx, &end, argc > 1 ? argv[1] : JS_UNDEFINED) < 0)
        return JS_EXCEPTION;
    if (!isfinite(start)) return JS_ThrowTypeError(ctx, "The start is not finite");
    DMediaSource *s = parent_of(dom, b);
    if (s == NULL) return removed_error(ctx);
    if (b->updating) return d_error(ctx, "InvalidStateError", "The SourceBuffer is updating");
    if (isnan(s->duration)) return JS_ThrowTypeError(ctx, "The MediaSource has no duration");
    if (start < 0 || start > s->duration) return JS_ThrowTypeError(ctx, "The start is outside the duration");
    if (isnan(end) || end <= start) return JS_ThrowTypeError(ctx, "The end is not after the start");
    reopen(dom, s);
    b->remove_start = start;
    b->remove_end = end;
    b->updating = true;
    b->removing = true;
    buffer_event(dom, b, "updatestart");
    b->pending = d_media_queue_work(dom, b->self, remove_run, work_left, b);
    if (b->pending == NULL) {
        b->updating = false;
        b->removing = false;
        return JS_ThrowOutOfMemory(ctx);
    }
    return JS_UNDEFINED;
}

static JSValue buffer_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL || dom->mse == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    DSourceBuffer *b = buffer_of(dom, self);
    if (b == NULL) return JS_ThrowTypeError(ctx, "Illegal invocation");
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    DMediaSource *s = parent_of(dom, b);
    switch (magic) {
    case B_MODE: {
        if (!set) return JS_NewString(ctx, b->mode == OS64_DOM_FED_SEQUENCE ? "sequence" : "segments");
        const char *text = JS_ToCString(ctx, value);
        if (text == NULL) return JS_EXCEPTION;
        int mode = os64_streq(text, "segments") ? OS64_DOM_FED_SEGMENTS
                   : os64_streq(text, "sequence") ? OS64_DOM_FED_SEQUENCE : -1;
        JS_FreeCString(ctx, text);
        /* An enumeration's unknown value is no assignment (WebIDL § 3.2.9). */
        if (mode < 0) return JS_UNDEFINED;
        if (s == NULL) return removed_error(ctx);
        if (b->updating) return d_error(ctx, "InvalidStateError", "The SourceBuffer is updating");
        reopen(dom, s);
        if (b->parsing_media) return d_error(ctx, "InvalidStateError", "A media segment is being parsed");
        b->mode = (os64_dom_fed_mode_t)mode;
        if (d_provider_begin(dom, ctx)) {
            push_settings(dom, b);
            d_provider_leave(dom);
        }
        return JS_UNDEFINED;
    }
    case B_UPDATING: return JS_NewBool(ctx, b->updating);
    case B_BUFFERED: {
        if (s == NULL) return removed_error(ctx);
        os64_dom_fed_range_t r[D_RANGES_MAX];
        double highest;
        uint32_t n = 0;
        if (d_provider_begin(dom, ctx)) {
            n = buffer_ranges(dom, s, b, r, D_RANGES_MAX, &highest);
            d_provider_leave(dom);
        }
        /* The same object while nothing changed (the getter's step 5). */
        if (!JS_IsUndefined(b->buffered) && n == b->cached_count &&
            (n == 0 || os64_memcmp(r, b->cached, sizeof(r[0]) * n) == 0))
            return JS_DupValue(ctx, b->buffered);
        JSValue o = d_media_ranges(dom, ctx, r, n);
        if (JS_IsException(o)) return o;
        JS_FreeValue(ctx, b->buffered);
        b->buffered = JS_DupValue(ctx, o);
        if (n != 0) os64_memcpy(b->cached, r, sizeof(r[0]) * n);
        b->cached_count = n;
        return o;
    }
    case B_OFFSET: {
        if (!set) return JS_NewFloat64(ctx, b->offset);
        double v;
        if (JS_ToFloat64(ctx, &v, value) < 0) return JS_EXCEPTION;
        if (!isfinite(v)) return JS_ThrowTypeError(ctx, "The offset is not finite");
        if (s == NULL) return removed_error(ctx);
        if (b->updating) return d_error(ctx, "InvalidStateError", "The SourceBuffer is updating");
        reopen(dom, s);
        if (b->parsing_media) return d_error(ctx, "InvalidStateError", "A media segment is being parsed");
        b->offset = v;
        if (d_provider_begin(dom, ctx)) {
            push_settings(dom, b);
            d_provider_leave(dom);
        }
        return JS_UNDEFINED;
    }
    case B_WINDOW_START:
    case B_WINDOW_END: {
        bool start = magic == B_WINDOW_START;
        if (!set) return JS_NewFloat64(ctx, start ? b->window_start : b->window_end);
        double v;
        if (JS_ToFloat64(ctx, &v, value) < 0) return JS_EXCEPTION;
        if (start && !isfinite(v)) return JS_ThrowTypeError(ctx, "The window's start is not finite");
        if (s == NULL) return removed_error(ctx);
        if (b->updating) return d_error(ctx, "InvalidStateError", "The SourceBuffer is updating");
        if (start && (v < 0 || v >= b->window_end))
            return JS_ThrowTypeError(ctx, "The window's start is negative or not before its end");
        if (!start && (isnan(v) || v <= b->window_start))
            return JS_ThrowTypeError(ctx, "The window's end is not after its start");
        if (start) b->window_start = v;
        else b->window_end = v;
        if (d_provider_begin(dom, ctx)) {
            push_settings(dom, b);
            d_provider_leave(dom);
        }
        return JS_UNDEFINED;
    }
    case B_APPEND: return buffer_append(dom, ctx, b, argc, argv);
    case B_ABORT: return buffer_abort(dom, ctx, b);
    case B_CHANGE_TYPE: {
        const char *type = JS_ToCString(ctx, value);
        if (type == NULL) return JS_EXCEPTION;
        bool empty = type[0] == '\0';
        JS_FreeCString(ctx, type);
        if (empty) return JS_ThrowTypeError(ctx, "The type is empty");
        if (s == NULL) return removed_error(ctx);
        if (b->updating) return d_error(ctx, "InvalidStateError", "The SourceBuffer is updating");
        return d_error(ctx, "NotSupportedError", "This SourceBuffer cannot change its type");
    }
    case B_REMOVE: return buffer_remove(dom, ctx, b, argc, argv);
    default: return JS_ThrowTypeError(ctx, "Unknown SourceBuffer member");
    }
}

static void buffer_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DSourceBuffer *b = JS_GetOpaque(value, os64_js_class_id(&source_buffer_slot));
    if (b == NULL) return;
    d_event_target_mark(rt, &b->target, mark);
    JS_MarkValue(rt, b->parent, mark);
    JS_MarkValue(rt, b->buffered, mark);
}

static void buffer_finalizer(JSRuntime *rt, JSValue value)
{
    DSourceBuffer *b = JS_GetOpaque(value, os64_js_class_id(&source_buffer_slot));
    if (b == NULL) return;
    d_event_target_finalize(rt, &b->target);
    JS_FreeValueRT(rt, b->parent);
    JS_FreeValueRT(rt, b->buffered);
    js_free_rt(rt, b);
}

/* ── SourceBufferList ───────────────────────────────────────────────── */

enum { L_LENGTH };

static uint32_t list_length(DMediaSource *s, const DSourceBufferList *l)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->count; i++) n += !l->active || s->active[i];
    return n;
}

static JSValue list_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    (void)argc; (void)argv; (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL || dom->mse == NULL) return JS_EXCEPTION;
    DSourceBufferList *l = JS_GetOpaque(self, dom->mse->list_class);
    DMediaSource *s = l != NULL ? source_of(dom, l->source) : NULL;
    if (s == NULL) return JS_ThrowTypeError(ctx, "Illegal invocation");
    return JS_NewUint32(ctx, list_length(s, l));
}

/* The list's indexed getter: [index] for an index below its length. */
static int list_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL || dom->mse == NULL) return 0;
    DSourceBufferList *l = JS_GetOpaque(obj, dom->mse->list_class);
    DMediaSource *s = l != NULL ? source_of(dom, l->source) : NULL;
    uint32_t index;
    int numeric = s != NULL ? d_index_key(ctx, prop, &index) : 0;
    if (numeric <= 0) return numeric;
    for (uint32_t i = 0; i < s->count; i++) {
        if (l->active && !s->active[i]) continue;
        if (index-- != 0) continue;
        if (desc != NULL) {
            desc->flags = JS_PROP_ENUMERABLE;
            desc->value = JS_DupValue(ctx, s->buffers[i]);
            desc->getter = desc->setter = JS_UNDEFINED;
        }
        return 1;
    }
    return 0;
}

static int list_names(JSContext *ctx, JSPropertyEnum **table, uint32_t *length, JSValueConst obj)
{
    os64_dom_t *dom = d_binding(ctx);
    DSourceBufferList *l = dom != NULL && dom->mse != NULL ? JS_GetOpaque(obj, dom->mse->list_class) : NULL;
    DMediaSource *s = l != NULL ? source_of(dom, l->source) : NULL;
    return d_index_names(ctx, s != NULL ? list_length(s, l) : 0, table, length);
}

static void list_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DSourceBufferList *l = JS_GetOpaque(value, os64_js_class_id(&source_buffer_list_slot));
    if (l == NULL) return;
    d_event_target_mark(rt, &l->target, mark);
    JS_MarkValue(rt, l->source, mark);
}

static void list_finalizer(JSRuntime *rt, JSValue value)
{
    DSourceBufferList *l = JS_GetOpaque(value, os64_js_class_id(&source_buffer_list_slot));
    if (l == NULL) return;
    d_event_target_finalize(rt, &l->target);
    JS_FreeValueRT(rt, l->source);
    js_free_rt(rt, l);
}

/* ── The element's side (media.c) ───────────────────────────────────── */

bool d_mse_is(os64_dom_t *dom, JSValueConst value)
{
    return source_of(dom, value) != NULL;
}

bool d_mse_attach(os64_dom_t *dom, JSValueConst source, void *media, const os64_html_node_t *node, void **session)
{
    DMediaSource *s = source_of(dom, source);
    *session = NULL;
    if (s == NULL || s->ready != MS_CLOSED || host(dom)->open == NULL) return false;
    void *opened = host(dom)->open(host(dom)->opaque, node);
    if (opened == NULL) return false;
    s->media = media;
    s->session = opened;
    s->ready = MS_OPEN;
    source_event(dom, s, "sourceopen");
    *session = opened;
    return true;
}

/* The detach (MSE § 3.15.2), and for work in flight what the other
 * browsers do (MSE_DOM.md § 9, question 1): an append ends with abort and
 * updateend (both), a removal ends silently (Chromium). */
void d_mse_detach(os64_dom_t *dom, JSValueConst source)
{
    DMediaSource *s = source_of(dom, source);
    if (s == NULL) return;
    for (uint32_t i = 0; i < s->count; i++) {
        DSourceBuffer *b = buffer_of(dom, s->buffers[i]);
        if (b == NULL || !b->updating) continue;
        bool removal = b->removing;
        work_cancel(dom, b);
        b->updating = false;
        b->removing = false;
        if (!removal) {
            buffer_event(dom, b, "abort");
            buffer_event(dom, b, "updateend");
        }
    }
    s->ready = MS_CLOSED;
    s->duration = NAN;
    list_event(dom, s->active_list, "removesourcebuffer");
    list_event(dom, s->list, "removesourcebuffer");
    for (uint32_t i = 0; i < s->count; i++) {
        DSourceBuffer *b = buffer_of(dom, s->buffers[i]);
        if (b != NULL) {
            b->handle = NULL;
            JS_FreeValueRT(dom->engine, b->parent);
            b->parent = JS_UNDEFINED;
        }
        JS_FreeValueRT(dom->engine, s->buffers[i]);
        s->buffers[i] = JS_UNDEFINED;
        s->active[i] = false;
    }
    s->count = 0;
    source_event(dom, s, "sourceclose");
    if (s->session != NULL && host(dom)->close != NULL) host(dom)->close(host(dom)->opaque, s->session);
    s->session = NULL;
    s->media = NULL;
}

bool d_mse_probed(os64_dom_t *dom, JSValueConst source)
{
    DMediaSource *s = source_of(dom, source);
    if (s == NULL || s->count == 0) return false;
    for (uint32_t i = 0; i < s->count; i++) {
        DSourceBuffer *b = buffer_of(dom, s->buffers[i]);
        if (b == NULL || !b->first_init) return false;
    }
    return true;
}

/* The element's buffered (MSE § 10.2): the active SourceBuffers' ranges
 * intersected, from [0, the highest end among them]; when the source has
 * ended, each one's last range reaches that highest end first. */
uint32_t d_mse_buffered(os64_dom_t *dom, JSValueConst source, os64_dom_fed_range_t *out, uint32_t cap)
{
    DMediaSource *s = source_of(dom, source);
    if (s == NULL || cap == 0) return 0;
    os64_dom_fed_range_t each[MSE_BUFFERS_MAX][D_RANGES_MAX];
    uint32_t counts[MSE_BUFFERS_MAX] = {0};
    double highest = 0;
    bool any = false;
    for (uint32_t i = 0; i < s->count; i++) {
        if (!s->active[i]) continue;
        double top;
        counts[i] = buffer_ranges(dom, s, buffer_of(dom, s->buffers[i]), each[i], D_RANGES_MAX, &top);
        if (counts[i] > 0 && each[i][counts[i] - 1].end > highest) highest = each[i][counts[i] - 1].end;
        any = true;
    }
    if (!any || !(highest > 0)) return 0;
    if (s->ready == MS_ENDED)
        for (uint32_t i = 0; i < s->count; i++)
            if (s->active[i] && counts[i] > 0) each[i][counts[i] - 1].end = highest;
    os64_dom_fed_range_t now[D_RANGES_MAX], next[D_RANGES_MAX];
    now[0] = (os64_dom_fed_range_t){0, highest};
    uint32_t n = 1;
    for (uint32_t i = 0; i < s->count; i++) {
        if (!s->active[i]) continue;
        n = intersect(now, n, each[i], counts[i], next, D_RANGES_MAX);
        os64_memcpy(now, next, sizeof(now[0]) * n);
    }
    if (n > cap) n = cap;
    os64_memcpy(out, now, sizeof(now[0]) * n);
    return n;
}

/* The element's seekable (MSE § 10.1). */
uint32_t d_mse_seekable(os64_dom_t *dom, JSValueConst source, os64_dom_fed_range_t *out)
{
    DMediaSource *s = source_of(dom, source);
    if (s == NULL || isnan(s->duration)) return 0;
    if (s->duration != INFINITY) {
        *out = (os64_dom_fed_range_t){0, s->duration};
        return 1;
    }
    os64_dom_fed_range_t r[D_RANGES_MAX];
    uint32_t n = d_mse_buffered(dom, source, r, D_RANGES_MAX);
    if (s->live) {
        double start = s->live_start, end = s->live_end;
        if (n > 0 && r[0].start < start) start = r[0].start;
        if (n > 0 && r[n - 1].end > end) end = r[n - 1].end;
        *out = (os64_dom_fed_range_t){start, end};
        return 1;
    }
    if (n == 0) return 0;
    *out = (os64_dom_fed_range_t){0, r[n - 1].end};
    return 1;
}

void d_mse_decode_error(os64_dom_t *dom, JSValueConst source)
{
    DMediaSource *s = source_of(dom, source);
    if (s == NULL || s->ready != MS_OPEN) return;
    s->ready = MS_ENDED;
    source_event(dom, s, "sourceended");
}

/* ── Object URLs (File API § 8: a blob URL whose entry is a MediaSource) ── */

static size_t without_fragment(const char *url)
{
    size_t n = 0;
    while (url[n] != '\0' && url[n] != '#') n++;
    return n;
}

JSValue d_mse_lookup(os64_dom_t *dom, const char *url)
{
    struct DMseState *m = dom->mse;
    if (m == NULL || url == NULL || os64_strlen(url) < 5 || os64_memcmp(url, "blob:", 5) != 0) return JS_UNDEFINED;
    size_t n = without_fragment(url);
    for (uint32_t i = 0; i < m->url_count; i++)
        if (os64_strlen(m->urls[i].url) == n && os64_memcmp(m->urls[i].url, url, n) == 0) return m->urls[i].source;
    return JS_UNDEFINED;
}

JSValue d_mse_url_create(os64_dom_t *dom, JSContext *ctx, JSValueConst source)
{
    struct DMseState *m = dom->mse;
    if (m == NULL || source_of(dom, source) == NULL) return JS_ThrowTypeError(ctx, "The argument is not a MediaSource");
    if (m->url_count >= MSE_URLS_MAX)
        return d_error(ctx, "QuotaExceededError", "The page names more MediaSources than its bound");
    char origin[WAY_ORIGIN_TEXT_MAX], uuid[37];
    if (!d_origin(dom->url != NULL ? dom->url : "about:blank", origin, sizeof(origin)))
        os64_strcopy(origin, sizeof(origin), "null");
    if (!d_uuid_text(dom, ctx, uuid)) return JS_EXCEPTION;
    size_t cap = 5 + os64_strlen(origin) + 1 + 36 + 1;
    char *url = d_alloc(dom, cap);
    if (url == NULL) return JS_ThrowOutOfMemory(ctx);
    os64_snprintf(url, cap, "blob:%s/%s", origin, uuid);
    if (m->url_count == m->url_cap) {
        uint32_t grow = m->url_cap != 0 ? m->url_cap * 2 : 8;
        DMseUrl *grown = d_alloc(dom, sizeof(*grown) * grow);
        if (grown == NULL) {
            d_free(dom, url);
            return JS_ThrowOutOfMemory(ctx);
        }
        if (m->url_count != 0) os64_memcpy(grown, m->urls, sizeof(*grown) * m->url_count);
        d_free(dom, m->urls);
        m->urls = grown;
        m->url_cap = grow;
    }
    m->urls[m->url_count++] = (DMseUrl){url, JS_DupValue(ctx, source)};
    return JS_NewString(ctx, url);
}

void d_mse_url_revoke(os64_dom_t *dom, const char *url)
{
    struct DMseState *m = dom->mse;
    if (m == NULL || url == NULL) return;
    for (uint32_t i = 0; i < m->url_count; i++)
        if (os64_streq(m->urls[i].url, url)) {
            d_free(dom, m->urls[i].url);
            JS_FreeValueRT(dom->engine, m->urls[i].source);
            m->urls[i] = m->urls[--m->url_count];
            return;
        }
}

/* ── The install, the drain and the free ────────────────────────────── */

void d_mse_drain(os64_dom_t *dom)
{
    struct DMseState *m = dom->mse;
    if (m == NULL) return;
    for (uint32_t i = 0; i < m->url_count; i++) {
        d_free(dom, m->urls[i].url);
        JS_FreeValueRT(dom->engine, m->urls[i].source);
    }
    m->url_count = 0;
    for (uint32_t i = 0; i < m->poll_count; i++) {
        m->polls[i].buffer->polling = false;
        JS_FreeValueRT(dom->engine, m->polls[i].object);
    }
    m->poll_count = 0;
}

void d_mse_free(os64_dom_t *dom)
{
    struct DMseState *m = dom->mse;
    if (m == NULL) return;
    for (uint32_t i = 0; i < m->url_count; i++) d_free(dom, m->urls[i].url);
    d_free(dom, m->urls);
    d_free(dom, m->polls);
    d_free(dom, m);
    dom->mse = NULL;
}

static int handlers(os64_dom_t *dom, JSContext *ctx, JSValueConst proto, const char *const *names, size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (d_event_handler_named(dom, ctx, proto, names[i]) < 0) return -1;
    return 0;
}

int d_mse_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    struct DMseState *m = d_alloc(dom, sizeof(*m));
    if (m == NULL) return -1;
    dom->mse = m;
    m->source_class = os64_js_class_id(&media_source_slot);
    m->buffer_class = os64_js_class_id(&source_buffer_slot);
    m->list_class = os64_js_class_id(&source_buffer_list_slot);

    /* SourceBufferList and SourceBuffer: objects only the binding makes. */
    static JSClassExoticMethods list_exotic = {.get_own_property = list_own, .get_own_property_names = list_names};
    static const JSClassDef list_class = {
        .class_name = "SourceBufferList", .finalizer = list_finalizer, .gc_mark = list_mark, .exotic = &list_exotic
    };
    static const JSClassDef buffer_class = {
        .class_name = "SourceBuffer", .finalizer = buffer_finalizer, .gc_mark = buffer_mark
    };
    JSValue list_proto, buffer_proto;
    static const char *const kListEvents[] = {"addsourcebuffer", "removesourcebuffer"};
    if (d_interface_class(dom, ctx, global, m->list_class, &list_class, &list_proto) < 0 ||
        d_event_target(dom, ctx, list_proto, m->list_class, NULL) < 0 ||
        handlers(dom, ctx, list_proto, kListEvents, 2) < 0 ||
        d_accessor(dom, ctx, list_proto, "length", list_member, L_LENGTH, false) < 0 ||
        d_iterable(ctx, list_proto, false) < 0)
        return -1;
    static const char *const kBufferEvents[] = {"updatestart", "update", "updateend", "error", "abort"};
    if (d_interface_class(dom, ctx, global, m->buffer_class, &buffer_class, &buffer_proto) < 0 ||
        d_event_target(dom, ctx, buffer_proto, m->buffer_class, NULL) < 0 ||
        handlers(dom, ctx, buffer_proto, kBufferEvents, 5) < 0)
        return -1;
    static const struct { const char *name; int magic; bool writable; } kBufferAttributes[] = {
        {"mode", B_MODE, true}, {"updating", B_UPDATING, false}, {"buffered", B_BUFFERED, false},
        {"timestampOffset", B_OFFSET, true}, {"appendWindowStart", B_WINDOW_START, true},
        {"appendWindowEnd", B_WINDOW_END, true},
    };
    for (size_t i = 0; i < sizeof(kBufferAttributes) / sizeof(kBufferAttributes[0]); i++)
        if (d_accessor(dom, ctx, buffer_proto, kBufferAttributes[i].name, buffer_member, kBufferAttributes[i].magic,
                       kBufferAttributes[i].writable) < 0)
            return -1;
    static const struct { const char *name; int magic, argc; } kBufferMethods[] = {
        {"appendBuffer", B_APPEND, 1}, {"abort", B_ABORT, 0}, {"changeType", B_CHANGE_TYPE, 1}, {"remove", B_REMOVE, 2},
    };
    for (size_t i = 0; i < sizeof(kBufferMethods) / sizeof(kBufferMethods[0]); i++)
        if (d_method_named(dom, ctx, buffer_proto, kBufferMethods[i].name, buffer_member, kBufferMethods[i].argc,
                           kBufferMethods[i].magic) < 0)
            return -1;

    /* MediaSource: constructible, as FileReader is. */
    static const JSClassDef source_class = {
        .class_name = "MediaSource", .finalizer = source_finalizer, .gc_mark = source_mark
    };
    if (!JS_IsRegisteredClass(dom->engine, m->source_class) && JS_NewClass(dom->engine, m->source_class, &source_class) < 0)
        return -1;
    DValue *proto = d_retain(dom, ctx, JS_NewObject(ctx));
    if (proto == NULL) return -1;
    JS_SetClassProto(ctx, m->source_class, JS_DupValue(ctx, proto->value));
    static const char *const kSourceEvents[] = {"sourceopen", "sourceended", "sourceclose"};
    if (d_event_target(dom, ctx, proto->value, m->source_class, NULL) < 0 ||
        handlers(dom, ctx, proto->value, kSourceEvents, 3) < 0)
        return -1;
    static const struct { const char *name; int magic; bool writable; } kSourceAttributes[] = {
        {"sourceBuffers", S_BUFFERS, false}, {"activeSourceBuffers", S_ACTIVE, false},
        {"readyState", S_READY, false}, {"duration", S_DURATION, true},
    };
    for (size_t i = 0; i < sizeof(kSourceAttributes) / sizeof(kSourceAttributes[0]); i++)
        if (d_accessor(dom, ctx, proto->value, kSourceAttributes[i].name, source_member, kSourceAttributes[i].magic,
                       kSourceAttributes[i].writable) < 0)
            return -1;
    static const struct { const char *name; int magic, argc; } kSourceMethods[] = {
        {"addSourceBuffer", S_ADD, 1}, {"removeSourceBuffer", S_REMOVE, 1}, {"endOfStream", S_END, 0},
        {"setLiveSeekableRange", S_LIVE_SET, 2}, {"clearLiveSeekableRange", S_LIVE_CLEAR, 0},
    };
    for (size_t i = 0; i < sizeof(kSourceMethods) / sizeof(kSourceMethods[0]); i++)
        if (d_method_named(dom, ctx, proto->value, kSourceMethods[i].name, source_member, kSourceMethods[i].argc,
                           kSourceMethods[i].magic) < 0)
            return -1;
    JSValue interface = d_interface_new(ctx, "MediaSource", 0, source_constructor, proto->value, JS_UNDEFINED);
    if (JS_IsException(interface)) return -1;
    if (d_method_named(dom, ctx, interface, "isTypeSupported", source_member, 1, S_IS_TYPE_SUPPORTED) < 0 ||
        d_accessor(dom, ctx, interface, "canConstructInDedicatedWorker", source_member, S_WORKER, false) < 0) {
        JS_FreeValue(ctx, interface);
        return -1;
    }
    return JS_DefinePropertyValueStr(ctx, global, "MediaSource", interface, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0
        ? -1 : 0;
}
