#include <math.h>

#include "internal.h"
#include "garb/garb.h"
#include "garb/timing.h"
#include "os64/fmt.h"

/* WEB ANIMATIONS (Web Animations 1; WEB_ANIMATIONS.md): Element.animate,
 * Animation and KeyframeEffect with their constructors, AnimationEffect's
 * timing, DocumentTimeline, getAnimations, commitStyles, the replacement
 * of filling animations (§ 5.5) and AnimationPlaybackEvent; and the CSS
 * animations a script sees (CSS Animations 2): CSSAnimation, which drives
 * the host's CSS animation, and the CSS animation events. This file
 * owns the script's half: the Animation's state machine (§ 4.4,
 * Animations; the numbers are the 2023-06-05 Working Draft's) as written,
 * its promises and events, and keyframe processing (§ 6.6.3). An effect is
 * an object of its own, which owns its target and keyframes; an animation
 * has one or none. The values are libflow's: each animation publishes
 * a garb_effect_t (garb/timing.h) — its keyframes as libgarb reads them,
 * its timing, and its clock — which the host hands to libflow's layouts
 * (os64_dom_script_effects), and libgarb's garb_clock_at is the one clock
 * both sides read, so they cannot disagree about where an animation is.
 *
 * THE TIMELINE'S TIME is sampled once a frame, in the update step
 * (os64_dom_web_animations_update), as HTML's "update animations and send
 * events" does: every currentTime, startTime and timeline.currentTime a
 * script reads between two steps agree. The step runs while anything
 * needs a fresh time (os64_dom_web_animations_next).
 *
 * THE KEYFRAMES OUTLIVE THEIR EFFECT. A layout tree keeps the effects it
 * was laid out with for its ticks and its paints, so an effect's data
 * (DWaData) — when the effect is collected, or setKeyframes replaces it —
 * is buried, marked with the generation it died in, and freed only once
 * the host says no tree that old is left (os64_dom_effects_mark /
 * os64_dom_effects_release). */

static JSClassID animation_slot, effect_slot, timeline_slot;

#define WA_FRAME_MS 20
#define WA_PROPERTIES_MAX 256      /* properties one keyframes argument may name */
#define WA_KEYFRAMES_MAX 1024      /* keyframes one effect may hold */

/* A keyframe as the page gave it, which getKeyframes answers and a copied
 * effect is made from: the offset it gave (NAN for none), its composite
 * (WaComposite), and its declarations as data_make read them. */
typedef enum { WA_COMPOSITE_REPLACE, WA_COMPOSITE_ADD, WA_COMPOSITE_ACCUMULATE, WA_COMPOSITE_AUTO } WaComposite;
static const char *const kComposites[] = {"replace", "add", "accumulate", "auto"};

typedef struct {
    double given;
    WaComposite composite;
    char *text;                     /* `name:value` texts, each ended by a NUL */
    size_t len;
} WaKept;

/* What libflow reads, and what a tree may keep after the animation goes. */
typedef struct DWaData DWaData;
struct DWaData {
    DWaData *next;                  /* on dom->wa_dead once buried */
    uint64_t died;                  /* the generation it was buried in */
    garb_effect_t fx;
    garb_effect_keyframe_t *frames;
    garb_set_t *sets;
    garb_parsed_t *parsed;          /* the declaration blocks the sets' values point into */
    int32_t nparsed;
    WaKept *kept;                   /* fx.nframes of them */
    /* The longhands its keyframes set, each once: its target properties,
     * which § 5.5 compares to replace it. */
    garb_prop_t *props;
    int32_t nprops;
};

/* A KeyframeEffect: its target, its keyframes and timing (DWaData, which
 * it owns and buries), what the page specified that the timing does not
 * keep (`fill: auto`, `duration: auto`), its composite operations, and the
 * animation it is the effect of — by serial, found in dom's list, so that
 * neither object's finalizer reads the other's memory. */
typedef struct DWaEffect DWaEffect;
struct DWaEffect {
    os64_dom_t *dom;                /* NULL once the binding drained it */
    DWaEffect *prev, *next;         /* dom->wa_fx_first */
    const os64_html_node_t *element;    /* the target, held */
    DWaData *data;
    uint64_t owner;                 /* the animation's serial, or 0 */
    bool fill_auto, duration_auto;
    WaComposite composite, iteration_composite;
};

typedef struct DWaAnimation DWaAnimation;
struct DWaAnimation {
    DValue target;                  /* first: the plain event target's entry, in the object */
    os64_dom_t *dom;
    DWaAnimation *prev, *next;      /* dom->wa_first..wa_last, in the order they were made */
    uint64_t serial;
    JSValue effect;                 /* the KeyframeEffect object, or null */
    JSValue hold;                   /* the animation itself while it is live (wa_live) */
    char *id;
    /* § 5.5, Replacing animations: active, removed (its effect applies to
     * nothing) or persisted (never removed). */
    enum { WA_ACTIVE, WA_REMOVED, WA_PERSISTED } replace;
    bool no_timeline;               /* made with a null timeline: it has no time */
    /* The clock: NAN is unresolved. */
    double start, held, rate, pending_rate, previous;
    bool has_pending_rate, pending_play, pending_pause;
    /* The promises, each with its resolving functions; `finished_done` is
     * whether the current finished promise is resolved. */
    JSValue ready, ready_resolve, ready_reject;
    JSValue finished, finished_resolve, finished_reject;
    bool ready_done, finished_done, notify_queued;
    bool was_finished;              /* as update_finished last saw it */
    /* The clock as last published (wa_changed): the hold and start times,
     * the rate and whether a task is pending, each NAN-for-unresolved
     * compared as equal to itself. */
    double pub_held, pub_start, pub_rate;
    bool pub_pending, published;
    /* A CSSAnimation (WEB_ANIMATIONS.md § 3): the host's id for the CSS
     * animation it drives, 0 once that is gone (its owning element
     * cleared); its animation-name; animation-play-state, and the timing,
     * as its style last gave them; whether a script has taken its play
     * state over from that property (css_take_over); and the step's pass
     * that last found it. */
    bool css;
    uint64_t css_id;
    char *css_name;
    size_t css_name_len;
    bool css_style_paused, css_overridden;
    garb_timing_t css_timing;
    uint32_t css_pass;
};

/* An animation event waiting for the next update step (§ 4.4.18,
 * Animation events: the "pending animation event queue"), in the order it
 * was queued: a playback event at its animation, or a CSS animation event
 * at `element` (held), with its animation-name and elapsed time in
 * seconds. */
typedef struct DWaEvent DWaEvent;
struct DWaEvent {
    DWaEvent *next;
    JSValue animation;
    const char *type;
    double current, timeline, scheduled;
    const os64_html_node_t *element;
    char *name;
    size_t name_len;
    double elapsed;
};

/* A CSS animation as the last step saw it, kept while a CSS animation
 * event has a listener, so the next can tell which of its boundaries it
 * crossed (CSS Animations 2 § 4.2): its element (held), its name, its
 * clock and timing, its phase (WaCssPhase) and iteration then, and its
 * place in the host's list then, which orders the cancels of those that
 * go together. */
typedef enum { CSS_IDLE, CSS_BEFORE, CSS_ACTIVE, CSS_AFTER } WaCssPhase;
typedef struct DWaSeen DWaSeen;
struct DWaSeen {
    uint64_t id;
    const os64_html_node_t *element;
    char *name;
    size_t name_len;
    bool paused, canceled;
    double start, hold, rate;
    garb_timing_t timing;
    WaCssPhase phase;
    double iteration;
    int32_t place;
    uint32_t pass;
};

static void css_push(DWaAnimation *a);

static DWaAnimation *wa_state(JSValueConst value)
{
    return JS_GetOpaque(value, os64_js_class_id(&animation_slot));
}

static DWaEffect *fx_state(JSValueConst value)
{
    return JS_GetOpaque(value, os64_js_class_id(&effect_slot));
}

/* An animation's effect, its keyframes and timing, and its target: each
 * NULL for an animation with no effect, or an effect with no target. */
static DWaEffect *wa_fx(const DWaAnimation *a)
{
    return fx_state(a->effect);
}

static DWaData *wa_data(const DWaAnimation *a)
{
    DWaEffect *e = wa_fx(a);
    return e != NULL ? e->data : NULL;
}

static const os64_html_node_t *wa_target(const DWaAnimation *a)
{
    DWaEffect *e = wa_fx(a);
    return e != NULL ? e->element : NULL;
}

/* ── The timeline ────────────────────────────────────────────────────── */

/* The document timeline's time (§ 4.3.1, Document timelines):
 * performance.now() as the last update step or frame-callback batch
 * sampled it (HTML gives both one time), and the face's clock at the same
 * moment, so an effect's start can be told on the face's clock
 * (wa_publish). The first read samples it. */
static void wa_sample_at(os64_dom_t *dom, double now)
{
    dom->wa_time = now;
    uint64_t face = dom->options.now_ms != NULL ? dom->options.now_ms(dom->options.host_opaque) : 0;
    dom->wa_face = (double)face - dom->wa_time;
    dom->wa_sampled = true;
}

static void wa_sample(os64_dom_t *dom)
{
    wa_sample_at(dom, d_performance_now(dom));
}

void d_web_animations_sample(os64_dom_t *dom, double now)
{
    wa_sample_at(dom, now);
}

static double wa_now(os64_dom_t *dom)
{
    if (!dom->wa_sampled) wa_sample(dom);
    return dom->wa_time;
}

/* ── The clock ──────────────────────────────────────────────────────── */

/* The associated effect's end (§ 4.4.6): 0 with no effect. */
static double effect_end(const DWaAnimation *a)
{
    DWaData *d = wa_data(a);
    return d != NULL ? garb_clock_at(&d->fx.timing, 0, false).end_time : 0;
}

static double effective_rate(const DWaAnimation *a)
{
    return a->has_pending_rate ? a->pending_rate : a->rate;
}

static double current_time(DWaAnimation *a)
{
    if (!isnan(a->held)) return a->held;
    if (isnan(a->start) || a->no_timeline) return NAN;
    return (wa_now(a->dom) - a->start) * a->rate;
}

typedef enum { WA_IDLE, WA_RUNNING, WA_PAUSED, WA_FINISHED } WaState;
static const char *const kStates[] = {"idle", "running", "paused", "finished"};

static WaState play_state(DWaAnimation *a)
{
    double t = current_time(a);
    bool pending = a->pending_play || a->pending_pause;
    if (isnan(t) && isnan(a->start) && !pending) return WA_IDLE;
    if (a->pending_pause || (isnan(a->start) && !a->pending_play)) return WA_PAUSED;
    double rate = effective_rate(a);
    if (!isnan(t) && ((rate > 0 && t >= effect_end(a)) || (rate < 0 && t <= 0))) return WA_FINISHED;
    return WA_RUNNING;
}

static void apply_pending_rate(DWaAnimation *a)
{
    if (!a->has_pending_rate) return;
    a->rate = a->pending_rate;
    a->has_pending_rate = false;
}

/* Where the effect is now, as libflow will see it. */
static garb_clock_t wa_clock(DWaAnimation *a)
{
    double t = current_time(a);
    DWaData *d = wa_data(a);
    if (isnan(t) || d == NULL) {
        garb_clock_t none;
        os64_memset(&none, 0, sizeof(none));
        none.phase = GARB_PHASE_BEFORE;
        return none;
    }
    return garb_clock_at(&d->fx.timing, t, a->rate < 0);
}

/* Relevant (§ 4.5.6, Relevant animations): not idle, and current — in
 * its active phase, or before it on the way to it — or in effect. What
 * getAnimations answers, and what the binding keeps alive unreferenced. */
static bool wa_relevant(DWaAnimation *a)
{
    if (play_state(a) == WA_IDLE || wa_data(a) == NULL) return false;
    garb_clock_t k = wa_clock(a);
    if (k.in_effect || k.phase == GARB_PHASE_ACTIVE) return true;
    /* At a rate of 0 it is in neither phase's way: it never gets there. */
    return a->rate > 0 ? k.phase == GARB_PHASE_BEFORE : a->rate < 0 && k.phase == GARB_PHASE_AFTER;
}

/* Any member's layout may read an effect created in another realm. */
static void index_dirty(os64_dom_t *dom)
{
    size_t n = dom->cluster ? dom->cluster->count : 1;
    for (size_t i = 0; i < n; ++i)
        (dom->cluster ? dom->cluster->members[i] : dom)->wa_index_stale = true;
}

/* The clock as libflow reads it (garb_effect_t): held at the hold time, or
 * running from the start on the face's clock at the rate. An idle one,
 * whose current time is unresolved, is handed to no layout
 * (os64_dom_script_effects); a tree laid out before it went idle reads it
 * held at minus infinity, before its start — no value, or its first
 * keyframe's with a backwards fill — until its next layout, which going
 * idle asks for (wa_changed). */
static void wa_publish(DWaAnimation *a)
{
    index_dirty(a->dom);
    DWaData *d = wa_data(a);
    if (d == NULL) return;
    garb_effect_t *fx = &d->fx;
    if (!isnan(a->held)) {
        fx->held = true;
        fx->hold_ms = a->held;
    } else if (!isnan(a->start)) {
        fx->held = false;
        fx->start_ms = a->start + a->dom->wa_face;
    } else {
        fx->held = true;
        fx->hold_ms = -INFINITY;
    }
    fx->rate = a->rate;
}

static bool same_time(double x, double y)
{
    return (isnan(x) && isnan(y)) || x == y;
}

/* After a procedure: when the clock libflow reads moved (the hold or start
 * time, the rate, or a pending task begun or ended), it is published, the
 * target's style moves (os64_html_touch) and the index of effects is
 * rebuilt — or, for a CSSAnimation, it is handed to the host (css_push).
 * A running animation the update step only looked at changed nothing:
 * libflow works where it is from the clock, so the page is not laid out
 * again for it. */
static void wa_changed(DWaAnimation *a)
{
    bool pending = a->pending_play || a->pending_pause;
    if (a->published && same_time(a->pub_held, a->held) && same_time(a->pub_start, a->start) &&
        a->pub_rate == a->rate && a->pub_pending == pending)
        return;
    a->published = true;
    a->pub_held = a->held;
    a->pub_start = a->start;
    a->pub_rate = a->rate;
    a->pub_pending = pending;
    /* A CSSAnimation's effect is the host's to draw: its clock goes there. */
    if (a->css) {
        css_push(a);
        return;
    }
    wa_publish(a);
    if (wa_target(a) != NULL) (void)os64_html_touch(d_owner(a->dom, wa_target(a))->document, (os64_html_node_t *)wa_target(a));
}

/* After a change the clock does not show — the effect's keyframes, timing
 * or target, which effect it is, its replace state: published and the
 * target's style moved whatever the clock says. */
static void wa_restyle(DWaAnimation *a)
{
    a->published = false;
    /* Which effect, target or keyframes it has moves what replaces what. */
    a->dom->wa_replace_check = true;
    wa_changed(a);
}

/* ── Promises ────────────────────────────────────────────────────────── */

static void free3(JSContext *ctx, JSValue *p, JSValue *res, JSValue *rej)
{
    JS_FreeValue(ctx, *p);
    JS_FreeValue(ctx, *res);
    JS_FreeValue(ctx, *rej);
    *p = *res = *rej = JS_UNDEFINED;
}

/* A new pending promise into `p`, its resolving functions beside it. */
static int promise_new(JSContext *ctx, JSValue *p, JSValue *res, JSValue *rej)
{
    JSValue funcs[2];
    JSValue made = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(made)) return -1;
    free3(ctx, p, res, rej);
    *p = made;
    *res = funcs[0];
    *rej = funcs[1];
    return 0;
}

static void promise_call(JSContext *ctx, JSValueConst fn, JSValueConst value)
{
    if (JS_IsUndefined(fn)) return;
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, &value);
    JS_FreeValue(ctx, r);
}

/* A promise rejected by the binding the page never asked for is marked
 * handled ("reset an animation's pending tasks", § 4.4.14), as a
 * handler that does nothing marks it, so no unhandledrejection is owed. */
static JSValue swallow(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)ctx; (void)self; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

static void mark_handled(os64_dom_t *dom, JSContext *ctx, JSValueConst promise)
{
    /* Through the then the binding kept at install, not the promise's own
     * property: a page that replaced it can neither stop the marking nor
     * run code of its own inside cancel. */
    JSValue noop = JS_NewCFunction(ctx, swallow, "", 1);
    if (JS_IsException(noop)) return;
    JSValue args[2] = {JS_UNDEFINED, noop};
    JSValue r = JS_Call(ctx, dom->wa_promise_then->value, promise, 2, args);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, noop);
}

static JSValue abort_error(JSContext *ctx)
{
    JSValue e = d_error(ctx, "AbortError", "The animation was aborted");
    JSValue reason = JS_GetException(ctx);
    JS_FreeValue(ctx, e);
    return reason;
}

static void resolve_ready(JSContext *ctx, DWaAnimation *a)
{
    if (a->ready_done) return;
    a->ready_done = true;
    promise_call(ctx, a->ready_resolve, a->target.value);
}

/* A ready promise that is resolved is replaced by a new pending one, as
 * play and pause do when they schedule a task. */
static int ready_pending(JSContext *ctx, DWaAnimation *a)
{
    if (!a->ready_done) return 0;
    if (promise_new(ctx, &a->ready, &a->ready_resolve, &a->ready_reject) < 0) return -1;
    a->ready_done = false;
    return 0;
}

/* ── Events ──────────────────────────────────────────────────────────── */

/* `scheduled` is when it happened on the timeline (§ 4.4.18.1), which the
 * update step sorts by; NAN for unresolved. */
static void queue_event(JSContext *ctx, DWaAnimation *a, const char *type, double current, double timeline,
                        double scheduled)
{
    os64_dom_t *dom = a->dom;
    DWaEvent *e = d_alloc(dom, sizeof(*e));
    if (e == NULL) return;
    *e = (DWaEvent){.animation = JS_DupValue(ctx, a->target.value), .type = type, .current = current,
                    .timeline = timeline, .scheduled = scheduled};
    if (dom->wa_events_last != NULL) dom->wa_events_last->next = e;
    else dom->wa_events = e;
    dom->wa_events_last = e;
}

static void event_free(os64_dom_t *dom, DWaEvent *e)
{
    JS_FreeValueRT(dom->engine, e->animation);
    if (e->element != NULL) os64_html_release(dom->document, e->element);
    d_free(dom, e->name);
    d_free(dom, e);
}

/* An AnimationEvent's own members (CSS Animations 1 § 4.1). */
static int css_event_fields(JSContext *ctx, JSValue event, const char *name, size_t len, double elapsed)
{
    if (JS_DefinePropertyValueStr(ctx, event, "animationName", JS_NewStringLen(ctx, name, len),
                                  JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE) < 0 ||
        JS_DefinePropertyValueStr(ctx, event, "elapsedTime", JS_NewFloat64(ctx, elapsed),
                                  JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE) < 0 ||
        JS_DefinePropertyValueStr(ctx, event, "pseudoElement", JS_NewString(ctx, ""),
                                  JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    return 0;
}

/* ── The procedures (§ 4.4) ──────────────────────────────────────────── */

static void finish_notify(JSContext *ctx, DWaAnimation *a)
{
    a->notify_queued = false;
    if (play_state(a) != WA_FINISHED || a->finished_done) return;
    a->finished_done = true;
    promise_call(ctx, a->finished_resolve, a->target.value);
    /* Scheduled when its end was reached: the animation time of the end
     * it plays to, on the timeline (§ 4.4.18.3). */
    double end = a->rate > 0 ? effect_end(a) : 0;
    double scheduled = !isnan(a->start) && a->rate != 0 ? a->start + end / a->rate : NAN;
    queue_event(ctx, a, "finish", current_time(a), wa_now(a->dom), scheduled);
}

/* The microtask "update an animation's finished state" queues: the
 * notification, if the animation is still finished when it runs. */
static JSValue finish_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    DWaAnimation *a = argc > 0 ? wa_state(argv[0]) : NULL;
    if (a != NULL && a->notify_queued) finish_notify(ctx, a);
    return JS_UNDEFINED;
}

/* § 4.4.12, Updating the finished state. After a seek the unconstrained
 * current time is the current time itself; otherwise the time the start
 * time gives, the hold time set aside. */
static void update_finished(JSContext *ctx, DWaAnimation *a, bool did_seek, bool sync)
{
    double unconstrained = did_seek ? current_time(a)
                         : isnan(a->start) ? NAN : (wa_now(a->dom) - a->start) * a->rate;
    if (!isnan(unconstrained) && !isnan(a->start) && !a->pending_play && !a->pending_pause) {
        double end = effect_end(a);
        if (a->rate > 0 && unconstrained >= end) {
            a->held = did_seek ? unconstrained : (isnan(a->previous) ? end : fmax(a->previous, end));
        } else if (a->rate < 0 && unconstrained <= 0) {
            a->held = did_seek ? unconstrained : (isnan(a->previous) ? 0 : fmin(a->previous, 0));
        } else if (a->rate != 0) {
            /* Inside its range and moving: the hold goes, whether or not
             * this was a seek (a finished animation whose rate turned it
             * round plays on from its start time); after a seek the start
             * time is moved to where the hold was. */
            if (did_seek && !isnan(a->held)) a->start = wa_now(a->dom) - a->held / a->rate;
            a->held = NAN;
        }
    }
    a->previous = current_time(a);
    bool finished = play_state(a) == WA_FINISHED;
    /* Becoming finished makes it replaceable, and may make it replace. */
    if (finished && !a->was_finished) a->dom->wa_replace_check = true;
    a->was_finished = finished;
    if (finished && !a->finished_done) {
        if (sync) {
            finish_notify(ctx, a);
        } else if (!a->notify_queued) {
            a->notify_queued = true;
            JSValue arg = a->target.value;
            if (JS_EnqueueJob(ctx, finish_job, 1, &arg) < 0) a->notify_queued = false;
        }
    }
    if (!finished && a->finished_done) {
        if (promise_new(ctx, &a->finished, &a->finished_resolve, &a->finished_reject) == 0)
            a->finished_done = false;
    }
    wa_changed(a);
}

/* § 4.4.4, Setting the current time, silently: TypeError for an unresolved time while the current
 * time is resolved. */
static int silently_set(JSContext *ctx, DWaAnimation *a, double seek)
{
    if (isnan(seek)) {
        if (!isnan(current_time(a))) {
            JS_ThrowTypeError(ctx, "An animation's current time cannot be made unresolved");
            return -1;
        }
        return 0;
    }
    if (!isnan(a->held) || isnan(a->start) || a->rate == 0) a->held = seek;
    else a->start = wa_now(a->dom) - seek / a->rate;
    a->previous = NAN;
    return 0;
}

/* § 4.4.4, Setting the current time: and a pause pending completes at
 * once (the sign-in page's pause() then currentTime = 440,
 * WEB_ANIMATIONS.md § 2). */
static int set_current(JSContext *ctx, DWaAnimation *a, double seek)
{
    if (silently_set(ctx, a, seek) < 0) return -1;
    if (a->pending_pause) {
        a->held = seek;
        apply_pending_rate(a);
        a->start = NAN;
        a->pending_pause = false;
        resolve_ready(ctx, a);
    }
    update_finished(ctx, a, true, false);
    return 0;
}

/* § 4.4.5, Setting the start time. */
static void set_start(JSContext *ctx, DWaAnimation *a, double start)
{
    double previous = current_time(a);
    apply_pending_rate(a);
    a->start = start;
    if (!isnan(start)) {
        if (a->rate != 0) a->held = NAN;
    } else {
        a->held = previous;
    }
    if (a->pending_play || a->pending_pause) {
        a->pending_play = a->pending_pause = false;
        resolve_ready(ctx, a);
    }
    update_finished(ctx, a, true, false);
}

/* § 4.4.8, Playing an animation. -1 with InvalidStateError for a backwards run with no end. */
static int play(JSContext *ctx, DWaAnimation *a, bool auto_rewind)
{
    bool aborted_pause = a->pending_pause, has_ready = false;
    double seek = NAN, rate = effective_rate(a), end = effect_end(a), t = current_time(a);
    if (rate > 0 && auto_rewind && (isnan(t) || t < 0 || t >= end)) {
        seek = 0;
    } else if (rate < 0 && auto_rewind && (isnan(t) || t <= 0 || t > end)) {
        if (isinf(end)) {
            d_error(ctx, "InvalidStateError", "An animation with no end cannot play backwards from it");
            return -1;
        }
        seek = end;
    } else if (rate == 0 && isnan(t)) {
        seek = 0;
    }
    if (!isnan(seek)) a->held = seek;
    if (!isnan(a->held)) a->start = NAN;
    if (a->pending_play || a->pending_pause) {
        a->pending_play = a->pending_pause = false;
        has_ready = true;
    }
    if (isnan(a->held) && !aborted_pause && !a->has_pending_rate) return 0;
    if (!has_ready && ready_pending(ctx, a) < 0) return -1;
    a->pending_play = true;
    update_finished(ctx, a, false, false);
    return 0;
}

/* § 4.4.9, Pausing an animation. */
static int pause(JSContext *ctx, DWaAnimation *a)
{
    if (a->pending_pause || play_state(a) == WA_PAUSED) return 0;
    bool has_ready = false;
    double seek = NAN;
    if (isnan(current_time(a))) {
        if (effective_rate(a) >= 0) {
            seek = 0;
        } else {
            double end = effect_end(a);
            if (isinf(end)) {
                d_error(ctx, "InvalidStateError", "An animation with no end cannot be paused at it");
                return -1;
            }
            seek = end;
        }
    }
    if (!isnan(seek)) a->held = seek;
    if (a->pending_play) {
        a->pending_play = false;
        has_ready = true;
    }
    if (!has_ready && ready_pending(ctx, a) < 0) return -1;
    a->pending_pause = true;
    update_finished(ctx, a, false, false);
    return 0;
}

/* The pending tasks, run at the update step with the timeline's time as
 * the ready time (§ 4.4.8's pending play task and § 4.4.9's pending pause task). */
static void run_pending(JSContext *ctx, DWaAnimation *a, double ready)
{
    if (a->pending_play) {
        a->pending_play = false;
        if (!isnan(a->held)) {
            apply_pending_rate(a);
            a->start = a->rate == 0 ? ready : ready - a->held / a->rate;
            if (a->rate != 0) a->held = NAN;
        } else if (!isnan(a->start) && a->has_pending_rate) {
            double t = (ready - a->start) * a->rate;
            apply_pending_rate(a);
            if (a->rate == 0) a->held = t;
            else a->start = ready - t / a->rate;
        }
        resolve_ready(ctx, a);
        update_finished(ctx, a, false, false);
    } else if (a->pending_pause) {
        a->pending_pause = false;
        if (!isnan(a->start) && isnan(a->held)) a->held = (ready - a->start) * a->rate;
        apply_pending_rate(a);
        a->start = NAN;
        resolve_ready(ctx, a);
        update_finished(ctx, a, false, false);
    }
}

/* § 4.4.13, Finishing an animation. */
static int finish(JSContext *ctx, DWaAnimation *a)
{
    double rate = effective_rate(a), end = effect_end(a);
    if (rate == 0 || (rate > 0 && isinf(end))) {
        d_error(ctx, "InvalidStateError", rate == 0 ? "An animation with a playback rate of 0 cannot finish"
                                                    : "An animation with no end cannot finish");
        return -1;
    }
    apply_pending_rate(a);
    double limit = a->rate > 0 ? end : 0;
    if (silently_set(ctx, a, limit) < 0) return -1;
    if (isnan(a->start)) a->start = wa_now(a->dom) - limit / a->rate;
    if (a->pending_pause && !isnan(a->start)) {
        a->held = NAN;
        a->pending_pause = false;
        resolve_ready(ctx, a);
    }
    if (a->pending_play && !isnan(a->start)) {
        a->pending_play = false;
        resolve_ready(ctx, a);
    }
    update_finished(ctx, a, true, true);
    return 0;
}

/* § 4.4.14, Canceling an animation, with its reset of the pending tasks. */
static int cancel(JSContext *ctx, DWaAnimation *a)
{
    if (play_state(a) != WA_IDLE) {
        if (a->pending_play || a->pending_pause) {
            a->pending_play = a->pending_pause = false;
            apply_pending_rate(a);
            JSValue reason = abort_error(ctx);
            promise_call(ctx, a->ready_reject, reason);
            mark_handled(a->dom, ctx, a->ready);
            JS_FreeValue(ctx, reason);
            JSValue resolved = JS_UNDEFINED, res = JS_UNDEFINED, rej = JS_UNDEFINED;
            if (promise_new(ctx, &resolved, &res, &rej) < 0) return -1;
            promise_call(ctx, res, a->target.value);
            free3(ctx, &a->ready, &a->ready_resolve, &a->ready_reject);
            a->ready = resolved;
            a->ready_resolve = res;
            a->ready_reject = rej;
            a->ready_done = true;
        }
        if (!a->finished_done) {
            JSValue reason = abort_error(ctx);
            promise_call(ctx, a->finished_reject, reason);
            mark_handled(a->dom, ctx, a->finished);
            JS_FreeValue(ctx, reason);
        }
        if (promise_new(ctx, &a->finished, &a->finished_resolve, &a->finished_reject) < 0) return -1;
        a->finished_done = false;
        a->notify_queued = false;
        queue_event(ctx, a, "cancel", NAN, wa_now(a->dom), wa_now(a->dom));
    }
    a->held = NAN;
    a->start = NAN;
    a->previous = NAN;
    wa_changed(a);
    return 0;
}

/* § 4.4.15.1, Setting the playback rate: the playbackRate setter. */
static int set_rate(JSContext *ctx, DWaAnimation *a, double rate)
{
    a->has_pending_rate = false;
    double previous = current_time(a);
    a->rate = rate;
    if (!isnan(previous)) return set_current(ctx, a, previous);
    wa_changed(a);
    return 0;
}

/* § 4.4.15.2, Seamlessly updating the playback rate. */
static int update_rate(JSContext *ctx, DWaAnimation *a, double rate)
{
    WaState was = play_state(a);
    a->pending_rate = rate;
    a->has_pending_rate = true;
    if (a->pending_play || a->pending_pause) return 0;
    if (was == WA_IDLE || was == WA_PAUSED) {
        apply_pending_rate(a);
        wa_changed(a);
    } else if (was == WA_FINISHED) {
        double unconstrained = isnan(a->start) ? NAN : (wa_now(a->dom) - a->start) * a->rate;
        if (rate == 0) a->start = wa_now(a->dom);
        else if (!isnan(unconstrained)) a->start = wa_now(a->dom) - unconstrained / rate;
        apply_pending_rate(a);
        update_finished(ctx, a, false, false);
    } else {
        return play(ctx, a, false);
    }
    return 0;
}

/* § 4.4.16, Reversing an animation. */
static int reverse(JSContext *ctx, DWaAnimation *a)
{
    bool had = a->has_pending_rate;
    double original = a->pending_rate;
    a->pending_rate = -effective_rate(a);
    a->has_pending_rate = true;
    if (play(ctx, a, true) < 0) {
        a->has_pending_rate = had;
        a->pending_rate = original;
        return -1;
    }
    return 0;
}

/* ── The effect's data ───────────────────────────────────────────────── */

static void data_free(os64_dom_t *dom, DWaData *d)
{
    if (d == NULL) return;
    for (int32_t i = 0; i < d->nparsed; i++) garb_free(&d->parsed[i]);
    for (int32_t i = 0; d->kept != NULL && i < d->fx.nframes; i++) d_free(dom, d->kept[i].text);
    d_free(dom, d->kept);
    d_free(dom, d->props);
    d_free(dom, d->parsed);
    d_free(dom, d->frames);
    d_free(dom, d->sets);
    d_free(dom, d);
}

/* An effect's data it lets go of, kept while a tree laid out before now may
 * read it (os64_dom_effects_release). */
static void data_bury(os64_dom_t *dom, DWaData *d)
{
    if (d == NULL) return;
    d->died = dom->cluster ? dom->cluster->effect_generation : dom->wa_generation;
    d->next = dom->wa_dead;
    dom->wa_dead = d;
}

/* ── Processing a keyframes argument (§ 6.6.3) ───────────────────────── */

/* One keyframe while it is read: its offset (NAN when it has none), where
 * a property-indexed object put it (`place`, which orders and merges
 * them, and is no offset the page gave), its easing, and its declarations
 * as `name:value` texts, each ended by a NUL and parsed on its own
 * (data_make), so that a value that does not parse cannot take another
 * with it. */
typedef struct {
    double offset, place, given;    /* `given`: the offset as the page gave it, NAN for none */
    bool has_easing;
    garb_easing_t easing;
    WaComposite composite;
    char *text;
    size_t len, cap;
} WaFrame;

typedef struct {
    WaFrame *v;
    int32_t n, cap;
} WaFrames;

static void frames_free(JSContext *ctx, WaFrames *f)
{
    for (int32_t i = 0; i < f->n; i++) js_free(ctx, f->v[i].text);
    js_free(ctx, f->v);
    f->v = NULL;
    f->n = f->cap = 0;
}

static WaFrame *frames_add(JSContext *ctx, WaFrames *f)
{
    if (f->n >= WA_KEYFRAMES_MAX) {
        JS_ThrowRangeError(ctx, "Too many keyframes");
        return NULL;
    }
    if (f->n == f->cap) {
        int32_t cap = f->cap ? f->cap * 2 : 4;
        WaFrame *grown = js_realloc(ctx, f->v, (size_t)cap * sizeof(*grown));
        if (grown == NULL) return NULL;
        f->v = grown;
        f->cap = cap;
    }
    WaFrame *k = &f->v[f->n++];
    os64_memset(k, 0, sizeof(*k));
    k->offset = k->place = k->given = NAN;
    k->composite = WA_COMPOSITE_AUTO;
    return k;
}

/* An IDL attribute name (`backgroundColor`, `cssFloat`) as its property's
 * (§ 6.6.2's "animation property name to IDL attribute name", reversed);
 * false for a name that is none: one with a hyphen, which no camel-cased
 * name has, and a custom property, which a script effect does not animate
 * yet. */
static bool css_name(const char *idl, char *out, size_t cap)
{
    if (os64_streq(idl, "cssFloat")) return os64_strcopy(out, cap, "float") < cap;
    if (os64_streq(idl, "cssOffset")) return os64_strcopy(out, cap, "offset") < cap;
    size_t n = 0;
    for (const char *c = idl; *c != '\0'; c++) {
        if (*c == '-') return false;
        bool upper = *c >= 'A' && *c <= 'Z';
        if (n + 3 >= cap) return false;
        if (upper) out[n++] = '-';
        out[n++] = upper ? (char)(*c - 'A' + 'a') : *c;
    }
    out[n] = '\0';
    return n > 0;
}

/* Whether a value read as text can stand alone as a declaration's value:
 * no `;`, `{`, `}` or `!` outside a string and no NUL, so nothing it holds
 * ends its declaration, makes it important, or ends its text early. One
 * that cannot is dropped, as an invalid value is. */
static bool value_safe(const char *v, size_t len)
{
    char quote = 0;
    for (size_t i = 0; i < len; i++) {
        char c = v[i];
        if (c == '\0') return false;
        if (quote != 0) {
            if (c == '\\' && i + 1 < len) i++;
            else if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == ';' || c == '{' || c == '}' || c == '!') {
            return false;
        }
    }
    return quote == 0;
}

static int frame_append(JSContext *ctx, WaFrame *k, const char *name, const char *value, size_t vlen)
{
    if (!value_safe(value, vlen)) return 0;
    size_t nlen = os64_strlen(name), need = k->len + nlen + vlen + 4;
    if (need > k->cap) {
        size_t cap = need * 2;
        char *grown = js_realloc(ctx, k->text, cap);
        if (grown == NULL) return -1;
        k->text = grown;
        k->cap = cap;
    }
    os64_memcpy(k->text + k->len, name, nlen);
    k->len += nlen;
    k->text[k->len++] = ':';
    os64_memcpy(k->text + k->len, value, vlen);
    k->len += vlen;
    k->text[k->len++] = '\0';
    return 0;
}

/* An easing as a script writes it ("ease-in", "cubic-bezier(...)"): read
 * as animation-timing-function's value is, one item of it. TypeError for
 * any other text ("parse an easing", § 6.6.3). */
static int easing_of(JSContext *ctx, JSValueConst v, garb_easing_t *out)
{
    size_t len;
    const char *text = JS_ToCStringLen(ctx, &len, v);
    if (text == NULL) return -1;
    static const char kPrefix[] = "animation-timing-function:";
    char buf[512];
    bool ok = false;
    if (len + sizeof(kPrefix) < sizeof(buf) && value_safe(text, len)) {
        os64_snprintf(buf, sizeof(buf), "%s%.*s", kPrefix, (int)len, text);
        garb_parsed_t p;
        os64_memset(&p, 0, sizeof(p));
        if (garb_parse_block(buf, os64_strlen(buf), &p) == GARB_OK && p.nitems == 1 &&
            p.items[0].kind == GARB_ITEM_DECL) {
            garb_set_t set[2];
            if (garb_read_declaration(&p, &p.items[0].decl, (garb_reading_t){0}, set) == 1) {
                const garb_val_t *e = set[0].value.kind == GARB_V_LAYERS
                                          ? (set[0].value.nitems == 1 ? &set[0].value.items[0] : NULL)
                                          : &set[0].value;
                ok = e != NULL && garb_easing_of(e, out);
            }
        }
        garb_free(&p);
    }
    JS_FreeCString(ctx, text);
    if (!ok) {
        JS_ThrowTypeError(ctx, "Not an easing");
        return -1;
    }
    return 0;
}

/* An offset member: NAN for null or undefined; TypeError outside [0, 1]. */
static int offset_of(JSContext *ctx, JSValueConst v, double *out)
{
    *out = NAN;
    if (JS_IsNull(v) || JS_IsUndefined(v)) return 0;
    double d;
    if (JS_ToFloat64(ctx, &d, v) < 0) return -1;
    if (!(d >= 0 && d <= 1)) {
        JS_ThrowTypeError(ctx, "A keyframe's offset is from 0 to 1");
        return -1;
    }
    *out = d;
    return 0;
}

static bool member_is(const char *name)
{
    return os64_streq(name, "offset") || os64_streq(name, "easing") || os64_streq(name, "composite");
}

/* Whether `v` is iterable (has Symbol.iterator), the test that tells a
 * sequence of keyframes from a property-indexed object. -1 on a throw. */
static int iterable(os64_dom_t *dom, JSContext *ctx, JSValueConst v)
{
    JSAtom key = JS_ValueToAtom(ctx, dom->iterator_symbol->value);
    if (key == JS_ATOM_NULL) return -1;
    JSValue method = JS_GetProperty(ctx, v, key);
    JS_FreeAtom(ctx, key);
    if (JS_IsException(method)) return -1;
    int r = !JS_IsUndefined(method) && !JS_IsNull(method);
    JS_FreeValue(ctx, method);
    return r;
}

static int enum_of(JSContext *ctx, JSValueConst v, const char *const *words, int n, int *out);

/* A keyframe's `composite` (CompositeOperationOrAuto), kept for
 * getKeyframes; TypeError for a word that is none of them. */
static int composite_read(JSContext *ctx, JSValueConst v, WaComposite *out)
{
    int w = WA_COMPOSITE_AUTO;
    if (!JS_IsUndefined(v) && enum_of(ctx, v, kComposites, 4, &w) < 0) return -1;
    *out = (WaComposite)w;
    return 0;
}

/* The properties of a keyframe-like object that animate: its own
 * enumerable string-keyed ones that are no member and name a property
 * libgarb reads (css_name, garb_prop_known), in ascending order of their
 * names, the order § 6.6.3 reads
 * them in. Each atom is the caller's to free (props_free). */
typedef struct {
    JSAtom atom;
    char idl[128], css[128];
} WaProp;

static void props_free(JSContext *ctx, WaProp *v, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) JS_FreeAtom(ctx, v[i].atom);
    js_free(ctx, v);
}

static int animation_props(JSContext *ctx, JSValueConst obj, WaProp **out, uint32_t *count)
{
    *out = NULL;
    *count = 0;
    JSPropertyEnum *props;
    uint32_t nprops;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) return -1;
    WaProp *v = nprops > 0 ? js_malloc(ctx, (size_t)nprops * sizeof(*v)) : NULL;
    int r = nprops > 0 && v == NULL ? -1 : 0;
    uint32_t n = 0;
    for (uint32_t i = 0; r == 0 && i < nprops && n < WA_PROPERTIES_MAX; i++) {
        const char *name = JS_AtomToCString(ctx, props[i].atom);
        if (name == NULL) {
            r = -1;
            break;
        }
        WaProp *p = &v[n];
        /* Only a property libgarb reads: anything else on the object is
         * the page's own (metadata, a symbol, a getter), never read. */
        bool custom = name[0] == '-' && name[1] == '-' && name[2] != '\0';
        if (!member_is(name) && os64_strcopy(p->idl, sizeof(p->idl), name) < sizeof(p->idl) &&
            (custom ? os64_strcopy(p->css, sizeof(p->css), name) < sizeof(p->css)
                    : css_name(name, p->css, sizeof(p->css)) && garb_prop_known(p->css, os64_strlen(p->css), false))) {
            p->atom = JS_DupAtom(ctx, props[i].atom);
            /* By name, as they come. */
            for (uint32_t j = n; j > 0 && os64_strcmp(v[j - 1].idl, v[j].idl) > 0; j--) {
                WaProp t = v[j];
                v[j] = v[j - 1];
                v[j - 1] = t;
            }
            n++;
        }
        JS_FreeCString(ctx, name);
    }
    for (uint32_t i = 0; i < nprops; i++) JS_FreeAtom(ctx, props[i].atom);
    js_free(ctx, props);
    if (r < 0) {
        props_free(ctx, v, n);
        return -1;
    }
    *out = v;
    *count = n;
    return 0;
}

/* One keyframe-like object of a sequence (a BaseKeyframe, null and
 * undefined an empty one): its members in WebIDL's order — composite,
 * easing, offset — then its properties. */
static int read_keyframe(JSContext *ctx, JSValueConst obj, WaFrames *out)
{
    bool empty = JS_IsNull(obj) || JS_IsUndefined(obj);
    if (!empty && !JS_IsObject(obj)) {
        JS_ThrowTypeError(ctx, "A keyframe is an object");
        return -1;
    }
    if (frames_add(ctx, out) == NULL) return -1;
    if (empty) return 0;
    JSValue v = JS_GetPropertyStr(ctx, obj, "composite");
    int r = JS_IsException(v) ? -1 : composite_read(ctx, v, &out->v[out->n - 1].composite);
    JS_FreeValue(ctx, v);
    if (r < 0) return -1;
    v = JS_GetPropertyStr(ctx, obj, "easing");
    if (JS_IsException(v)) r = -1;
    else if (!JS_IsUndefined(v)) {
        garb_easing_t e;
        r = easing_of(ctx, v, &e);
        if (r == 0) {
            out->v[out->n - 1].easing = e;
            out->v[out->n - 1].has_easing = true;
        }
    }
    JS_FreeValue(ctx, v);
    if (r < 0) return -1;
    v = JS_GetPropertyStr(ctx, obj, "offset");
    if (JS_IsException(v)) return -1;
    double offset;
    r = offset_of(ctx, v, &offset);
    JS_FreeValue(ctx, v);
    if (r < 0) return -1;
    out->v[out->n - 1].offset = offset;
    WaProp *props;
    uint32_t nprops;
    if (animation_props(ctx, obj, &props, &nprops) < 0) return -1;
    for (uint32_t i = 0; r == 0 && i < nprops; i++) {
        JSValue pv = JS_GetProperty(ctx, obj, props[i].atom);
        size_t len;
        const char *text = JS_IsException(pv) ? NULL : JS_ToCStringLen(ctx, &len, pv);
        if (text == NULL) r = -1;
        else {
            r = frame_append(ctx, &out->v[out->n - 1], props[i].css, text, len);
            JS_FreeCString(ctx, text);
        }
        JS_FreeValue(ctx, pv);
    }
    props_free(ctx, props, nprops);
    return r;
}

/* A member of a property-indexed object as a list: a sequence's items, or
 * the one value. */
static int as_list(os64_dom_t *dom, JSContext *ctx, JSValueConst v, JSValue **items, size_t *n)
{
    int it = JS_IsObject(v) ? iterable(dom, ctx, v) : 0;
    if (it < 0) return -1;
    if (it) return d_sequence(dom, ctx, v, WA_KEYFRAMES_MAX, items, n);
    *items = js_malloc(ctx, sizeof(**items));
    if (*items == NULL) return -1;
    (*items)[0] = JS_DupValue(ctx, v);
    *n = 1;
    return 0;
}

/* The keyframe at place `at` among `out`'s, made when there is none. */
static WaFrame *frame_at(JSContext *ctx, WaFrames *out, double at)
{
    for (int32_t i = 0; i < out->n; i++)
        if (out->v[i].place == at) return &out->v[i];
    WaFrame *k = frames_add(ctx, out);
    if (k != NULL) k->place = at;
    return k;
}

/* A property-indexed object (a BasePropertyIndexedKeyframe): its members
 * first, in WebIDL's order — composite, easing, offset — each a value or a
 * list, every composite and easing given checked whether or not a keyframe
 * takes it; then each property's values placed evenly from 0 to 1 (one
 * value alone at 1), keyframes at one place merged and sorted by place;
 * then the offsets laid over them in order (a place is no offset: one the
 * page left out is spaced later, process_keyframes) and the easings
 * repeated. */
static int read_indexed(os64_dom_t *dom, JSContext *ctx, JSValueConst obj, WaFrames *out)
{
    static const char *const kMembers[] = {"composite", "easing", "offset"};
    JSValue *items[3] = {NULL, NULL, NULL};
    size_t n[3] = {0, 0, 0};
    garb_easing_t *easings = NULL;
    WaComposite *composites = NULL;
    double *offsets = NULL;
    int r = 0;
    for (int m = 0; r == 0 && m < 3; m++) {
        JSValue mv = JS_GetPropertyStr(ctx, obj, kMembers[m]);
        if (JS_IsException(mv)) r = -1;
        else if (!JS_IsUndefined(mv)) r = as_list(dom, ctx, mv, &items[m], &n[m]);
        JS_FreeValue(ctx, mv);
    }
    if (r == 0 && n[0] > 0) {
        composites = js_malloc(ctx, n[0] * sizeof(*composites));
        if (composites == NULL) r = -1;
        for (size_t i = 0; r == 0 && i < n[0]; i++) r = composite_read(ctx, items[0][i], &composites[i]);
    }
    if (r == 0 && n[1] > 0) {
        easings = js_malloc(ctx, n[1] * sizeof(*easings));
        if (easings == NULL) r = -1;
        for (size_t i = 0; r == 0 && i < n[1]; i++) r = easing_of(ctx, items[1][i], &easings[i]);
    }
    if (r == 0 && n[2] > 0) {
        offsets = js_malloc(ctx, n[2] * sizeof(*offsets));
        if (offsets == NULL) r = -1;
        for (size_t i = 0; r == 0 && i < n[2]; i++) r = offset_of(ctx, items[2][i], &offsets[i]);
    }
    for (int m = 0; m < 3; m++)
        if (items[m] != NULL) d_sequence_free(ctx, items[m], n[m]);
    WaProp *props = NULL;
    uint32_t nprops = 0;
    if (r == 0) r = animation_props(ctx, obj, &props, &nprops);
    for (uint32_t i = 0; r == 0 && i < nprops; i++) {
        JSValue pv = JS_GetProperty(ctx, obj, props[i].atom);
        JSValue *values = NULL;
        size_t nv = 0;
        r = JS_IsException(pv) ? -1 : as_list(dom, ctx, pv, &values, &nv);
        for (size_t j = 0; r == 0 && j < nv; j++) {
            double at = nv == 1 ? 1 : (double)j / (double)(nv - 1);
            WaFrame *k = frame_at(ctx, out, at);
            size_t len;
            const char *text = k != NULL ? JS_ToCStringLen(ctx, &len, values[j]) : NULL;
            if (text == NULL) r = -1;
            else {
                r = frame_append(ctx, k, props[i].css, text, len);
                JS_FreeCString(ctx, text);
            }
        }
        if (values != NULL) d_sequence_free(ctx, values, nv);
        JS_FreeValue(ctx, pv);
    }
    props_free(ctx, props, nprops);
    if (r == 0) {
        /* By place, keeping the order merged keyframes were made in. */
        for (int32_t i = 1; i < out->n; i++)
            for (int32_t j = i; j > 0 && out->v[j - 1].place > out->v[j].place; j--) {
                WaFrame t = out->v[j];
                out->v[j] = out->v[j - 1];
                out->v[j - 1] = t;
            }
        for (int32_t i = 0; i < out->n && (size_t)i < n[2]; i++) out->v[i].offset = offsets[i];
        for (int32_t i = 0; i < out->n && n[1] > 0; i++) {
            out->v[i].easing = easings[(size_t)i % n[1]];
            out->v[i].has_easing = true;
        }
        for (int32_t i = 0; i < out->n && n[0] > 0; i++) out->v[i].composite = composites[(size_t)i % n[0]];
    }
    js_free(ctx, easings);
    js_free(ctx, composites);
    js_free(ctx, offsets);
    return r;
}

/* § 6.6.3: the keyframes, in order, each with an offset: a lone keyframe
 * without one at 1; of several, the first at 0 and the last at 1 when they
 * have none, and the others missing one spaced evenly between the offsets
 * either side. TypeError for offsets that fall. */
static int process_keyframes(os64_dom_t *dom, JSContext *ctx, JSValueConst arg, WaFrames *out)
{
    if (JS_IsNull(arg) || JS_IsUndefined(arg)) return 0;
    if (!JS_IsObject(arg)) {
        JS_ThrowTypeError(ctx, "Keyframes are an object or a sequence of them");
        return -1;
    }
    int it = iterable(dom, ctx, arg);
    if (it < 0) return -1;
    if (it) {
        JSValue *items;
        size_t n;
        if (d_sequence(dom, ctx, arg, WA_KEYFRAMES_MAX, &items, &n) < 0) return -1;
        int r = 0;
        for (size_t i = 0; r == 0 && i < n; i++) r = read_keyframe(ctx, items[i], out);
        d_sequence_free(ctx, items, n);
        if (r < 0) return -1;
    } else if (read_indexed(dom, ctx, arg, out) < 0) {
        return -1;
    }
    double last = -1;
    for (int32_t i = 0; i < out->n; i++) out->v[i].given = out->v[i].offset;
    for (int32_t i = 0; i < out->n; i++)
        if (!isnan(out->v[i].offset)) {
            if (out->v[i].offset < last) {
                JS_ThrowTypeError(ctx, "Keyframe offsets go from 0 to 1 in order");
                return -1;
            }
            last = out->v[i].offset;
        }
    if (out->n == 1 && isnan(out->v[0].offset)) out->v[0].offset = 1;
    if (out->n > 1) {
        if (isnan(out->v[0].offset)) out->v[0].offset = 0;
        if (isnan(out->v[out->n - 1].offset)) out->v[out->n - 1].offset = 1;
    }
    for (int32_t i = 0; i < out->n;) {
        int32_t j = i + 1;
        while (j < out->n && isnan(out->v[j].offset)) j++;
        for (int32_t k = i + 1; k < j && j < out->n; k++)
            out->v[k].offset = out->v[i].offset +
                               (out->v[j].offset - out->v[i].offset) * (double)(k - i) / (double)(j - i);
        i = j;
    }
    return 0;
}

/* The frames read into the data libflow takes: each keyframe's
 * declarations, each read on its own as a declaration block into
 * longhands, so one that does not parse drops itself alone, and each
 * longhand once, from the declaration that set the fewest. A value
 * holding var() is held by libgarb (GARB_DECL_HELD) and dropped
 * (WEB_ANIMATIONS.md § 2, DEBTS). */
/* Whether a declaration's text holds var(), which libgarb holds for the
 * cascade to substitute (GARB_DECL_HELD) rather than reading it. */
static bool holds_var(const char *text)
{
    for (const char *c = text; *c != '\0'; c++)
        if ((c[0] == 'v' || c[0] == 'V') && (c[1] == 'a' || c[1] == 'A') && (c[2] == 'r' || c[2] == 'R') &&
            c[3] == '(')
            return true;
    return false;
}

static DWaData *data_make(os64_dom_t *dom, JSContext *ctx, const WaFrames *f)
{
    DWaData *d = d_alloc(dom, sizeof(*d));
    if (d == NULL) return NULL;
    os64_memset(d, 0, sizeof(*d));
    int32_t n = f->n;
    size_t pieces = 0;
    for (int32_t i = 0; i < n; i++)
        for (size_t at = 0; at < f->v[i].len; at++) pieces += f->v[i].text[at] == '\0';
    d->frames = n > 0 ? d_alloc(dom, (size_t)n * sizeof(*d->frames)) : NULL;
    d->parsed = pieces > 0 ? d_alloc(dom, pieces * sizeof(*d->parsed)) : NULL;
    if ((n > 0 && d->frames == NULL) || (pieces > 0 && d->parsed == NULL)) {
        data_free(dom, d);
        return NULL;
    }
    if (pieces > 0) os64_memset(d->parsed, 0, pieces * sizeof(*d->parsed));
    size_t nsets = 0, capsets = 0, piece_at = 0;
    int32_t *ranks = NULL;     /* each set's declaration's longhand count */
    size_t *firsts = n > 0 ? js_malloc(ctx, (size_t)n * 2 * sizeof(size_t)) : NULL;
    /* Which declarations getKeyframes gives back: each that parsed, a
     * custom property's and one libgarb holds for its var() included,
     * though neither is animated (DEBTS). */
    bool *accepted = pieces > 0 ? js_mallocz(ctx, pieces * sizeof(*accepted)) : NULL;
    if ((n > 0 && firsts == NULL) || (pieces > 0 && accepted == NULL)) {
        js_free(ctx, firsts);
        data_free(dom, d);
        return NULL;
    }
    for (int32_t i = 0; i < n; i++) {
        firsts[2 * i] = nsets;
        for (size_t at = 0; at < f->v[i].len;) {
            const char *piece = f->v[i].text + at;
            size_t len = os64_strlen(piece);
            at += len + 1;
            size_t this_piece = piece_at++;
            garb_parsed_t *p = &d->parsed[d->nparsed];
            if (garb_parse_block(piece, len, p) != GARB_OK) {
                garb_free(p);
                os64_memset(p, 0, sizeof(*p));
                continue;
            }
            d->nparsed++;
            for (int32_t j = 0; j < p->nitems; j++) {
                if (p->items[j].kind != GARB_ITEM_DECL) continue;
                if (garb_decl_is_custom(&p->items[j].decl)) {
                    accepted[this_piece] = true;
                    continue;
                }
                garb_set_t sets[64];
                int32_t k = garb_read_declaration(p, &p->items[j].decl, (garb_reading_t){0}, sets);
                if (k > 0 || holds_var(piece)) accepted[this_piece] = true;
                for (int32_t s = 0; s < k; s++) {
                    /* A longhand two declarations of this keyframe set
                     * (§ 5.3.3): the one that set fewer longhands wins — a
                     * longhand given itself over any shorthand, a narrower
                     * shorthand over a wider — and of two alike, the first
                     * by name, which is the order they are read in. `k`
                     * is how many this declaration set. */
                    size_t had = firsts[2 * i];
                    while (had < nsets && d->sets[had].prop != sets[s].prop) had++;
                    if (had < nsets) {
                        if (k < ranks[had]) {
                            d->sets[had] = sets[s];
                            ranks[had] = k;
                        }
                        continue;
                    }
                    if (nsets == capsets) {
                        size_t cap = capsets ? capsets * 2 : 16;
                        garb_set_t *grown = d_alloc(dom, cap * sizeof(*grown));
                        int32_t *ranked = js_realloc(ctx, ranks, cap * sizeof(*ranks));
                        if (ranked != NULL) ranks = ranked;
                        if (grown == NULL || ranked == NULL) {
                            d_free(dom, grown);
                            js_free(ctx, ranks);
                            js_free(ctx, firsts);
                            js_free(ctx, accepted);
                            data_free(dom, d);
                            return NULL;
                        }
                        if (nsets > 0) os64_memcpy(grown, d->sets, nsets * sizeof(*grown));
                        d_free(dom, d->sets);
                        d->sets = grown;
                        capsets = cap;
                    }
                    ranks[nsets] = k;
                    d->sets[nsets++] = sets[s];
                }
            }
        }
        firsts[2 * i + 1] = nsets;
    }
    for (int32_t i = 0; i < n; i++) {
        garb_effect_keyframe_t *k = &d->frames[i];
        k->offset = f->v[i].offset;
        k->has_easing = f->v[i].has_easing;
        k->easing = f->v[i].easing;
        k->sets = d->sets != NULL ? d->sets + firsts[2 * i] : NULL;
        k->nsets = (int32_t)(firsts[2 * i + 1] - firsts[2 * i]);
    }
    js_free(ctx, ranks);
    js_free(ctx, firsts);
    d->fx.frames = d->frames;
    d->fx.nframes = n;
    /* What the page gave, as it gave it, and the properties once each. */
    d->kept = n > 0 ? d_alloc(dom, (size_t)n * sizeof(*d->kept)) : NULL;
    d->props = nsets > 0 ? d_alloc(dom, nsets * sizeof(*d->props)) : NULL;
    if ((n > 0 && d->kept == NULL) || (nsets > 0 && d->props == NULL)) {
        d_free(dom, d->kept);
        d->kept = NULL;
        js_free(ctx, accepted);
        data_free(dom, d);
        return NULL;
    }
    if (n > 0) os64_memset(d->kept, 0, (size_t)n * sizeof(*d->kept));
    piece_at = 0;
    for (int32_t i = 0; i < n; i++) {
        d->kept[i].given = f->v[i].given;
        d->kept[i].composite = f->v[i].composite;
        size_t keep = 0;
        for (size_t at = 0, pi = piece_at; at < f->v[i].len; pi++) {
            size_t len = os64_strlen(f->v[i].text + at) + 1;
            if (accepted[pi]) keep += len;
            at += len;
        }
        d->kept[i].text = keep > 0 ? d_alloc(dom, keep) : NULL;
        if (keep > 0 && d->kept[i].text == NULL) {
            js_free(ctx, accepted);
            data_free(dom, d);
            return NULL;
        }
        for (size_t at = 0; at < f->v[i].len; piece_at++) {
            size_t len = os64_strlen(f->v[i].text + at) + 1;
            if (accepted[piece_at]) {
                os64_memcpy(d->kept[i].text + d->kept[i].len, f->v[i].text + at, len);
                d->kept[i].len += len;
            }
            at += len;
        }
    }
    js_free(ctx, accepted);
    for (size_t i = 0; i < nsets; i++) {
        int32_t j = 0;
        while (j < d->nprops && d->props[j] != d->sets[i].prop) j++;
        if (j == d->nprops) d->props[d->nprops++] = d->sets[i].prop;
    }
    return d;
}

/* A keyframes argument processed into the data libflow takes; NULL on a
 * throw. */
static DWaData *keyframes_data(os64_dom_t *dom, JSContext *ctx, JSValueConst arg)
{
    WaFrames frames = {0};
    if (process_keyframes(dom, ctx, arg, &frames) < 0) {
        frames_free(ctx, &frames);
        return NULL;
    }
    DWaData *d = data_make(dom, ctx, &frames);
    frames_free(ctx, &frames);
    if (d == NULL) d_error(ctx, "QuotaExceededError", "No room for an animation's keyframes");
    return d;
}

/* A copy of `from`'s keyframes, as the page gave them (a KeyframeEffect
 * made from another), its timing beside them; NULL on a throw. */
static DWaData *data_copy(os64_dom_t *dom, JSContext *ctx, const DWaData *from)
{
    WaFrames frames = {0};
    for (int32_t i = 0; i < from->fx.nframes; i++) {
        WaFrame *k = frames_add(ctx, &frames);
        char *text = k != NULL && from->kept[i].len > 0 ? js_malloc(ctx, from->kept[i].len) : NULL;
        if (k == NULL || (from->kept[i].len > 0 && text == NULL)) {
            frames_free(ctx, &frames);
            return NULL;
        }
        k->offset = from->frames[i].offset;
        k->given = from->kept[i].given;
        k->has_easing = from->frames[i].has_easing;
        k->easing = from->frames[i].easing;
        k->composite = from->kept[i].composite;
        if (text != NULL) os64_memcpy(text, from->kept[i].text, from->kept[i].len);
        k->text = text;
        k->len = k->cap = from->kept[i].len;
    }
    DWaData *d = data_make(dom, ctx, &frames);
    frames_free(ctx, &frames);
    if (d == NULL) {
        d_error(ctx, "QuotaExceededError", "No room for an animation's keyframes");
        return NULL;
    }
    d->fx.timing = from->fx.timing;
    return d;
}

/* ── Timing (§ 6.5.1 EffectTiming, KeyframeAnimationOptions) ───────────── */

static int enum_of(JSContext *ctx, JSValueConst v, const char *const *words, int n, int *out)
{
    const char *text = JS_ToCString(ctx, v);
    if (text == NULL) return -1;
    int found = -1;
    for (int i = 0; i < n; i++)
        if (os64_streq(text, words[i])) found = i;
    JS_FreeCString(ctx, text);
    if (found < 0) {
        JS_ThrowTypeError(ctx, "Not one of the values this member takes");
        return -1;
    }
    *out = found;
    return 0;
}

static int finite_of(JSContext *ctx, JSValueConst v, double *out, bool nonnegative)
{
    if (JS_ToFloat64(ctx, out, v) < 0) return -1;
    if (!isfinite(*out) || (nonnegative && *out < 0)) {
        JS_ThrowTypeError(ctx, nonnegative ? "A non-negative finite number" : "A finite number");
        return -1;
    }
    return 0;
}

/* Whether `s` names a pseudo-element a selector can (CSS Pseudo-Elements
 * 4; the four CSS 2 kept their single colon): any other text is a
 * SyntaxError for pseudoElement, a known one NotSupportedError, as a
 * pseudo-element's animations are not run (WEB_ANIMATIONS.md § 4). */
static bool pseudo_known(const char *s)
{
    static const char *const kOld[] = {"before", "after", "first-line", "first-letter"};
    static const char *const kNew[] = {"marker", "placeholder", "selection", "backdrop", "file-selector-button",
                                       "target-text", "spelling-error", "grammar-error"};
    if (s[0] != ':') return false;
    bool doubled = s[1] == ':';
    const char *name = s + (doubled ? 2 : 1);
    for (size_t i = 0; i < sizeof(kOld) / sizeof(kOld[0]); i++)
        if (os64_streq(name, kOld[i])) return true;
    for (size_t i = 0; doubled && i < sizeof(kNew) / sizeof(kNew[0]); i++)
        if (os64_streq(name, kNew[i])) return true;
    return false;
}

/* `options`: a number is the duration; a dictionary its members, read in
 * WebIDL's order — an inherited dictionary's first, each dictionary's in
 * name order: EffectTiming's, KeyframeEffectOptions' composite,
 * iterationComposite and pseudoElement, then KeyframeAnimationOptions' id. `id` is that id (NULL for none),
 * js_malloc'd. */
/* What a page's timing says that garb_timing_t does not keep, and the
 * options beside the timing: `fill: auto` and `duration: auto` (getTiming
 * gives them back), the effect's composite operations, and the id. */
typedef struct {
    bool fill_auto, duration_auto;
    WaComposite composite, iteration_composite;
    char *id;
} WaSpec;

/* Which dictionary a timing argument converts to: animate()'s
 * KeyframeAnimationOptions, the KeyframeEffect constructor's
 * KeyframeEffectOptions, or updateTiming()'s OptionalEffectTiming, whose
 * members left out keep what `t` and `spec` hold. */
typedef enum { WA_TIMING_ANIMATE, WA_TIMING_EFFECT, WA_TIMING_UPDATE } WaTimingFor;

static void timing_default(garb_timing_t *t, WaSpec *spec)
{
    os64_memset(t, 0, sizeof(*t));
    t->iterations = 1;
    os64_memset(spec, 0, sizeof(*spec));
    spec->fill_auto = spec->duration_auto = true;
    spec->composite = spec->iteration_composite = WA_COMPOSITE_REPLACE;
}

/* The members in WebIDL's order, an inherited dictionary's first and each
 * dictionary's by name; all read and checked before any is kept, so a
 * throw leaves `t` and `spec` as they were. */
static int timing_of(JSContext *ctx, JSValueConst options, garb_timing_t *t, WaSpec *spec, WaTimingFor what)
{
    garb_timing_t nt = *t;
    WaSpec ns = *spec;
    ns.id = NULL;
    if (JS_IsUndefined(options) || (what == WA_TIMING_UPDATE && JS_IsNull(options))) return 0;
    if (!JS_IsObject(options)) {
        if (what == WA_TIMING_UPDATE) {
            JS_ThrowTypeError(ctx, "updateTiming takes an object");
            return -1;
        }
        double d;
        if (JS_ToFloat64(ctx, &d, options) < 0) return -1;
        if (isnan(d) || d < 0) {
            JS_ThrowTypeError(ctx, "A duration is a non-negative number");
            return -1;
        }
        t->duration_ms = d;
        spec->duration_auto = false;
        return 0;
    }
    static const char *const kDirections[] = {"normal", "reverse", "alternate", "alternate-reverse"};
    static const char *const kFills[] = {"none", "forwards", "backwards", "both", "auto"};
    static const char *const kMembers[] = {"delay", "direction", "duration", "easing", "endDelay", "fill",
                                           "iterationStart", "iterations", "composite", "iterationComposite",
                                           "pseudoElement", "id"};
    int members = what == WA_TIMING_UPDATE ? 8 : what == WA_TIMING_EFFECT ? 11 : 12;
    for (int m = 0; m < members; m++) {
        JSValue v = JS_GetPropertyStr(ctx, options, kMembers[m]);
        if (JS_IsException(v)) {
            js_free(ctx, ns.id);
            return -1;
        }
        int r = 0, w;
        if (!JS_IsUndefined(v)) {
            switch (m) {
            case 0: r = finite_of(ctx, v, &nt.delay_ms, false); break;
            case 1:
                r = enum_of(ctx, v, kDirections, 4, &w);
                if (r == 0) nt.direction = (garb_direction_t)w;
                break;
            case 2:
                if (JS_IsString(v)) {
                    const char *str = JS_ToCString(ctx, v);
                    r = str != NULL && os64_streq(str, "auto") ? 0 : -1;
                    if (str != NULL) JS_FreeCString(ctx, str);
                    if (r < 0) JS_ThrowTypeError(ctx, "A duration is a number or auto");
                    else {
                        nt.duration_ms = 0;
                        ns.duration_auto = true;
                    }
                } else {
                    r = JS_ToFloat64(ctx, &nt.duration_ms, v);
                    if (r == 0 && (isnan(nt.duration_ms) || nt.duration_ms < 0)) {
                        JS_ThrowTypeError(ctx, "A duration is a non-negative number");
                        r = -1;
                    }
                    ns.duration_auto = false;
                }
                break;
            case 3: r = easing_of(ctx, v, &nt.easing); break;
            case 4: r = finite_of(ctx, v, &nt.end_delay_ms, false); break;
            case 5:
                r = enum_of(ctx, v, kFills, 5, &w);
                /* `auto` is none for a keyframe effect (§ 4.6.1, Fill modes). */
                if (r == 0) {
                    nt.fill = w == 4 ? GARB_FILL_NONE : (garb_fill_t)w;
                    ns.fill_auto = w == 4;
                }
                break;
            case 6: r = finite_of(ctx, v, &nt.iteration_start, true); break;
            case 7:
                r = JS_ToFloat64(ctx, &nt.iterations, v);
                if (r == 0 && (isnan(nt.iterations) || nt.iterations < 0)) {
                    JS_ThrowTypeError(ctx, "Iterations are a non-negative number");
                    r = -1;
                }
                break;
            case 8:
                r = enum_of(ctx, v, kComposites, 3, &w);
                if (r == 0) ns.composite = (WaComposite)w;
                break;
            case 9:
                r = enum_of(ctx, v, (const char *const[]){"replace", "accumulate"}, 2, &w);
                if (r == 0) ns.iteration_composite = w == 0 ? WA_COMPOSITE_REPLACE : WA_COMPOSITE_ACCUMULATE;
                break;
            case 10:
                if (!JS_IsNull(v)) {
                    const char *str = JS_ToCString(ctx, v);
                    if (str == NULL) {
                        r = -1;
                        break;
                    }
                    bool known = pseudo_known(str);
                    JS_FreeCString(ctx, str);
                    d_error(ctx, known ? "NotSupportedError" : "SyntaxError",
                            known ? "A pseudo-element's animations are not run here" : "Not a pseudo-element");
                    r = -1;
                }
                break;
            case 11: {
                size_t len;
                const char *str = JS_ToCStringLen(ctx, &len, v);
                if (str == NULL) r = -1;
                else {
                    ns.id = js_malloc(ctx, len + 1);
                    if (ns.id != NULL) os64_memcpy(ns.id, str, len + 1);
                    JS_FreeCString(ctx, str);
                    r = ns.id != NULL ? 0 : -1;
                }
                break;
            }
            }
        }
        JS_FreeValue(ctx, v);
        if (r < 0) {
            js_free(ctx, ns.id);
            return -1;
        }
    }
    *t = nt;
    *spec = ns;
    return 0;
}

/* ── The objects ─────────────────────────────────────────────────────── */

static void effect_unlink(os64_dom_t *dom, DWaEffect *e)
{
    if (e->prev != NULL) e->prev->next = e->next;
    else dom->wa_fx_first = e->next;
    if (e->next != NULL) e->next->prev = e->prev;
    e->prev = e->next = NULL;
}

/* An effect lets go of what it holds: its data buried (a tree may still
 * read it) and its target released. */
static void effect_let_go(os64_dom_t *dom, DWaEffect *e)
{
    data_bury(dom, e->data);
    e->data = NULL;
    if (e->element != NULL) os64_html_release(dom->document, e->element);
    e->element = NULL;
}

/* The composite each keyframe hands libflow: its own, or the effect's
 * where it said auto; and the effect's iterationComposite. WaComposite's
 * first three are garb_composite_t's, in order. */
static void effect_composites(DWaEffect *e)
{
    if (e->data == NULL) return;
    DWaData *d = e->data;
    for (int32_t i = 0; i < d->fx.nframes; i++) {
        WaComposite c = d->kept[i].composite == WA_COMPOSITE_AUTO ? e->composite : d->kept[i].composite;
        d->frames[i].composite = (garb_composite_t)c;
    }
    d->fx.iteration_accumulate = e->iteration_composite == WA_COMPOSITE_ACCUMULATE;
}

static void effect_finalizer(JSRuntime *rt, JSValue value)
{
    DWaEffect *e = fx_state(value);
    if (e == NULL) return;
    if (e->dom != NULL) {
        effect_unlink(e->dom, e);
        effect_let_go(e->dom, e);
    }
    js_free_rt(rt, e);
}

/* A new effect, its target held and its data taken, on `prototype` (a
 * subclass's, from a constructor's new.target) or the class's; an
 * exception on a throw, the data freed. */
static JSValue effect_new(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *element, DWaData *data,
                          const WaSpec *spec, JSValueConst prototype)
{
    DWaEffect *e = js_mallocz(ctx, sizeof(*e));
    JSClassID id = os64_js_class_id(&effect_slot);
    JSValue object = e == NULL ? JS_EXCEPTION
                   : JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id)
                                            : JS_NewObjectClass(ctx, (int)id);
    if (JS_IsException(object)) {
        js_free(ctx, e);
        data_free(dom, data);
        return JS_EXCEPTION;
    }
    e->dom = dom;
    e->data = data;
    e->fill_auto = spec->fill_auto;
    e->duration_auto = spec->duration_auto;
    e->composite = spec->composite;
    e->iteration_composite = spec->iteration_composite;
    effect_composites(e);
    if (element != NULL) {
        os64_html_hold(dom->document, element);
        e->element = element;
    }
    e->next = dom->wa_fx_first;
    if (e->next != NULL) e->next->prev = e;
    dom->wa_fx_first = e;
    JS_SetOpaque(object, e);
    return object;
}

static void unlink_animation(os64_dom_t *dom, DWaAnimation *a)
{
    if (a->prev != NULL) a->prev->next = a->next;
    else dom->wa_first = a->next;
    if (a->next != NULL) a->next->prev = a->prev;
    else dom->wa_last = a->prev;
    a->prev = a->next = NULL;
    dom->wa_count--;
    index_dirty(dom);
}

/* An animation goes with the garbage once nothing holds it: neither a
 * script nor the binding, which holds one while it is live (wa_keep). Its
 * effect is an object of its own, which a script may hold on after it. */
static void animation_finalizer(JSRuntime *rt, JSValue value)
{
    DWaAnimation *a = wa_state(value);
    if (a == NULL) return;
    if (a->dom != NULL) unlink_animation(a->dom, a);
    d_event_target_finalize(rt, &a->target);
    JS_FreeValueRT(rt, a->effect);
    JS_FreeValueRT(rt, a->hold);
    JS_FreeValueRT(rt, a->ready);
    JS_FreeValueRT(rt, a->ready_resolve);
    JS_FreeValueRT(rt, a->ready_reject);
    JS_FreeValueRT(rt, a->finished);
    JS_FreeValueRT(rt, a->finished_resolve);
    JS_FreeValueRT(rt, a->finished_reject);
    js_free_rt(rt, a->id);
    js_free_rt(rt, a->css_name);
    js_free_rt(rt, a);
}

static void animation_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DWaAnimation *a = wa_state(value);
    if (a == NULL) return;
    d_event_target_mark(rt, &a->target, mark);
    /* Not `hold`: the binding's hold is a reference from outside the
     * graph, which the cycle collector must count as one. */
    JS_MarkValue(rt, a->effect, mark);
    JS_MarkValue(rt, a->ready, mark);
    JS_MarkValue(rt, a->ready_resolve, mark);
    JS_MarkValue(rt, a->ready_reject, mark);
    JS_MarkValue(rt, a->finished, mark);
    JS_MarkValue(rt, a->finished_resolve, mark);
    JS_MarkValue(rt, a->finished_reject, mark);
}

/* The animation `e` is the effect of, or NULL. */
static DWaAnimation *effect_owner(DWaEffect *e)
{
    if (e == NULL || e->owner == 0 || e->dom == NULL) return NULL;
    size_t n = e->dom->cluster ? e->dom->cluster->count : 1;
    for (size_t i = 0; i < n; ++i)
    for (DWaAnimation *a = (e->dom->cluster ? e->dom->cluster->members[i] : e->dom)->wa_first; a; a = a->next)
        if (a->serial == e->owner) return wa_fx(a) == e ? a : NULL;
    return NULL;
}

static void update_finished(JSContext *ctx, DWaAnimation *a, bool did_seek, bool sync);

/* § 4.4.2, Setting the associated effect of an animation: `effect` (null
 * or a KeyframeEffect) taken from the animation it was the effect of, the
 * old one's target and the new one's restyled, and the finished state
 * updated. */
static void set_effect(JSContext *ctx, DWaAnimation *a, JSValueConst effect)
{
    DWaEffect *e = fx_state(effect), *old = wa_fx(a);
    if (e == old) return;
    DWaAnimation *previous = effect_owner(e);
    if (previous != NULL) set_effect(ctx, previous, JS_NULL);
    if (old != NULL) {
        old->owner = 0;
        if (old->element != NULL) (void)os64_html_touch(d_owner(a->dom, old->element)->document, (os64_html_node_t *)old->element);
    }
    JSValue was = a->effect;
    a->effect = e != NULL ? JS_DupValue(ctx, effect) : JS_NULL;
    JS_FreeValue(ctx, was);
    if (e != NULL) e->owner = a->serial;
    update_finished(ctx, a, false, false);
    wa_restyle(a);
}

/* A new animation of `effect` (null for none), idle, on `prototype` or
 * the class's: the caller plays it. */
static JSValue animation_new(os64_dom_t *dom, JSContext *ctx, JSValueConst effect, char *id,
                             JSValueConst prototype)
{
    JSClassID cid = os64_js_class_id(&animation_slot);
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, cid)
                                            : JS_NewObjectClass(ctx, (int)cid);
    DWaAnimation *a = JS_IsException(object) ? NULL : js_mallocz(ctx, sizeof(*a));
    if (a == NULL) {
        JS_FreeValue(ctx, object);
        js_free(ctx, id);
        return JS_EXCEPTION;
    }
    d_event_target_init(dom, &a->target, object);
    a->dom = dom;
    a->id = id;
    a->start = a->held = a->previous = NAN;
    a->rate = 1;
    a->effect = JS_NULL;
    a->hold = JS_UNDEFINED;
    a->ready = a->ready_resolve = a->ready_reject = JS_UNDEFINED;
    a->finished = a->finished_resolve = a->finished_reject = JS_UNDEFINED;
    JS_SetOpaque(object, a);
    a->serial = dom->cluster ? ++dom->cluster->animation_serial : ++dom->wa_serial;
    a->prev = dom->wa_last;
    if (dom->wa_last != NULL) dom->wa_last->next = a;
    else dom->wa_first = a;
    dom->wa_last = a;
    dom->wa_count++;
    index_dirty(dom);
    /* A new animation's ready promise is resolved, its finished one
     * pending (an animation's initial state, § 4.4.7 and § 4.4.11). */
    if (promise_new(ctx, &a->ready, &a->ready_resolve, &a->ready_reject) < 0 ||
        promise_new(ctx, &a->finished, &a->finished_resolve, &a->finished_reject) < 0) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    promise_call(ctx, a->ready_resolve, object);
    a->ready_done = true;
    if (fx_state(effect) != NULL) set_effect(ctx, a, effect);
    return object;
}

/* Whether an animation may still change what is drawn or fire: one with a
 * pending task, one running (on its way to its finish, which a listener may
 * hear, even through a stretch where it is not relevant: reversed into its
 * delay), relevant, or a CSSAnimation whose CSS animation lives. */
static bool wa_live(DWaAnimation *a)
{
    /* A CSSAnimation is kept while its CSS animation is, so a script that
     * asks again gets the same object. */
    if (a->css_id != 0) return true;
    /* With no timeline nothing moves it but a script: it is kept while it
     * shows (relevant), and never for a task it cannot run. */
    if (a->no_timeline) return a->replace != WA_REMOVED && wa_relevant(a);
    if (a->pending_play || a->pending_pause || play_state(a) == WA_RUNNING) return true;
    return a->replace != WA_REMOVED && wa_relevant(a);
}

/* The binding holds a live animation (wa_live), so that nothing a page
 * left unreferenced stops it short; the update step lets go of one that is
 * not, after the step's own walk of them is done. */
static void wa_keep(JSContext *ctx, DWaAnimation *a)
{
    if (JS_IsUndefined(a->hold) && wa_live(a))
        a->hold = JS_DupValue(ctx, a->target.value);
}

/* A time a script reads: null when unresolved, and never -0, which the
 * arithmetic makes (a time of 0 on a backwards clock) and no browser
 * answers. */
static JSValue time_value(JSContext *ctx, double t)
{
    return isnan(t) ? JS_NULL : JS_NewFloat64(ctx, t + 0.0);
}

/* A `double?` member: NAN for null. */
static int time_arg(JSContext *ctx, JSValueConst v, double *out)
{
    if (JS_IsNull(v)) {
        *out = NAN;
        return 0;
    }
    if (JS_ToFloat64(ctx, out, v) < 0) return -1;
    if (!isfinite(*out)) {
        JS_ThrowTypeError(ctx, "A time is a finite number or null");
        return -1;
    }
    return 0;
}

/* ── commitStyles (Web Animations 1's commitStyles(), its steps) ──────── */

/* Each property the effect animates written into its target's style
 * attribute as its value now: NoModificationAllowedError with no target,
 * InvalidStateError for a target that is not rendered (target_rendered).
 * The value is the target's computed one, which is the
 * effect stack's result with every animation in it: the spec's is the
 * stack up to and including this one (DEBTS). */
/* Whether `target` has a box (HTML's "being rendered"): in the document,
 * neither it nor an ancestor `display: none`, and not itself `display:
 * contents`, which makes its children's boxes and none of its own. -1 on
 * a throw. */
static int target_rendered(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *target)
{
    dom = d_owner(dom, target);
    if (!d_in_window_tree(dom, target)) return 0;
    for (const os64_html_node_t *n = target; n != NULL && n->kind == OS64_HTML_ELEMENT; n = n->parent) {
        JSValue display = d_computed_property(dom, ctx, n, GARB_DISPLAY, "none");
        if (JS_IsException(display)) return -1;
        const char *word = JS_ToCString(ctx, display);
        JS_FreeValue(ctx, display);
        if (word == NULL) return -1;
        bool boxless = os64_streq(word, "none") || (n == target && os64_streq(word, "contents"));
        JS_FreeCString(ctx, word);
        if (boxless) return 0;
    }
    return 1;
}

static int commit_styles(JSContext *ctx, DWaAnimation *a)
{
    os64_dom_t *dom = a->dom;
    const os64_html_node_t *target = wa_target(a);
    DWaData *d = wa_data(a);
    if (target == NULL || d == NULL) {
        d_error(ctx, "NoModificationAllowedError", "An animation with no target has nothing to commit");
        return -1;
    }
    int rendered = target_rendered(dom, ctx, target);
    if (rendered < 0) return -1;
    if (!rendered) {
        d_error(ctx, "InvalidStateError", "The animation's target is not rendered");
        return -1;
    }
    for (int32_t i = 0; i < d->nprops; i++) {
        JSValue v = d_computed_property(dom, ctx, target, d->props[i], "");
        if (JS_IsException(v)) return -1;
        const char *text = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (text == NULL) return -1;
        JSValue done = text[0] != '\0' ? d_style_write(dom, ctx, target, garb_prop_name(d->props[i]), text)
                                       : JS_UNDEFINED;
        JS_FreeCString(ctx, text);
        if (JS_IsException(done)) return -1;
        JS_FreeValue(ctx, done);
    }
    return 0;
}

/* ── KeyframeEffect's members (§ 6.6) ────────────────────────────────── */

/* An easing as Chrome writes it, which is how libflow's computed style
 * writes one: a keyword when it was one, else the function. */
static void easing_text(const garb_easing_t *e, char *out, size_t cap)
{
    if (e->kind == GARB_EASE_LINEAR) {
        os64_strcopy(out, cap, "linear");
        return;
    }
    char n[4][32];
    if (e->kind == GARB_EASE_CUBIC) {
        static const struct { double p[4]; const char *name; } kNamed[] = {
            {{0.25, 0.1, 0.25, 1}, "ease"}, {{0.42, 0, 1, 1}, "ease-in"},
            {{0, 0, 0.58, 1}, "ease-out"}, {{0.42, 0, 0.58, 1}, "ease-in-out"},
        };
        for (size_t k = 0; e->named && k < sizeof(kNamed) / sizeof(kNamed[0]); k++)
            if (os64_memcmp(kNamed[k].p, e->p, sizeof(e->p)) == 0) {
                os64_strcopy(out, cap, kNamed[k].name);
                return;
            }
        for (int k = 0; k < 4; k++) garb_css_number(e->p[k], n[k], sizeof(n[k]));
        os64_snprintf(out, cap, "cubic-bezier(%s, %s, %s, %s)", n[0], n[1], n[2], n[3]);
        return;
    }
    static const char *const kJumps[] = {"end", "start", "jump-none", "jump-both"};
    if (e->jump == GARB_JUMP_END) os64_snprintf(out, cap, "steps(%d)", (int)e->steps);
    else os64_snprintf(out, cap, "steps(%d, %s)", (int)e->steps, kJumps[e->jump]);
}

static const char *const kDirectionWords[] = {"normal", "reverse", "alternate", "alternate-reverse"};
static const char *const kFillWords[] = {"none", "forwards", "backwards", "both", "auto"};

static int put_number(JSContext *ctx, JSValue o, const char *name, double v)
{
    return JS_DefinePropertyValueStr(ctx, o, name, JS_NewFloat64(ctx, v + 0.0), JS_PROP_C_W_E);
}

static int put_string(JSContext *ctx, JSValue o, const char *name, const char *v)
{
    return JS_DefinePropertyValueStr(ctx, o, name, JS_NewString(ctx, v), JS_PROP_C_W_E);
}

/* The EffectTiming members both getTiming and getComputedTiming answer;
 * `computed` resolves `auto`, as getComputedTiming does. */
static int put_timing(JSContext *ctx, JSValue o, const DWaEffect *e, bool computed)
{
    const garb_timing_t *t = &e->data->fx.timing;
    char easing[160];
    easing_text(&t->easing, easing, sizeof(easing));
    int r = put_number(ctx, o, "delay", t->delay_ms) | put_number(ctx, o, "endDelay", t->end_delay_ms) |
            put_string(ctx, o, "fill", kFillWords[e->fill_auto && !computed ? 4 : (int)t->fill]) |
            put_number(ctx, o, "iterationStart", t->iteration_start) |
            put_number(ctx, o, "iterations", t->iterations) |
            put_string(ctx, o, "direction", kDirectionWords[t->direction]) | put_string(ctx, o, "easing", easing);
    if (e->duration_auto && !computed) r |= put_string(ctx, o, "duration", "auto");
    else r |= put_number(ctx, o, "duration", t->duration_ms);
    return r < 0 ? -1 : 0;
}

/* A property's name as a keyframe object writes it (§ 6.6.2, "animation
 * property name to IDL attribute name"): `float` as cssFloat, `offset` as
 * cssOffset, the rest camel-cased. */
static void idl_name(const char *css, size_t len, char *out, size_t cap)
{
    if (len >= 2 && css[0] == '-' && css[1] == '-') {
        os64_snprintf(out, cap, "%.*s", (int)len, css);
        return;
    }
    if (len == 5 && os64_memcmp(css, "float", 5) == 0) {
        os64_strcopy(out, cap, "cssFloat");
        return;
    }
    if (len == 6 && os64_memcmp(css, "offset", 6) == 0) {
        os64_strcopy(out, cap, "cssOffset");
        return;
    }
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < cap; i++) {
        if (css[i] == '-' && i + 1 < len) {
            char c = css[++i];
            out[n++] = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
        } else {
            out[n++] = css[i];
        }
    }
    out[n] = '\0';
}

/* getKeyframes (§ 6.6, KeyframeEffect): each keyframe as the page gave it — the offset
 * it gave or null, the one it was given, its easing, its composite, and
 * its properties' values as their texts. */
static JSValue get_keyframes(JSContext *ctx, const DWaEffect *e)
{
    JSValue list = JS_NewArray(ctx);
    if (JS_IsException(list) || e->data == NULL) return list;
    const DWaData *d = e->data;
    for (int32_t i = 0; i < d->fx.nframes; i++) {
        JSValue k = JS_NewObject(ctx);
        if (JS_IsException(k)) goto fail;
        char easing[160];
        if (d->frames[i].has_easing) easing_text(&d->frames[i].easing, easing, sizeof(easing));
        else os64_strcopy(easing, sizeof(easing), "linear");
        int r = JS_DefinePropertyValueStr(ctx, k, "offset",
                                          isnan(d->kept[i].given) ? JS_NULL : JS_NewFloat64(ctx, d->kept[i].given),
                                          JS_PROP_C_W_E) |
                put_number(ctx, k, "computedOffset", d->frames[i].offset) | put_string(ctx, k, "easing", easing) |
                put_string(ctx, k, "composite", kComposites[d->kept[i].composite]);
        for (size_t at = 0; r >= 0 && at < d->kept[i].len;) {
            const char *piece = d->kept[i].text + at;
            size_t len = os64_strlen(piece);
            at += len + 1;
            const char *colon = os64_strchr(piece, ':');
            if (colon == NULL) continue;
            char name[160];
            idl_name(piece, (size_t)(colon - piece), name, sizeof(name));
            r = JS_DefinePropertyValueStr(ctx, k, name, JS_NewStringLen(ctx, colon + 1, len - (size_t)(colon - piece) - 1),
                                          JS_PROP_C_W_E);
        }
        if (r < 0 || JS_SetPropertyUint32(ctx, list, (uint32_t)i, k) < 0) {
            if (r < 0) JS_FreeValue(ctx, k);
            goto fail;
        }
    }
    return list;
fail:
    JS_FreeValue(ctx, list);
    return JS_EXCEPTION;
}

/* getComputedTiming (§ 6.5, AnimationEffect): the timing with `auto` resolved, and where
 * the effect is at its animation's current time — its local time, null
 * with no animation or no time, and its progress and iteration, null
 * when it is not in effect. */
static JSValue computed_timing(JSContext *ctx, DWaEffect *e)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o) || e->data == NULL) return o;
    DWaAnimation *a = effect_owner(e);
    double local = a != NULL ? current_time(a) : NAN;
    garb_clock_t k = garb_clock_at(&e->data->fx.timing, isnan(local) ? 0 : local, a != NULL && a->rate < 0);
    bool shows = !isnan(local) && k.in_effect;
    int r = put_timing(ctx, o, e, true) | put_number(ctx, o, "startTime", 0) |
            put_number(ctx, o, "endTime", k.end_time) | put_number(ctx, o, "activeDuration", k.active_duration);
    r |= JS_DefinePropertyValueStr(ctx, o, "localTime", time_value(ctx, local), JS_PROP_C_W_E);
    r |= JS_DefinePropertyValueStr(ctx, o, "progress", shows ? JS_NewFloat64(ctx, k.progress + 0.0) : JS_NULL,
                                   JS_PROP_C_W_E);
    r |= JS_DefinePropertyValueStr(ctx, o, "currentIteration", shows ? JS_NewFloat64(ctx, k.iteration) : JS_NULL,
                                   JS_PROP_C_W_E);
    if (r < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

/* After the effect's keyframes, timing or target changed: its animation
 * restyled and, for its timing, its finished state updated (updateTiming's
 * steps, § 6.5). */
static void effect_changed(JSContext *ctx, DWaEffect *e, bool timing)
{
    DWaAnimation *a = effect_owner(e);
    if (a == NULL) return;
    if (timing) update_finished(ctx, a, false, false);
    wa_restyle(a);
    wa_keep(ctx, a);
}

enum { E_TARGET, E_PSEUDO, E_COMPOSITE, E_ITERATION_COMPOSITE, E_GET_KEYFRAMES, E_SET_KEYFRAMES, E_GET_TIMING,
       E_COMPUTED_TIMING, E_UPDATE_TIMING };

static JSValue effect_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                             JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    JSValueConst arg = argc > 0 ? argv[0] : JS_UNDEFINED;
    DWaEffect *e = fx_state(self);
    if (e == NULL) return JS_ThrowTypeError(ctx, "Expected a KeyframeEffect");
    if (e->dom == NULL || e->data == NULL) return JS_UNDEFINED;
    dom = e->dom;
    switch (magic) {
    case E_TARGET: {
        if (!set) return e->element != NULL ? d_wrap(dom, ctx, e->element) : JS_NULL;
        const os64_html_node_t *element = NULL;
        if (!JS_IsNull(arg)) {
            element = d_node(dom, ctx, arg);
            if (element == NULL || element->kind != OS64_HTML_ELEMENT)
                return JS_ThrowTypeError(ctx, "An effect's target is an element or null");
        }
        if (element == e->element) return JS_UNDEFINED;
        if (e->element != NULL) {
            (void)os64_html_touch(d_owner(dom, e->element)->document, (os64_html_node_t *)e->element);
            os64_html_release(dom->document, e->element);
        }
        if (element != NULL) os64_html_hold(dom->document, element);
        e->element = element;
        effect_changed(ctx, e, false);
        return JS_UNDEFINED;
    }
    case E_PSEUDO: {
        if (!set) return JS_NULL;
        if (JS_IsNull(arg)) return JS_UNDEFINED;
        const char *str = JS_ToCString(ctx, arg);
        if (str == NULL) return JS_EXCEPTION;
        bool known = pseudo_known(str);
        JS_FreeCString(ctx, str);
        return d_error(ctx, known ? "NotSupportedError" : "SyntaxError",
                       known ? "A pseudo-element's animations are not run here" : "Not a pseudo-element");
    }
    case E_COMPOSITE:
    case E_ITERATION_COMPOSITE: {
        WaComposite *field = magic == E_COMPOSITE ? &e->composite : &e->iteration_composite;
        if (!set) return JS_NewString(ctx, kComposites[*field]);
        int w;
        if (magic == E_COMPOSITE) {
            if (enum_of(ctx, arg, kComposites, 3, &w) < 0) return JS_EXCEPTION;
        } else {
            if (enum_of(ctx, arg, (const char *const[]){"replace", "accumulate"}, 2, &w) < 0) return JS_EXCEPTION;
            w = w == 0 ? WA_COMPOSITE_REPLACE : WA_COMPOSITE_ACCUMULATE;
        }
        *field = (WaComposite)w;
        effect_composites(e);
        effect_changed(ctx, e, false);
        return JS_UNDEFINED;
    }
    case E_GET_KEYFRAMES: return get_keyframes(ctx, e);
    case E_SET_KEYFRAMES: {
        if (argc < 1) return JS_ThrowTypeError(ctx, "setKeyframes needs keyframes");
        DWaData *d = keyframes_data(dom, ctx, arg);
        if (d == NULL) return JS_EXCEPTION;
        /* The new keyframes take the timing and the clock as they were; the
         * old ones are buried, as a tree laid out with them may read them. */
        d->fx.timing = e->data->fx.timing;
        d->fx.start_ms = e->data->fx.start_ms;
        d->fx.hold_ms = e->data->fx.hold_ms;
        d->fx.rate = e->data->fx.rate;
        d->fx.held = e->data->fx.held;
        data_bury(dom, e->data);
        e->data = d;
        effect_composites(e);
        effect_changed(ctx, e, false);
        return JS_UNDEFINED;
    }
    case E_GET_TIMING: {
        JSValue o = JS_NewObject(ctx);
        if (!JS_IsException(o) && put_timing(ctx, o, e, false) < 0) {
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
        return o;
    }
    case E_COMPUTED_TIMING: return computed_timing(ctx, e);
    case E_UPDATE_TIMING: {
        /* Into a copy: the members' getters are the page's, and may give
         * the effect new keyframes before the timing lands. */
        WaSpec spec = {.fill_auto = e->fill_auto, .duration_auto = e->duration_auto,
                       .composite = e->composite, .iteration_composite = e->iteration_composite};
        garb_timing_t timing = e->data->fx.timing;
        if (timing_of(ctx, arg, &timing, &spec, WA_TIMING_UPDATE) < 0) return JS_EXCEPTION;
        if (e->data == NULL) return JS_UNDEFINED;
        e->data->fx.timing = timing;
        e->fill_auto = spec.fill_auto;
        e->duration_auto = spec.duration_auto;
        effect_changed(ctx, e, true);
        return JS_UNDEFINED;
    }
    }
    return JS_UNDEFINED;
}

static JSValue effect_make(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv, JSValueConst prototype);

/* `new KeyframeEffect(target, keyframes, options)`, or `new
 * KeyframeEffect(source)`, a copy of another (§ 6.6): the options before
 * the keyframes, as WebIDL converts them first. */
static JSValue effect_construct(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSValue made = effect_make(dom, ctx, argc, argv, prototype);
    JS_FreeValue(ctx, prototype);
    return made;
}

static JSValue effect_make(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv, JSValueConst prototype)
{
    /* WebIDL's overloads: one argument is the copy, more the target's. */
    DWaEffect *source = argc == 1 ? fx_state(argv[0]) : NULL;
    if (argc == 1 && source == NULL) return JS_ThrowTypeError(ctx, "KeyframeEffect copies a KeyframeEffect");
    if (source != NULL) {
        if (source->data == NULL) return JS_ThrowTypeError(ctx, "That effect is gone");
        DWaData *d = data_copy(dom, ctx, source->data);
        if (d == NULL) return JS_EXCEPTION;
        WaSpec spec = {.fill_auto = source->fill_auto, .duration_auto = source->duration_auto,
                       .composite = source->composite, .iteration_composite = source->iteration_composite};
        return effect_new(dom, ctx, source->element, d, &spec, prototype);
    }
    if (argc < 2) return JS_ThrowTypeError(ctx, "KeyframeEffect needs a target and keyframes");
    const os64_html_node_t *element = NULL;
    if (!JS_IsNull(argv[0])) {
        element = d_node(dom, ctx, argv[0]);
        if (element == NULL || element->kind != OS64_HTML_ELEMENT)
            return JS_ThrowTypeError(ctx, "An effect's target is an element or null");
    }
    garb_timing_t timing;
    WaSpec spec;
    timing_default(&timing, &spec);
    if (timing_of(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, &timing, &spec, WA_TIMING_EFFECT) < 0)
        return JS_EXCEPTION;
    DWaData *d = keyframes_data(dom, ctx, argv[1]);
    if (d == NULL) return JS_EXCEPTION;
    d->fx.timing = timing;
    return effect_new(dom, ctx, element, d, &spec, prototype);
}

/* `new Animation(effect, timeline)` (§ 6.4, Animation): idle, of `effect` (null or
 * undefined for none), on the document's timeline unless `timeline` is
 * null; any other timeline is not one this binding has. */
static JSValue animation_construct(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValueConst effect = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (!JS_IsNull(effect) && !JS_IsUndefined(effect) && fx_state(effect) == NULL)
        return JS_ThrowTypeError(ctx, "An animation's effect is a KeyframeEffect or null");
    JSValueConst timeline = argc > 1 ? argv[1] : JS_UNDEFINED;
    bool none = JS_IsNull(timeline);
    if (!none && !JS_IsUndefined(timeline) && !JS_StrictEq(ctx, timeline, dom->wa_timeline->value))
        return JS_ThrowTypeError(ctx, "An animation's timeline is the document's or null");
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSValue animation = animation_new(dom, ctx, effect, NULL, prototype);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(animation)) return animation;
    wa_state(animation)->no_timeline = none;
    return animation;
}

enum {
    A_ID, A_EFFECT, A_TIMELINE, A_START, A_CURRENT, A_RATE, A_STATE, A_REPLACE, A_PENDING, A_READY, A_FINISHED,
    A_CANCEL, A_FINISH, A_PLAY, A_PAUSE, A_UPDATE_RATE, A_REVERSE, A_PERSIST, A_COMMIT, T_CURRENT, D_TIMELINE,
    A_CSS_NAME
};

static void css_take_over(DWaAnimation *a, int magic, bool set, bool was_paused);

static JSValue animation_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                                JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    JSValueConst arg = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (magic == T_CURRENT) {
        dom->wa_wanted = true;
        return JS_NewFloat64(ctx, wa_now(dom));
    }
    if (magic == D_TIMELINE) return JS_DupValue(ctx, dom->wa_timeline->value);
    DWaAnimation *a = wa_state(self);
    if (a == NULL) return JS_ThrowTypeError(ctx, "Expected an Animation");
    if (a->dom == NULL) return JS_UNDEFINED;
    dom = a->dom;
    bool was_paused = play_state(a) == WA_PAUSED;
    int r = 0;
    switch (magic) {
    case A_ID:
        if (set) {
            size_t len;
            const char *s = JS_ToCStringLen(ctx, &len, arg);
            if (s == NULL) return JS_EXCEPTION;
            char *copy = js_malloc(ctx, len + 1);
            if (copy != NULL) os64_memcpy(copy, s, len + 1);
            JS_FreeCString(ctx, s);
            if (copy == NULL) return JS_EXCEPTION;
            js_free(ctx, a->id);
            a->id = copy;
            return JS_UNDEFINED;
        }
        return JS_NewString(ctx, a->id != NULL ? a->id : "");
    case A_EFFECT:
        if (set) {
            if (!JS_IsNull(arg) && fx_state(arg) == NULL)
                return JS_ThrowTypeError(ctx, "An animation's effect is a KeyframeEffect or null");
            set_effect(ctx, a, arg);
            break;
        }
        return JS_DupValue(ctx, a->effect);
    case A_TIMELINE: return a->no_timeline ? JS_NULL : JS_DupValue(ctx, dom->wa_timeline->value);
    case A_START:
        if (set) {
            double t;
            if (time_arg(ctx, arg, &t) < 0) return JS_EXCEPTION;
            set_start(ctx, a, t);
            break;
        }
        return time_value(ctx, a->start);
    case A_CURRENT:
        if (set) {
            double t;
            if (time_arg(ctx, arg, &t) < 0) return JS_EXCEPTION;
            r = set_current(ctx, a, t);
            break;
        }
        return time_value(ctx, current_time(a));
    case A_RATE:
        if (set) {
            double rate;
            if (JS_ToFloat64(ctx, &rate, arg) < 0) return JS_EXCEPTION;
            if (!isfinite(rate)) return JS_ThrowTypeError(ctx, "A playback rate is a finite number");
            r = set_rate(ctx, a, rate);
            break;
        }
        return JS_NewFloat64(ctx, a->rate + 0.0);   /* reverse() at a rate of 0 makes -0 */
    case A_STATE: return JS_NewString(ctx, kStates[play_state(a)]);
    case A_CSS_NAME:
        if (!a->css) return JS_ThrowTypeError(ctx, "Expected a CSSAnimation");
        return JS_NewStringLen(ctx, a->css_name, a->css_name_len);
    case A_REPLACE:
        return JS_NewString(ctx, a->replace == WA_REMOVED ? "removed" : a->replace == WA_PERSISTED ? "persisted"
                                                                                                  : "active");
    case A_PERSIST:
        /* § 5.5: never removed, and a removed one's effect applies again. */
        if (a->replace != WA_PERSISTED) {
            a->replace = WA_PERSISTED;
            wa_restyle(a);
        }
        break;
    case A_COMMIT: r = commit_styles(ctx, a); break;
    case A_PENDING: return JS_NewBool(ctx, a->pending_play || a->pending_pause);
    case A_READY: return JS_DupValue(ctx, a->ready);
    case A_FINISHED: return JS_DupValue(ctx, a->finished);
    case A_CANCEL: r = cancel(ctx, a); break;
    case A_FINISH: r = finish(ctx, a); break;
    case A_PLAY: r = play(ctx, a, true); break;
    case A_PAUSE: r = pause(ctx, a); break;
    case A_UPDATE_RATE: {
        double rate;
        if (JS_ToFloat64(ctx, &rate, arg) < 0) return JS_EXCEPTION;
        if (!isfinite(rate)) return JS_ThrowTypeError(ctx, "A playback rate is a finite number");
        r = update_rate(ctx, a, rate);
        break;
    }
    case A_REVERSE: r = reverse(ctx, a); break;
    default: return JS_UNDEFINED;
    }
    if (r < 0) return JS_EXCEPTION;
    if (a->css) css_take_over(a, magic, set, was_paused);
    wa_keep(ctx, a);
    return JS_UNDEFINED;
}

/* ── Element.animate and getAnimations ───────────────────────────────── */

static JSValue animate_method(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                              JSValue *data)
{
    (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *element = d_node(dom, ctx, self);
    if (element == NULL || element->kind != OS64_HTML_ELEMENT)
        return JS_ThrowTypeError(ctx, "Element.animate needs an element");
    /* The options first: they are the call's argument to convert, where
     * the keyframes are an object the method's steps then process. */
    garb_timing_t timing;
    WaSpec spec;
    timing_default(&timing, &spec);
    if (timing_of(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &timing, &spec, WA_TIMING_ANIMATE) < 0)
        return JS_EXCEPTION;
    DWaData *d = keyframes_data(dom, ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (d == NULL) {
        js_free(ctx, spec.id);
        return JS_EXCEPTION;
    }
    d->fx.timing = timing;
    JSValue effect = effect_new(dom, ctx, element, d, &spec, JS_UNDEFINED);
    if (JS_IsException(effect)) {
        js_free(ctx, spec.id);
        return effect;
    }
    JSValue animation = animation_new(dom, ctx, effect, spec.id, JS_UNDEFINED);
    JS_FreeValue(ctx, effect);
    if (JS_IsException(animation)) return animation;
    DWaAnimation *a = wa_state(animation);
    if (play(ctx, a, true) < 0) {
        JS_FreeValue(ctx, animation);
        return JS_EXCEPTION;
    }
    wa_keep(ctx, a);
    return animation;
}

/* Whether `node` is `root` or inside it. */
static bool within(const os64_html_node_t *node, const os64_html_node_t *root)
{
    for (const os64_html_node_t *n = node; n != NULL; n = n->parent)
        if (n == root) return true;
    return false;
}

/* ── CSS animations (CSSAnimation; WEB_ANIMATIONS.md § 3) ─────────────── */

/* The host keeps the CSS animations (os64_dom_set_css_animations); a
 * script sees one as a CSSAnimation, made when getAnimations first answers
 * it and kept while the CSS animation lives (wa_live), so asking again
 * gives the same object. From then on the CSSAnimation's state machine is
 * its clock: every change is handed to the host (css_push, from
 * wa_changed), which draws it, and animation-play-state's changes reach it
 * as pause() and play() (css_pull; CSS Animations 2, the play state's
 * interaction with the API). Its effect is a KeyframeEffect with the
 * animation's target and timing and no keyframes of its own (DEBTS.md). */

/* A CSS animation's timing as libgarb reads it: no end delay, no
 * iteration start, a linear effect easing. */
static garb_timing_t css_timing_of(const os64_dom_css_animation_t *r)
{
    garb_timing_t t;
    os64_memset(&t, 0, sizeof(t));
    t.duration_ms = r->duration_ms;
    t.delay_ms = r->delay_ms;
    t.iterations = r->iterations;
    t.direction = (garb_direction_t)r->direction;
    t.fill = (garb_fill_t)r->fill;
    return t;
}

/* A CSS animation's local time at `now` on the host's clock: NAN for a
 * canceled one. */
static double css_local(bool paused, bool canceled, double start, double hold, double rate, double now)
{
    if (canceled) return NAN;
    return paused ? hold : (now - start) * rate;
}

/* The face's clock now, as the timeline's time was last sampled. */
static double css_face_now(os64_dom_t *dom)
{
    return wa_now(dom) + dom->wa_face;
}

/* Relevant as wa_relevant reads an animation: not canceled, and in effect,
 * active, or on its way to its active phase. */
static bool css_record_relevant(os64_dom_t *dom, const os64_dom_css_animation_t *r)
{
    if (r->canceled) return false;
    garb_timing_t t = css_timing_of(r);
    double local = css_local(r->paused, false, r->start_ms, r->hold_ms, r->rate, css_face_now(dom));
    garb_clock_t k = garb_clock_at(&t, local, r->rate < 0);
    if (k.in_effect || k.phase == GARB_PHASE_ACTIVE) return true;
    return r->rate > 0 ? k.phase == GARB_PHASE_BEFORE : r->rate < 0 && k.phase == GARB_PHASE_AFTER;
}

static DWaAnimation *css_find(os64_dom_t *dom, uint64_t id)
{
    for (DWaAnimation *a = dom->wa_first; a != NULL; a = a->next)
        if (a->css_id == id) return a;
    return NULL;
}

/* The clock goes to the host, as the CSS animation's: held while the hold
 * time is resolved, running from the start time at the rate, and canceled
 * while idle; and the target's style moves (os64_html_touch), so the host
 * lays it out again — a held clock moved would otherwise wait for
 * something else to draw it. */
static void css_push(DWaAnimation *a)
{
    os64_dom_t *dom = a->dom;
    if (a->css_id == 0 || dom == NULL || dom->wa_css.drive == NULL) return;
    bool paused = !isnan(a->held), canceled = !paused && isnan(a->start);
    double start = paused || canceled ? 0 : a->start + dom->wa_face;
    /* False only when the host's animation went: the next step finds that. */
    if (dom->wa_css.drive(dom->wa_css.opaque, a->css_id, paused, canceled, start, paused ? a->held : 0, a->rate) &&
        wa_target(a) != NULL)
        (void)os64_html_touch(d_owner(dom, wa_target(a))->document, (os64_html_node_t *)wa_target(a));
}

/* A procedure a CSS animation's style asked for, whose throw (a backwards
 * run with no end) no script is there to catch. */
static void css_quietly(JSContext *ctx, int r)
{
    if (r < 0) JS_FreeValue(ctx, JS_GetException(ctx));
}

/* Whether member `magic`, which succeeded, takes a CSSAnimation's play
 * state over from animation-play-state, whose changes it then answers no
 * more (CSS Animations 2; WPT's CSSAnimation-pausing, as Chrome and Gecko
 * do): play() and pause() always; reverse(), and a start time set, when
 * they move it between paused and not. */
static void css_take_over(DWaAnimation *a, int magic, bool set, bool was_paused)
{
    bool flipped = (play_state(a) == WA_PAUSED) != was_paused;
    if (magic == A_PLAY || magic == A_PAUSE || (magic == A_REVERSE && flipped) ||
        (magic == A_START && set && flipped))
        a->css_overridden = true;
}

/* What the style changed since the CSSAnimation last looked: its timing,
 * taken as the effect's, and animation-play-state, which pauses it or
 * plays it on without rewinding, until a script takes its play state over
 * (css_take_over). */
static void css_pull(JSContext *ctx, DWaAnimation *a, const os64_dom_css_animation_t *r)
{
    garb_timing_t t = css_timing_of(r);
    DWaData *d = wa_data(a);
    bool timing = d != NULL && os64_memcmp(&t, &a->css_timing, sizeof(t)) != 0;
    if (timing) {
        a->css_timing = t;
        d->fx.timing = t;
    }
    bool restyled = r->style_paused != a->css_style_paused;
    a->css_style_paused = r->style_paused;
    if (restyled && !a->css_overridden) {
        css_quietly(ctx, r->style_paused ? pause(ctx, a) : play(ctx, a, false));
    } else if (timing) {
        update_finished(ctx, a, false, false);
    }
}

/* The CSS animation went (its name left the list, its element went or
 * stopped making a box): its CSSAnimation is canceled and has no owning
 * element from now on. */
static void css_vanish(JSContext *ctx, DWaAnimation *a)
{
    css_quietly(ctx, cancel(ctx, a));
    a->css_id = 0;
}

/* The CSSAnimation of `r`, made on first sight with its clock as the host
 * holds it — one the page laid out since the timeline's time was sampled
 * waits for the next step to start, as a play does (its pending play
 * task) — and from then on driving the host's. NULL with an exception
 * pending. */
static DWaAnimation *css_wrap(os64_dom_t *dom, JSContext *ctx, const os64_dom_css_animation_t *r)
{
    WaFrames none = {0};
    DWaData *d = data_make(dom, ctx, &none);
    if (d == NULL) {
        d_error(ctx, "QuotaExceededError", "No room for a CSS animation's effect");
        return NULL;
    }
    garb_timing_t t = css_timing_of(r), unused;
    d->fx.timing = t;
    WaSpec spec;
    timing_default(&unused, &spec);
    spec.fill_auto = spec.duration_auto = false;
    JSValue effect = effect_new(dom, ctx, r->element, d, &spec, JS_UNDEFINED);
    if (JS_IsException(effect)) return NULL;
    char *name = js_malloc(ctx, r->name_len + 1);
    JSValue animation = name == NULL ? JS_EXCEPTION : animation_new(dom, ctx, JS_NULL, NULL, dom->wa_css_prototype->value);
    if (JS_IsException(animation)) {
        js_free(ctx, name);
        JS_FreeValue(ctx, effect);
        return NULL;
    }
    DWaAnimation *a = wa_state(animation);
    os64_memcpy(name, r->name, r->name_len);
    name[r->name_len] = '\0';
    a->css = true;
    a->css_id = r->id;
    a->css_name = name;
    a->css_name_len = r->name_len;
    a->css_style_paused = r->style_paused;
    a->css_timing = t;
    double now = css_face_now(dom);
    if (r->paused) {
        a->held = r->hold_ms;
    } else if (!r->canceled && r->start_ms > now) {
        a->held = 0;
        a->pending_play = true;
        if (ready_pending(ctx, a) < 0) {
            JS_FreeValue(ctx, effect);
            JS_FreeValue(ctx, animation);
            return NULL;
        }
    } else if (!r->canceled) {
        a->start = r->start_ms - dom->wa_face;
    }
    a->rate = r->rate;
    set_effect(ctx, a, effect);
    JS_FreeValue(ctx, effect);
    wa_keep(ctx, a);
    JS_FreeValue(ctx, animation);
    return a;
}

/* ── The CSS animation events (CSS Animations 2 § 4.2) ──────────────── */

static bool css_listening(os64_dom_t *dom)
{
    static const char *const kTypes[] = {"animationstart", "animationend", "animationiteration", "animationcancel"};
    for (size_t i = 0; i < sizeof(kTypes) / sizeof(kTypes[0]); i++)
        if (os64_dom_listens(dom, kTypes[i])) return true;
    return false;
}

static void seen_drop(os64_dom_t *dom, size_t i)
{
    DWaSeen *s = &dom->wa_seen[i];
    os64_html_release(dom->document, s->element);
    d_free(dom, s->name);
    dom->wa_nseen--;
    if (i < dom->wa_nseen) os64_memmove(&dom->wa_seen[i], &dom->wa_seen[i + 1], (dom->wa_nseen - i) * sizeof(*s));
}

static void seen_clear(os64_dom_t *dom)
{
    while (dom->wa_nseen > 0) seen_drop(dom, dom->wa_nseen - 1);
    d_free(dom, dom->wa_seen);
    dom->wa_seen = NULL;
    dom->wa_seen_cap = 0;
}

/* `r` as the table knows it, a new one first seen idle; NULL on no memory. */
static DWaSeen *seen_of(os64_dom_t *dom, const os64_dom_css_animation_t *r)
{
    for (size_t i = 0; i < dom->wa_nseen; i++)
        if (dom->wa_seen[i].id == r->id) return &dom->wa_seen[i];
    if (dom->wa_nseen == dom->wa_seen_cap) {
        size_t grown = dom->wa_seen_cap > 0 ? dom->wa_seen_cap * 2 : 8;
        DWaSeen *more = d_alloc(dom, grown * sizeof(*more));
        if (more == NULL) return NULL;
        if (dom->wa_nseen > 0) os64_memcpy(more, dom->wa_seen, dom->wa_nseen * sizeof(*more));
        d_free(dom, dom->wa_seen);
        dom->wa_seen = more;
        dom->wa_seen_cap = grown;
    }
    char *name = d_alloc(dom, r->name_len + 1);
    if (name == NULL) return NULL;
    os64_memcpy(name, r->name, r->name_len);
    name[r->name_len] = '\0';
    DWaSeen *s = &dom->wa_seen[dom->wa_nseen++];
    os64_memset(s, 0, sizeof(*s));
    s->id = r->id;
    os64_html_hold(dom->document, r->element);
    s->element = r->element;
    s->name = name;
    s->name_len = r->name_len;
    s->phase = CSS_IDLE;
    return s;
}

/* A CSS animation event at the seen animation's element, scheduled when
 * its boundary was crossed on the timeline: `elapsed` milliseconds into
 * its active interval, for a clock that runs; now, for one that stands. */
static void css_queue(os64_dom_t *dom, const DWaSeen *s, const char *type, double elapsed, double scheduled)
{
    DWaEvent *e = d_alloc(dom, sizeof(*e));
    char *name = e != NULL ? d_alloc(dom, s->name_len + 1) : NULL;
    if (name == NULL) {
        d_free(dom, e);
        return;
    }
    os64_memcpy(name, s->name, s->name_len + 1);
    os64_html_hold(dom->document, s->element);
    *e = (DWaEvent){.animation = JS_UNDEFINED, .type = type, .current = NAN, .timeline = NAN,
                    .scheduled = scheduled, .element = s->element, .name = name, .name_len = s->name_len,
                    .elapsed = elapsed / 1000};
    if (dom->wa_events_last != NULL) dom->wa_events_last->next = e;
    else dom->wa_events = e;
    dom->wa_events_last = e;
}

/* The seen animation's active time at `now` on the host's clock, by its
 * clock as the last step saw it: where a cancel found it, the clock it had
 * until then (CSS Animations 2: animationcancel's elapsed time). */
static double seen_active_at(const DWaSeen *s, double now)
{
    double local = css_local(s->paused, false, s->start, s->hold, s->rate, now);
    return garb_clock_at(&s->timing, local, s->rate < 0).active_time;
}

/* When on the timeline the seen animation's local time was `local`. */
static double css_when(os64_dom_t *dom, const DWaSeen *s, double local)
{
    if (s->paused || s->canceled || s->rate == 0) return wa_now(dom);
    return s->start - dom->wa_face + local / s->rate;
}

/* The events the animation owes for the boundaries it crossed since the
 * last step, by its phase then and now (CSS Animations 2 § 4.2's table):
 * the interval's start and end are where its active interval begins and
 * ends, clamped to it, and an iteration's elapsed time is the boundary
 * between iterations it crossed last. `quiet` takes where it is and owes
 * nothing; `pending` (a pending play or pause task) keeps it where it was,
 * owing nothing yet. */
static void css_events(os64_dom_t *dom, const os64_dom_css_animation_t *r, int32_t place, double now, bool quiet,
                       bool pending)
{
    DWaSeen *s = seen_of(dom, r);
    if (s == NULL) return;
    double canceled_at = seen_active_at(s, now);
    s->pass = dom->wa_css_pass;
    s->place = place;
    if (pending) return;
    s->paused = r->paused;
    s->canceled = r->canceled;
    s->start = r->start_ms;
    s->hold = r->hold_ms;
    s->rate = r->rate;
    s->timing = css_timing_of(r);
    double local = css_local(r->paused, r->canceled, r->start_ms, r->hold_ms, r->rate, now);
    garb_clock_t k = garb_clock_at(&s->timing, isnan(local) ? 0 : local, r->rate < 0);
    WaCssPhase phase = r->canceled ? CSS_IDLE
                     : k.phase == GARB_PHASE_BEFORE ? CSS_BEFORE
                     : k.phase == GARB_PHASE_ACTIVE ? CSS_ACTIVE : CSS_AFTER;
    double delay = s->timing.delay_ms;
    double from = fmax(fmin(-delay, k.active_duration), 0);
    double to = fmax(fmin(k.end_time - delay, k.active_duration), 0);
    WaCssPhase was = quiet ? phase : s->phase;
    if (quiet && phase == CSS_ACTIVE) s->iteration = k.iteration;
    if ((was == CSS_IDLE || was == CSS_BEFORE) && phase == CSS_ACTIVE) {
        css_queue(dom, s, "animationstart", from, css_when(dom, s, delay + from));
    } else if ((was == CSS_IDLE || was == CSS_BEFORE) && phase == CSS_AFTER) {
        css_queue(dom, s, "animationstart", from, css_when(dom, s, delay + from));
        css_queue(dom, s, "animationend", to, css_when(dom, s, delay + to));
    } else if (was == CSS_ACTIVE && phase == CSS_BEFORE) {
        css_queue(dom, s, "animationend", from, css_when(dom, s, delay + from));
    } else if (was == CSS_ACTIVE && phase == CSS_ACTIVE && k.iteration != s->iteration) {
        /* The boundary crossed last: the new iteration's start going
         * forwards, its end going back. */
        double at = (k.iteration < s->iteration ? k.iteration + 1 : k.iteration) * s->timing.duration_ms;
        css_queue(dom, s, "animationiteration", at, css_when(dom, s, delay + at));
    } else if (was == CSS_ACTIVE && phase == CSS_AFTER) {
        css_queue(dom, s, "animationend", to, css_when(dom, s, delay + to));
    } else if (was == CSS_AFTER && phase == CSS_ACTIVE) {
        css_queue(dom, s, "animationstart", to, css_when(dom, s, delay + to));
    } else if (was == CSS_AFTER && phase == CSS_BEFORE) {
        css_queue(dom, s, "animationstart", to, css_when(dom, s, delay + to));
        css_queue(dom, s, "animationend", from, css_when(dom, s, delay + from));
    } else if (was != CSS_IDLE && was != CSS_AFTER && phase == CSS_IDLE) {
        css_queue(dom, s, "animationcancel", canceled_at, wa_now(dom));
    }
    s->phase = phase;
    if (phase != CSS_IDLE) s->iteration = k.iteration;
}

/* The step's look at the CSS animations: each CSSAnimation takes what its
 * style changed, one whose CSS animation went is canceled, and while a CSS
 * animation event has a listener each animation's crossed boundaries are
 * queued — a cancel for one that went before its end. In the first step
 * that hears a listener, an animation that was there at the step before
 * — its start, paused or not, at or before `since`, on the host's clock;
 * -INFINITY before the first — is taken where it stands: what it crossed
 * then, nobody was listening for, and a listener added once it had ended
 * must not hear its end. (A record holds the time it was made until a
 * play moves it, which is never later; a driven, held one hands 0.) */
static void css_step(JSContext *ctx, os64_dom_t *dom, double now, double since)
{
    os64_dom_css_animations_t *h = &dom->wa_css;
    bool listening = h->count != NULL && css_listening(dom);
    bool seeding = listening && !dom->wa_css_listening;
    dom->wa_css_listening = listening;
    if (!listening) seen_clear(dom);
    if (h->count == NULL) return;
    dom->wa_css_version = h->version != NULL ? h->version(h->opaque) : 0;
    uint32_t pass = ++dom->wa_css_pass;
    int32_t n = h->count(h->opaque, false);
    for (int32_t i = 0; i < n; i++) {
        os64_dom_css_animation_t r;
        if (!h->at(h->opaque, i, &r)) break;
        DWaAnimation *a = css_find(dom, r.id);
        if (a != NULL) {
            a->css_pass = pass;
            css_pull(ctx, a, &r);
        }
        /* CSS Animations 2 works the events out only for an animation with
         * no pending play or pause task (one the style's change just
         * gave it): it keeps the phase it had until that task runs. */
        bool pending = a != NULL && (a->pending_play || a->pending_pause);
        if (listening) css_events(dom, &r, i, now, seeding && r.start_ms <= since, pending);
    }
    /* Each gone one held while all are canceled: a cancel settles promises,
     * which may run a page's getter, which may make or drop animations. */
    size_t ngone = 0;
    for (DWaAnimation *a = dom->wa_first; a != NULL; a = a->next) ngone += a->css_id != 0 && a->css_pass != pass;
    JSValue *gone = ngone > 0 ? js_malloc(ctx, ngone * sizeof(*gone)) : NULL;
    size_t k = 0;
    for (DWaAnimation *a = dom->wa_first; gone != NULL && a != NULL && k < ngone; a = a->next)
        if (a->css_id != 0 && a->css_pass != pass) gone[k++] = JS_DupValue(ctx, a->target.value);
    for (size_t i = 0; i < k; i++) {
        DWaAnimation *a = wa_state(gone[i]);
        if (a != NULL && a->css_id != 0) css_vanish(ctx, a);
        JS_FreeValue(ctx, gone[i]);
    }
    js_free(ctx, gone);
    /* The ones that went, cancelled in the order the host last listed
     * them: composite order, as they last stood. */
    for (int32_t next = INT32_MIN; listening;) {
        DWaSeen *first = NULL;
        for (size_t i = 0; i < dom->wa_nseen; i++) {
            DWaSeen *s = &dom->wa_seen[i];
            if (s->pass != pass && s->place >= next && (first == NULL || s->place < first->place)) first = s;
        }
        if (first == NULL) break;
        if (first->phase == CSS_BEFORE || first->phase == CSS_ACTIVE)
            css_queue(dom, first, "animationcancel", seen_active_at(first, now), wa_now(dom));
        if (first->place == INT32_MAX) break;
        next = first->place + 1;
    }
    for (size_t i = dom->wa_nseen; listening && i-- > 0;)
        if (dom->wa_seen[i].pass != pass) seen_drop(dom, i);
}

/* When a seen animation crosses its next boundary after `then` (the step
 * that saw it), on the host's clock: its active phase's start, an
 * iteration's, or its end, for a clock running forwards at rate 1; `then`
 * for one at another rate, which the step then watches every frame;
 * INFINITY for one that stands. */
static double seen_next(const DWaSeen *s, double then)
{
    if (s->paused || s->canceled || s->rate == 0 || s->phase == CSS_IDLE) return INFINITY;
    if (s->rate != 1) return then;
    double local = then - s->start, delay = s->timing.delay_ms, dur = s->timing.duration_ms;
    double active = garb_clock_at(&s->timing, 0, false).active_duration;
    double b = INFINITY;
    if (local < delay) b = delay;
    else if (local < delay + active) b = dur > 0 ? fmin(delay + (floor((local - delay) / dur) + 1) * dur, delay + active)
                                                 : delay + active;
    return s->start + b;
}

/* When the CSS animations next owe the step a look: at once when the
 * host's animations moved and a CSSAnimation or a listener cares, or a
 * listener came or went; else at the next boundary a listener may hear;
 * UINT64_MAX for never. */
static uint64_t css_due(os64_dom_t *dom)
{
    os64_dom_css_animations_t *h = &dom->wa_css;
    if (h->count == NULL) return UINT64_MAX;
    bool listening = css_listening(dom), wrapped = false;
    for (DWaAnimation *a = dom->wa_first; a != NULL && !wrapped; a = a->next) wrapped = a->css_id != 0;
    if (listening != dom->wa_css_listening) return 0;
    if (!listening && !wrapped) return UINT64_MAX;
    if (h->version != NULL && h->version(h->opaque) != dom->wa_css_version) return 0;
    if (!listening) return UINT64_MAX;
    double due = INFINITY;
    for (size_t i = 0; i < dom->wa_nseen; i++) due = fmin(due, seen_next(&dom->wa_seen[i], (double)dom->wa_last_step));
    return isinf(due) ? UINT64_MAX : due <= 0 ? 0 : (uint64_t)ceil(due);
}

void os64_dom_set_css_animations(os64_dom_t *dom, const os64_dom_css_animations_t *css)
{
    if (dom == NULL) return;
    if (css != NULL) dom->wa_css = *css;
    else os64_memset(&dom->wa_css, 0, sizeof(dom->wa_css));
}

/* Each binding keeps its creation-ordered records. Merge their heads by the
 * cluster serial so queries and layout see the same composite order. */
static size_t animation_heads(os64_dom_t *dom, DWaAnimation **heads)
{
    size_t members = dom->cluster ? dom->cluster->count : 1;
    for (size_t i = 0; i < members; ++i)
        heads[i] = (dom->cluster ? dom->cluster->members[i] : dom)->wa_first;
    return members;
}

static DWaAnimation *animation_next(DWaAnimation **heads, size_t members)
{
    size_t first = members;
    for (size_t i = 0; i < members; ++i)
        if (heads[i] && (first == members || heads[i]->serial < heads[first]->serial)) first = i;
    if (first == members) return NULL;
    DWaAnimation *a = heads[first];
    heads[first] = a->next;
    return a;
}

static bool animation_in_query(const os64_html_node_t *target, const os64_html_node_t *root, bool subtree)
{
    if (root->kind == OS64_HTML_DOCUMENT)
        return d_connected(target) && os64_html_node_document(target) == root;
    return subtree ? within(target, root) : target == root;
}

/* getAnimations (§ 6.8 Animatable, § 6.10 DocumentOrShadowRoot): the relevant
 * animations of the element (of its subtree with `subtree`), or of the
 * document's connected elements, in composite order — the CSS animations
 * first, as the host orders them, each a CSSAnimation made on first sight
 * (the style made current first, as other browsers flush it), then the
 * rest in the order they were made. */
static JSValue get_animations(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                              JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *root = d_node(dom, ctx, self);
    if (root == NULL) return JS_EXCEPTION;
    if (root->kind != (magic == 0 ? OS64_HTML_ELEMENT : OS64_HTML_DOCUMENT))
        return JS_ThrowTypeError(ctx, "getAnimations needs an element or document");
    bool subtree = false;
    if (magic == 0) {
        if (argc > 0 && JS_IsObject(argv[0])) {
            JSValue v = JS_GetPropertyStr(ctx, argv[0], "subtree");
            if (JS_IsException(v)) return v;
            int b = JS_ToBool(ctx, v);
            JS_FreeValue(ctx, v);
            if (b < 0) return JS_EXCEPTION;
            subtree = b != 0;
        }
    }
    /* Reading subtree may have adopted the receiver into another document. */
    dom = d_owner(dom, root);
    JSValue list = JS_NewArray(ctx);
    if (JS_IsException(list)) return list;
    uint32_t n = 0;
    os64_dom_css_animations_t *h = &dom->wa_css;
    int32_t ncss = h->count != NULL ? h->count(h->opaque, true) : 0;
    for (int32_t i = 0; i < ncss; i++) {
        os64_dom_css_animation_t r;
        if (!h->at(h->opaque, i, &r)) break;
        bool mine = animation_in_query(r.element, root, subtree);
        DWaAnimation *a = mine ? css_find(dom, r.id) : NULL;
        if (a != NULL) css_pull(ctx, a, &r);
        else if (mine && css_record_relevant(dom, &r)) a = css_wrap(dom, ctx, &r);
        else continue;
        if (a == NULL) {
            JS_FreeValue(ctx, list);
            return JS_EXCEPTION;
        }
        if (!wa_relevant(a)) continue;
        if (JS_SetPropertyUint32(ctx, list, n++, JS_DupValue(ctx, a->target.value)) < 0) {
            JS_FreeValue(ctx, list);
            return JS_EXCEPTION;
        }
    }
    DWaAnimation *heads[D_REALMS];
    size_t members = animation_heads(dom, heads);
    /* Array growth can collect unreferenced records. Pin each queued head;
     * advancing transfers its reference to this iteration and pins its next. */
    for (size_t i = 0; i < members; ++i)
        if (heads[i]) JS_DupValue(ctx, heads[i]->target.value);
    bool failed = false;
    for (DWaAnimation *a; (a = animation_next(heads, members)) != NULL;) {
        if (a->next) JS_DupValue(ctx, a->next->target.value);
        const os64_html_node_t *target = wa_target(a);
        if (a->css_id == 0 && target != NULL && a->replace != WA_REMOVED && wa_relevant(a) &&
            animation_in_query(target, root, subtree))
            failed = JS_SetPropertyUint32(ctx, list, n++, JS_DupValue(ctx, a->target.value)) < 0;
        JS_FreeValue(ctx, a->target.value);
        if (failed) break;
    }
    for (size_t i = 0; i < members; ++i)
        if (heads[i]) JS_FreeValue(ctx, heads[i]->target.value);
    if (failed) { JS_FreeValue(ctx, list); return JS_EXCEPTION; }
    return list;
}

/* ── AnimationPlaybackEvent ──────────────────────────────────────────── */

static int playback_fields(JSContext *ctx, JSValue event, JSValueConst current, JSValueConst timeline)
{
    if (JS_DefinePropertyValueStr(ctx, event, "currentTime", JS_DupValue(ctx, current),
                                  JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE) < 0 ||
        JS_DefinePropertyValueStr(ctx, event, "timelineTime", JS_DupValue(ctx, timeline),
                                  JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    return 0;
}

/* `new AnimationPlaybackEvent(type, {currentTime, timelineTime, ...})`. */
static JSValue playback_construct(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSValue event = d_event_construct(dom, ctx, argc, argv,
                                      JS_IsObject(prototype) ? prototype : dom->wa_event_prototype->value);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(event)) return event;
    JSValue times[2] = {JS_NULL, JS_NULL};
    static const char *const kNames[] = {"currentTime", "timelineTime"};
    for (int i = 0; i < 2; i++) {
        JSValue v = argc > 1 && JS_IsObject(argv[1]) ? JS_GetPropertyStr(ctx, argv[1], kNames[i]) : JS_UNDEFINED;
        double t;
        if (JS_IsException(v) || (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToFloat64(ctx, &t, v) < 0)) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, event);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) times[i] = JS_NewFloat64(ctx, t);
        JS_FreeValue(ctx, v);
    }
    if (playback_fields(ctx, event, times[0], times[1]) < 0) {
        JS_FreeValue(ctx, event);
        return JS_EXCEPTION;
    }
    return event;
}

/* `new AnimationEvent(type, {animationName, elapsedTime, pseudoElement,
 * ...})` (CSS Animations 1 § 4.1). */
static JSValue css_event_construct(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSValue event = d_event_construct(dom, ctx, argc, argv,
                                      JS_IsObject(prototype) ? prototype : dom->wa_css_event_prototype->value);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(event)) return event;
    JSValueConst init = argc > 1 && JS_IsObject(argv[1]) ? argv[1] : JS_UNDEFINED;
    JSValue name = JS_IsUndefined(init) ? JS_UNDEFINED : JS_GetPropertyStr(ctx, init, "animationName");
    JSValue elapsed = JS_IsException(name) || JS_IsUndefined(init) ? JS_UNDEFINED
                    : JS_GetPropertyStr(ctx, init, "elapsedTime");
    JSValue pseudo = JS_IsException(name) || JS_IsException(elapsed) || JS_IsUndefined(init) ? JS_UNDEFINED
                   : JS_GetPropertyStr(ctx, init, "pseudoElement");
    double t = 0;
    const char *n = NULL, *ps = NULL;
    size_t nlen = 0, plen = 0;
    bool ok = !JS_IsException(name) && !JS_IsException(elapsed) && !JS_IsException(pseudo);
    if (ok && !JS_IsUndefined(name)) ok = (n = JS_ToCStringLen(ctx, &nlen, name)) != NULL;
    if (ok && !JS_IsUndefined(elapsed)) ok = JS_ToFloat64(ctx, &t, elapsed) == 0;
    if (ok && !JS_IsUndefined(pseudo)) ok = (ps = JS_ToCStringLen(ctx, &plen, pseudo)) != NULL;
    if (ok && !isfinite(t)) {
        JS_ThrowTypeError(ctx, "An AnimationEvent's elapsedTime is a finite number");
        ok = false;
    }
    if (ok) ok = css_event_fields(ctx, event, n != NULL ? n : "", nlen, t) == 0;
    if (ok && ps != NULL)
        ok = JS_DefinePropertyValueStr(ctx, event, "pseudoElement", JS_NewStringLen(ctx, ps, plen),
                                       JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE) >= 0;
    JS_FreeCString(ctx, n);
    JS_FreeCString(ctx, ps);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, elapsed);
    JS_FreeValue(ctx, pseudo);
    if (!ok) {
        JS_FreeValue(ctx, event);
        return JS_EXCEPTION;
    }
    return event;
}

/* ── The update step (HTML's "update animations and send events") ────── */

/* Whether the step has work: a queued event, a timeline a script read, an
 * animation that finished since the last look for replaced ones, an
 * animation on the timeline with a pending task or running, or one the
 * binding holds that is no longer live (wa_live) and is to be let go. */
static bool wa_busy(os64_dom_t *dom)
{
    if (dom->wa_events != NULL || dom->wa_wanted || dom->wa_replace_check) return true;
    for (DWaAnimation *a = dom->wa_first; a != NULL; a = a->next) {
        if (!JS_IsUndefined(a->hold) && !wa_live(a)) return true;
        if (a->no_timeline) continue;
        if (a->pending_play || a->pending_pause || play_state(a) == WA_RUNNING) return true;
    }
    return false;
}

/* § 5.5, Replacing animations: replaceable is finished, filling, of a
 * target in the document, on the document's timeline, and not removed; a
 * CSSAnimation never is (CSS Animations 2: one with an owning element is
 * not, and one without has no keyframes here to replace anything). */
static bool wa_replaceable(DWaAnimation *a)
{
    return !a->css && a->replace != WA_REMOVED && !a->no_timeline && wa_target(a) != NULL &&
           d_in_window_tree(a->dom, wa_target(a)) && wa_data(a) != NULL &&
           play_state(a) == WA_FINISHED && wa_clock(a).in_effect;
}

/* § 5.5, removing replaced animations: an active replaceable animation
 * goes when, for every property it sets, a later replaceable one of the
 * same target sets it too; its effect then applies to nothing, and its
 * `remove` event is queued. A persisted one replaces others and is never
 * removed itself. */
static void remove_replaced(JSContext *ctx, os64_dom_t *dom)
{
    dom->wa_replace_check = false;
    size_t n = 0;
    for (DWaAnimation *a = dom->wa_first; a != NULL; a = a->next) n += wa_replaceable(a);
    if (n < 2) return;
    DWaAnimation **v = js_malloc(ctx, n * sizeof(*v));
    if (v == NULL) return;
    size_t k = 0;
    for (DWaAnimation *a = dom->wa_first; a != NULL && k < n; a = a->next)
        if (wa_replaceable(a)) v[k++] = a;
    for (size_t i = 0; i < k; i++) {
        DWaAnimation *a = v[i];
        if (a->replace != WA_ACTIVE) continue;
        DWaData *d = wa_data(a);
        /* One that sets nothing is covered by any later one of its target. */
        bool covered = d->nprops > 0;
        for (size_t j = i + 1; !covered && j < k; j++) covered = wa_target(v[j]) == wa_target(a);
        for (int32_t p = 0; covered && d->nprops > 0 && p < d->nprops; p++) {
            bool found = false;
            for (size_t j = i + 1; !found && j < k; j++) {
                if (wa_target(v[j]) != wa_target(a)) continue;
                DWaData *later = wa_data(v[j]);
                for (int32_t q = 0; !found && q < later->nprops; q++) found = later->props[q] == d->props[p];
            }
            covered = found;
        }
        if (!covered) continue;
        a->replace = WA_REMOVED;
        queue_event(ctx, a, "remove", current_time(a), wa_now(dom), wa_now(dom));
        wa_restyle(a);
    }
    js_free(ctx, v);
}

uint64_t os64_dom_web_animations_next(os64_dom_t *dom)
{
    if (dom == NULL || dom->closed || dom->wa_running) return UINT64_MAX;
    uint64_t soon = dom->wa_stepped ? dom->wa_last_step + WA_FRAME_MS : 0;
    if (wa_busy(dom)) return soon;
    uint64_t due = css_due(dom);
    return due == UINT64_MAX ? UINT64_MAX : due > soon ? due : soon;
}

/* Stable by scheduled time (§ 4.4.18.1: the events in the order they would
 * have happened, an unresolved time first), the order they were queued in
 * between equals, which is composite order. */
static double event_when(const DWaEvent *e)
{
    return isnan(e->scheduled) ? -INFINITY : e->scheduled;
}

static DWaEvent *events_sorted(DWaEvent *list)
{
    DWaEvent *sorted = NULL;
    while (list != NULL) {
        DWaEvent *e = list;
        list = list->next;
        DWaEvent **at = &sorted;
        while (*at != NULL && !(event_when(e) < event_when(*at))) at = &(*at)->next;
        e->next = *at;
        *at = e;
    }
    return sorted;
}

bool os64_dom_web_animations_update(os64_dom_t *dom, uint64_t now, os64_js_outcome_t *out)
{
    if (out) os64_memset(out, 0, sizeof(*out));
    if (out == NULL || os64_dom_web_animations_next(dom) > now) return false;
    double since = dom->wa_stepped ? (double)dom->wa_last_step : -INFINITY;
    wa_sample(dom);
    dom->wa_wanted = false;
    dom->wa_stepped = true;
    dom->wa_last_step = now;
    char name[OS64_JS_SOURCE_NAME_CAP];
    os64_snprintf(name, sizeof(name), "%.*s#web-animations", (int)(sizeof(name) - 20),
                  dom->url ? dom->url : "about:blank");
    dom->wa_running = true;
    if (os64_js_realm_task_begin(dom->realm, OS64_JS_ABI_ID, name, out) == OS64_JS_OK) {
        os64_js_outcome_t access;
        JSContext *ctx = os64_js_realm_context(dom->realm, OS64_JS_ABI_ID, &access);
        if (ctx == NULL) d_outcome_fold(out, &access);
        /* Each animation held for the step, so none is collected while
         * the step walks them, whatever a listener does. */
        size_t n = 0;
        JSValue *held = ctx != NULL && dom->wa_count > 0 ? js_malloc(ctx, dom->wa_count * sizeof(*held)) : NULL;
        for (DWaAnimation *a = dom->wa_first; held != NULL && a != NULL && n < dom->wa_count; a = a->next)
            held[n++] = JS_DupValue(ctx, a->target.value);
        for (size_t i = 0; i < n; i++) {
            DWaAnimation *a = wa_state(held[i]);
            if (a == NULL) continue;
            if (a->no_timeline) continue;
            if (a->pending_play || a->pending_pause) run_pending(ctx, a, dom->wa_time);
            else if (play_state(a) != WA_IDLE) update_finished(ctx, a, false, false);
        }
        /* After the pending tasks: a pause or play the style asks for now
         * waits for the next step, as a script's does. */
        if (ctx != NULL) css_step(ctx, dom, (double)now, since);
        if (ctx != NULL) remove_replaced(ctx, dom);
        if (ctx != NULL) d_task_settle(dom, out);
        DWaEvent *events = events_sorted(dom->wa_events);
        dom->wa_events = dom->wa_events_last = NULL;
        while (events != NULL) {
            DWaEvent *e = events;
            events = e->next;
            DWaAnimation *a = wa_state(e->animation);
            if (ctx != NULL && e->element != NULL) {
                JSValue event = d_event_new_proto(dom, ctx, dom->wa_css_event_prototype->value, e->type, true,
                                                  false, true);
                if (!JS_IsException(event) &&
                    css_event_fields(ctx, event, e->name, e->name_len, e->elapsed) == 0)
                    (void)d_dispatch_node_task(dom, ctx, event, e->element, out);
                JS_FreeValue(ctx, event);
            } else if (ctx != NULL && a != NULL) {
                JSValue event = d_event_new_proto(dom, ctx, dom->wa_event_prototype->value, e->type, false,
                                                  false, true);
                if (!JS_IsException(event)) {
                    JSValue current = time_value(ctx, e->current), timeline = time_value(ctx, e->timeline);
                    if (playback_fields(ctx, event, current, timeline) == 0)
                        (void)d_dispatch_plain_task(dom, ctx, event, &a->target, out);
                    JS_FreeValue(ctx, current);
                    JS_FreeValue(ctx, timeline);
                }
                JS_FreeValue(ctx, event);
            }
            event_free(dom, e);
        }
        /* Let go of what can change nothing more; the step's own hold
         * keeps each alive until it is freed below. */
        for (size_t i = 0; i < n; i++) {
            DWaAnimation *a = wa_state(held[i]);
            if (a == NULL || JS_IsUndefined(a->hold)) continue;
            if (!wa_live(a)) {
                JSValue h = a->hold;
                a->hold = JS_UNDEFINED;
                JS_FreeValue(ctx, h);
            }
        }
        for (size_t i = 0; i < n; i++) JS_FreeValue(ctx, held[i]);
        js_free(ctx, held);
        d_task_finish(dom, out);
    }
    dom->wa_running = false;
    d_task_close(dom, name, out);
    return true;
}

/* ── What libflow reads ──────────────────────────────────────────────── */

/* Whether an animation's effect is handed to the layouts: it has one, with
 * a target, it is not idle, and it was not removed (§ 5.5); a
 * CSSAnimation's is the host's own. */
static bool wa_applies(DWaAnimation *a)
{
    return !a->css && wa_target(a) != NULL && wa_data(a) != NULL && a->replace != WA_REMOVED && play_state(a) != WA_IDLE;
}

/* The effects by target, rebuilt when an animation changed: (element,
 * effect) pairs sorted by element, each element's in composite order (the
 * order they were made). */
static void index_rebuild(os64_dom_t *dom)
{
    d_free(dom, dom->wa_index_nodes);
    d_free(dom, dom->wa_index_fx);
    dom->wa_index_nodes = NULL;
    dom->wa_index_fx = NULL;
    dom->wa_index_n = 0;
    dom->wa_index_stale = false;
    DWaAnimation *heads[D_REALMS];
    size_t n = 0, members = animation_heads(dom, heads);
    for (size_t i = 0; i < members; ++i)
        for (DWaAnimation *a = heads[i]; a; a = a->next) n += wa_applies(a);
    if (n == 0) return;
    dom->wa_index_nodes = d_alloc(dom, n * sizeof(*dom->wa_index_nodes));
    dom->wa_index_fx = d_alloc(dom, n * sizeof(*dom->wa_index_fx));
    if (dom->wa_index_nodes == NULL || dom->wa_index_fx == NULL) {
        d_free(dom, dom->wa_index_nodes);
        d_free(dom, dom->wa_index_fx);
        dom->wa_index_nodes = NULL;
        dom->wa_index_fx = NULL;
        index_dirty(dom);
        return;
    }
    size_t k = 0;
    for (DWaAnimation *a; (a = animation_next(heads, members)) != NULL;) {
        if (!wa_applies(a)) continue;
        const os64_html_node_t *target = wa_target(a);
        /* Insertion by element; the list's order keeps each element's
         * effects in the order they were made. */
        size_t at = k;
        while (at > 0 && (uintptr_t)dom->wa_index_nodes[at - 1] > (uintptr_t)target) {
            dom->wa_index_nodes[at] = dom->wa_index_nodes[at - 1];
            dom->wa_index_fx[at] = dom->wa_index_fx[at - 1];
            at--;
        }
        dom->wa_index_nodes[at] = target;
        dom->wa_index_fx[at] = &wa_data(a)->fx;
        k++;
    }
    dom->wa_index_n = k;
}

int32_t os64_dom_script_effects(os64_dom_t *dom, const os64_html_node_t *node, const struct garb_effect *const **out)
{
    *out = NULL;
    if (dom == NULL || d_owner(dom, node) != dom) return 0;
    if (!dom->cluster && dom->wa_count == 0) return 0;
    if (dom->wa_index_stale) index_rebuild(dom);
    size_t lo = 0, hi = dom->wa_index_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if ((uintptr_t)dom->wa_index_nodes[mid] < (uintptr_t)node) lo = mid + 1;
        else hi = mid;
    }
    size_t first = lo;
    while (lo < dom->wa_index_n && dom->wa_index_nodes[lo] == node) lo++;
    if (lo == first) return 0;
    *out = &dom->wa_index_fx[first];
    return (int32_t)(lo - first);
}

uint64_t os64_dom_effects_mark(os64_dom_t *dom)
{
    if (!dom) return 0;
    uint64_t mark = dom->cluster ? ++dom->cluster->effect_generation : ++dom->wa_generation;
    if (mark < dom->wa_borrowed_floor) dom->wa_borrowed_floor = mark;
    return mark;
}

static void effects_release_native(os64_dom_t *dom, uint64_t oldest)
{
    if (dom == NULL) return;
    for (DWaData **at = &dom->wa_dead; *at != NULL;) {
        DWaData *d = *at;
        if (d->died < oldest) {
            *at = d->next;
            data_free(dom, d);
        } else {
            at = &d->next;
        }
    }
}

/* A source layout can still read data after the target moves. Retire data
 * against the oldest borrowing host in the cluster, not its current owner. */
void os64_dom_effects_release(os64_dom_t *dom, uint64_t oldest)
{
    if (!dom) return;
    dom->wa_borrowed_floor = oldest;
    if (!dom->cluster) { effects_release_native(dom, oldest); return; }
    for (size_t i = 0; i < dom->cluster->count; ++i)
        if (dom->cluster->members[i]->wa_borrowed_floor < oldest)
            oldest = dom->cluster->members[i]->wa_borrowed_floor;
    for (size_t i = 0; i < dom->cluster->count; ++i)
        effects_release_native(dom->cluster->members[i], oldest);
}

bool os64_dom_effects_held(const os64_dom_t *dom)
{
    return dom != NULL && (dom->wa_dead != NULL || dom->wa_first != NULL || dom->wa_fx_first != NULL);
}

/* ── The binding's lifetime ──────────────────────────────────────────── */

void d_web_animations_drain(os64_dom_t *dom)
{
    while (dom->wa_events != NULL) {
        DWaEvent *e = dom->wa_events;
        dom->wa_events = e->next;
        event_free(dom, e);
    }
    dom->wa_events_last = NULL;
    seen_clear(dom);
    os64_memset(&dom->wa_css, 0, sizeof(dom->wa_css));
    /* The binding's holds first: an animation they alone kept goes now,
     * and finalizes against a live binding. Freeing one may free others
     * its listeners alone kept, so the walk starts again after each. */
    for (DWaAnimation *a = dom->wa_first; a != NULL;) {
        if (JS_IsUndefined(a->hold)) {
            a = a->next;
            continue;
        }
        JSValue h = a->hold;
        a->hold = JS_UNDEFINED;
        JS_FreeValueRT(dom->engine, h);
        a = dom->wa_first;
    }
    /* What a script still holds outlives the binding: it is cut loose,
     * each effect's data buried, as a tree laid out with it may still be
     * painted until the binding is freed (d_web_animations_free). */
    while (dom->wa_first != NULL) {
        DWaAnimation *a = dom->wa_first;
        unlink_animation(dom, a);
        a->dom = NULL;
    }
    while (dom->wa_fx_first != NULL) {
        DWaEffect *e = dom->wa_fx_first;
        effect_unlink(dom, e);
        effect_let_go(dom, e);
        e->dom = NULL;
    }
}

void d_web_animations_free(os64_dom_t *dom)
{
    effects_release_native(dom, UINT64_MAX);
    d_free(dom, dom->wa_index_nodes);
    d_free(dom, dom->wa_index_fx);
    dom->wa_index_nodes = NULL;
    dom->wa_index_fx = NULL;
    dom->wa_index_n = 0;
}

int d_web_animations_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef animation_class = {.class_name = "Animation", .finalizer = animation_finalizer,
                                               .gc_mark = animation_mark};
    static const JSClassDef effect_class = {.class_name = "KeyframeEffect", .finalizer = effect_finalizer};
    static const JSClassDef timeline_class = {.class_name = "DocumentTimeline"};
    JSClassID aid = os64_js_class_id(&animation_slot), eid = os64_js_class_id(&effect_slot),
              tid = os64_js_class_id(&timeline_slot);
    if ((!JS_IsRegisteredClass(dom->engine, aid) && JS_NewClass(dom->engine, aid, &animation_class) < 0) ||
        (!JS_IsRegisteredClass(dom->engine, eid) && JS_NewClass(dom->engine, eid, &effect_class) < 0) ||
        (!JS_IsRegisteredClass(dom->engine, tid) && JS_NewClass(dom->engine, tid, &timeline_class) < 0))
        return -1;
    DValue *ap = d_retain(dom, ctx, JS_NewObject(ctx));
    DValue *aep = ap != NULL ? d_retain(dom, ctx, JS_NewObject(ctx)) : NULL;
    DValue *ep = aep != NULL ? d_retain(dom, ctx, JS_NewObjectProto(ctx, aep->value)) : NULL;
    DValue *atp = ep != NULL ? d_retain(dom, ctx, JS_NewObject(ctx)) : NULL;
    if (atp == NULL) return -1;
    JSValue dtp_value = JS_NewObjectProto(ctx, atp->value);
    DValue *dtp = d_retain(dom, ctx, dtp_value);
    if (dtp == NULL) return -1;
    JS_SetClassProto(ctx, aid, JS_DupValue(ctx, ap->value));
    JS_SetClassProto(ctx, eid, JS_DupValue(ctx, ep->value));
    JS_SetClassProto(ctx, tid, JS_DupValue(ctx, dtp->value));
    static const struct { const char *name; int magic; bool writable; } kAccessors[] = {
        {"id", A_ID, true}, {"effect", A_EFFECT, true}, {"timeline", A_TIMELINE, false},
        {"startTime", A_START, true}, {"currentTime", A_CURRENT, true}, {"playbackRate", A_RATE, true},
        {"playState", A_STATE, false}, {"replaceState", A_REPLACE, false}, {"pending", A_PENDING, false},
        {"ready", A_READY, false}, {"finished", A_FINISHED, false},
    };
    static const struct { const char *name; int magic, length; } kMethods[] = {
        {"cancel", A_CANCEL, 0}, {"finish", A_FINISH, 0}, {"play", A_PLAY, 0}, {"pause", A_PAUSE, 0},
        {"updatePlaybackRate", A_UPDATE_RATE, 1}, {"reverse", A_REVERSE, 0}, {"persist", A_PERSIST, 0},
        {"commitStyles", A_COMMIT, 0},
    };
    JSValueConst a = ap->value;
    for (size_t i = 0; i < sizeof(kAccessors) / sizeof(kAccessors[0]); i++)
        if (d_accessor(dom, ctx, a, kAccessors[i].name, animation_member, kAccessors[i].magic,
                       kAccessors[i].writable) < 0)
            return -1;
    for (size_t i = 0; i < sizeof(kMethods) / sizeof(kMethods[0]); i++)
        if (d_method(dom, ctx, a, kMethods[i].name, animation_member, kMethods[i].length, kMethods[i].magic) < 0)
            return -1;
    if (d_event_target(dom, ctx, a, aid, NULL) < 0 || d_event_handler_named(dom, ctx, a, "finish") < 0 ||
        d_event_handler_named(dom, ctx, a, "cancel") < 0 || d_event_handler_named(dom, ctx, a, "remove") < 0 ||
        d_accessor(dom, ctx, atp->value, "currentTime", animation_member, T_CURRENT, false) < 0)
        return -1;
    /* AnimationEffect's members, and KeyframeEffect's own on the
     * prototype that inherits them. */
    static const struct { const char *name; int magic, length; bool keyframe; } kEffectMethods[] = {
        {"getTiming", E_GET_TIMING, 0, false}, {"getComputedTiming", E_COMPUTED_TIMING, 0, false},
        {"updateTiming", E_UPDATE_TIMING, 0, false}, {"getKeyframes", E_GET_KEYFRAMES, 0, true},
        {"setKeyframes", E_SET_KEYFRAMES, 1, true},
    };
    static const struct { const char *name; int magic; } kEffectAccessors[] = {
        {"target", E_TARGET}, {"pseudoElement", E_PSEUDO}, {"composite", E_COMPOSITE},
        {"iterationComposite", E_ITERATION_COMPOSITE},
    };
    for (size_t i = 0; i < sizeof(kEffectMethods) / sizeof(kEffectMethods[0]); i++)
        if (d_method(dom, ctx, kEffectMethods[i].keyframe ? ep->value : aep->value, kEffectMethods[i].name,
                     effect_member, kEffectMethods[i].length, kEffectMethods[i].magic) < 0)
            return -1;
    for (size_t i = 0; i < sizeof(kEffectAccessors) / sizeof(kEffectAccessors[0]); i++)
        if (d_accessor(dom, ctx, ep->value, kEffectAccessors[i].name, effect_member, kEffectAccessors[i].magic,
                       true) < 0)
            return -1;
    JSValue timeline = JS_NewObjectClass(ctx, (int)tid);
    dom->wa_timeline = d_retain(dom, ctx, timeline);
    if (dom->wa_timeline == NULL) return -1;
    JSValue promise = JS_GetPropertyStr(ctx, global, "Promise");
    JSValue promise_prototype = JS_IsException(promise) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, promise, "prototype");
    JS_FreeValue(ctx, promise);
    dom->wa_promise_then = JS_IsException(promise_prototype) ? NULL
                         : d_intrinsic(dom, ctx, promise_prototype, "then");
    JS_FreeValue(ctx, promise_prototype);
    if (dom->wa_promise_then == NULL) return -1;
    JSValueConst element = dom->prototypes[D_PROTO_ELEMENT]->value;
    JSValueConst document = dom->prototypes[D_PROTO_DOCUMENT]->value;
    if (d_method(dom, ctx, element, "animate", animate_method, 1, 0) < 0 ||
        d_method(dom, ctx, element, "getAnimations", get_animations, 0, 0) < 0 ||
        d_method(dom, ctx, document, "getAnimations", get_animations, 0, 1) < 0 ||
        d_accessor(dom, ctx, document, "timeline", animation_member, D_TIMELINE, false) < 0)
        return -1;
    /* The interfaces: Animation and KeyframeEffect with their
     * constructors, AnimationEffect with none (it is abstract), and
     * DocumentTimeline made by the document. */
    JSValue event_prototype = JS_GetClassProto(ctx, dom->event_class);
    JSValue event_interface = JS_GetPropertyStr(ctx, global, "Event");
    JSValue playback_proto = JS_NewObjectProto(ctx, event_prototype);
    JS_FreeValue(ctx, event_prototype);
    dom->wa_event_prototype = d_retain(dom, ctx, playback_proto);
    if (dom->wa_event_prototype == NULL || JS_IsException(event_interface)) {
        JS_FreeValue(ctx, event_interface);
        return -1;
    }
    /* CSSAnimation's prototype inherits Animation's; AnimationEvent's,
     * Event's. */
    JSValue css_event_proto = JS_GetClassProto(ctx, dom->event_class);
    dom->wa_css_event_prototype = d_retain(dom, ctx, JS_NewObjectProto(ctx, css_event_proto));
    JS_FreeValue(ctx, css_event_proto);
    dom->wa_css_prototype = dom->wa_css_event_prototype != NULL ? d_retain(dom, ctx, JS_NewObjectProto(ctx, ap->value))
                                                                : NULL;
    if (dom->wa_css_prototype == NULL ||
        d_accessor(dom, ctx, dom->wa_css_prototype->value, "animationName", animation_member, A_CSS_NAME, false) < 0) {
        JS_FreeValue(ctx, event_interface);
        return -1;
    }
    /* The CSS animation events' handlers, where GlobalEventHandlers puts
     * them. */
    JSValueConst holders[] = {global, element, document};
    for (int kind = D_EVENT_ANIMATIONSTART; kind <= D_EVENT_ANIMATIONCANCEL; kind++)
        for (size_t i = 0; i < sizeof(holders) / sizeof(holders[0]); i++)
            if (d_event_handler(dom, ctx, holders[i], kind) < 0) {
                JS_FreeValue(ctx, event_interface);
                return -1;
            }
    JSValue timeline_interface = d_interface_new(ctx, "AnimationTimeline", 0, NULL, atp->value, JS_UNDEFINED);
    JSValue effect_interface = d_interface_new(ctx, "AnimationEffect", 0, NULL, aep->value, JS_UNDEFINED);
    struct { const char *name; int length; JSCFunction *construct; JSValueConst proto, parent; } kInterfaces[] = {
        {"Animation", 0, animation_construct, ap->value, JS_UNDEFINED},
        {"KeyframeEffect", 1, effect_construct, ep->value, effect_interface},
        {"DocumentTimeline", 0, NULL, dtp->value, timeline_interface},
        {"AnimationPlaybackEvent", 1, playback_construct, dom->wa_event_prototype->value, event_interface},
        {"AnimationEvent", 1, css_event_construct, dom->wa_css_event_prototype->value, event_interface},
    };
    int result = JS_IsException(timeline_interface) || JS_IsException(effect_interface) ? -1 : 0;
    if (result == 0 && JS_DefinePropertyValueStr(ctx, global, "AnimationTimeline", JS_DupValue(ctx, timeline_interface),
                                                 JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        result = -1;
    if (result == 0 && JS_DefinePropertyValueStr(ctx, global, "AnimationEffect", JS_DupValue(ctx, effect_interface),
                                                 JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        result = -1;
    for (size_t i = 0; result == 0 && i < sizeof(kInterfaces) / sizeof(kInterfaces[0]); i++) {
        JSValue f = d_interface_new(ctx, kInterfaces[i].name, kInterfaces[i].length, kInterfaces[i].construct,
                                    kInterfaces[i].proto, kInterfaces[i].parent);
        if (JS_IsException(f) ||
            JS_DefinePropertyValueStr(ctx, global, kInterfaces[i].name, f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
            result = -1;
    }
    /* CSSAnimation has no constructor: the style makes them. */
    JSValue animation_interface = result == 0 ? JS_GetPropertyStr(ctx, global, "Animation") : JS_UNDEFINED;
    if (JS_IsException(animation_interface)) result = -1;
    if (result == 0) {
        JSValue f = d_interface_new(ctx, "CSSAnimation", 0, NULL, dom->wa_css_prototype->value, animation_interface);
        if (JS_IsException(f) ||
            JS_DefinePropertyValueStr(ctx, global, "CSSAnimation", f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
            result = -1;
    }
    JS_FreeValue(ctx, animation_interface);
    JS_FreeValue(ctx, timeline_interface);
    JS_FreeValue(ctx, effect_interface);
    JS_FreeValue(ctx, event_interface);
    return result;
}
