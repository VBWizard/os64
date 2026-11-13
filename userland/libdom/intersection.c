#include "internal.h"
#include "os64/fmt.h"
#include <math.h>

/* INTERSECTIONOBSERVER (Intersection Observer, W3C; INTERSECTION_OBSERVER.md).
 *
 * The binding keeps the observers, their targets and their entries, and does
 * the standard's arithmetic; the host answers the geometry. At its
 * rendering step the host calls os64_dom_intersections_update, which hands
 * each measurement document its observed (target, root) pairs in one batch
 * and runs "update intersection observations" over the answers: the root intersection
 * rectangle grown by rootMargin, the target's box cut by the clips between
 * it and the root and then by that rectangle, edge-inclusively, the ratio,
 * the threshold index, and an entry wherever the index or the intersecting
 * state moved. Nothing there enters the engine: the entries are records,
 * made IntersectionObserverEntry objects when they are handed over, at the
 * "notify intersection observers" task (os64_dom_intersections_fire) or by
 * takeRecords().
 *
 * An observer stays alive while it observes a target or has entries
 * waiting: then the binding holds its object strongly, and lets go when it
 * has neither. An observer whose object the engine collects is marked dead
 * and swept at the next step or task, never while a walk of the list is
 * under way: a callback may collect another observer, and its C state must
 * outlive the walk that would visit it. */

#define D_IO_BUCKETS 256

typedef struct DTarget {
    /* The observer's targets in the order observe() was called on them,
     * and the chain of the node's bucket. */
    struct DTarget *prev, *next, *bucket_next;
    struct DIObserver *observer;
    const os64_html_node_t *node;   /* held */
    /* The registration's state: -1 before the first observation. */
    int32_t previous_index;
    bool previous_intersecting;
    /* lastUpdateTime, for `delay`; `updated` once there is one. */
    double last_update;
    bool updated;
} DTarget;

typedef struct DEntry {
    struct DEntry *next;
    const os64_html_node_t *target; /* held */
    double time, ratio;
    double root[4], rect[4], inter[4];
    bool intersecting;
} DEntry;

typedef struct DIObserver {
    struct DIObserver *prev, *next;
    os64_dom_t *dom;
    /* The object while it lives (its finalizer clears it, and marks the
     * observer `dead`), and whether the binding holds it. */
    JSValue object;
    bool held, dead;
    JSValue callback, thresholds_array;
    uint32_t script;
    os64_dom_t *report_owner;
    /* The root as given: NULL for none, else an element or a document,
     * held. */
    const os64_html_node_t *root;
    /* rootMargin and scrollMargin: top, right, bottom, left, each pixels or
     * a percentage. The scroll margin is kept and reported; its clipping
     * is booked (INTERSECTION_OBSERVER.md § 6). */
    double margin[4], scroll_margin[4];
    bool percent[4], scroll_percent[4];
    double *thresholds;
    size_t nthresholds;
    int32_t delay;
    bool track_visibility;
    DTarget *targets, *targets_last;
    size_t ntargets;
    DEntry *first, *last;
} DIObserver;

struct DIntersections {
    JSClassID observer_class, entry_class;
    /* Every observer the page made and has not lost, in the order it made
     * them: what the step and the notify task walk. */
    DIObserver *observers, *observers_last;
    DTarget *buckets[D_IO_BUCKETS];
    size_t observed;
    /* Targets waiting for their first observation. */
    size_t owed;
    /* The notify task is queued; an observer has died since the last
     * sweep; the notify is walking the observers, and no sweep may run. */
    bool queued, has_dead, walking;
    /* Observations not made, or entries not kept, for want of memory:
     * reported at the next notify, which a loss queues. */
    uint64_t lost;
    /* The budget refused the last step its batch, and the loss is said; a
     * step refused again before one is had says nothing more. */
    bool starved;
    /* A required measurement provider or batch allocation refused. */
    bool undone;
    /* When, on the host's clock (options.now_ms), the first target the
     * last step passed over for its observer's `delay` may be observed:
     * the step is owed then whatever moves. UINT64_MAX for none. */
    uint64_t delayed_due;
    DValue *entry_prototype;
    os64_dom_intersection_t *batch;
    size_t batch_cap;
};

static JSClassID observer_slot, entry_slot;

static size_t node_bucket(const os64_html_node_t *node)
{
    return ((uintptr_t)node >> 4) % D_IO_BUCKETS;
}

/* ── Holding ─────────────────────────────────────────────────────────── */

/* The binding holds the object exactly while the observer observes a
 * target or has entries waiting. */
static void hold_update(os64_dom_t *dom, JSContext *ctx, DIObserver *o)
{
    bool want = !o->dead && JS_IsObject(o->object) && (o->ntargets != 0 || o->first != NULL);
    if (want == o->held) return;
    o->held = want;
    if (want) {
        dom->intersections_held++;
        if (ctx != NULL) JS_DupValue(ctx, o->object);
        else JS_DupValueRT(dom->engine, o->object);
    } else {
        dom->intersections_held--;
        JS_FreeValueRT(dom->engine, o->object);
    }
}

/* ── Entries ─────────────────────────────────────────────────────────── */

static void entries_clear(os64_dom_t *dom, DIObserver *o)
{
    while (o->first != NULL) {
        DEntry *next = o->first->next;
        os64_html_release(dom->document, o->first->target);
        d_free(dom, o->first);
        o->first = next;
    }
    o->last = NULL;
}

/* ── Targets ─────────────────────────────────────────────────────────── */

static DTarget *target_find(struct DIntersections *s, const DIObserver *o, const os64_html_node_t *node)
{
    for (DTarget *t = s->buckets[node_bucket(node)]; t != NULL; t = t->bucket_next)
        if (t->node == node && t->observer == o) return t;
    return NULL;
}

static void target_remove(os64_dom_t *dom, DTarget *t)
{
    struct DIntersections *s = dom->intersections;
    DIObserver *o = t->observer;
    for (DTarget **link = &s->buckets[node_bucket(t->node)]; *link != NULL; link = &(*link)->bucket_next)
        if (*link == t) {
            *link = t->bucket_next;
            break;
        }
    if (t->prev != NULL) t->prev->next = t->next;
    else o->targets = t->next;
    if (t->next != NULL) t->next->prev = t->prev;
    else o->targets_last = t->prev;
    o->ntargets--;
    s->observed--;
    if (t->previous_index < 0) s->owed--;
    /* With nothing observed no step runs, so no delay can be owed one. */
    if (s->observed == 0) s->delayed_due = UINT64_MAX;
    os64_html_release(dom->document, t->node);
    d_free(dom, t);
}

static void targets_clear(os64_dom_t *dom, DIObserver *o)
{
    while (o->targets != NULL) target_remove(dom, o->targets);
}

/* ── The dead ────────────────────────────────────────────────────────── */

static void observer_unlink_free(os64_dom_t *dom, DIObserver *o)
{
    struct DIntersections *s = dom->intersections;
    targets_clear(dom, o);
    entries_clear(dom, o);
    if (o->root != NULL) os64_html_release(dom->document, o->root);
    if (o->prev != NULL) o->prev->next = o->next;
    else s->observers = o->next;
    if (o->next != NULL) o->next->prev = o->prev;
    else s->observers_last = o->prev;
    d_free(dom, o->thresholds);
    d_free(dom, o);
}

/* Observers whose objects are gone, freed: only between walks. The step,
 * the notify and every new observer sweep, so a page that makes and drops
 * observers holds no more of them than it has made since its last. */
static void sweep(os64_dom_t *dom)
{
    struct DIntersections *s = dom->intersections;
    if (!s->has_dead || s->walking) return;
    s->has_dead = false;
    for (DIObserver *o = s->observers; o != NULL;) {
        DIObserver *next = o->next;
        if (o->dead) observer_unlink_free(dom, o);
        o = next;
    }
}

/* ── The step ────────────────────────────────────────────────────────── */

void os64_dom_set_intersections(os64_dom_t *dom, os64_dom_intersections_provider_t provider, void *opaque)
{
    if (dom == NULL || dom->closed || dom->departure_finishing || dom->departure_finished) return;
    dom->intersections_provider = provider;
    dom->intersections_opaque = opaque;
}

size_t os64_dom_intersections_observed(const os64_dom_t *dom)
{
    return dom != NULL && !dom->closed && dom->intersections != NULL ? dom->intersections->observed : 0;
}

static uint64_t host_now(const os64_dom_t *dom)
{
    return d_host_now_ms(dom);
}

static os64_dom_t *host_root(os64_dom_t *dom, const DIObserver *o, const os64_html_node_t **out);

/* A retained observation can resume after its explicit root is adopted into
 * an active document. Until then it must not keep the old provider awake. */
static bool observation_active(const os64_dom_t *dom, const DIObserver *o)
{
    if (o->dead || o->ntargets == 0) return false;
    const os64_html_node_t *root;
    os64_dom_t *owner = host_root((os64_dom_t *)dom, o, &root);
    return owner == NULL || d_delivery_active(owner);
}

uint64_t os64_dom_intersections_next(const os64_dom_t *dom)
{
    if (!d_delivery_active(dom) || dom->intersections == NULL) return UINT64_MAX;
    for (DIObserver *o = dom->intersections->observers; o != NULL; o = o->next)
        if (observation_active(dom, o)) return dom->intersections->delayed_due;
    return UINT64_MAX;
}

bool os64_dom_intersections_owed(const os64_dom_t *dom)
{
    if (!d_delivery_active(dom) || dom->intersections == NULL) return false;
    const struct DIntersections *s = dom->intersections;
    for (DIObserver *o = s->observers; o != NULL; o = o->next) {
        if (!observation_active(dom, o)) continue;
        if (s->undone) return true;
        if (s->delayed_due != UINT64_MAX) {
            uint64_t now = host_now(dom);
            if (!d_delivery_active(dom)) return false;
            if (now >= s->delayed_due) return true;
        }
        for (DTarget *t = o->targets; t != NULL; t = t->next)
            if (t->previous_index < 0) return true;
    }
    return false;
}

bool os64_dom_intersections_due(const os64_dom_t *dom)
{
    return d_delivery_active(dom) && dom->intersections != NULL && dom->intersections->queued;
}

enum { L, T, R, B };

/* The edge-inclusive meet of two rectangles into `out`: false when they
 * neither overlap nor touch. */
static bool meet(const double a[4], const double b[4], double out[4])
{
    double m[4] = {a[L] > b[L] ? a[L] : b[L], a[T] > b[T] ? a[T] : b[T],
                   a[R] < b[R] ? a[R] : b[R], a[B] < b[B] ? a[B] : b[B]};
    if (!(m[L] <= m[R] && m[T] <= m[B])) return false;
    for (int i = 0; i < 4; i++) out[i] = m[i];
    return true;
}

static double area(const double r[4])
{
    return (r[R] - r[L]) * (r[B] - r[T]);
}

/* The root intersection rectangle grown by rootMargin: a percentage of the
 * undilated rectangle's height on the top and bottom, of its width on the
 * left and right, as other browsers resolve it, and held to layout's 1/64
 * of a pixel, so margins that sum to the whole leave a rectangle of no
 * height, and not one a rounding turned inside out (WPT's
 * root-margin-rounding). */
static void root_bounds(const DIObserver *o, const double plain[4], double out[4])
{
    double width = plain[R] - plain[L], height = plain[B] - plain[T];
    double grow[4];
    for (int i = 0; i < 4; i++)
        grow[i] = o->percent[i] ? floor(o->margin[i] * ((i == 0 || i == 2) ? height : width) / 100.0 * 64 + 0.5) / 64
                                : o->margin[i];
    out[L] = plain[L] - grow[3];
    out[T] = plain[T] - grow[0];
    out[R] = plain[R] + grow[1];
    out[B] = plain[B] + grow[2];
}

/* The measurement binding and host root: the implicit root keeps the
 * observer's window; an explicit root follows its current document. A
 * windowless document cannot supply a viewport. `out` is NULL for the
 * viewport, or the explicit element. */
static os64_dom_t *host_root(os64_dom_t *dom, const DIObserver *o, const os64_html_node_t **out)
{
    *out = NULL;
    if (o->root == NULL) return dom;
    dom = d_owner(dom, o->root);
    if (!d_window_document(dom, o->root)) return NULL;
    if (o->root->kind != OS64_HTML_DOCUMENT) *out = o->root;
    return dom;
}

static void queue_entry(os64_dom_t *dom, DIObserver *o, const DTarget *t, double time, const double root[4],
                        const double rect[4], const double inter[4], bool intersecting, double ratio)
{
    struct DIntersections *s = dom->intersections;
    DEntry *e = d_alloc_kind(dom, sizeof(*e), OS64_DOM_MEM_INTERSECTION);
    if (e == NULL) {
        s->lost++;
        return;
    }
    *e = (DEntry){.target = t->node, .time = time, .ratio = ratio, .intersecting = intersecting};
    for (int i = 0; i < 4; i++) {
        e->root[i] = root[i];
        e->rect[i] = rect[i];
        e->inter[i] = inter[i];
    }
    os64_html_hold(dom->document, t->node);
    if (o->last != NULL) o->last->next = e;
    else o->first = e;
    o->last = e;
}

/* One target's observation against its answer `a` (NULL when the root
 * cannot be met): the standard's steps 4 to 21. True when it queued an
 * entry. */
static bool observe_one(os64_dom_t *dom, DIObserver *o, DTarget *t, const os64_dom_intersection_t *a,
                        const double viewport[4], double time)
{
    struct DIntersections *s = dom->intersections;
    if (o->delay > 0 && t->updated && time - t->last_update < o->delay) {
        /* Passed over: owed again once its delay has run, moved or not. */
        double wait = o->delay - (time - t->last_update);
        uint64_t due = host_now(dom) + (uint64_t)ceil(wait);
        if (!d_delivery_active(dom)) return false;
        if (due < s->delayed_due) s->delayed_due = due;
        return false;
    }
    t->last_update = time;
    t->updated = true;
    double plain[4] = {0, 0, 0, 0}, root[4];
    bool root_ok = true;
    if (a != NULL && a->root != NULL) {
        root_ok = a->root_boxed;
        if (root_ok)
            for (int i = 0; i < 4; i++) plain[i] = a->root_rect[i];
    } else if (a != NULL) {
        for (int i = 0; i < 4; i++) plain[i] = viewport[i];
    }
    root_bounds(o, plain, root);
    double rect[4] = {0, 0, 0, 0}, inter[4] = {0, 0, 0, 0};
    bool meets = false;
    if (a != NULL && a->boxed && a->contained && root_ok) {
        for (int i = 0; i < 4; i++) rect[i] = a->rect[i];
        double clipped[4];
        meets = meet(rect, a->clip, clipped) && meet(clipped, root, inter);
        if (!meets)
            for (int i = 0; i < 4; i++) inter[i] = 0;
    }
    /* A target with no box reports every rectangle zero, rootBounds too, as
     * other browsers do (WPT's display-none), though the standard's text
     * would keep the root's. */
    if (a == NULL || !a->boxed)
        for (int i = 0; i < 4; i++) root[i] = 0;
    double target_area = area(rect);
    double ratio = target_area > 0 ? area(inter) / target_area : meets ? 1 : 0;
    /* The index counts the thresholds the ratio has reached, and is 0 for a
     * target that meets nothing; intersecting is an index above 0, so a
     * ratio short of the smallest threshold is not intersecting though the
     * rectangles meet. That is other browsers' answer to the standard's
     * issue 432 (WPT's initial-observation-with-threshold and
     * isIntersecting-threshold); the rectangle and the ratio still say
     * what meets. */
    /* The thresholds are sorted (the constructor's): the count of those
     * the ratio reached is an upper-bound search, not a walk of them all
     * for every target every step. */
    size_t lo = 0, hi = meets ? o->nthresholds : 0;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (o->thresholds[mid] <= ratio) lo = mid + 1;
        else hi = mid;
    }
    int32_t index = (int32_t)lo;
    bool intersecting = index > 0;
    bool queued = false;
    if (index != t->previous_index || intersecting != t->previous_intersecting) {
        queue_entry(dom, o, t, time, root, rect, inter, intersecting, ratio);
        queued = true;
    }
    if (t->previous_index < 0) s->owed--;
    t->previous_index = index;
    t->previous_intersecting = intersecting;
    return queued;
}

bool os64_dom_intersections_update(os64_dom_t *dom)
{
    struct DIntersections *s = d_delivery_active(dom) ? dom->intersections : NULL;
    if (s == NULL) return false;
    sweep(dom);
    if (s->observed == 0) return false;
    s->undone = true;
    /* The batch: pairs grouped by the root's measurement document. */
    if (s->batch_cap < s->observed) {
        os64_dom_intersection_t *grown = d_alloc_kind(dom, s->observed * sizeof(*grown), OS64_DOM_MEM_INTERSECTION);
        if (grown == NULL) {
            /* No room to ask: every observation this step would have made
             * is lost, and the first refusal queues the notify that says so
             * as the page's quota. Its targets stay owed, so the host steps
             * again whenever it runs; a refusal after the first, before a
             * batch is had, has nothing new to say and queues nothing, or a
             * page at its budget would be stepped and told without end. */
            if (s->starved) return false;
            s->starved = true;
            s->lost += s->observed;
            s->queued = true;
            return true;
        }
        d_free(dom, s->batch);
        s->batch = grown;
        s->batch_cap = s->observed;
    }
    s->starved = false;
    struct {
        os64_dom_t *owner;
        size_t at;
        double viewport[4];
    } groups[D_REALMS] = {0};
    size_t members = dom->cluster != NULL ? dom->cluster->count : 1;
    size_t n = 0;
    for (size_t i = 0; i < members; i++) {
        os64_dom_t *owner = dom->cluster != NULL ? dom->cluster->members[i] : dom;
        groups[i].owner = owner;
        groups[i].at = n;
        if (!d_delivery_active(owner)) continue;
        for (DIObserver *o = s->observers; o != NULL; o = o->next) {
            const os64_html_node_t *root;
            if (o->dead || host_root(dom, o, &root) != owner) continue;
            for (DTarget *t = o->targets; t != NULL; t = t->next)
                if (d_window_document(owner, t->node))
                    s->batch[n++] = (os64_dom_intersection_t){.target = t->node, .root = root};
        }
        size_t count = n - groups[i].at;
        if (count == 0) continue;
        os64_dom_geometry_stats_t cost = {0};
        bool answered = false;
        if (owner->intersections_provider != NULL && d_provider_enter(owner)) {
            answered = owner->intersections_provider(owner->intersections_opaque, s->batch + groups[i].at, count, &cost);
            d_provider_leave(owner);
        }
        owner->geometry_stats.layouts += cost.layouts;
        owner->geometry_stats.elapsed_us += cost.elapsed_us;
        if (!d_delivery_active(dom)) return false;
        if (!d_delivery_active(owner)) continue;
        if (!answered) return false;
        os64_dom_view_t view = {0};
        if (owner->view.read != NULL && d_provider_enter(owner)) {
            owner->view.read(owner->view.opaque, &view);
            d_provider_leave(owner);
        }
        if (!d_delivery_active(dom)) return false;
        if (!d_delivery_active(owner)) continue;
        groups[i].viewport[2] = view.width;
        groups[i].viewport[3] = view.height;
    }
    /* Providers run no script: roots and target order are stable across
     * gathering and this original-order walk. Commit observations after
     * every required provider has answered, so refusal advances no target. */
    s->undone = false;
    double time = d_performance_now(dom);
    if (!d_delivery_active(dom)) return false;
    s->delayed_due = UINT64_MAX;
    bool queued = false;
    const double empty_viewport[4] = {0};
    for (DIObserver *o = s->observers; o != NULL; o = o->next) {
        const os64_html_node_t *root;
        if (o->dead) continue;
        os64_dom_t *owner = host_root(dom, o, &root);
        if (owner != NULL && !d_delivery_active(owner)) continue;
        size_t i = 0;
        while (i < members && groups[i].owner != owner) i++;
        const double *viewport = i < members ? groups[i].viewport : empty_viewport;
        for (DTarget *t = o->targets; t != NULL; t = t->next) {
            bool asked = i < members && d_window_document(owner, t->node);
            queued = observe_one(dom, o, t, asked ? &s->batch[groups[i].at++] : NULL, viewport, time) || queued;
            if (!d_delivery_active(dom)) return false;
        }
        hold_update(dom, NULL, o);
    }
    if (queued) s->queued = true;
    return s->queued;
}

/* ── Delivery ────────────────────────────────────────────────────────── */

enum {
    E_TIME, E_ROOT_BOUNDS, E_BOUNDING, E_INTERSECTION, E_IS_INTERSECTING, E_IS_VISIBLE, E_RATIO, E_TARGET,
    E_FIELDS
};
static const char *const kEntryFields[E_FIELDS] = {
    "time", "rootBounds", "boundingClientRect", "intersectionRect", "isIntersecting", "isVisible",
    "intersectionRatio", "target",
};

typedef struct {
    JSValue fields[E_FIELDS];
} DEntryObject;

static void entry_finalizer(JSRuntime *rt, JSValue value)
{
    DEntryObject *e = JS_GetOpaque(value, os64_js_class_id(&entry_slot));
    if (e == NULL) return;
    for (int i = 0; i < E_FIELDS; i++) JS_FreeValueRT(rt, e->fields[i]);
    js_free_rt(rt, e);
}

static void entry_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DEntryObject *e = JS_GetOpaque(value, os64_js_class_id(&entry_slot));
    if (e == NULL) return;
    for (int i = 0; i < E_FIELDS; i++) JS_MarkValue(rt, e->fields[i], mark);
}

static JSValue rect_value(os64_dom_t *dom, JSContext *ctx, const double r[4])
{
    return d_rect_new(dom, ctx, true, r[L], r[T], r[R] - r[L], r[B] - r[T]);
}

/* An entry object of `fields`, which it takes, whatever happens: JS_EXCEPTION
 * when one of them is one or it could not be made. */
static JSValue entry_object(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype, JSValue fields[E_FIELDS])
{
    bool made = true;
    for (int i = 0; i < E_FIELDS; i++) made = made && !JS_IsException(fields[i]);
    JSValue object = made ? JS_NewObjectProtoClass(ctx, prototype, dom->intersections->entry_class) : JS_EXCEPTION;
    DEntryObject *e = !JS_IsException(object) ? js_mallocz(ctx, sizeof(*e)) : NULL;
    if (e == NULL) {
        for (int i = 0; i < E_FIELDS; i++) JS_FreeValue(ctx, fields[i]);
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    for (int i = 0; i < E_FIELDS; i++) e->fields[i] = fields[i];
    JS_SetOpaque(object, e);
    return object;
}

static JSValue record_object(os64_dom_t *dom, JSContext *ctx, const DEntry *r)
{
    JSValue fields[E_FIELDS] = {
        JS_NewFloat64(ctx, r->time), rect_value(dom, ctx, r->root), rect_value(dom, ctx, r->rect),
        rect_value(dom, ctx, r->inter), JS_NewBool(ctx, r->intersecting), JS_FALSE,
        JS_NewFloat64(ctx, r->ratio), d_wrap(dom, ctx, r->target),
    };
    return entry_object(dom, ctx, dom->intersections->entry_prototype->value, fields);
}

/* An observer's queue as an array of entries, and the queue emptied,
 * whatever happens. */
static JSValue take_records(os64_dom_t *dom, JSContext *ctx, DIObserver *o)
{
    JSValue array = JS_NewArray(ctx);
    uint32_t i = 0;
    for (DEntry *r = o->first; r != NULL && !JS_IsException(array); r = r->next) {
        JSValue entry = record_object(dom, ctx, r);
        if (JS_IsException(entry) || JS_DefinePropertyValueUint32(ctx, array, i++, entry, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, array);
            array = JS_EXCEPTION;
        }
    }
    entries_clear(dom, o);
    return array;
}

/* Entries lost for want of memory are said once, as the page's failure. */
static void report_lost(os64_dom_t *dom, JSContext *ctx)
{
    struct DIntersections *s = dom->intersections;
    if (s->lost == 0) return;
    char text[112];
    os64_snprintf(text, sizeof(text), "%llu intersection observations were lost: the DOM quota was exceeded",
                  (unsigned long long)s->lost);
    s->lost = 0;
    d_error(ctx, "QuotaExceededError", text);
    if (d_exception_catchable(ctx)) {
        JSValue error = JS_GetException(ctx);
        d_report(dom, ctx, error);
        JS_FreeValue(ctx, error);
    }
}

bool os64_dom_intersections_fire(os64_dom_t *dom, os64_js_outcome_t *out)
{
    if (out != NULL) os64_memset(out, 0, sizeof(*out));
    struct DIntersections *s = d_delivery_active(dom) ? dom->intersections : NULL;
    if (out == NULL || s == NULL || !s->queued) return false;
    s->queued = false;
    sweep(dom);
    char name[OS64_JS_SOURCE_NAME_CAP];
    os64_snprintf(name, sizeof(name), "%.*s#intersection-observer", (int)(sizeof(name) - 24),
                  dom->url != NULL ? dom->url : "about:blank");
    if (os64_js_realm_task_begin(dom->realm, OS64_JS_ABI_ID, name, out) == OS64_JS_OK) {
        os64_js_outcome_t access;
        JSContext *ctx = os64_js_realm_context(dom->realm, OS64_JS_ABI_ID, &access);
        if (ctx == NULL) d_outcome_fold(out, &access);
        if (ctx != NULL) report_lost(dom, ctx);
        /* The observers made by a callback meanwhile wait for the next
         * task; none of those visited can be freed under the walk, since
         * no sweep runs while it walks (`walking`), a constructor's in a
         * callback included. */
        DIObserver *last = s->observers_last;
        s->walking = true;
        for (DIObserver *o = s->observers; ctx != NULL && o != NULL && d_delivery_active(dom);) {
            DIObserver *next = o == last ? NULL : o->next;
            if (o->first != NULL && !o->dead && JS_IsObject(o->object)) {
                int eligible = d_delivery_eligible(dom, ctx, o->callback);
                if (eligible <= 0) {
                    entries_clear(dom, o);
                    hold_update(dom, ctx, o);
                    if (eligible < 0 && d_exception_catchable(ctx)) {
                        JSValue error = JS_GetException(ctx);
                        d_report_as(o->report_owner, ctx, error, o->script);
                        JS_FreeValue(ctx, error);
                    }
                    o = next;
                    continue;
                }
                JSValue self = JS_DupValue(ctx, o->object);
                JSValue callback = JS_DupValue(ctx, o->callback);
                JSValue records = take_records(dom, ctx, o);
                hold_update(dom, ctx, o);
                os64_js_outcome_t call;
                os64_memset(&call, 0, sizeof(call));
                if (JS_IsException(records)) {
                    if (d_exception_catchable(ctx)) {
                        JSValue error = JS_GetException(ctx);
                        d_report(dom, ctx, error);
                        JS_FreeValue(ctx, error);
                    }
                } else {
                    JSValue args[2] = {records, self};
                    DScripts before = d_script_enter(o->report_owner, o->script);
                    os64_js_realm_call(o->report_owner->realm, OS64_JS_ABI_ID, callback, self, 2, args, NULL, &call);
                    d_script_leave(o->report_owner, &before, true);
                    d_outcome_fold(out, &call);
                    if (call.status == OS64_JS_EXCEPTION) d_errors_fire(o->report_owner, out);
                    d_task_settle(dom, out);
                }
                JS_FreeValue(ctx, records);
                JS_FreeValue(ctx, callback);
                JS_FreeValue(ctx, self);
                if (out->status != OS64_JS_OK && out->status != OS64_JS_EXCEPTION &&
                    out->status != OS64_JS_UNHANDLED_REJECTION)
                    break;
            }
            o = next;
        }
        s->walking = false;
        d_task_finish(dom, out);
    }
    d_task_close(dom, name, out);
    if (!dom->closed) sweep(dom);
    return true;
}

/* ── The interface ───────────────────────────────────────────────────── */

static DIObserver *observer_of(os64_dom_t *dom, JSValueConst self)
{
    return JS_GetOpaque(self, dom->intersections->observer_class);
}

static void observer_finalizer(JSRuntime *rt, JSValue value)
{
    DIObserver *o = JS_GetOpaque(value, os64_js_class_id(&observer_slot));
    if (o == NULL) return;
    JS_FreeValueRT(rt, o->callback);
    JS_FreeValueRT(rt, o->thresholds_array);
    o->callback = o->thresholds_array = JS_UNDEFINED;
    o->object = JS_UNDEFINED;
    o->dead = true;
    if (o->dom->intersections != NULL) o->dom->intersections->has_dead = true;
}

static void observer_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DIObserver *o = JS_GetOpaque(value, os64_js_class_id(&observer_slot));
    if (o == NULL) return;
    JS_MarkValue(rt, o->callback, mark);
    JS_MarkValue(rt, o->thresholds_array, mark);
}

/* A run of CSS's whitespace. */
static bool css_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* One margin token, `s` for `n` bytes: a number and then `%` or an absolute
 * length's unit, taken to pixels, floored, as other browsers keep them. */
static bool margin_token(const char *s, size_t n, double *value, bool *percent)
{
    size_t i = 0;
    bool negative = false;
    if (i < n && (s[i] == '+' || s[i] == '-')) negative = s[i++] == '-';
    double v = 0;
    bool digits = false;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i++] - '0');
        digits = true;
    }
    if (i < n && s[i] == '.' && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '9') {
        double scale = 0.1;
        for (i++; i < n && s[i] >= '0' && s[i] <= '9'; i++, scale /= 10) v += (s[i] - '0') * scale;
        digits = true;
    }
    if (!digits) return false;
    if (i + 1 < n && (s[i] == 'e' || s[i] == 'E') &&
        ((s[i + 1] >= '0' && s[i + 1] <= '9') ||
         (i + 2 < n && (s[i + 1] == '+' || s[i + 1] == '-') && s[i + 2] >= '0' && s[i + 2] <= '9'))) {
        size_t j = i + 1;
        bool minus = false;
        if (s[j] == '+' || s[j] == '-') minus = s[j++] == '-';
        int exponent = 0;
        while (j < n && s[j] >= '0' && s[j] <= '9') {
            if (exponent < 400) exponent = exponent * 10 + (s[j] - '0');
            j++;
        }
        /* Past 400 either way the value is 0 or no finite number. */
        for (int k = 0; k < exponent && v != 0 && isfinite(v); k++) v = minus ? v / 10 : v * 10;
        i = j;
    }
    if (negative) v = -v;
    if (!isfinite(v)) return false;
    const char *unit = s + i;
    size_t len = n - i;
    if (len == 1 && unit[0] == '%') {
        *value = v;
        *percent = true;
        return true;
    }
    static const struct { const char *name; double px; } kUnits[] = {
        {"px", 1}, {"cm", 96 / 2.54}, {"mm", 96 / 25.4}, {"q", 96 / 101.6}, {"in", 96}, {"pt", 96.0 / 72},
        {"pc", 16},
    };
    for (size_t u = 0; u < sizeof(kUnits) / sizeof(kUnits[0]); u++) {
        size_t ulen = os64_strlen(kUnits[u].name);
        if (ulen != len) continue;
        bool same = true;
        for (size_t k = 0; k < len && same; k++) {
            char c = unit[k];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            same = c == kUnits[u].name[k];
        }
        if (same) {
            *value = floor(v * kUnits[u].px);
            *percent = false;
            return true;
        }
    }
    return false;
}

/* The standard's "parse a margin": 1 to 4 tokens, CSS's margin order and
 * repetition. False for anything else. */
static bool parse_margin(const char *text, size_t length, double margin[4], bool percent[4])
{
    double v[4];
    bool p[4];
    size_t count = 0, i = 0;
    while (i < length) {
        while (i < length && css_space(text[i])) i++;
        if (i == length) break;
        size_t start = i;
        while (i < length && !css_space(text[i])) i++;
        if (count == 4 || !margin_token(text + start, i - start, &v[count], &p[count])) return false;
        count++;
    }
    if (count == 0) {
        v[0] = 0;
        p[0] = false;
        count = 1;
    }
    static const int kFrom[5][4] = {{0}, {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
    for (int k = 0; k < 4; k++) {
        margin[k] = v[kFrom[count][k]];
        percent[k] = p[kFrom[count][k]];
    }
    return true;
}

/* The four values space-separated, each as JS writes a number, then "px"
 * or "%". */
static JSValue margin_string(JSContext *ctx, const double margin[4], const bool percent[4])
{
    char text[256];
    size_t at = 0;
    for (int i = 0; i < 4; i++) {
        /* -0 writes as 0, which it is. */
        JSValue number = JS_NewFloat64(ctx, margin[i] == 0 ? 0 : margin[i]);
        size_t len;
        const char *digits = JS_ToCStringLen(ctx, &len, number);
        JS_FreeValue(ctx, number);
        if (digits == NULL) return JS_EXCEPTION;
        at += os64_snprintf(text + at, sizeof(text) - at, "%s%.*s%s", i ? " " : "", (int)len, digits,
                            percent[i] ? "%" : "px");
        JS_FreeCString(ctx, digits);
        if (at >= sizeof(text)) at = sizeof(text) - 1;
    }
    return JS_NewString(ctx, text);
}

typedef struct {
    os64_dom_t *dom;
    double *values;
    size_t count, cap;
    bool failed;
} DNumbers;

/* WebIDL's double: finite, or a TypeError. */
static int to_double(JSContext *ctx, JSValueConst value, double *out)
{
    if (JS_ToFloat64(ctx, out, value) < 0) return -1;
    if (!isfinite(*out)) {
        JS_ThrowTypeError(ctx, "A threshold must be a finite number");
        return -1;
    }
    return 0;
}

static int number_each(void *opaque, JSContext *ctx, JSValueConst item)
{
    DNumbers *n = opaque;
    double v;
    if (to_double(ctx, item, &v) < 0) return -1;
    if (n->count == n->cap) {
        size_t grown = n->cap != 0 ? n->cap * 2 : 8;
        double *array = d_alloc_kind(n->dom, grown * sizeof(*array), OS64_DOM_MEM_INTERSECTION);
        if (array == NULL) {
            d_error(ctx, "QuotaExceededError", "IntersectionObserver quota exceeded");
            return -1;
        }
        if (n->count != 0) os64_memcpy(array, n->values, n->count * sizeof(*array));
        d_free(n->dom, n->values);
        n->values = array;
        n->cap = grown;
    }
    n->values[n->count++] = v;
    return 0;
}

/* `threshold`, (double or sequence<double>): an object with an iterator is
 * a sequence, anything else one number. -1 with an exception pending. */
static int read_thresholds(os64_dom_t *dom, JSContext *ctx, JSValueConst value, DNumbers *n)
{
    n->dom = dom;
    if (JS_IsUndefined(value)) return number_each(n, ctx, JS_NewFloat64(ctx, 0));
    if (JS_IsObject(value)) {
        JSAtom symbol = JS_ValueToAtom(ctx, dom->iterator_symbol->value);
        if (symbol == JS_ATOM_NULL) return -1;
        JSValue method = JS_GetProperty(ctx, value, symbol);
        JS_FreeAtom(ctx, symbol);
        if (JS_IsException(method)) return -1;
        if (!JS_IsUndefined(method) && !JS_IsNull(method)) {
            int status = d_sequence_each(dom, ctx, value, method, number_each, n);
            JS_FreeValue(ctx, method);
            return status;
        }
    }
    return number_each(n, ctx, value);
}

static void sift_down(double *v, size_t at, size_t n)
{
    for (;;) {
        size_t big = at, l = 2 * at + 1, r = l + 1;
        if (l < n && v[l] > v[big]) big = l;
        if (r < n && v[r] > v[big]) big = r;
        if (big == at) return;
        double x = v[at];
        v[at] = v[big];
        v[big] = x;
        at = big;
    }
}

/* Ascending, in place, a heap sort: a page may hand over as long a list
 * as its quota holds, and the constructor must not take quadratic time
 * over it. */
static void sort_numbers(double *v, size_t n)
{
    for (size_t i = n / 2; i-- > 0;) sift_down(v, i, n);
    for (size_t end = n; end > 1; end--) {
        double x = v[0];
        v[0] = v[end - 1];
        v[end - 1] = x;
        sift_down(v, 0, end - 1);
    }
}

/* The thresholds as the attribute's FrozenArray: its elements and its
 * length unwritable, and the array closed, which is what Object.freeze
 * leaves, without calling it. */
static JSValue frozen_numbers(JSContext *ctx, const double *values, size_t count)
{
    JSValue array = JS_NewArray(ctx);
    for (size_t i = 0; i < count && !JS_IsException(array); i++)
        if (JS_DefinePropertyValueUint32(ctx, array, (uint32_t)i, JS_NewFloat64(ctx, values[i]),
                                         JS_PROP_ENUMERABLE) < 0) {
            JS_FreeValue(ctx, array);
            array = JS_EXCEPTION;
        }
    if (!JS_IsException(array) &&
        (JS_DefinePropertyValueStr(ctx, array, "length", JS_NewUint32(ctx, (uint32_t)count), 0) < 0 ||
         JS_PreventExtensions(ctx, array) < 0)) {
        JS_FreeValue(ctx, array);
        array = JS_EXCEPTION;
    }
    return array;
}

/* (Element or Document)? root: NULL for none, the node otherwise; -1 with
 * a TypeError pending for anything else. */
static int read_root(os64_dom_t *dom, JSContext *ctx, JSValueConst value, const os64_html_node_t **out)
{
    *out = NULL;
    if (JS_IsUndefined(value) || JS_IsNull(value)) return 0;
    const os64_html_node_t *node = d_node(dom, ctx, value);
    if (node == NULL) return -1;
    if (node->kind != OS64_HTML_ELEMENT && node->kind != OS64_HTML_DOCUMENT) {
        JS_ThrowTypeError(ctx, "An IntersectionObserver's root must be an Element or a Document");
        return -1;
    }
    *out = node;
    return 0;
}

static JSValue observer_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    struct DIntersections *s = dom->intersections;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "IntersectionObserver needs a callback");
    sweep(dom);
    /* IntersectionObserverInit converted whole first, its members read in
     * WebIDL's order (lexicographic): delay, root, rootMargin,
     * scrollMargin, threshold, trackVisibility. Then the standard's
     * constructor steps: the margins parsed, then the thresholds checked.
     * scrollMargin is kept and reported, and clips nothing: its clipping is
     * booked (INTERSECTION_OBSERVER.md § 6). */
    const os64_html_node_t *root = NULL;
    double margins[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
    bool percents[2][4] = {{false, false, false, false}, {false, false, false, false}};
    DString margin_text[2] = {{0}, {0}};
    bool margin_given[2] = {false, false};
    DNumbers thresholds = {0};
    int32_t delay = 0;
    bool track = false;
    int present = d_dictionary(ctx, argc, argv, 1);
    if (present < 0) return JS_EXCEPTION;
    JSValueConst init = present ? argv[1] : JS_UNDEFINED;
    JSValue v = present ? JS_GetPropertyStr(ctx, init, "delay") : JS_UNDEFINED;
    if (JS_IsException(v) || (!JS_IsUndefined(v) && JS_ToInt32(ctx, &delay, v) < 0))
        goto failed_value;
    JS_FreeValue(ctx, v);
    v = present ? JS_GetPropertyStr(ctx, init, "root") : JS_UNDEFINED;
    if (JS_IsException(v) || read_root(dom, ctx, v, &root) < 0) goto failed_value;
    JS_FreeValue(ctx, v);
    static const char *const kMargins[] = {"rootMargin", "scrollMargin"};
    for (int m = 0; m < 2; m++) {
        v = present ? JS_GetPropertyStr(ctx, init, kMargins[m]) : JS_UNDEFINED;
        if (JS_IsException(v)) goto failed_value;
        if (!JS_IsUndefined(v)) {
            if (!d_string(dom, ctx, v, &margin_text[m])) goto failed_value;
            margin_given[m] = true;
        }
        JS_FreeValue(ctx, v);
    }
    v = present ? JS_GetPropertyStr(ctx, init, "threshold") : JS_UNDEFINED;
    if (JS_IsException(v) || read_thresholds(dom, ctx, v, &thresholds) < 0) goto failed_value;
    JS_FreeValue(ctx, v);
    v = present ? JS_GetPropertyStr(ctx, init, "trackVisibility") : JS_UNDEFINED;
    if (JS_IsException(v)) goto failed_value;
    if (!JS_IsUndefined(v)) {
        int b = JS_ToBool(ctx, v);
        if (b < 0) goto failed_value;
        track = b != 0;
    }
    JS_FreeValue(ctx, v);
    for (int m = 0; m < 2; m++) {
        if (margin_given[m] && !parse_margin(margin_text[m].data, margin_text[m].length, margins[m], percents[m])) {
            d_error(ctx, "SyntaxError", m == 0 ? "rootMargin must be 1 to 4 lengths or percentages"
                                               : "scrollMargin must be 1 to 4 lengths or percentages");
            goto failed;
        }
        d_string_free(dom, &margin_text[m]);
    }
    for (size_t i = 0; i < thresholds.count; i++)
        if (thresholds.values[i] < 0 || thresholds.values[i] > 1) {
            JS_ThrowRangeError(ctx, "A threshold must be between 0 and 1");
            goto failed;
        }
    sort_numbers(thresholds.values, thresholds.count);
    if (thresholds.count == 0 && number_each(&thresholds, ctx, JS_NewFloat64(ctx, 0)) < 0) goto failed;
    if (track && delay < 100) delay = 100;

    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) goto failed;
    if (!JS_IsObject(prototype)) {
        JS_FreeValue(ctx, prototype);
        prototype = JS_GetClassProto(ctx, s->observer_class);
    }
    os64_dom_t *report_owner;
    uint32_t script = d_callback_record(dom, ctx, argv[0], &report_owner);
    DIObserver *o = script != D_SCRIPT_FAILED ? d_alloc_kind(dom, sizeof(*o), OS64_DOM_MEM_INTERSECTION) : NULL;
    JSValue frozen = o != NULL ? frozen_numbers(ctx, thresholds.values, thresholds.count) : JS_EXCEPTION;
    JSValue object = !JS_IsException(frozen) ? JS_NewObjectProtoClass(ctx, prototype, s->observer_class)
                                              : JS_EXCEPTION;
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object)) {
        JS_FreeValue(ctx, frozen);
        d_free(dom, o);
        if (!JS_HasException(ctx)) d_error(ctx, "QuotaExceededError", "IntersectionObserver quota exceeded");
        goto failed;
    }
    os64_memset(o, 0, sizeof(*o));
    o->dom = dom;
    o->object = object;
    o->callback = JS_DupValue(ctx, argv[0]);
    o->thresholds_array = frozen;
    o->script = script;
    o->report_owner = report_owner;
    o->root = root;
    if (root != NULL) os64_html_hold(dom->document, root);
    for (int i = 0; i < 4; i++) {
        o->margin[i] = margins[0][i];
        o->percent[i] = percents[0][i];
        o->scroll_margin[i] = margins[1][i];
        o->scroll_percent[i] = percents[1][i];
    }
    o->thresholds = thresholds.values;
    o->nthresholds = thresholds.count;
    o->delay = delay;
    o->track_visibility = track;
    o->prev = s->observers_last;
    if (s->observers_last != NULL) s->observers_last->next = o;
    else s->observers = o;
    s->observers_last = o;
    JS_SetOpaque(object, o);
    return object;
failed_value:
    JS_FreeValue(ctx, v);
failed:
    d_free(dom, thresholds.values);
    d_string_free(dom, &margin_text[0]);
    d_string_free(dom, &margin_text[1]);
    return JS_EXCEPTION;
}

static JSValue observe(os64_dom_t *dom, JSContext *ctx, DIObserver *o, int argc, JSValueConst *argv)
{
    struct DIntersections *s = dom->intersections;
    const os64_html_node_t *target = argc > 0 ? d_node(dom, ctx, argv[0]) : NULL;
    if (target == NULL || target->kind != OS64_HTML_ELEMENT) {
        if (target != NULL || !JS_HasException(ctx)) JS_ThrowTypeError(ctx, "observe needs an Element");
        return JS_EXCEPTION;
    }
    if (target_find(s, o, target) != NULL) return JS_UNDEFINED;
    DTarget *t = d_alloc_kind(dom, sizeof(*t), OS64_DOM_MEM_INTERSECTION);
    if (t == NULL) return d_error(ctx, "QuotaExceededError", "IntersectionObserver quota exceeded");
    *t = (DTarget){.observer = o, .node = target, .previous_index = -1, .prev = o->targets_last};
    os64_html_hold(dom->document, target);
    size_t b = node_bucket(target);
    t->bucket_next = s->buckets[b];
    s->buckets[b] = t;
    if (o->targets_last != NULL) o->targets_last->next = t;
    else o->targets = t;
    o->targets_last = t;
    o->ntargets++;
    s->observed++;
    s->owed++;
    hold_update(dom, ctx, o);
    return JS_UNDEFINED;
}

enum { M_OBSERVE, M_UNOBSERVE, M_DISCONNECT, M_TAKE, M_ROOT, M_ROOT_MARGIN, M_SCROLL_MARGIN, M_THRESHOLDS,
       M_DELAY, M_TRACK };

static JSValue observer_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                               JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DIObserver *o = observer_of(dom, self);
    if (o == NULL || !d_same_cluster(dom, o->dom) || o->dom->closed)
        return JS_ThrowTypeError(ctx, "Expected a live IntersectionObserver in this cluster");
    dom = o->dom;
    switch (magic) {
    case M_OBSERVE: return observe(dom, ctx, o, argc, argv);
    case M_UNOBSERVE: {
        const os64_html_node_t *target = argc > 0 ? d_node(dom, ctx, argv[0]) : NULL;
        if (target == NULL || target->kind != OS64_HTML_ELEMENT) {
            if (target != NULL || !JS_HasException(ctx)) JS_ThrowTypeError(ctx, "unobserve needs an Element");
            return JS_EXCEPTION;
        }
        DTarget *t = target_find(dom->intersections, o, target);
        if (t != NULL) target_remove(dom, t);
        hold_update(dom, ctx, o);
        return JS_UNDEFINED;
    }
    case M_DISCONNECT:
        targets_clear(dom, o);
        hold_update(dom, ctx, o);
        return JS_UNDEFINED;
    case M_TAKE: {
        JSValue records = take_records(dom, ctx, o);
        hold_update(dom, ctx, o);
        return records;
    }
    case M_ROOT: return o->root != NULL ? d_wrap(dom, ctx, o->root) : JS_NULL;
    case M_ROOT_MARGIN: return margin_string(ctx, o->margin, o->percent);
    case M_SCROLL_MARGIN: return margin_string(ctx, o->scroll_margin, o->scroll_percent);
    case M_THRESHOLDS: return JS_DupValue(ctx, o->thresholds_array);
    case M_DELAY: return JS_NewInt32(ctx, o->delay);
    case M_TRACK: return JS_NewBool(ctx, o->track_visibility);
    default: return JS_ThrowTypeError(ctx, "Unknown IntersectionObserver member");
    }
}

static JSValue entry_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                            JSValue *data)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DEntryObject *e = JS_GetOpaque(self, dom->intersections->entry_class);
    if (e == NULL) return JS_ThrowTypeError(ctx, "Expected an IntersectionObserverEntry");
    return JS_DupValue(ctx, e->fields[magic]);
}

/* A DOMRectInit member of the entry's init as a DOMRectReadOnly; null
 * allowed for rootBounds. */
static JSValue init_rect(os64_dom_t *dom, JSContext *ctx, JSValueConst value, bool nullable)
{
    if (nullable && JS_IsNull(value)) return JS_NULL;
    double r[4] = {0, 0, 0, 0};
    if (!JS_IsUndefined(value) && !JS_IsNull(value)) {
        if (!JS_IsObject(value)) return JS_ThrowTypeError(ctx, "A DOMRectInit must be an object");
        static const int kOrder[] = {3, 2, 0, 1};  /* height, width, x, y */
        static const char *const kNames[] = {"x", "y", "width", "height"};
        for (int j = 0; j < 4; j++) {
            JSValue v = JS_GetPropertyStr(ctx, value, kNames[kOrder[j]]);
            if (JS_IsException(v)) return v;
            int status = JS_IsUndefined(v) ? 0 : JS_ToFloat64(ctx, &r[kOrder[j]], v);
            JS_FreeValue(ctx, v);
            if (status < 0) return JS_EXCEPTION;
        }
    }
    return d_rect_new(dom, ctx, true, r[0], r[1], r[2], r[3]);
}

/* new IntersectionObserverEntry(init): every member required, read in
 * WebIDL's order (lexicographic). */
static JSValue entry_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "IntersectionObserverEntry needs an IntersectionObserverEntryInit");
    static const struct { const char *name; int field; } kMembers[] = {
        {"boundingClientRect", E_BOUNDING}, {"intersectionRatio", E_RATIO}, {"intersectionRect", E_INTERSECTION},
        {"isIntersecting", E_IS_INTERSECTING}, {"isVisible", E_IS_VISIBLE}, {"rootBounds", E_ROOT_BOUNDS},
        {"target", E_TARGET}, {"time", E_TIME},
    };
    JSValue fields[E_FIELDS];
    for (int i = 0; i < E_FIELDS; i++) fields[i] = JS_UNDEFINED;
    bool ok = true;
    for (size_t i = 0; i < sizeof(kMembers) / sizeof(kMembers[0]) && ok; i++) {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], kMembers[i].name);
        if (JS_IsException(v)) { ok = false; break; }
        if (JS_IsUndefined(v)) {
            JS_ThrowTypeError(ctx, "IntersectionObserverEntryInit needs %s", kMembers[i].name);
            ok = false;
            break;
        }
        JSValue converted;
        int f = kMembers[i].field;
        if (f == E_BOUNDING || f == E_INTERSECTION || f == E_ROOT_BOUNDS) {
            converted = init_rect(dom, ctx, v, f == E_ROOT_BOUNDS);
        } else if (f == E_IS_INTERSECTING || f == E_IS_VISIBLE) {
            int b = JS_ToBool(ctx, v);
            converted = b < 0 ? JS_EXCEPTION : JS_NewBool(ctx, b);
        } else if (f == E_TARGET) {
            const os64_html_node_t *node = d_node(dom, ctx, v);
            converted = node == NULL ? JS_EXCEPTION
                      : node->kind != OS64_HTML_ELEMENT ? JS_ThrowTypeError(ctx, "target must be an Element")
                      : JS_DupValue(ctx, v);
        } else {
            double d;
            converted = JS_ToFloat64(ctx, &d, v) < 0 ? JS_EXCEPTION
                      : !isfinite(d) ? JS_ThrowTypeError(ctx, "%s must be finite", kMembers[i].name)
                      : JS_NewFloat64(ctx, d);
        }
        JS_FreeValue(ctx, v);
        if (JS_IsException(converted)) ok = false;
        else fields[f] = converted;
    }
    JSValue prototype = ok ? JS_GetPropertyStr(ctx, new_target, "prototype") : JS_EXCEPTION;
    if (!ok || JS_IsException(prototype)) {
        for (int i = 0; i < E_FIELDS; i++) JS_FreeValue(ctx, fields[i]);
        return JS_EXCEPTION;
    }
    if (!JS_IsObject(prototype)) {
        JS_FreeValue(ctx, prototype);
        prototype = JS_DupValue(ctx, dom->intersections->entry_prototype->value);
    }
    JSValue object = entry_object(dom, ctx, prototype, fields);
    JS_FreeValue(ctx, prototype);
    return object;
}

/* The IntersectionObserver row (modern.c): IntersectionObserver, which
 * constructs, and IntersectionObserverEntry, after the DOMRect row whose
 * rects its entries carry. */
int d_intersections_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef observer_def = {.class_name = "IntersectionObserver", .finalizer = observer_finalizer,
                                            .gc_mark = observer_mark};
    static const JSClassDef entry_def = {.class_name = "IntersectionObserverEntry", .finalizer = entry_finalizer,
                                         .gc_mark = entry_mark};
    struct DIntersections *s = d_alloc_kind(dom, sizeof(*s), OS64_DOM_MEM_INTERSECTION);
    if (s == NULL) {
        d_error(ctx, "QuotaExceededError", "IntersectionObserver quota exceeded");
        return -1;
    }
    os64_memset(s, 0, sizeof(*s));
    s->delayed_due = UINT64_MAX;
    dom->intersections = s;
    s->observer_class = os64_js_class_id(&observer_slot);
    s->entry_class = os64_js_class_id(&entry_slot);
    if ((!JS_IsRegisteredClass(dom->engine, s->observer_class) &&
         JS_NewClass(dom->engine, s->observer_class, &observer_def) < 0) ||
        (!JS_IsRegisteredClass(dom->engine, s->entry_class) && JS_NewClass(dom->engine, s->entry_class, &entry_def) < 0))
        return -1;
    DValue *observer = d_retain(dom, ctx, JS_NewObject(ctx));
    s->entry_prototype = observer != NULL ? d_retain(dom, ctx, JS_NewObject(ctx)) : NULL;
    if (s->entry_prototype == NULL) return -1;
    JS_SetClassProto(ctx, s->observer_class, JS_DupValue(ctx, observer->value));
    JS_SetClassProto(ctx, s->entry_class, JS_DupValue(ctx, s->entry_prototype->value));
    static const struct { const char *name; int magic, argc; } kMethods[] = {
        {"observe", M_OBSERVE, 1}, {"unobserve", M_UNOBSERVE, 1}, {"disconnect", M_DISCONNECT, 0},
        {"takeRecords", M_TAKE, 0},
    };
    static const struct { const char *name; int magic; } kAttributes[] = {
        {"root", M_ROOT}, {"rootMargin", M_ROOT_MARGIN}, {"scrollMargin", M_SCROLL_MARGIN},
        {"thresholds", M_THRESHOLDS}, {"delay", M_DELAY}, {"trackVisibility", M_TRACK},
    };
    for (size_t i = 0; i < sizeof(kAttributes) / sizeof(kAttributes[0]); i++)
        if (d_accessor(dom, ctx, observer->value, kAttributes[i].name, observer_member, kAttributes[i].magic,
                       false) < 0)
            return -1;
    for (size_t i = 0; i < sizeof(kMethods) / sizeof(kMethods[0]); i++)
        if (d_method_named(dom, ctx, observer->value, kMethods[i].name, observer_member, kMethods[i].argc,
                           kMethods[i].magic) < 0)
            return -1;
    for (int i = 0; i < E_FIELDS; i++)
        if (d_accessor(dom, ctx, s->entry_prototype->value, kEntryFields[i], entry_member, i, false) < 0) return -1;
    JSValue observer_interface = d_interface_new(ctx, "IntersectionObserver", 1, observer_constructor,
                                                 observer->value, JS_UNDEFINED);
    if (JS_IsException(observer_interface) ||
        JS_DefinePropertyValueStr(ctx, global, "IntersectionObserver", observer_interface,
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    JSValue entry_interface = d_interface_new(ctx, "IntersectionObserverEntry", 1, entry_constructor,
                                              s->entry_prototype->value, JS_UNDEFINED);
    if (JS_IsException(entry_interface) ||
        JS_DefinePropertyValueStr(ctx, global, "IntersectionObserverEntry", entry_interface,
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    return 0;
}

bool d_intersections_busy(const os64_dom_t *dom)
{
    return dom->intersections && dom->intersections->walking;
}

void d_intersections_cancel(os64_dom_t *dom)
{
    struct DIntersections *s = dom->intersections;
    if (!s) return;
    for (DIObserver *o = s->observers; o != NULL; o = o->next) entries_clear(dom, o);
    s->queued = false;
    s->lost = 0;
}

/* Root teardown releases registration holds; departure only clears payloads. */
void d_intersections_drain(os64_dom_t *dom)
{
    struct DIntersections *s = dom->intersections;
    if (s == NULL) return;
    for (DIObserver *o = s->observers; o != NULL; o = o->next)
        if (o->held) {
            o->held = false;
            JS_FreeValueRT(dom->engine, o->object);
        }
    dom->intersections_held = 0;
}

void d_intersections_free(os64_dom_t *dom)
{
    struct DIntersections *s = dom->intersections;
    if (s == NULL) return;
    while (s->observers != NULL) observer_unlink_free(dom, s->observers);
    d_free(dom, s->batch);
    d_free(dom, s);
    dom->intersections = NULL;
}
