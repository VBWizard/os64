#include "internal.h"

/* AbortController and AbortSignal (DOM § 3.1, § 3.2). A signal is a plain
 * event target whose entry is IN THE OBJECT (internal.h, A PLAIN EVENT
 * TARGET): a page makes one per request, and it goes with the garbage like
 * any object.
 *
 * DOM'S WEAK SETS. A signal's "source signals" and "dependent signals" are
 * weak sets, and so are they here: a LINK between a source and a dependent
 * is a C record on two lists (the source's dependents, the dependent's
 * sources), holding neither object, and each signal's finalizer unlinks
 * itself from the other side, so either may go first, in either order a
 * collection takes them. No engine internals are read. DOM's GC clause
 * keeps a dependent someone could still observe: while it has a live
 * `abort` listener or a pending algorithm, and is not aborted, each of its
 * links HOLDS it (a reference the source's gc_mark traces), so its sources
 * keep it alive. The hold is released at exactly three points: the
 * dependent is aborted; its last abort listener goes (the plain-target
 * listener-change hook, signal_listened, which nothing else reads); its
 * source aborts, after which it can abort nothing more. A dependent is
 * never itself a source: a dependent's dependents are linked to its
 * sources (DOM's "create a dependent abort signal" flattens), so an abort
 * walks one level.
 *
 * A signal's ALGORITHMS are C's, never a container a page can reach: each
 * a fetch's abort (fetch.c's d_fetch_abort), by its request's token, and,
 * once answered, the Response it errors, known by C pointer both ways
 * (d_signal_fetch_answered/d_signal_fetch_settled): the Response holds the signal
 * until its body settles, the algorithm points at the Response, and each
 * side's finalizer clears the other's pointer. The list is taken whole
 * before it runs, so nothing a run does can change what is walked. */

static JSClassID signal_slot, controller_slot;

typedef struct DSignal DSignal;
typedef struct DLink DLink;

typedef struct {
    uint32_t token;             /* the request's */
    void *answered;             /* its Response's state once answered (fetch.c's), else NULL */
    void **back;                /* where that Response keeps this signal, cleared when this one goes */
} DAlgorithm;

struct DLink {
    DSignal *source, *dependent;
    DLink *next_dependent, *prev_dependent;     /* on source->dependents */
    DLink *next_source, *prev_source;           /* on dependent->sources */
    JSValue hold;               /* the dependent, while the GC clause keeps it; else undefined */
};

struct DSignal {
    DValue target;              /* first: the plain target's entry, in the object */
    bool aborted;
    bool dependent;             /* made by AbortSignal.any or for a Request */
    JSValue reason;
    DAlgorithm *algorithms;     /* js_malloc'd */
    size_t count, cap;
    uint32_t timer;             /* AbortSignal.timeout's, until it fires */
    /* The dependents in the order they were linked (DOM's ordered set: an
     * abort runs their steps in that order), and the sources. */
    DLink *dependents, *dependents_last, *sources;
};

typedef struct {
    JSValue signal;
} DController;

/* ── State ───────────────────────────────────────────────────────────── */

static DSignal *signal_state(JSValueConst value)
{
    return JS_GetOpaque(value, os64_js_class_id(&signal_slot));
}

static void link_unlink(DLink *l)
{
    if (l->prev_dependent != NULL) l->prev_dependent->next_dependent = l->next_dependent;
    else l->source->dependents = l->next_dependent;
    if (l->next_dependent != NULL) l->next_dependent->prev_dependent = l->prev_dependent;
    else l->source->dependents_last = l->prev_dependent;
    if (l->prev_source != NULL) l->prev_source->next_source = l->next_source;
    else l->dependent->sources = l->next_source;
    if (l->next_source != NULL) l->next_source->prev_source = l->prev_source;
}

/* Whether a listener waits on the signal's `abort`. */
static bool abort_listened(const DSignal *s)
{
    for (const DListener *l = s->target.listeners; l != NULL; l = l->next)
        if (!l->removed && !JS_IsUndefined(l->callback) && os64_streq(l->type, "abort")) return true;
    return false;
}

/* DOM's GC clause for one dependent: its links hold it while it is not
 * aborted and has an abort listener or a pending algorithm; else they
 * hold nothing. Every caller holds the dependent itself (the page's call
 * on it, the walk of an abort, the Response that keeps it), so a release
 * here never finalizes it under the walk. */
static void hold_update(JSRuntime *rt, DSignal *d)
{
    bool keep = !d->aborted && (d->count != 0 || abort_listened(d));
    for (DLink *l = d->sources; l != NULL; l = l->next_source) {
        if (keep && JS_IsUndefined(l->hold)) l->hold = JS_DupValueRT(rt, d->target.value);
        else if (!keep && !JS_IsUndefined(l->hold)) {
            JSValue released = l->hold;
            l->hold = JS_UNDEFINED;
            JS_FreeValueRT(rt, released);
        }
    }
}

/* The listener-change hook (event.c), for a signal's entry. */
static void signal_listened(JSRuntime *rt, DValue *entry)
{
    hold_update(rt, (DSignal *)entry);
}

static void signal_finalizer(JSRuntime *rt, JSValue value)
{
    DSignal *s = signal_state(value);
    if (s == NULL) return;
    /* Both sides unlinked, the holds this one's links had (on its
     * dependents) freed after: freeing one may finalize that dependent,
     * which no longer finds a link to this one. */
    JSValue *holds = NULL;
    size_t nholds = 0, cap = 0;
    while (s->dependents != NULL) {
        DLink *l = s->dependents;
        link_unlink(l);
        if (!JS_IsUndefined(l->hold)) {
            if (nholds == cap) {
                size_t grown_cap = cap ? cap * 2 : 8;
                JSValue *grown = js_realloc_rt(rt, holds, grown_cap * sizeof(*grown));
                if (grown != NULL) {
                    holds = grown;
                    cap = grown_cap;
                }
            }
            if (nholds < cap) holds[nholds++] = l->hold;
            else JS_FreeValueRT(rt, l->hold);
        }
        js_free_rt(rt, l);
    }
    while (s->sources != NULL) {
        DLink *l = s->sources;
        link_unlink(l);
        JS_FreeValueRT(rt, l->hold);
        js_free_rt(rt, l);
    }
    for (size_t i = 0; i < s->count; i++)
        if (s->algorithms[i].back != NULL) *s->algorithms[i].back = NULL;
    d_event_target_finalize(rt, &s->target);
    JS_FreeValueRT(rt, s->reason);
    js_free_rt(rt, s->algorithms);
    js_free_rt(rt, s);
    for (size_t i = 0; i < nholds; i++) JS_FreeValueRT(rt, holds[i]);
    js_free_rt(rt, holds);
}

static void signal_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DSignal *s = signal_state(value);
    if (s == NULL) return;
    d_event_target_mark(rt, &s->target, mark);
    JS_MarkValue(rt, s->reason, mark);
    for (DLink *l = s->dependents; l != NULL; l = l->next_dependent) JS_MarkValue(rt, l->hold, mark);
}

static void controller_finalizer(JSRuntime *rt, JSValue value)
{
    DController *c = JS_GetOpaque(value, os64_js_class_id(&controller_slot));
    if (c == NULL) return;
    JS_FreeValueRT(rt, c->signal);
    js_free_rt(rt, c);
}

static void controller_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DController *c = JS_GetOpaque(value, os64_js_class_id(&controller_slot));
    if (c != NULL) JS_MarkValue(rt, c->signal, mark);
}

bool d_signal_is(JSValueConst value)
{
    return signal_state(value) != NULL;
}

bool d_signal_aborted(JSValueConst signal, JSValue *reason, JSContext *ctx)
{
    DSignal *s = signal_state(signal);
    if (s == NULL || !s->aborted) return false;
    if (reason != NULL) *reason = JS_DupValue(ctx, s->reason);
    return true;
}

size_t d_signal_dependents(JSValueConst signal)
{
    size_t n = 0;
    const DSignal *s = signal_state(signal);
    for (const DLink *l = s != NULL ? s->dependents : NULL; l != NULL; l = l->next_dependent) n++;
    return n;
}

JSValue d_signal_new(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype)
{
    JSClassID id = os64_js_class_id(&signal_slot);
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id)
                                            : JS_NewObjectClass(ctx, (int)id);
    if (JS_IsException(object)) return object;
    DSignal *s = js_mallocz(ctx, sizeof(*s));
    if (s == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    d_event_target_init(dom, &s->target, object);
    s->target.listeners_changed = signal_listened;
    s->reason = JS_UNDEFINED;
    JS_SetOpaque(object, s);
    return object;
}

int d_signal_add_fetch(JSContext *ctx, JSValueConst signal, uint32_t token)
{
    DSignal *s = signal_state(signal);
    if (s == NULL) return 0;
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 4;
        DAlgorithm *grown = js_realloc(ctx, s->algorithms, cap * sizeof(*grown));
        if (grown == NULL) return -1;
        s->algorithms = grown;
        s->cap = cap;
    }
    s->algorithms[s->count++] = (DAlgorithm){token, NULL, NULL};
    hold_update(JS_GetRuntime(ctx), s);
    return 0;
}

/* The algorithm for `token` dropped (both pointers cleared), and the GC
 * clause asked again. */
static void algorithm_drop(JSRuntime *rt, DSignal *s, size_t i)
{
    if (s->algorithms[i].back != NULL) *s->algorithms[i].back = NULL;
    s->algorithms[i] = s->algorithms[--s->count];
    hold_update(rt, s);
}

void d_signal_fetch_answered(JSContext *ctx, JSValueConst signal, uint32_t token, void *answered, void **back)
{
    DSignal *s = signal_state(signal);
    for (size_t i = 0; s != NULL && i < s->count; i++)
        if (s->algorithms[i].token == token) {
            if (answered == NULL) {
                algorithm_drop(JS_GetRuntime(ctx), s, i);
                return;
            }
            s->algorithms[i].answered = answered;
            s->algorithms[i].back = back;
            *back = s;
            return;
        }
}

void d_signal_fetch_settled(JSRuntime *rt, void *signal, void **back)
{
    DSignal *s = signal;
    for (size_t i = 0; s != NULL && i < s->count; i++)
        if (s->algorithms[i].back == back) {
            algorithm_drop(rt, s, i);
            return;
        }
}

void d_signal_remove_fetch(JSContext *ctx, JSValueConst signal, uint32_t token)
{
    d_signal_fetch_answered(ctx, signal, token, NULL, NULL);
}

/* ── Aborting (DOM § 3.2 "signal abort") ─────────────────────────────── */

/* The default reason: a new "AbortError" DOMException. */
static JSValue default_reason(os64_dom_t *dom, JSContext *ctx, const char *name, const char *message)
{
    return d_dom_exception_new(dom, ctx, name, message);
}

/* Marks `signal` aborted with `reason` (taken); false when it already was. */
static bool mark_aborted(os64_dom_t *dom, JSContext *ctx, DSignal *s, JSValue reason)
{
    if (s->aborted) {
        JS_FreeValue(ctx, reason);
        return false;
    }
    s->aborted = true;
    JS_FreeValue(ctx, s->reason);
    s->reason = reason;
    if (s->timer != 0) {
        d_timer_cancel(dom, s->timer);
        s->timer = 0;
    }
    return true;
}

/* A signal's abort steps: its fetches aborted, then its `abort` event. The
 * list is taken whole: what a run adds goes nowhere, since an aborted
 * signal never runs again. -1 when the runtime is finished with. */
static int abort_steps(os64_dom_t *dom, JSContext *ctx, JSValueConst signal)
{
    DSignal *s = signal_state(signal);
    if (s == NULL) return 0;
    DAlgorithm *list = s->algorithms;
    size_t count = s->count;
    s->algorithms = NULL;
    s->count = s->cap = 0;
    JSValue reason = JS_DupValue(ctx, s->reason);
    for (size_t i = 0; i < count; i++) {
        /* The Response no longer keeps a pointer to this signal's list. */
        if (list[i].back != NULL) *list[i].back = NULL;
        d_fetch_abort(dom, ctx, list[i].token, list[i].answered, reason);
    }
    js_free(ctx, list);
    JS_FreeValue(ctx, reason);
    JSValue event = d_event_new(dom, ctx, "abort", false, false, true);
    if (JS_IsException(event)) return -1;
    int dispatched = d_dispatch_plain(dom, ctx, event, &signal_state(signal)->target);
    JS_FreeValue(ctx, event);
    return dispatched < 0 ? -1 : 0;
}

/* DOM § 3.2 "signal abort": the reason set; every dependent not yet
 * aborted marked with it, and the holds released (the source has aborted,
 * and so has each dependent); then the signal's own steps, its event
 * first, then each dependent's in its list's order. -1 with an exception
 * pending only when the runtime failed under it. */
static int signal_abort(os64_dom_t *dom, JSContext *ctx, JSValueConst signal, JSValueConst reason)
{
    DSignal *s = signal_state(signal);
    if (s == NULL || s->aborted) return 0;
    JSValue value = JS_IsUndefined(reason) ? default_reason(dom, ctx, "AbortError", "The operation was aborted.")
                                           : JS_DupValue(ctx, reason);
    if (JS_IsException(value)) return -1;
    mark_aborted(dom, ctx, s, value);
    size_t count = 0;
    for (DLink *l = s->dependents; l != NULL; l = l->next_dependent) count += !l->dependent->aborted;
    JSValue *gathered = count != 0 ? js_malloc(ctx, count * sizeof(*gathered)) : NULL;
    if (count != 0 && gathered == NULL) return -1;
    size_t n = 0;
    for (DLink *l = s->dependents; l != NULL; l = l->next_dependent) {
        DSignal *d = l->dependent;
        if (n < count && mark_aborted(dom, ctx, d, JS_DupValue(ctx, s->reason)))
            gathered[n++] = JS_DupValue(ctx, d->target.value);
    }
    /* Release points one and three: each dependent is aborted, and the
     * source can abort nothing more. The dependents gathered are held by
     * the walk until their steps have run. */
    for (size_t i = 0; i < n; i++) hold_update(JS_GetRuntime(ctx), signal_state(gathered[i]));
    JSValue *released = NULL;
    size_t nreleased = 0;
    for (DLink *l = s->dependents; l != NULL; l = l->next_dependent)
        nreleased += !JS_IsUndefined(l->hold);
    if (nreleased != 0 && (released = js_malloc(ctx, nreleased * sizeof(*released))) != NULL) {
        size_t r = 0;
        for (DLink *l = s->dependents; l != NULL && r < nreleased; l = l->next_dependent)
            if (!JS_IsUndefined(l->hold)) {
                released[r++] = l->hold;
                l->hold = JS_UNDEFINED;
            }
        for (size_t i = 0; i < r; i++) JS_FreeValue(ctx, released[i]);
        js_free(ctx, released);
    }
    int result = abort_steps(dom, ctx, signal);
    for (size_t i = 0; i < n; i++) {
        if (result == 0) result = abort_steps(dom, ctx, gathered[i]);
        JS_FreeValue(ctx, gathered[i]);
    }
    js_free(ctx, gathered);
    return result;
}

int d_signal_abort(os64_dom_t *dom, JSContext *ctx, JSValueConst signal, JSValueConst reason)
{
    return signal_abort(dom, ctx, signal, reason);
}

/* One link, `source` to `dependent`, unless there is one already. -1 on no
 * memory. */
static int link_add(JSContext *ctx, DSignal *source, DSignal *dependent)
{
    for (DLink *l = dependent->sources; l != NULL; l = l->next_source)
        if (l->source == source) return 0;
    DLink *l = js_mallocz(ctx, sizeof(*l));
    if (l == NULL) return -1;
    l->source = source;
    l->dependent = dependent;
    l->hold = JS_UNDEFINED;
    l->prev_dependent = source->dependents_last;
    if (source->dependents_last != NULL) source->dependents_last->next_dependent = l;
    else source->dependents = l;
    source->dependents_last = l;
    l->next_source = dependent->sources;
    if (dependent->sources != NULL) dependent->sources->prev_source = l;
    dependent->sources = l;
    return 0;
}

/* DOM's "create a dependent abort signal", one source at a time: aborted
 * now when the source is, else linked to it, or, when the source is a
 * dependent itself, to each of its sources. */
int d_signal_follow(os64_dom_t *dom, JSContext *ctx, JSValueConst dependent, JSValueConst source)
{
    DSignal *s = signal_state(source), *d = signal_state(dependent);
    if (s == NULL || d == NULL) return 0;
    d->dependent = true;
    if (s->aborted) return signal_abort(dom, ctx, dependent, s->reason);
    if (!s->dependent) {
        if (link_add(ctx, s, d) < 0) return -1;
    } else
        for (DLink *l = s->sources; l != NULL; l = l->next_source)
            if (link_add(ctx, l->source, d) < 0) return -1;
    hold_update(JS_GetRuntime(ctx), d);
    return 0;
}

/* ── The script surface ──────────────────────────────────────────────── */

enum {
    S_ABORTED, S_REASON, S_THROW, S_ABORT_STATIC, S_TIMEOUT_STATIC, S_ANY_STATIC, C_SIGNAL, C_ABORT, S_FIRED
};

static JSValue signal_prototype(os64_dom_t *dom, JSContext *ctx)
{
    (void)dom;
    return JS_GetClassProto(ctx, os64_js_class_id(&signal_slot));
}

/* AbortSignal.timeout's timer: data[1] is the signal. */
static JSValue timeout_fired(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                             JSValue *data)
{
    (void)self; (void)argc; (void)argv; (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DSignal *s = signal_state(data[1]);
    if (s == NULL) return JS_UNDEFINED;
    s->timer = 0;
    JSValue reason = default_reason(dom, ctx, "TimeoutError", "The operation timed out.");
    if (JS_IsException(reason)) return reason;
    int done = signal_abort(dom, ctx, data[1], reason);
    JS_FreeValue(ctx, reason);
    return done < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue abort_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                            JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    JSValueConst arg = argc > 0 ? argv[0] : JS_UNDEFINED;
    switch (magic) {
    case S_ABORT_STATIC: {
        JSValue prototype = signal_prototype(dom, ctx);
        JSValue signal = d_signal_new(dom, ctx, prototype);
        JS_FreeValue(ctx, prototype);
        if (JS_IsException(signal)) return signal;
        if (signal_abort(dom, ctx, signal, arg) < 0) {
            JS_FreeValue(ctx, signal);
            return JS_EXCEPTION;
        }
        return signal;
    }
    case S_TIMEOUT_STATIC: {
        /* [EnforceRange] unsigned long long. */
        double ms;
        if (JS_ToFloat64(ctx, &ms, arg) < 0) return JS_EXCEPTION;
        if (!(ms >= 0 && ms <= 9007199254740991.0) || ms != ms)
            return JS_ThrowTypeError(ctx, "AbortSignal.timeout needs a time in milliseconds");
        JSValue prototype = signal_prototype(dom, ctx);
        JSValue signal = d_signal_new(dom, ctx, prototype);
        JS_FreeValue(ctx, prototype);
        if (JS_IsException(signal)) return signal;
        JSValue fn_data[2] = {dom->anchor->value, signal};
        JSValue fired = JS_NewCFunctionData(ctx, timeout_fired, 0, 0, 2, fn_data);
        uint32_t timer = JS_IsException(fired) ? 0 : d_timer_add(dom, ctx, fired, (uint64_t)ms);
        if (timer == 0) {
            JS_FreeValue(ctx, signal);
            return JS_EXCEPTION;
        }
        signal_state(signal)->timer = timer;
        return signal;
    }
    case S_ANY_STATIC: {
        /* sequence<AbortSignal>: converted whole, before anything is made,
         * as WebIDL converts a sequence. */
        JSValue *signals;
        size_t count;
        if (d_sequence(dom, ctx, arg, D_SEQUENCE_MAX, &signals, &count) < 0) return JS_EXCEPTION;
        JSValue result = JS_UNDEFINED;
        for (size_t i = 0; i < count && !JS_IsException(result); i++)
            if (signal_state(signals[i]) == NULL)
                result = JS_ThrowTypeError(ctx, "AbortSignal.any needs AbortSignals");
        if (!JS_IsException(result)) {
            JSValue prototype = signal_prototype(dom, ctx);
            result = d_signal_new(dom, ctx, prototype);
            JS_FreeValue(ctx, prototype);
        }
        /* Aborted already when any source is, with its reason; else it
         * follows each. */
        for (size_t i = 0; !JS_IsException(result) && i < count; i++)
            if (signal_state(signals[i])->aborted) {
                if (signal_abort(dom, ctx, result, signal_state(signals[i])->reason) < 0) {
                    JS_FreeValue(ctx, result);
                    result = JS_EXCEPTION;
                }
                break;
            }
        for (size_t i = 0; !JS_IsException(result) && !signal_state(result)->aborted && i < count; i++)
            if (d_signal_follow(dom, ctx, result, signals[i]) < 0) {
                JS_FreeValue(ctx, result);
                result = JS_EXCEPTION;
            }
        d_sequence_free(ctx, signals, count);
        return result;
    }
    case C_SIGNAL: case C_ABORT: {
        DController *c = JS_GetOpaque(self, os64_js_class_id(&controller_slot));
        if (c == NULL) return JS_ThrowTypeError(ctx, "Expected an AbortController");
        if (magic == C_SIGNAL) return JS_DupValue(ctx, c->signal);
        return signal_abort(dom, ctx, c->signal, arg) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    }
    default: break;
    }
    DSignal *s = signal_state(self);
    if (s == NULL) return JS_ThrowTypeError(ctx, "Expected an AbortSignal");
    switch (magic) {
    case S_ABORTED: return JS_NewBool(ctx, s->aborted);
    case S_REASON: return JS_DupValue(ctx, s->reason);
    case S_THROW:
        if (s->aborted) return JS_Throw(ctx, JS_DupValue(ctx, s->reason));
        return JS_UNDEFINED;
    default: return JS_UNDEFINED;
    }
}

/* new AbortController(): its signal is made with it. */
static JSValue controller_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSClassID id = os64_js_class_id(&controller_slot);
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id) : JS_NewObjectClass(ctx, (int)id);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object)) return object;
    DController *c = js_mallocz(ctx, sizeof(*c));
    if (c == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    c->signal = JS_UNDEFINED;
    JS_SetOpaque(object, c);
    JSValue signal_proto = signal_prototype(dom, ctx);
    c->signal = d_signal_new(dom, ctx, signal_proto);
    JS_FreeValue(ctx, signal_proto);
    if (JS_IsException(c->signal)) {
        c->signal = JS_UNDEFINED;
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
}

int d_abort_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef signal_class = {.class_name = "AbortSignal", .finalizer = signal_finalizer,
                                            .gc_mark = signal_mark};
    static const JSClassDef controller_class = {.class_name = "AbortController", .finalizer = controller_finalizer,
                                                .gc_mark = controller_mark};
    JSClassID signal_id = os64_js_class_id(&signal_slot), controller_id = os64_js_class_id(&controller_slot);
    if ((!JS_IsRegisteredClass(dom->engine, signal_id) && JS_NewClass(dom->engine, signal_id, &signal_class) < 0) ||
        (!JS_IsRegisteredClass(dom->engine, controller_id) &&
         JS_NewClass(dom->engine, controller_id, &controller_class) < 0))
        return -1;
    DValue *signal = d_retain(dom, ctx, JS_NewObject(ctx));
    DValue *controller = signal != NULL ? d_retain(dom, ctx, JS_NewObject(ctx)) : NULL;
    if (controller == NULL) return -1;
    JS_SetClassProto(ctx, signal_id, JS_DupValue(ctx, signal->value));
    JS_SetClassProto(ctx, controller_id, JS_DupValue(ctx, controller->value));
    JSValueConst sp = signal->value, cp = controller->value;
    if (d_accessor(dom, ctx, sp, "aborted", abort_member, S_ABORTED, false) < 0 ||
        d_accessor(dom, ctx, sp, "reason", abort_member, S_REASON, false) < 0 ||
        d_method(dom, ctx, sp, "throwIfAborted", abort_member, 0, S_THROW) < 0 ||
        d_event_target(dom, ctx, sp, signal_id, NULL) < 0 || d_event_handler_named(dom, ctx, sp, "abort") < 0 ||
        d_accessor(dom, ctx, cp, "signal", abort_member, C_SIGNAL, false) < 0 ||
        d_method(dom, ctx, cp, "abort", abort_member, 0, C_ABORT) < 0)
        return -1;
    JSValue signal_interface = d_interface_new(ctx, "AbortSignal", 0, NULL, sp, JS_UNDEFINED);
    if (JS_IsException(signal_interface)) return -1;
    if (d_method(dom, ctx, signal_interface, "abort", abort_member, 0, S_ABORT_STATIC) < 0 ||
        d_method(dom, ctx, signal_interface, "timeout", abort_member, 1, S_TIMEOUT_STATIC) < 0 ||
        d_method(dom, ctx, signal_interface, "any", abort_member, 1, S_ANY_STATIC) < 0) {
        JS_FreeValue(ctx, signal_interface);
        return -1;
    }
    if (JS_DefinePropertyValueStr(ctx, global, "AbortSignal", signal_interface, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    JSValue controller_interface = d_interface_new(ctx, "AbortController", 0, controller_constructor, cp, JS_UNDEFINED);
    if (JS_IsException(controller_interface)) return -1;
    return JS_DefinePropertyValueStr(ctx, global, "AbortController", controller_interface,
                                     JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}
