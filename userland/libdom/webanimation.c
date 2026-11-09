#include <math.h>

#include "internal.h"
#include "garb/garb.h"
#include "garb/timing.h"
#include "os64/fmt.h"

/* WEB ANIMATIONS (Web Animations 1; WEB_ANIMATIONS.md): Element.animate,
 * Animation, KeyframeEffect's target, DocumentTimeline, getAnimations and
 * AnimationPlaybackEvent. This file owns the script's half: the Animation's
 * state machine (§ 4.4, Animations; the numbers are the 2023-06-05 Working
 * Draft's) as written, its promises and events, and keyframe processing
 * (§ 6.6.3). The values are libflow's: each animation publishes
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
 * THE KEYFRAMES OUTLIVE THEIR ANIMATION. A layout tree keeps the effects
 * it was laid out with for its ticks and its paints, so a collected
 * animation's data (DWaData) is buried, marked with the generation it died
 * in, and freed only once the host says no tree that old is left
 * (os64_dom_effects_mark / os64_dom_effects_release). */

static JSClassID animation_slot, effect_slot, timeline_slot;

#define WA_FRAME_MS 20
#define WA_PROPERTIES_MAX 256      /* properties one keyframes argument may name */
#define WA_KEYFRAMES_MAX 1024      /* keyframes one effect may hold */

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
};

typedef struct DWaAnimation DWaAnimation;
struct DWaAnimation {
    DValue target;                  /* first: the plain event target's entry, in the object */
    os64_dom_t *dom;
    DWaAnimation *prev, *next;      /* dom->wa_first..wa_last, in the order they were made */
    uint64_t serial;
    const os64_html_node_t *element;    /* the effect's target, held */
    JSValue effect;                 /* the KeyframeEffect object */
    JSValue hold;                   /* the animation itself while it is relevant (wa_relevant) */
    DWaData *data;
    char *id;
    /* The clock: NAN is unresolved. */
    double start, held, rate, pending_rate, previous;
    bool has_pending_rate, pending_play, pending_pause;
    /* The promises, each with its resolving functions; `finished_done` is
     * whether the current finished promise is resolved. */
    JSValue ready, ready_resolve, ready_reject;
    JSValue finished, finished_resolve, finished_reject;
    bool ready_done, finished_done, notify_queued;
    /* The clock as last published (wa_changed): the hold and start times,
     * the rate and whether a task is pending, each NAN-for-unresolved
     * compared as equal to itself. */
    double pub_held, pub_start, pub_rate;
    bool pub_pending, published;
};

/* An animation playback event waiting for the next update step (§ 4.4.18,
 * Animation events: the "pending animation event queue"), in the order it was queued. */
typedef struct DWaEvent DWaEvent;
struct DWaEvent {
    DWaEvent *next;
    JSValue animation;
    const char *type;
    double current, timeline, scheduled;
};

static DWaAnimation *wa_state(JSValueConst value)
{
    return JS_GetOpaque(value, os64_js_class_id(&animation_slot));
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

static double effect_end(const DWaAnimation *a)
{
    return garb_clock_at(&a->data->fx.timing, 0, false).end_time;
}

static double effective_rate(const DWaAnimation *a)
{
    return a->has_pending_rate ? a->pending_rate : a->rate;
}

static double current_time(DWaAnimation *a)
{
    if (!isnan(a->held)) return a->held;
    if (isnan(a->start)) return NAN;
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
    if (isnan(t)) {
        garb_clock_t none;
        os64_memset(&none, 0, sizeof(none));
        none.phase = GARB_PHASE_BEFORE;
        return none;
    }
    return garb_clock_at(&a->data->fx.timing, t, a->rate < 0);
}

/* Relevant (§ 4.5.6, Relevant animations): not idle, and current — in
 * its active phase, or before it on the way to it — or in effect. What
 * getAnimations answers, and what the binding keeps alive unreferenced. */
static bool wa_relevant(DWaAnimation *a)
{
    if (play_state(a) == WA_IDLE) return false;
    garb_clock_t k = wa_clock(a);
    if (k.in_effect || k.phase == GARB_PHASE_ACTIVE) return true;
    /* At a rate of 0 it is in neither phase's way: it never gets there. */
    return a->rate > 0 ? k.phase == GARB_PHASE_BEFORE : a->rate < 0 && k.phase == GARB_PHASE_AFTER;
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
    garb_effect_t *fx = &a->data->fx;
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
    a->dom->wa_index_stale = true;
}

static bool same_time(double x, double y)
{
    return (isnan(x) && isnan(y)) || x == y;
}

/* After a procedure: when the clock libflow reads moved (the hold or start
 * time, the rate, or a pending task begun or ended), it is published, the
 * target's style moves (os64_html_touch) and the index of effects is
 * rebuilt. A running animation the update step only looked at changed
 * nothing: libflow works where it is from the clock, so the page is not
 * laid out again for it. */
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
    wa_publish(a);
    if (a->element != NULL) (void)os64_html_touch(a->dom->document, (os64_html_node_t *)a->element);
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
    d_free(dom, d->parsed);
    d_free(dom, d->frames);
    d_free(dom, d->sets);
    d_free(dom, d);
}

/* A collected animation's data, kept while a tree laid out before now may
 * read it (os64_dom_effects_release). */
static void data_bury(os64_dom_t *dom, DWaData *d)
{
    if (d == NULL) return;
    d->died = dom->wa_generation;
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
    double offset, place;
    bool has_easing;
    garb_easing_t easing;
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
    k->offset = k->place = NAN;
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

/* A keyframe's `composite` (CompositeOperationOrAuto): checked and not
 * kept, as every effect of slice A replaces (WEB_ANIMATIONS.md § 4,
 * DEBTS). TypeError for a word that is none of them. */
static int composite_check(JSContext *ctx, JSValueConst v)
{
    static const char *const kWords[] = {"replace", "add", "accumulate", "auto"};
    int w;
    return JS_IsUndefined(v) ? 0 : enum_of(ctx, v, kWords, 4, &w);
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
        if (!member_is(name) && os64_strcopy(p->idl, sizeof(p->idl), name) < sizeof(p->idl) &&
            css_name(name, p->css, sizeof(p->css)) && garb_prop_known(p->css, os64_strlen(p->css), false)) {
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
    int r = JS_IsException(v) ? -1 : composite_check(ctx, v);
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
    double *offsets = NULL;
    int r = 0;
    for (int m = 0; r == 0 && m < 3; m++) {
        JSValue mv = JS_GetPropertyStr(ctx, obj, kMembers[m]);
        if (JS_IsException(mv)) r = -1;
        else if (!JS_IsUndefined(mv)) r = as_list(dom, ctx, mv, &items[m], &n[m]);
        JS_FreeValue(ctx, mv);
    }
    for (size_t i = 0; r == 0 && i < n[0]; i++) r = composite_check(ctx, items[0][i]);
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
    }
    js_free(ctx, easings);
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
 * longhands, so one that does not parse drops itself alone. A value
 * holding var() is held by libgarb (GARB_DECL_HELD) and dropped
 * (WEB_ANIMATIONS.md § 2, DEBTS). */
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
    size_t nsets = 0, capsets = 0;
    size_t *firsts = n > 0 ? js_malloc(ctx, (size_t)n * 2 * sizeof(size_t)) : NULL;
    if (n > 0 && firsts == NULL) {
        data_free(dom, d);
        return NULL;
    }
    for (int32_t i = 0; i < n; i++) {
        firsts[2 * i] = nsets;
        for (size_t at = 0; at < f->v[i].len;) {
            const char *piece = f->v[i].text + at;
            size_t len = os64_strlen(piece);
            at += len + 1;
            garb_parsed_t *p = &d->parsed[d->nparsed];
            if (garb_parse_block(piece, len, p) != GARB_OK) {
                garb_free(p);
                os64_memset(p, 0, sizeof(*p));
                continue;
            }
            d->nparsed++;
            for (int32_t j = 0; j < p->nitems; j++) {
                if (p->items[j].kind != GARB_ITEM_DECL || garb_decl_is_custom(&p->items[j].decl)) continue;
                garb_set_t sets[64];
                int32_t k = garb_read_declaration(p, &p->items[j].decl, (garb_reading_t){0}, sets);
                for (int32_t s = 0; s < k; s++) {
                    if (nsets == capsets) {
                        size_t cap = capsets ? capsets * 2 : 16;
                        garb_set_t *grown = d_alloc(dom, cap * sizeof(*grown));
                        if (grown == NULL) {
                            js_free(ctx, firsts);
                            data_free(dom, d);
                            return NULL;
                        }
                        if (nsets > 0) os64_memcpy(grown, d->sets, nsets * sizeof(*grown));
                        d_free(dom, d->sets);
                        d->sets = grown;
                        capsets = cap;
                    }
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
    js_free(ctx, firsts);
    d->fx.frames = d->frames;
    d->fx.nframes = n;
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
static int timing_of(JSContext *ctx, JSValueConst options, garb_timing_t *t, char **id)
{
    os64_memset(t, 0, sizeof(*t));
    t->iterations = 1;
    *id = NULL;
    if (JS_IsUndefined(options)) return 0;
    if (!JS_IsObject(options)) {
        double d;
        if (JS_ToFloat64(ctx, &d, options) < 0) return -1;
        if (isnan(d) || d < 0) {
            JS_ThrowTypeError(ctx, "A duration is a non-negative number");
            return -1;
        }
        t->duration_ms = d;
        return 0;
    }
    static const char *const kDirections[] = {"normal", "reverse", "alternate", "alternate-reverse"};
    static const char *const kFills[] = {"none", "forwards", "backwards", "both", "auto"};
    static const char *const kComposites[] = {"replace", "add", "accumulate"};
    static const char *const kIterationComposites[] = {"replace", "accumulate"};
    static const char *const kMembers[] = {"delay", "direction", "duration", "easing", "endDelay", "fill",
                                           "iterationStart", "iterations", "composite", "iterationComposite",
                                           "pseudoElement", "id"};
    for (int m = 0; m < (int)(sizeof(kMembers) / sizeof(kMembers[0])); m++) {
        JSValue v = JS_GetPropertyStr(ctx, options, kMembers[m]);
        if (JS_IsException(v)) return -1;
        int r = 0, w;
        if (!JS_IsUndefined(v)) {
            switch (m) {
            case 0: r = finite_of(ctx, v, &t->delay_ms, false); break;
            case 1:
                r = enum_of(ctx, v, kDirections, 4, &w);
                if (r == 0) t->direction = (garb_direction_t)w;
                break;
            case 2:
                if (JS_IsString(v)) {
                    const char *s = JS_ToCString(ctx, v);
                    r = s != NULL && os64_streq(s, "auto") ? 0 : -1;
                    if (s != NULL) JS_FreeCString(ctx, s);
                    if (r < 0) JS_ThrowTypeError(ctx, "A duration is a number or auto");
                } else {
                    r = JS_ToFloat64(ctx, &t->duration_ms, v);
                    if (r == 0 && (isnan(t->duration_ms) || t->duration_ms < 0)) {
                        JS_ThrowTypeError(ctx, "A duration is a non-negative number");
                        r = -1;
                    }
                }
                break;
            case 3: r = easing_of(ctx, v, &t->easing); break;
            case 4: r = finite_of(ctx, v, &t->end_delay_ms, false); break;
            case 5:
                r = enum_of(ctx, v, kFills, 5, &w);
                /* `auto` is none for a keyframe effect (§ 4.6.1, Fill modes). */
                if (r == 0) t->fill = w == 4 ? GARB_FILL_NONE : (garb_fill_t)w;
                break;
            case 6: r = finite_of(ctx, v, &t->iteration_start, true); break;
            case 7:
                r = JS_ToFloat64(ctx, &t->iterations, v);
                if (r == 0 && (isnan(t->iterations) || t->iterations < 0)) {
                    JS_ThrowTypeError(ctx, "Iterations are a non-negative number");
                    r = -1;
                }
                break;
            /* Checked and not kept, as a keyframe's composite is
             * (composite_check). */
            case 8: r = enum_of(ctx, v, kComposites, 3, &w); break;
            case 9: r = enum_of(ctx, v, kIterationComposites, 2, &w); break;
            case 10:
                if (!JS_IsNull(v)) {
                    const char *s = JS_ToCString(ctx, v);
                    if (s == NULL) {
                        r = -1;
                        break;
                    }
                    bool known = pseudo_known(s);
                    JS_FreeCString(ctx, s);
                    d_error(ctx, known ? "NotSupportedError" : "SyntaxError",
                            known ? "A pseudo-element's animations are not run here" : "Not a pseudo-element");
                    r = -1;
                }
                break;
            case 11: {
                size_t len;
                const char *s = JS_ToCStringLen(ctx, &len, v);
                if (s == NULL) r = -1;
                else {
                    *id = js_malloc(ctx, len + 1);
                    if (*id != NULL) os64_memcpy(*id, s, len + 1);
                    JS_FreeCString(ctx, s);
                    r = *id != NULL ? 0 : -1;
                }
                break;
            }
            }
        }
        JS_FreeValue(ctx, v);
        if (r < 0) {
            js_free(ctx, *id);
            *id = NULL;
            return -1;
        }
    }
    return 0;
}

/* ── The objects ─────────────────────────────────────────────────────── */

typedef struct {
    JSValue animation;
} DWaEffect;

static void effect_finalizer(JSRuntime *rt, JSValue value)
{
    DWaEffect *e = JS_GetOpaque(value, os64_js_class_id(&effect_slot));
    if (e == NULL) return;
    JS_FreeValueRT(rt, e->animation);
    js_free_rt(rt, e);
}

static void effect_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DWaEffect *e = JS_GetOpaque(value, os64_js_class_id(&effect_slot));
    if (e != NULL) JS_MarkValue(rt, e->animation, mark);
}

static void unlink_animation(os64_dom_t *dom, DWaAnimation *a)
{
    if (a->prev != NULL) a->prev->next = a->next;
    else dom->wa_first = a->next;
    if (a->next != NULL) a->next->prev = a->prev;
    else dom->wa_last = a->prev;
    a->prev = a->next = NULL;
    dom->wa_count--;
    dom->wa_index_stale = true;
}

/* An animation goes with the garbage once nothing holds it: neither a
 * script nor the binding, which holds one while it is live (wa_keep). Its data is buried, not freed: a tree may still read it. */
static void animation_finalizer(JSRuntime *rt, JSValue value)
{
    DWaAnimation *a = wa_state(value);
    if (a == NULL) return;
    os64_dom_t *dom = a->dom;
    if (dom != NULL) {
        unlink_animation(dom, a);
        data_bury(dom, a->data);
        if (a->element != NULL) os64_html_release(dom->document, a->element);
    }
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

/* A new animation of `element` by `data` (its timing set), idle: the
 * caller plays it. Takes `data` and `id`. */
static JSValue animation_new(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *element, DWaData *data,
                             char *id)
{
    JSClassID cid = os64_js_class_id(&animation_slot);
    JSValue object = JS_NewObjectClass(ctx, (int)cid);
    DWaAnimation *a = JS_IsException(object) ? NULL : js_mallocz(ctx, sizeof(*a));
    if (a == NULL) {
        JS_FreeValue(ctx, object);
        data_free(dom, data);
        js_free(ctx, id);
        return JS_EXCEPTION;
    }
    d_event_target_init(dom, &a->target, object);
    a->dom = dom;
    a->data = data;
    a->id = id;
    a->start = a->held = a->previous = NAN;
    a->rate = 1;
    a->effect = a->hold = JS_UNDEFINED;
    a->ready = a->ready_resolve = a->ready_reject = JS_UNDEFINED;
    a->finished = a->finished_resolve = a->finished_reject = JS_UNDEFINED;
    JS_SetOpaque(object, a);
    a->serial = ++dom->wa_serial;
    a->prev = dom->wa_last;
    if (dom->wa_last != NULL) dom->wa_last->next = a;
    else dom->wa_first = a;
    dom->wa_last = a;
    dom->wa_count++;
    dom->wa_index_stale = true;
    if (element != NULL) {
        os64_html_hold(dom->document, element);
        a->element = element;
    }
    /* A new animation's ready promise is resolved, its finished one
     * pending (an animation's initial state, § 4.4.7 and § 4.4.11). */
    if (promise_new(ctx, &a->ready, &a->ready_resolve, &a->ready_reject) < 0 ||
        promise_new(ctx, &a->finished, &a->finished_resolve, &a->finished_reject) < 0) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    promise_call(ctx, a->ready_resolve, object);
    a->ready_done = true;
    DWaEffect *e = js_mallocz(ctx, sizeof(*e));
    JSValue effect = e != NULL ? JS_NewObjectClass(ctx, (int)os64_js_class_id(&effect_slot)) : JS_EXCEPTION;
    if (JS_IsException(effect)) {
        js_free(ctx, e);
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    e->animation = JS_DupValue(ctx, object);
    JS_SetOpaque(effect, e);
    a->effect = effect;
    return object;
}

/* Whether an animation may still change what is drawn or fire: one with a
 * pending task, one running (on its way to its finish, which a listener may
 * hear, even through a stretch where it is not relevant: reversed into its
 * delay), or relevant. */
static bool wa_live(DWaAnimation *a)
{
    return a->pending_play || a->pending_pause || play_state(a) == WA_RUNNING || wa_relevant(a);
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

enum {
    A_ID, A_EFFECT, A_TIMELINE, A_START, A_CURRENT, A_RATE, A_STATE, A_REPLACE, A_PENDING, A_READY, A_FINISHED,
    A_CANCEL, A_FINISH, A_PLAY, A_PAUSE, A_UPDATE_RATE, A_REVERSE, E_TARGET, T_CURRENT, D_TIMELINE
};

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
    if (magic == E_TARGET) {
        DWaEffect *e = JS_GetOpaque(self, os64_js_class_id(&effect_slot));
        if (e == NULL) return JS_ThrowTypeError(ctx, "Expected a KeyframeEffect");
        DWaAnimation *a = wa_state(e->animation);
        return a != NULL && a->element != NULL ? d_wrap(dom, ctx, a->element) : JS_NULL;
    }
    DWaAnimation *a = wa_state(self);
    if (a == NULL) return JS_ThrowTypeError(ctx, "Expected an Animation");
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
    case A_EFFECT: return JS_DupValue(ctx, a->effect);
    case A_TIMELINE: return JS_DupValue(ctx, dom->wa_timeline->value);
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
    case A_REPLACE: return JS_NewString(ctx, "active");
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
    char *id = NULL;
    if (timing_of(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &timing, &id) < 0) return JS_EXCEPTION;
    WaFrames frames = {0};
    if (process_keyframes(dom, ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &frames) < 0) {
        frames_free(ctx, &frames);
        js_free(ctx, id);
        return JS_EXCEPTION;
    }
    DWaData *d = data_make(dom, ctx, &frames);
    frames_free(ctx, &frames);
    if (d == NULL) {
        js_free(ctx, id);
        return d_error(ctx, "QuotaExceededError", "No room for an animation's keyframes");
    }
    d->fx.timing = timing;
    JSValue animation = animation_new(dom, ctx, element, d, id);
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

/* getAnimations (§ 6.8 Animatable, § 6.10 DocumentOrShadowRoot): the relevant
 * animations of the element (of its subtree with `subtree`), or of the
 * document's connected elements, in composite order. */
static JSValue get_animations(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                              JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *root = NULL;
    bool subtree = false;
    if (magic == 0) {
        root = d_node(dom, ctx, self);
        if (root == NULL || root->kind != OS64_HTML_ELEMENT)
            return JS_ThrowTypeError(ctx, "getAnimations needs an element");
        if (argc > 0 && JS_IsObject(argv[0])) {
            JSValue v = JS_GetPropertyStr(ctx, argv[0], "subtree");
            if (JS_IsException(v)) return v;
            int b = JS_ToBool(ctx, v);
            JS_FreeValue(ctx, v);
            if (b < 0) return JS_EXCEPTION;
            subtree = b != 0;
        }
    }
    JSValue list = JS_NewArray(ctx);
    if (JS_IsException(list)) return list;
    uint32_t n = 0;
    for (DWaAnimation *a = dom->wa_first; a != NULL; a = a->next) {
        if (a->element == NULL || !wa_relevant(a)) continue;
        bool mine = magic == 0 ? (subtree ? within(a->element, root) : a->element == root) : d_connected(a->element);
        if (!mine) continue;
        if (JS_SetPropertyUint32(ctx, list, n++, JS_DupValue(ctx, a->target.value)) < 0) {
            JS_FreeValue(ctx, list);
            return JS_EXCEPTION;
        }
    }
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

/* ── The update step (HTML's "update animations and send events") ────── */

/* Whether the step has work: a queued event, a timeline a script read, an
 * animation with a pending task or running, or one the binding holds that
 * is no longer live (wa_live) and is to be let go. */
static bool wa_busy(os64_dom_t *dom)
{
    if (dom->wa_events != NULL || dom->wa_wanted) return true;
    for (DWaAnimation *a = dom->wa_first; a != NULL; a = a->next) {
        if (a->pending_play || a->pending_pause || play_state(a) == WA_RUNNING) return true;
        if (!JS_IsUndefined(a->hold) && !wa_live(a)) return true;
    }
    return false;
}

uint64_t os64_dom_web_animations_next(os64_dom_t *dom)
{
    if (dom == NULL || dom->closed || dom->wa_running || !wa_busy(dom)) return UINT64_MAX;
    return dom->wa_stepped ? dom->wa_last_step + WA_FRAME_MS : 0;
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
    wa_sample(dom);
    dom->wa_wanted = false;
    dom->wa_stepped = true;
    dom->wa_last_step = now;
    char name[OS64_JS_SOURCE_NAME_CAP];
    os64_snprintf(name, sizeof(name), "%.*s#web-animations", (int)(sizeof(name) - 20),
                  dom->url ? dom->url : "about:blank");
    dom->wa_running = true;
    if (os64_js_task_begin(dom->runtime, OS64_JS_ABI_ID, name, out) == OS64_JS_OK) {
        os64_js_outcome_t access;
        JSContext *ctx = os64_js_context(dom->runtime, OS64_JS_ABI_ID, &access);
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
            if (a->pending_play || a->pending_pause) run_pending(ctx, a, dom->wa_time);
            else if (play_state(a) != WA_IDLE) update_finished(ctx, a, false, false);
        }
        if (ctx != NULL) d_task_settle(dom, out);
        DWaEvent *events = events_sorted(dom->wa_events);
        dom->wa_events = dom->wa_events_last = NULL;
        while (events != NULL) {
            DWaEvent *e = events;
            events = e->next;
            DWaAnimation *a = wa_state(e->animation);
            if (ctx != NULL && a != NULL) {
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
            JS_FreeValueRT(dom->engine, e->animation);
            d_free(dom, e);
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

/* The effects by target, rebuilt when an animation changed: (element,
 * effect) pairs sorted by element, each element's in composite order (the
 * order they were made). An idle animation is handed to no layout. */
static void index_rebuild(os64_dom_t *dom)
{
    d_free(dom, dom->wa_index_nodes);
    d_free(dom, dom->wa_index_fx);
    dom->wa_index_nodes = NULL;
    dom->wa_index_fx = NULL;
    dom->wa_index_n = 0;
    dom->wa_index_stale = false;
    size_t n = 0;
    for (DWaAnimation *a = dom->wa_first; a != NULL; a = a->next)
        n += a->element != NULL && play_state(a) != WA_IDLE;
    if (n == 0) return;
    dom->wa_index_nodes = d_alloc(dom, n * sizeof(*dom->wa_index_nodes));
    dom->wa_index_fx = d_alloc(dom, n * sizeof(*dom->wa_index_fx));
    if (dom->wa_index_nodes == NULL || dom->wa_index_fx == NULL) {
        d_free(dom, dom->wa_index_nodes);
        d_free(dom, dom->wa_index_fx);
        dom->wa_index_nodes = NULL;
        dom->wa_index_fx = NULL;
        dom->wa_index_stale = true;
        return;
    }
    size_t k = 0;
    for (DWaAnimation *a = dom->wa_first; a != NULL; a = a->next) {
        if (a->element == NULL || play_state(a) == WA_IDLE) continue;
        /* Insertion by element; the list's order keeps each element's
         * effects in the order they were made. */
        size_t at = k;
        while (at > 0 && (uintptr_t)dom->wa_index_nodes[at - 1] > (uintptr_t)a->element) {
            dom->wa_index_nodes[at] = dom->wa_index_nodes[at - 1];
            dom->wa_index_fx[at] = dom->wa_index_fx[at - 1];
            at--;
        }
        dom->wa_index_nodes[at] = a->element;
        dom->wa_index_fx[at] = &a->data->fx;
        k++;
    }
    dom->wa_index_n = k;
}

int32_t os64_dom_script_effects(os64_dom_t *dom, const os64_html_node_t *node, const struct garb_effect *const **out)
{
    *out = NULL;
    if (dom == NULL || dom->wa_count == 0) return 0;
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
    return dom != NULL ? ++dom->wa_generation : 0;
}

void os64_dom_effects_release(os64_dom_t *dom, uint64_t oldest)
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

bool os64_dom_effects_held(const os64_dom_t *dom)
{
    return dom != NULL && (dom->wa_dead != NULL || dom->wa_first != NULL);
}

/* ── The binding's lifetime ──────────────────────────────────────────── */

void d_web_animations_drain(os64_dom_t *dom)
{
    while (dom->wa_events != NULL) {
        DWaEvent *e = dom->wa_events;
        dom->wa_events = e->next;
        JS_FreeValueRT(dom->engine, e->animation);
        d_free(dom, e);
    }
    dom->wa_events_last = NULL;
    /* The binding's holds first: an animation they alone kept goes now,
     * and finalizes against a live binding. */
    for (DWaAnimation *a = dom->wa_first; a != NULL;) {
        DWaAnimation *next = a->next;
        if (!JS_IsUndefined(a->hold)) {
            JSValue h = a->hold;
            a->hold = JS_UNDEFINED;
            JS_FreeValueRT(dom->engine, h);
        }
        a = next;
    }
    /* What a script still holds outlives the binding: it is cut loose and
     * its data buried, as a tree laid out with it may still be painted
     * until the binding is freed (d_web_animations_free). */
    while (dom->wa_first != NULL) {
        DWaAnimation *a = dom->wa_first;
        unlink_animation(dom, a);
        data_bury(dom, a->data);
        a->data = NULL;
        if (a->element != NULL) os64_html_release(dom->document, a->element);
        a->element = NULL;
        a->dom = NULL;
    }
}

void d_web_animations_free(os64_dom_t *dom)
{
    os64_dom_effects_release(dom, UINT64_MAX);
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
    static const JSClassDef effect_class = {.class_name = "KeyframeEffect", .finalizer = effect_finalizer,
                                            .gc_mark = effect_mark};
    static const JSClassDef timeline_class = {.class_name = "DocumentTimeline"};
    JSClassID aid = os64_js_class_id(&animation_slot), eid = os64_js_class_id(&effect_slot),
              tid = os64_js_class_id(&timeline_slot);
    if ((!JS_IsRegisteredClass(dom->engine, aid) && JS_NewClass(dom->engine, aid, &animation_class) < 0) ||
        (!JS_IsRegisteredClass(dom->engine, eid) && JS_NewClass(dom->engine, eid, &effect_class) < 0) ||
        (!JS_IsRegisteredClass(dom->engine, tid) && JS_NewClass(dom->engine, tid, &timeline_class) < 0))
        return -1;
    DValue *ap = d_retain(dom, ctx, JS_NewObject(ctx));
    DValue *ep = ap != NULL ? d_retain(dom, ctx, JS_NewObject(ctx)) : NULL;
    DValue *atp = ep != NULL ? d_retain(dom, ctx, JS_NewObject(ctx)) : NULL;
    if (atp == NULL) return -1;
    JSValue dtp_value = JS_NewObjectProto(ctx, atp->value);
    DValue *dtp = d_retain(dom, ctx, dtp_value);
    if (dtp == NULL) return -1;
    JS_SetClassProto(ctx, aid, JS_DupValue(ctx, ap->value));
    JS_SetClassProto(ctx, eid, JS_DupValue(ctx, ep->value));
    JS_SetClassProto(ctx, tid, JS_DupValue(ctx, dtp->value));
    static const struct { const char *name; int magic; bool writable; } kAccessors[] = {
        {"id", A_ID, true}, {"effect", A_EFFECT, false}, {"timeline", A_TIMELINE, false},
        {"startTime", A_START, true}, {"currentTime", A_CURRENT, true}, {"playbackRate", A_RATE, true},
        {"playState", A_STATE, false}, {"replaceState", A_REPLACE, false}, {"pending", A_PENDING, false},
        {"ready", A_READY, false}, {"finished", A_FINISHED, false},
    };
    static const struct { const char *name; int magic, length; } kMethods[] = {
        {"cancel", A_CANCEL, 0}, {"finish", A_FINISH, 0}, {"play", A_PLAY, 0}, {"pause", A_PAUSE, 0},
        {"updatePlaybackRate", A_UPDATE_RATE, 1}, {"reverse", A_REVERSE, 0},
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
        d_accessor(dom, ctx, ep->value, "target", animation_member, E_TARGET, false) < 0 ||
        d_accessor(dom, ctx, atp->value, "currentTime", animation_member, T_CURRENT, false) < 0)
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
    /* The interfaces: Animation and KeyframeEffect are made by animate()
     * alone in this slice (their constructors are slice B's,
     * WEB_ANIMATIONS.md § 4), DocumentTimeline by the document. */
    JSValue event_prototype = JS_GetClassProto(ctx, dom->event_class);
    JSValue event_interface = JS_GetPropertyStr(ctx, global, "Event");
    JSValue playback_proto = JS_NewObjectProto(ctx, event_prototype);
    JS_FreeValue(ctx, event_prototype);
    dom->wa_event_prototype = d_retain(dom, ctx, playback_proto);
    if (dom->wa_event_prototype == NULL || JS_IsException(event_interface)) {
        JS_FreeValue(ctx, event_interface);
        return -1;
    }
    JSValue timeline_interface = d_interface_new(ctx, "AnimationTimeline", 0, NULL, atp->value, JS_UNDEFINED);
    struct { const char *name; int length; JSCFunction *construct; JSValueConst proto, parent; } kInterfaces[] = {
        {"Animation", 0, NULL, ap->value, JS_UNDEFINED},
        {"KeyframeEffect", 0, NULL, ep->value, JS_UNDEFINED},
        {"DocumentTimeline", 0, NULL, dtp->value, timeline_interface},
        {"AnimationPlaybackEvent", 1, playback_construct, dom->wa_event_prototype->value, event_interface},
    };
    int result = JS_IsException(timeline_interface) ? -1 : 0;
    if (result == 0 && JS_DefinePropertyValueStr(ctx, global, "AnimationTimeline", JS_DupValue(ctx, timeline_interface),
                                                 JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        result = -1;
    for (size_t i = 0; result == 0 && i < sizeof(kInterfaces) / sizeof(kInterfaces[0]); i++) {
        JSValue f = d_interface_new(ctx, kInterfaces[i].name, kInterfaces[i].length, kInterfaces[i].construct,
                                    kInterfaces[i].proto, kInterfaces[i].parent);
        if (JS_IsException(f) ||
            JS_DefinePropertyValueStr(ctx, global, kInterfaces[i].name, f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
            result = -1;
    }
    JS_FreeValue(ctx, timeline_interface);
    JS_FreeValue(ctx, event_interface);
    return result;
}
