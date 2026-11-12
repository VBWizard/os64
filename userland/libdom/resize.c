#include "internal.h"
#include "os64/fmt.h"
#include <math.h>

/* RESIZEOBSERVER (Resize Observer 1, W3C; INTERSECTION_OBSERVER.md § 11).
 *
 * The binding keeps the observers and their targets; the host answers the
 * sizes, batching targets by their current document (os64_dom_set_resizes),
 * from layouts the providers make current. At its rendering step the host calls
 * os64_dom_resizes_step, which is the standard's loop as one task: gather
 * the observations whose observed size moved, deeper than a depth;
 * broadcast them, each observer's callback handed its entries in the order
 * the page made the observers; ask the host again, which lays out what the
 * callbacks changed; gather deeper than the shallowest target broadcast;
 * until none is active. One left over is the loop error, an ErrorEvent.
 *
 * Lifetime is IntersectionObserver's (intersection.c): an observer is held
 * while it observes a target, its finalizer only marks it dead, and the
 * dead are swept between walks. */

#define D_RO_BUCKETS 256

enum { BOX_CONTENT, BOX_BORDER, BOX_DEVICE };
static const char *const kBoxes[] = {"content-box", "border-box", "device-pixel-content-box"};

typedef struct DRTarget {
    struct DRTarget *prev, *next, *bucket_next;
    struct DRObserver *observer;
    const os64_html_node_t *node;   /* held */
    uint8_t box;
    /* The last reported size of the observed box: (-1, -1) before the
     * first report. */
    double last[2];
    /* The last gather's answer, read by the broadcast that follows it. */
    os64_dom_resize_t now;
    bool active, skipped;
} DRTarget;

typedef struct DRObserver {
    struct DRObserver *prev, *next;
    os64_dom_t *dom;
    JSValue object, callback;
    bool held, dead;
    uint32_t script;
    DRTarget *targets, *targets_last;
    size_t ntargets;
} DRObserver;

struct DResizes {
    JSClassID observer_class, entry_class, size_class;
    DRObserver *observers, *observers_last;
    /* The targets by node, D_RO_BUCKETS of them, made at the first
     * observe: a page that observes nothing does not pay for the table. */
    DRTarget **buckets;
    size_t observed;
    /* Targets that have never been reported, which a step owes; and those
     * the last step's loop passed over (the loop error's), which the next
     * step owes, whether or not anything is laid out again. */
    size_t owed, skipped;
    /* The last step left work undone: the provider refused a gather,
     * first or later, or a callback's entries could not be made. What it
     * did not measure or deliver is owed to the next step, which the host
     * cannot tell from "nothing moved" by the step's answer alone. */
    bool undone;
    /* An observer has died since the last sweep; the broadcast is walking
     * the observers, and no sweep may run. */
    bool has_dead, walking;
    DValue *entry_prototype, *size_prototype;
    os64_dom_resize_t *batch;
    size_t batch_cap;
};

static JSClassID observer_slot, entry_slot, size_slot;

static size_t node_bucket(const os64_html_node_t *node)
{
    return ((uintptr_t)node >> 4) % D_RO_BUCKETS;
}

/* ── Holding and the dead ────────────────────────────────────────────── */

static void hold_update(os64_dom_t *dom, JSContext *ctx, DRObserver *o)
{
    bool want = !o->dead && JS_IsObject(o->object) && o->ntargets != 0;
    if (want == o->held) return;
    o->held = want;
    if (want) {
        dom->resizes_held++;
        if (ctx != NULL) JS_DupValue(ctx, o->object);
        else JS_DupValueRT(dom->engine, o->object);
    } else {
        dom->resizes_held--;
        JS_FreeValueRT(dom->engine, o->object);
    }
}

static DRTarget *target_find(struct DResizes *s, const DRObserver *o, const os64_html_node_t *node)
{
    if (s->buckets == NULL) return NULL;
    for (DRTarget *t = s->buckets[node_bucket(node)]; t != NULL; t = t->bucket_next)
        if (t->node == node && t->observer == o) return t;
    return NULL;
}

static void target_remove(os64_dom_t *dom, DRTarget *t)
{
    struct DResizes *s = dom->resizes;
    DRObserver *o = t->observer;
    for (DRTarget **link = &s->buckets[node_bucket(t->node)]; *link != NULL; link = &(*link)->bucket_next)
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
    if (t->last[0] < 0) s->owed--;
    os64_html_release(dom->document, t->node);
    d_free(dom, t);
}

static void observer_unlink_free(os64_dom_t *dom, DRObserver *o)
{
    struct DResizes *s = dom->resizes;
    while (o->targets != NULL) target_remove(dom, o->targets);
    if (o->prev != NULL) o->prev->next = o->next;
    else s->observers = o->next;
    if (o->next != NULL) o->next->prev = o->prev;
    else s->observers_last = o->prev;
    d_free(dom, o);
}

/* The observers whose objects are gone, freed: never while the broadcast
 * walks them. The step and every new observer sweep. */
static void sweep(os64_dom_t *dom)
{
    struct DResizes *s = dom->resizes;
    if (!s->has_dead || s->walking) return;
    s->has_dead = false;
    for (DRObserver *o = s->observers; o != NULL;) {
        DRObserver *next = o->next;
        if (o->dead) observer_unlink_free(dom, o);
        o = next;
    }
}

/* ── The step ────────────────────────────────────────────────────────── */

void os64_dom_set_resizes(os64_dom_t *dom, os64_dom_resizes_provider_t provider, void *opaque)
{
    if (dom == NULL || dom->closed) return;
    dom->resizes_provider = provider;
    dom->resizes_opaque = opaque;
}

size_t os64_dom_resizes_observed(const os64_dom_t *dom)
{
    return dom != NULL && !dom->closed && dom->resizes != NULL ? dom->resizes->observed : 0;
}

bool os64_dom_resizes_owed(const os64_dom_t *dom)
{
    if (!d_delivery_active(dom) || dom->resizes == NULL) return false;
    const struct DResizes *s = dom->resizes;
    for (DRObserver *o = s->observers; o != NULL; o = o->next) {
        if (o->dead) continue;
        for (DRTarget *t = o->targets; t != NULL; t = t->next)
            if (d_delivery_active(d_owner((os64_dom_t *)dom, t->node)) &&
                (t->last[0] < 0 || t->skipped || s->undone)) return true;
    }
    return false;
}

/* The standard's "calculate depth for node": how many nodes from it to the
 * root of its flattened tree, itself included. */
static size_t node_depth(os64_dom_t *dom, const os64_html_node_t *node)
{
    dom = d_owner(dom, node);
    size_t depth = 0;
    for (; node != NULL; node = os64_html_flat_parent(dom->document, node)) depth++;
    return depth;
}

/* The observed box's size in an answer: inline then block (horizontal
 * writing modes, INTERSECTION_OBSERVER.md § 11). An element with no box
 * is 0 by 0. */
static void observed_size(const os64_dom_resize_t *a, uint8_t box, double out[2])
{
    const double *v = box == BOX_BORDER ? a->border : box == BOX_DEVICE ? a->device : a->content;
    out[0] = a->boxed ? v[0] : 0;
    out[1] = a->boxed ? v[1] : 0;
}

/* "Gather active resize observations at depth": each current-document
 * provider is asked once for its targets. After every batch succeeds, a
 * target is marked active when its observed size moved and it
 * is deeper than `depth`, skipped when it moved and is not. -1 when the
 * provider refused; else how many are active, and `*skipped` how many were
 * passed over. */
static int64_t gather(os64_dom_t *dom, size_t depth, size_t *skipped)
{
    struct DResizes *s = dom->resizes;
    *skipped = 0;
    if (s->batch_cap < s->observed) {
        os64_dom_resize_t *grown = d_alloc_kind(dom, s->observed * sizeof(*grown), OS64_DOM_MEM_RESIZE);
        if (grown == NULL) return -1;
        d_free(dom, s->batch);
        s->batch = grown;
        s->batch_cap = s->observed;
    }
    size_t members = dom->cluster != NULL ? dom->cluster->count : 1;
    for (size_t i = 0; i < members; i++) {
        os64_dom_t *owner = dom->cluster != NULL ? dom->cluster->members[i] : dom;
        if (!d_delivery_active(owner)) continue;
        size_t n = 0;
        for (DRObserver *o = s->observers; o != NULL; o = o->next) {
            if (o->dead) continue;
            for (DRTarget *t = o->targets; t != NULL; t = t->next)
                if (os64_html_node_owner(t->node) == owner->document)
                    s->batch[n++] = (os64_dom_resize_t){.target = t->node};
        }
        if (n == 0) continue;
        os64_dom_geometry_stats_t cost = {0};
        bool answered = false;
        if (owner->resizes_provider != NULL && d_provider_enter(owner)) {
            answered = owner->resizes_provider(owner->resizes_opaque, s->batch, n, &cost);
            d_provider_leave(owner);
        }
        owner->geometry_stats.layouts += cost.layouts;
        owner->geometry_stats.elapsed_us += cost.elapsed_us;
        if (!d_delivery_active(dom)) return 0;
        if (!d_delivery_active(owner)) continue;
        if (!answered) return -1;
        size_t at = 0;
        for (DRObserver *o = s->observers; o != NULL; o = o->next) {
            if (o->dead) continue;
            for (DRTarget *t = o->targets; t != NULL; t = t->next)
                if (os64_html_node_owner(t->node) == owner->document) t->now = s->batch[at++];
        }
    }
    int64_t active = 0;
    for (DRObserver *o = s->observers; o != NULL; o = o->next) {
        if (o->dead) continue;
        for (DRTarget *t = o->targets; t != NULL; t = t->next) {
            double size[2];
            t->active = t->skipped = false;
            if (!d_delivery_active(d_owner(dom, t->node))) continue;
            observed_size(&t->now, t->box, size);
            if (size[0] == t->last[0] && size[1] == t->last[1]) continue;
            if (node_depth(dom, t->node) > depth) {
                t->active = true;
                active++;
            } else {
                t->skipped = true;
                (*skipped)++;
            }
        }
    }
    return active;
}

static JSValue size_value(os64_dom_t *dom, JSContext *ctx, double inline_size, double block_size)
{
    struct DResizes *s = dom->resizes;
    JSValue object = JS_NewObjectProtoClass(ctx, s->size_prototype->value, s->size_class);
    if (JS_IsException(object)) return object;
    double *v = js_malloc(ctx, 2 * sizeof(double));
    if (v == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    v[0] = inline_size;
    v[1] = block_size;
    JS_SetOpaque(object, v);
    return object;
}

/* A FrozenArray of one ResizeObserverSize: one fragment, as the standard
 * has it today. */
static JSValue size_list(os64_dom_t *dom, JSContext *ctx, const double v[2])
{
    JSValue size = size_value(dom, ctx, v[0], v[1]);
    if (JS_IsException(size)) return size;
    JSValue array = JS_NewArray(ctx);
    if (JS_IsException(array) || JS_DefinePropertyValueUint32(ctx, array, 0, size, JS_PROP_ENUMERABLE) < 0 ||
        JS_DefinePropertyValueStr(ctx, array, "length", JS_NewUint32(ctx, 1), 0) < 0 ||
        JS_PreventExtensions(ctx, array) < 0) {
        if (JS_IsException(array)) JS_FreeValue(ctx, size);
        JS_FreeValue(ctx, array);
        return JS_EXCEPTION;
    }
    return array;
}

enum { RE_TARGET, RE_CONTENT_RECT, RE_BORDER, RE_CONTENT, RE_DEVICE, RE_FIELDS };
static const char *const kEntryFields[RE_FIELDS] = {
    "target", "contentRect", "borderBoxSize", "contentBoxSize", "devicePixelContentBoxSize",
};

typedef struct {
    JSValue fields[RE_FIELDS];
} DREntry;

/* "Create and populate a ResizeObserverEntry" from a gather's answer. */
static JSValue entry_object(os64_dom_t *dom, JSContext *ctx, const DRTarget *t)
{
    double border[2], content[2], device[2];
    observed_size(&t->now, BOX_BORDER, border);
    observed_size(&t->now, BOX_CONTENT, content);
    observed_size(&t->now, BOX_DEVICE, device);
    double left = t->now.boxed ? t->now.padding[0] : 0, top = t->now.boxed ? t->now.padding[1] : 0;
    JSValue fields[RE_FIELDS] = {
        d_wrap(dom, ctx, t->node), d_rect_new(dom, ctx, true, left, top, content[0], content[1]),
        size_list(dom, ctx, border), size_list(dom, ctx, content), size_list(dom, ctx, device),
    };
    bool made = true;
    for (int i = 0; i < RE_FIELDS; i++) made = made && !JS_IsException(fields[i]);
    JSValue object = made ? JS_NewObjectProtoClass(ctx, dom->resizes->entry_prototype->value,
                                                   dom->resizes->entry_class)
                          : JS_EXCEPTION;
    DREntry *e = !JS_IsException(object) ? js_mallocz(ctx, sizeof(*e)) : NULL;
    if (e == NULL) {
        for (int i = 0; i < RE_FIELDS; i++) JS_FreeValue(ctx, fields[i]);
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    for (int i = 0; i < RE_FIELDS; i++) e->fields[i] = fields[i];
    JS_SetOpaque(object, e);
    return object;
}

static bool sticky(const os64_js_outcome_t *out)
{
    return out->status != OS64_JS_OK && out->status != OS64_JS_EXCEPTION &&
           out->status != OS64_JS_UNHANDLED_REJECTION;
}

/* "Broadcast active resize observations": each observer with active
 * targets, in the order the page made them (those made meanwhile wait for
 * the next round), called with its entries, a checkpoint after each. The
 * depth of the shallowest target broadcast, SIZE_MAX for none. */
static size_t broadcast(os64_dom_t *dom, JSContext *ctx, os64_js_outcome_t *out)
{
    struct DResizes *s = dom->resizes;
    size_t shallowest = SIZE_MAX;
    DRObserver *last = s->observers_last;
    s->walking = true;
    for (DRObserver *o = s->observers; o != NULL && d_delivery_active(dom) && !sticky(out);) {
        DRObserver *next = o == last ? NULL : o->next;
        bool any = false;
        for (DRTarget *t = o->targets; t != NULL; t = t->next) {
            if (!d_delivery_active(d_owner(dom, t->node))) t->active = false;
            any = any || t->active;
        }
        if (any && !o->dead && JS_IsObject(o->object)) {
            int eligible = d_delivery_eligible(dom, ctx, o->callback);
            if (eligible <= 0) {
                /* A permanently retired callback cannot consume a later
                 * resize either. Remember the sample without constructing
                 * entries or keeping the host awake for the same sizes. */
                for (DRTarget *t = o->targets; t != NULL; t = t->next) {
                    if (!t->active) continue;
                    t->active = false;
                    if (t->last[0] < 0) s->owed--;
                    observed_size(&t->now, t->box, t->last);
                }
                if (eligible < 0 && d_exception_catchable(ctx)) {
                    JSValue error = JS_GetException(ctx);
                    d_report_as(dom, ctx, error, o->script);
                    JS_FreeValue(ctx, error);
                }
                o = next;
                continue;
            }
            JSValue entries = JS_NewArray(ctx);
            uint32_t i = 0;
            for (DRTarget *t = o->targets; t != NULL && !JS_IsException(entries); t = t->next) {
                if (!t->active) continue;
                JSValue entry = entry_object(dom, ctx, t);
                if (JS_IsException(entry) || JS_DefinePropertyValueUint32(ctx, entries, i++, entry, JS_PROP_C_W_E) < 0) {
                    JS_FreeValue(ctx, entries);
                    entries = JS_EXCEPTION;
                    break;
                }
            }
            /* Reported only once every entry is made: a callback that is
             * not called reports none of its targets, which stay moved and
             * are owed to the next step (`undone`). (Making entries runs no
             * page code, so the targets are as they were.) */
            if (JS_IsException(entries)) s->undone = true;
            for (DRTarget *t = o->targets; t != NULL && !JS_IsException(entries); t = t->next) {
                if (!t->active) continue;
                t->active = false;
                if (t->last[0] < 0) s->owed--;
                observed_size(&t->now, t->box, t->last);
                size_t depth = node_depth(dom, t->node);
                if (depth < shallowest) shallowest = depth;
            }
            JSValue self = JS_DupValue(ctx, o->object), callback = JS_DupValue(ctx, o->callback);
            if (JS_IsException(entries)) {
                if (d_exception_catchable(ctx)) {
                    JSValue error = JS_GetException(ctx);
                    d_report(dom, ctx, error);
                    JS_FreeValue(ctx, error);
                }
            } else {
                JSValue args[2] = {entries, self};
                os64_js_outcome_t call;
                DScripts before = d_script_enter(dom, o->script);
                os64_js_realm_call(dom->realm, OS64_JS_ABI_ID, callback, self, 2, args, NULL, &call);
                d_script_leave(dom, &before, true);
                d_outcome_fold(out, &call);
                if (call.status == OS64_JS_EXCEPTION) d_errors_fire(dom, out);
                d_task_settle(dom, out);
            }
            JS_FreeValue(ctx, entries);
            JS_FreeValue(ctx, callback);
            JS_FreeValue(ctx, self);
        }
        o = next;
    }
    s->walking = false;
    for (DRObserver *o = s->observers; o != NULL; o = o->next)
        for (DRTarget *t = o->targets; t != NULL; t = t->next) t->active = false;
    return shallowest;
}

/* "Deliver resize loop error notification": an ErrorEvent at window, each
 * listener a budgeted call of the step's task, as the callbacks are, so a
 * listener may read geometry or set a style. */
static void loop_error(os64_dom_t *dom, JSContext *ctx, os64_js_outcome_t *out)
{
    if (!os64_dom_listens(dom, "error")) return;
    JSValue event = d_error_event_new(dom, ctx, "ResizeObserver loop completed with undelivered notifications.",
                                      "", 0, 0, JS_NULL);
    if (JS_IsException(event)) {
        if (d_exception_catchable(ctx)) JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    if (d_dispatch_window_task(dom, ctx, event, out) < 0 && JS_HasException(ctx) && d_exception_catchable(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, event);
}

bool os64_dom_resizes_step(os64_dom_t *dom, os64_js_outcome_t *out)
{
    if (out != NULL) os64_memset(out, 0, sizeof(*out));
    struct DResizes *s = d_delivery_active(dom) ? dom->resizes : NULL;
    if (out == NULL || s == NULL) return false;
    sweep(dom);
    if (s->observed == 0) return false;
    size_t skipped;
    int64_t active = gather(dom, 0, &skipped);
    s->undone = active < 0;
    if (active < 0) return false;
    s->skipped = 0;
    if (active == 0) return false;
    char name[OS64_JS_SOURCE_NAME_CAP];
    os64_snprintf(name, sizeof(name), "%.*s#resize-observer", (int)(sizeof(name) - 20),
                  dom->url != NULL ? dom->url : "about:blank");
    if (os64_js_realm_task_begin(dom->realm, OS64_JS_ABI_ID, name, out) == OS64_JS_OK) {
        os64_js_outcome_t access;
        JSContext *ctx = os64_js_realm_context(dom->realm, OS64_JS_ABI_ID, &access);
        if (ctx == NULL) d_outcome_fold(out, &access);
        /* Each round's depth is deeper than the last's shallowest, and a
         * tree is finitely deep: the loop ends. */
        while (ctx != NULL && active > 0 && d_delivery_active(dom) && !sticky(out)) {
            size_t depth = broadcast(dom, ctx, out);
            if (sticky(out) || !d_delivery_active(dom)) break;
            active = gather(dom, depth, &skipped);
            if (active < 0) s->undone = true;
        }
        if (ctx != NULL && skipped != 0 && d_delivery_active(dom) && !sticky(out)) loop_error(dom, ctx, out);
        s->skipped = skipped;
        d_task_finish(dom, out);
    }
    d_task_close(dom, name, out);
    if (!dom->closed) sweep(dom);
    return true;
}

/* ── The interface ───────────────────────────────────────────────────── */

static void observer_finalizer(JSRuntime *rt, JSValue value)
{
    DRObserver *o = JS_GetOpaque(value, os64_js_class_id(&observer_slot));
    if (o == NULL) return;
    JS_FreeValueRT(rt, o->callback);
    o->callback = o->object = JS_UNDEFINED;
    o->dead = true;
    if (o->dom->resizes != NULL) o->dom->resizes->has_dead = true;
}

static void observer_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DRObserver *o = JS_GetOpaque(value, os64_js_class_id(&observer_slot));
    if (o != NULL) JS_MarkValue(rt, o->callback, mark);
}

static void entry_finalizer(JSRuntime *rt, JSValue value)
{
    DREntry *e = JS_GetOpaque(value, os64_js_class_id(&entry_slot));
    if (e == NULL) return;
    for (int i = 0; i < RE_FIELDS; i++) JS_FreeValueRT(rt, e->fields[i]);
    js_free_rt(rt, e);
}

static void entry_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DREntry *e = JS_GetOpaque(value, os64_js_class_id(&entry_slot));
    if (e == NULL) return;
    for (int i = 0; i < RE_FIELDS; i++) JS_MarkValue(rt, e->fields[i], mark);
}

static void size_finalizer(JSRuntime *rt, JSValue value)
{
    js_free_rt(rt, JS_GetOpaque(value, os64_js_class_id(&size_slot)));
}

static JSValue observer_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    struct DResizes *s = dom->resizes;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "ResizeObserver needs a callback");
    sweep(dom);
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    if (!JS_IsObject(prototype)) {
        JS_FreeValue(ctx, prototype);
        prototype = JS_GetClassProto(ctx, s->observer_class);
    }
    uint32_t script = d_callback_script(dom, ctx, argv[0]);
    DRObserver *o = script != D_SCRIPT_FAILED ? d_alloc_kind(dom, sizeof(*o), OS64_DOM_MEM_RESIZE) : NULL;
    JSValue object = o != NULL ? JS_NewObjectProtoClass(ctx, prototype, s->observer_class) : JS_EXCEPTION;
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object)) {
        d_free(dom, o);
        if (!JS_HasException(ctx)) d_error(ctx, "QuotaExceededError", "ResizeObserver quota exceeded");
        return JS_EXCEPTION;
    }
    os64_memset(o, 0, sizeof(*o));
    o->dom = dom;
    o->object = object;
    o->callback = JS_DupValue(ctx, argv[0]);
    o->script = script;
    o->prev = s->observers_last;
    if (s->observers_last != NULL) s->observers_last->next = o;
    else s->observers = o;
    s->observers_last = o;
    JS_SetOpaque(object, o);
    return object;
}

/* ResizeObserverOptions' one member, `box`, an enum. -1 with a TypeError
 * pending for any other word. */
static int read_box(JSContext *ctx, int argc, JSValueConst *argv, uint8_t *box)
{
    *box = BOX_CONTENT;
    int present = d_dictionary(ctx, argc, argv, 1);
    if (present <= 0) return present;
    JSValue v = JS_GetPropertyStr(ctx, argv[1], "box");
    if (JS_IsException(v)) return -1;
    if (JS_IsUndefined(v)) return 0;
    const char *word = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (word == NULL) return -1;
    int found = -1;
    for (int i = 0; i < 3; i++)
        if (os64_strcmp(word, kBoxes[i]) == 0) found = i;
    JS_FreeCString(ctx, word);
    if (found < 0) {
        JS_ThrowTypeError(ctx, "box must be content-box, border-box or device-pixel-content-box");
        return -1;
    }
    *box = (uint8_t)found;
    return 0;
}

enum { M_OBSERVE, M_UNOBSERVE, M_DISCONNECT };

static JSValue observer_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                               JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DRObserver *o = JS_GetOpaque(self, dom->resizes->observer_class);
    if (o == NULL || !d_same_cluster(dom, o->dom) || o->dom->closed)
        return JS_ThrowTypeError(ctx, "Expected a live ResizeObserver in this cluster");
    dom = o->dom;
    struct DResizes *s = dom->resizes;
    if (magic == M_DISCONNECT) {
        while (o->targets != NULL) target_remove(dom, o->targets);
        hold_update(dom, ctx, o);
        return JS_UNDEFINED;
    }
    const os64_html_node_t *target = argc > 0 ? d_node(dom, ctx, argv[0]) : NULL;
    if (target == NULL || target->kind != OS64_HTML_ELEMENT) {
        if (target != NULL || !JS_HasException(ctx)) JS_ThrowTypeError(ctx, "ResizeObserver needs an Element");
        return JS_EXCEPTION;
    }
    uint8_t box = BOX_CONTENT;
    if (magic == M_OBSERVE && read_box(ctx, argc, argv, &box) < 0) return JS_EXCEPTION;
    /* Observing a target again starts it over, with its new box. */
    DRTarget *t = target_find(s, o, target);
    if (t != NULL) target_remove(dom, t);
    if (magic == M_UNOBSERVE) {
        hold_update(dom, ctx, o);
        return JS_UNDEFINED;
    }
    if (s->buckets == NULL) {
        s->buckets = d_alloc_kind(dom, D_RO_BUCKETS * sizeof(*s->buckets), OS64_DOM_MEM_RESIZE);
        if (s->buckets != NULL) os64_memset(s->buckets, 0, D_RO_BUCKETS * sizeof(*s->buckets));
    }
    t = s->buckets != NULL ? d_alloc_kind(dom, sizeof(*t), OS64_DOM_MEM_RESIZE) : NULL;
    if (t == NULL) {
        hold_update(dom, ctx, o);
        return d_error(ctx, "QuotaExceededError", "ResizeObserver quota exceeded");
    }
    *t = (DRTarget){.observer = o, .node = target, .box = box, .last = {-1, -1}, .prev = o->targets_last};
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

static JSValue entry_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                            JSValue *data)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DREntry *e = JS_GetOpaque(self, dom->resizes->entry_class);
    if (e == NULL) return JS_ThrowTypeError(ctx, "Expected a ResizeObserverEntry");
    return JS_DupValue(ctx, e->fields[magic]);
}

static JSValue size_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                           JSValue *data)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    double *v = JS_GetOpaque(self, dom->resizes->size_class);
    if (v == NULL) return JS_ThrowTypeError(ctx, "Expected a ResizeObserverSize");
    return JS_NewFloat64(ctx, v[magic]);
}

/* The ResizeObserver row (modern.c): ResizeObserver, which constructs, and
 * ResizeObserverEntry and ResizeObserverSize, Illegal constructors, after
 * the DOMRect row whose rect an entry carries. */
int d_resizes_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef observer_def = {.class_name = "ResizeObserver", .finalizer = observer_finalizer,
                                            .gc_mark = observer_mark};
    static const JSClassDef entry_def = {.class_name = "ResizeObserverEntry", .finalizer = entry_finalizer,
                                         .gc_mark = entry_mark};
    static const JSClassDef size_def = {.class_name = "ResizeObserverSize", .finalizer = size_finalizer};
    struct DResizes *s = d_alloc_kind(dom, sizeof(*s), OS64_DOM_MEM_RESIZE);
    if (s == NULL) {
        d_error(ctx, "QuotaExceededError", "ResizeObserver quota exceeded");
        return -1;
    }
    os64_memset(s, 0, sizeof(*s));
    dom->resizes = s;
    s->observer_class = os64_js_class_id(&observer_slot);
    s->entry_class = os64_js_class_id(&entry_slot);
    s->size_class = os64_js_class_id(&size_slot);
    const struct { JSClassID id; const JSClassDef *def; } kClasses[] = {
        {s->observer_class, &observer_def}, {s->entry_class, &entry_def}, {s->size_class, &size_def},
    };
    for (int i = 0; i < 3; i++)
        if (!JS_IsRegisteredClass(dom->engine, kClasses[i].id) && JS_NewClass(dom->engine, kClasses[i].id, kClasses[i].def) < 0)
            return -1;
    DValue *observer = d_retain(dom, ctx, JS_NewObject(ctx));
    s->entry_prototype = observer != NULL ? d_retain(dom, ctx, JS_NewObject(ctx)) : NULL;
    s->size_prototype = s->entry_prototype != NULL ? d_retain(dom, ctx, JS_NewObject(ctx)) : NULL;
    if (s->size_prototype == NULL) return -1;
    JS_SetClassProto(ctx, s->observer_class, JS_DupValue(ctx, observer->value));
    JS_SetClassProto(ctx, s->entry_class, JS_DupValue(ctx, s->entry_prototype->value));
    JS_SetClassProto(ctx, s->size_class, JS_DupValue(ctx, s->size_prototype->value));
    if (d_method_named(dom, ctx, observer->value, "observe", observer_member, 1, M_OBSERVE) < 0 ||
        d_method_named(dom, ctx, observer->value, "unobserve", observer_member, 1, M_UNOBSERVE) < 0 ||
        d_method_named(dom, ctx, observer->value, "disconnect", observer_member, 0, M_DISCONNECT) < 0)
        return -1;
    for (int i = 0; i < RE_FIELDS; i++)
        if (d_accessor(dom, ctx, s->entry_prototype->value, kEntryFields[i], entry_member, i, false) < 0) return -1;
    if (d_accessor(dom, ctx, s->size_prototype->value, "inlineSize", size_member, 0, false) < 0 ||
        d_accessor(dom, ctx, s->size_prototype->value, "blockSize", size_member, 1, false) < 0)
        return -1;
    const struct { const char *name; int length; JSCFunction *constructor; JSValueConst proto; } kInterfaces[] = {
        {"ResizeObserver", 1, observer_constructor, observer->value},
        {"ResizeObserverEntry", 0, NULL, s->entry_prototype->value},
        {"ResizeObserverSize", 0, NULL, s->size_prototype->value},
    };
    for (int i = 0; i < 3; i++) {
        JSValue interface = d_interface_new(ctx, kInterfaces[i].name, kInterfaces[i].length,
                                            kInterfaces[i].constructor, kInterfaces[i].proto, JS_UNDEFINED);
        if (JS_IsException(interface) ||
            JS_DefinePropertyValueStr(ctx, global, kInterfaces[i].name, interface,
                                      JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
            return -1;
    }
    return 0;
}

void d_resizes_drain(os64_dom_t *dom)
{
    struct DResizes *s = dom->resizes;
    if (s == NULL) return;
    for (DRObserver *o = s->observers; o != NULL; o = o->next)
        if (o->held) {
            o->held = false;
            JS_FreeValueRT(dom->engine, o->object);
        }
    dom->resizes_held = 0;
}

void d_resizes_free(os64_dom_t *dom)
{
    struct DResizes *s = dom->resizes;
    if (s == NULL) return;
    while (s->observers != NULL) observer_unlink_free(dom, s->observers);
    d_free(dom, s->batch);
    d_free(dom, s->buckets);
    d_free(dom, s);
    dom->resizes = NULL;
}
