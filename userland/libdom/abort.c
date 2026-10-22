#include "internal.h"

/* AbortController and AbortSignal (DOM § 3.1, § 3.2). A signal is a
 * COLLECTABLE plain event target (event.c): a page makes one per request,
 * and it goes with the garbage like any object. What a signal does when it
 * aborts is a list of ALGORITHMS kept in C, never a container a page can
 * reach: abort a fetch (fetch.c's d_fetch_abort, by its token), or abort a
 * dependent signal (AbortSignal.any, and a Request's signal that follows
 * its input's). The list is taken whole before it runs, so nothing a run
 * does can change what is walked. */

static JSClassID signal_slot, controller_slot;

enum { A_FETCH = 1, A_FOLLOW };

typedef struct {
    int kind;
    uint32_t token;             /* A_FETCH: the request's */
    JSValue target;             /* A_FOLLOW: the dependent signal; A_FETCH: its Response, once made */
} DAlgorithm;

typedef struct {
    DValue target;              /* first: the collectable plain target's record */
    bool aborted;
    JSValue reason;
    DAlgorithm *algorithms;     /* js_malloc'd */
    size_t count, cap;
    uint32_t timer;             /* AbortSignal.timeout's, until it fires */
} DSignal;

typedef struct {
    JSValue signal;
} DController;

/* ── State ───────────────────────────────────────────────────────────── */

static void signal_finalizer(JSRuntime *rt, JSValue value)
{
    DSignal *s = JS_GetOpaque(value, os64_js_class_id(&signal_slot));
    if (s == NULL) return;
    d_event_target_finalize(rt, &s->target);
    JS_FreeValueRT(rt, s->reason);
    for (size_t i = 0; i < s->count; i++) JS_FreeValueRT(rt, s->algorithms[i].target);
    js_free_rt(rt, s->algorithms);
    js_free_rt(rt, s);
}

static void signal_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DSignal *s = JS_GetOpaque(value, os64_js_class_id(&signal_slot));
    if (s == NULL) return;
    d_event_target_mark(rt, &s->target, mark);
    JS_MarkValue(rt, s->reason, mark);
    for (size_t i = 0; i < s->count; i++) JS_MarkValue(rt, s->algorithms[i].target, mark);
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

static DSignal *signal_state(JSValueConst value)
{
    return JS_GetOpaque(value, os64_js_class_id(&signal_slot));
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
    s->reason = JS_UNDEFINED;
    JS_SetOpaque(object, s);
    return object;
}

/* True when `value` is held by the one reference the caller has: nothing
 * else (no page variable, no other object) can reach it, so nothing that
 * happens to it can be seen. A cycle reads as held, which only keeps. */
static bool held_once(JSValueConst value)
{
    return !JS_VALUE_HAS_REF_COUNT(value) || __js_rc(JS_VALUE_GET_PTR(value))->ref_count <= 1;
}

/* A dependent signal whose abort nobody could see: only this list holds
 * it, no listener waits on it, and each of its own algorithms is a fetch
 * that has ended whose Response (if any) is likewise unreachable or has
 * no body left to error. Its A_FOLLOW may go. */
static bool dependent_inert(const DAlgorithm *a)
{
    if (a->kind != A_FOLLOW || !held_once(a->target)) return false;
    DSignal *d = signal_state(a->target);
    if (d == NULL) return true;
    for (const DListener *l = d->target.listeners; l != NULL; l = l->next)
        if (!l->removed) return false;
    for (size_t i = 0; i < d->count; i++) {
        const DAlgorithm *own = &d->algorithms[i];
        if (own->kind != A_FETCH) return false;
        if (!JS_IsUndefined(own->target) && !held_once(own->target) && !d_response_body_settled(own->target))
            return false;
    }
    return true;
}

static int algorithm_add(JSContext *ctx, DSignal *s, int kind, uint32_t token, JSValue target)
{
    if (s->count == s->cap) {
        /* A source signal holds its dependents (a Request's signal per
         * request) where the standard holds them weakly; a page that
         * reuses one controller for every request would otherwise keep
         * every request's signal, and its Response, until the controller
         * goes. So a full list is swept first: an entry nobody could see
         * abort is dropped. One sweep per doubling keeps adding linear. */
        size_t kept = 0;
        for (size_t i = 0; i < s->count; i++) {
            if (dependent_inert(&s->algorithms[i])) {
                JS_FreeValue(ctx, s->algorithms[i].target);
                continue;
            }
            s->algorithms[kept++] = s->algorithms[i];
        }
        s->count = kept;
    }
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 4;
        DAlgorithm *grown = js_realloc(ctx, s->algorithms, cap * sizeof(*grown));
        if (grown == NULL) {
            JS_FreeValue(ctx, target);
            return -1;
        }
        s->algorithms = grown;
        s->cap = cap;
    }
    s->algorithms[s->count++] = (DAlgorithm){kind, token, target};
    return 0;
}

int d_signal_add_fetch(JSContext *ctx, JSValueConst signal, uint32_t token)
{
    DSignal *s = signal_state(signal);
    return s == NULL ? 0 : algorithm_add(ctx, s, A_FETCH, token, JS_UNDEFINED);
}

void d_signal_fetch_answered(JSContext *ctx, JSValueConst signal, uint32_t token, JSValueConst response)
{
    DSignal *s = signal_state(signal);
    for (size_t i = 0; s != NULL && i < s->count; i++)
        if (s->algorithms[i].kind == A_FETCH && s->algorithms[i].token == token) {
            JS_FreeValue(ctx, s->algorithms[i].target);
            s->algorithms[i].target = JS_DupValue(ctx, response);
        }
}

void d_signal_remove_fetch(JSContext *ctx, JSValueConst signal, uint32_t token)
{
    DSignal *s = signal_state(signal);
    if (s == NULL) return;
    size_t kept = 0;
    for (size_t i = 0; i < s->count; i++) {
        if (s->algorithms[i].kind == A_FETCH && s->algorithms[i].token == token) {
            JS_FreeValue(ctx, s->algorithms[i].target);
            continue;
        }
        s->algorithms[kept++] = s->algorithms[i];
    }
    s->count = kept;
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
 * signal never runs again; its dependents were gathered before (below).
 * -1 when the runtime is finished with. */
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
        if (list[i].kind == A_FETCH) d_fetch_abort(dom, ctx, list[i].token, list[i].target, reason);
        JS_FreeValue(ctx, list[i].target);
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
 * aborted marked with it, and theirs in turn (a Request's signal follows
 * its input's, so a page can chain them as deep as it likes: the walk is a
 * worklist, never recursion); then the signal's own steps, its event
 * first, then each dependent's in the order gathered. -1 with an exception
 * pending only when the runtime failed under it. */
static int signal_abort(os64_dom_t *dom, JSContext *ctx, JSValueConst signal, JSValueConst reason)
{
    DSignal *s = signal_state(signal);
    if (s == NULL || s->aborted) return 0;
    JSValue value = JS_IsUndefined(reason) ? default_reason(dom, ctx, "AbortError", "The operation was aborted.")
                                           : JS_DupValue(ctx, reason);
    if (JS_IsException(value)) return -1;
    mark_aborted(dom, ctx, s, value);
    JSValue *gathered = NULL;
    size_t count = 0, cap = 0;
    int result = 0;
    /* signal first, then each gathered dependent, scanned in turn. */
    for (size_t at = 0; result == 0; at++) {
        JSValueConst from = at == 0 ? signal : gathered[at - 1];
        DSignal *f = signal_state(from);
        for (size_t i = 0; f != NULL && i < f->count && result == 0; i++) {
            DSignal *d = f->algorithms[i].kind == A_FOLLOW ? signal_state(f->algorithms[i].target) : NULL;
            if (d == NULL || !mark_aborted(dom, ctx, d, JS_DupValue(ctx, s->reason))) continue;
            if (count == cap) {
                size_t grown_cap = cap ? cap * 2 : 8;
                JSValue *grown = js_realloc(ctx, gathered, grown_cap * sizeof(*grown));
                if (grown == NULL) {
                    result = -1;
                    break;
                }
                gathered = grown;
                cap = grown_cap;
            }
            gathered[count++] = JS_DupValue(ctx, f->algorithms[i].target);
        }
        if (at == count) break;
    }
    if (result == 0) result = abort_steps(dom, ctx, signal);
    for (size_t i = 0; i < count; i++) {
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

/* A dependent of `source`: aborted now when it is, else when it is. */
int d_signal_follow(os64_dom_t *dom, JSContext *ctx, JSValueConst dependent, JSValueConst source)
{
    DSignal *s = signal_state(source);
    if (s == NULL) return 0;
    if (s->aborted) return signal_abort(dom, ctx, dependent, s->reason);
    return algorithm_add(ctx, s, A_FOLLOW, 0, JS_DupValue(ctx, dependent));
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
        d_event_target(dom, ctx, sp, signal_id) < 0 || d_event_handler_named(dom, ctx, sp, "abort") < 0 ||
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
