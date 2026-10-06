#include "internal.h"
#include "os64/fmt.h"

/* Events: the DOM Standard's EventTarget and Event, HTML's event handlers
 * (the on<type> properties and content attributes), and the dispatch a host
 * starts for a person's input. LIBDOM.md § Events is the design record. */

static JSClassID event_slot;

static const char *const kinds[D_EVENT_COUNT] = {
    "click", "mousedown", "mouseup", "mouseover", "mouseout", "mousemove",
    "keydown", "keypress", "keyup", "input", "change", "submit", "reset",
    "focus", "blur", "load", "DOMContentLoaded"
};

enum { PHASE_NONE, PHASE_CAPTURING, PHASE_AT_TARGET, PHASE_BUBBLING };

/* An Event's state lives in engine memory and its references are traced by
 * the collector, because a script may keep an event long after its dispatch:
 * nothing native is owned, so its finalizer frees only engine storage. */
typedef struct {
    JSValue target, current;
    char *type;
    double time;
    uint8_t phase;
    bool bubbles, cancelable, canceled, stop, stop_now, dispatching, initialized, trusted;
} DEvent;

int d_event_kind(const char *type)
{
    for (int i = 0; i < D_EVENT_COUNT; i++)
        if (os64_strcmp(kinds[i], type) == 0) return i;
    return -1;
}

static bool has_handler_name(int kind)
{
    return kind >= 0 && kind != D_EVENT_DOM_CONTENT_LOADED;
}

/* HTML's "window-reflecting body element event handler set", as far as the
 * types dispatched here reach: the body's onload is window's. */
static bool window_reflecting(int kind)
{
    return kind == D_EVENT_LOAD || kind == D_EVENT_FOCUS || kind == D_EVENT_BLUR;
}

static void event_finalizer(JSRuntime *rt, JSValue value)
{
    DEvent *event = JS_GetOpaque(value, event_slot);
    if (event == NULL) return;
    JS_FreeValueRT(rt, event->target);
    JS_FreeValueRT(rt, event->current);
    js_free_rt(rt, event->type);
    js_free_rt(rt, event);
}

static void event_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DEvent *event = JS_GetOpaque(value, event_slot);
    if (event == NULL) return;
    JS_MarkValue(rt, event->target, mark);
    JS_MarkValue(rt, event->current, mark);
}

static uint64_t now_ms(const os64_dom_t *dom)
{
    return dom->options.now_ms != NULL ? dom->options.now_ms(dom->options.host_opaque) : 0;
}

JSValue d_event_new(os64_dom_t *dom, JSContext *ctx, const char *type, bool bubbles,
                    bool cancelable, bool trusted)
{
    JSValue object = JS_NewObjectClass(ctx, dom->event_class);
    if (JS_IsException(object)) return object;
    DEvent *event = js_mallocz(ctx, sizeof(*event));
    char *copy = event != NULL ? js_strdup(ctx, type) : NULL;
    if (copy == NULL) {
        js_free(ctx, event);
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    *event = (DEvent){.target = JS_NULL, .current = JS_NULL, .type = copy,
                      .time = (double)now_ms(dom), .bubbles = bubbles,
                      .cancelable = cancelable, .initialized = true, .trusted = trusted};
    JS_SetOpaque(object, event);
    return object;
}

static DEvent *event_of(os64_dom_t *dom, JSContext *ctx, JSValueConst value)
{
    DEvent *event = JS_GetOpaque(value, dom->event_class);
    if (event == NULL) JS_ThrowTypeError(ctx, "Expected an Event");
    return event;
}

static void cancel(DEvent *event)
{
    if (event->cancelable) event->canceled = true;
}

void d_report(os64_dom_t *dom, JSContext *ctx, JSValueConst error)
{
    if (dom->report.status != OS64_JS_OK) return;
    os64_js_outcome_t *out = &dom->report;
    out->status = OS64_JS_EXCEPTION;
    os64_strcopy(out->message, sizeof(out->message), "JavaScript exception; diagnostic unavailable");
    os64_strcopy(out->source_name, sizeof(out->source_name), "event handler");
    const char *text = JS_ToCString(ctx, error);
    if (text != NULL) {
        out->diagnostic_truncated = os64_strcopy(out->message, sizeof(out->message), text) >= sizeof(out->message);
        JS_FreeCString(ctx, text);
    } else {
        JSValue secondary = JS_GetException(ctx);
        JS_FreeValue(ctx, secondary);
    }
}

/* LISTENER RECORDS. A callback held by a record is one of the registry's
 * values: listeners_held counts them and drain releases them. */

static void count(os64_dom_t *dom, const DListener *listener, int delta)
{
    if (listener->kind >= 0) dom->listen_counts[listener->kind] += (uint32_t)delta;
}

static void set_callback(os64_dom_t *dom, DListener *listener, JSValue value)
{
    if (!JS_IsUndefined(listener->callback)) {
        JS_FreeValueRT(dom->engine, listener->callback);
        dom->listeners_held--;
    }
    listener->callback = value;
    if (!JS_IsUndefined(value)) dom->listeners_held++;
}

static void slot_state(os64_dom_t *dom, DListener *slot, uint8_t state)
{
    if (slot->handler == D_SLOT_FUNCTION && state != D_SLOT_FUNCTION) count(dom, slot, -1);
    if (slot->handler != D_SLOT_FUNCTION && state == D_SLOT_FUNCTION) count(dom, slot, 1);
    slot->handler = state;
    if (state != D_SLOT_FUNCTION) set_callback(dom, slot, JS_UNDEFINED);
}

static void unlink_removed(os64_dom_t *dom, DValue *entry)
{
    DListener **link = &entry->listeners;
    entry->last_listener = NULL;
    while (*link != NULL) {
        DListener *listener = *link;
        if (listener->removed) {
            *link = listener->next;
            d_free(dom, listener->type);
            d_free(dom, listener->seen);
            d_free(dom, listener);
        } else {
            entry->last_listener = listener;
            link = &listener->next;
        }
    }
}

/* A dispatch walks records by their links, so a record removed under it is
 * only marked; the last dispatch to leave sweeps the lists, when a removal
 * was deferred at all. */
static void sweep(os64_dom_t *dom)
{
    if (dom->dispatching != 0 || !dom->sweep_pending) return;
    dom->sweep_pending = false;
    for (DValue *entry = dom->values; entry != NULL; entry = entry->next)
        unlink_removed(dom, entry);
}

static void remove_listener(os64_dom_t *dom, DValue *entry, DListener *listener)
{
    if (listener->removed) return;
    listener->removed = true;
    if (listener->handler == D_LISTENER) count(dom, listener, -1);
    else slot_state(dom, listener, D_SLOT_EMPTY);
    set_callback(dom, listener, JS_UNDEFINED);
    if (dom->dispatching == 0) unlink_removed(dom, entry);
    else dom->sweep_pending = true;
}

static DListener *listener_new(os64_dom_t *dom, JSContext *ctx, DValue *entry, const char *type,
                               bool at_head)
{
    DListener *listener = d_alloc(dom, sizeof(*listener));
    size_t length = os64_strlen(type) + 1;
    char *copy = listener != NULL ? d_alloc(dom, length) : NULL;
    if (copy == NULL) {
        d_free(dom, listener);
        d_error(ctx, "QuotaExceededError", "DOM listener quota exceeded");
        return NULL;
    }
    os64_memcpy(copy, type, length);
    listener->type = copy;
    listener->kind = d_event_kind(type);
    listener->callback = JS_UNDEFINED;
    if (at_head || entry->listeners == NULL) {
        listener->next = entry->listeners;
        entry->listeners = listener;
        if (entry->last_listener == NULL) entry->last_listener = listener;
    } else {
        entry->last_listener->next = listener;
        entry->last_listener = listener;
    }
    return listener;
}

/* HANDLER SLOTS. A target's on<type> handler is one record in its list, in
 * the place it was first set (HTML's "event handler map" order). Content
 * attributes are read when the slot is consulted: a slot remembers the text
 * it last saw, so a set, changed or removed attribute is noticed by
 * whichever route it changed. */

static DListener *slot_find(DValue *entry, const char *type)
{
    for (DListener *listener = entry->listeners; listener != NULL; listener = listener->next)
        if (listener->handler != D_LISTENER && os64_strcmp(listener->type, type) == 0)
            return listener;
    return NULL;
}

static const os64_html_node_t *body_element(const os64_dom_t *dom)
{
    const os64_html_node_t *html = dom->document->html;
    if (html == NULL || html->parent != dom->document->document) return NULL;
    for (const os64_html_node_t *at = html->first_child; at != NULL; at = at->next)
        if (at->kind == OS64_HTML_ELEMENT && at->ns == OS64_HTML_NS_HTML &&
            (at->tag == OS64_HTML_TAG_BODY || at->tag == OS64_HTML_TAG_FRAMESET)) return at;
    return NULL;
}

static bool body_like(const os64_html_node_t *node)
{
    return node != NULL && node->kind == OS64_HTML_ELEMENT && node->ns == OS64_HTML_NS_HTML &&
        (node->tag == OS64_HTML_TAG_BODY || node->tag == OS64_HTML_TAG_FRAMESET);
}

/* The element whose content attribute feeds this target's slot. */
static const os64_html_node_t *attribute_source(os64_dom_t *dom, DValue *entry, int kind)
{
    if (!has_handler_name(kind)) return NULL;
    if (entry == dom->window) return window_reflecting(kind) ? body_element(dom) : NULL;
    const os64_html_node_t *node = entry->node;
    if (node == NULL || node->kind != OS64_HTML_ELEMENT) return NULL;
    /* The body's window-reflecting attributes belong to window, not to it. */
    if (body_like(node) && window_reflecting(kind)) return NULL;
    return node;
}

static const char *handler_attribute(const os64_html_node_t *node, int kind)
{
    char name[32] = "on";
    os64_strcopy(name + 2, sizeof(name) - 2, kinds[kind]);
    if (node->ns == OS64_HTML_NS_HTML) d_fold(name);
    const os64_html_attr_t *attr = os64_html_attr(node, name);
    return attr != NULL ? attr->value : NULL;
}

/* Bring a slot up to date with its attribute. -1 with an exception pending
 * when the binding could not keep the text it needs to compare against. */
static int slot_refresh(os64_dom_t *dom, JSContext *ctx, DValue *entry, DListener *slot)
{
    const os64_html_node_t *source = attribute_source(dom, entry, slot->kind);
    const char *text = source != NULL ? handler_attribute(source, slot->kind) : NULL;
    if (text == NULL ? slot->seen == NULL : slot->seen != NULL && os64_strcmp(text, slot->seen) == 0)
        return 0;
    char *copy = NULL;
    if (text != NULL) {
        size_t length = os64_strlen(text) + 1;
        copy = d_alloc(dom, length);
        if (copy == NULL) {
            d_error(ctx, "QuotaExceededError", "DOM handler quota exceeded");
            return -1;
        }
        os64_memcpy(copy, text, length);
    }
    d_free(dom, slot->seen);
    slot->seen = copy;
    slot_state(dom, slot, text != NULL ? D_SLOT_UNCOMPILED : D_SLOT_EMPTY);
    return 0;
}

/* A slot for an attribute nobody has looked at yet goes to the head of an
 * ELEMENT's list: the parser and innerHTML give an element its attributes
 * before any script can reach it, and a script's own setAttribute makes its
 * slot then. Window's slots come from the body, which window predates: one
 * found late goes to the tail, and window takes in the body's attributes
 * before every listener added to it (target_method), so each lands where
 * the body was parsed among them. */
static DListener *slot_for(os64_dom_t *dom, JSContext *ctx, DValue *entry, int kind, bool at_head)
{
    DListener *slot = slot_find(entry, kinds[kind]);
    if (slot != NULL) return slot;
    slot = listener_new(dom, ctx, entry, kinds[kind], at_head);
    if (slot != NULL) slot->handler = D_SLOT_EMPTY;
    return slot;
}

static int slot_attribute_pending(os64_dom_t *dom, JSContext *ctx, DValue *entry, int kind)
{
    const os64_html_node_t *source = attribute_source(dom, entry, kind);
    if (source == NULL || slot_find(entry, kinds[kind]) != NULL ||
        handler_attribute(source, kind) == NULL) return 0;
    return slot_for(dom, ctx, entry, kind, entry != dom->window) != NULL ? 0 : -1;
}

/* Whether an attribute name is on<type> for a handler type, ignoring ASCII
 * case as HTML's attribute names do. */
static int handler_kind(const char *name)
{
    if ((name[0] | 0x20) != 'o' || (name[1] | 0x20) != 'n') return -1;
    for (int kind = 0; kind < D_EVENT_COUNT; kind++) {
        if (!has_handler_name(kind)) continue;
        const char *a = name + 2, *b = kinds[kind];
        while (*a != '\0' && *b != '\0' && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
        if (*a == '\0' && *b == '\0') return kind;
    }
    return -1;
}

int d_handler_attribute_set(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                            const char *name)
{
    int kind = handler_kind(name);
    if (kind < 0 || node->kind != OS64_HTML_ELEMENT) return 0;
    DValue *entry = body_like(node) && window_reflecting(kind) ? dom->window : d_find(dom, node);
    if (entry == NULL) return 0;
    return slot_for(dom, ctx, entry, kind, false) != NULL ? 0 : -1;
}

static const os64_html_node_t *document_node(const os64_dom_t *dom)
{
    return dom->document->document;
}

/* HTML's "getting the current value of the event handler": the body is
 * compiled as a function of `event` inside three object environments,
 * document, then the form owner, then the element. The body is first given
 * to the intrinsic Function constructor, which refuses most bodies that are
 * not a FunctionBody; QuickJS builds that function by joining strings, so a
 * body written to close the function early in both shapes still compiles.
 * That is the page's own code in its own realm: it can change when that
 * code runs, not what it may do (LIBDOM.md § Events). Anything but a
 * function out of the wrapper is a compile failure. */
static int slot_compile(os64_dom_t *dom, JSContext *ctx, DValue *entry, DListener *slot)
{
    const os64_html_node_t *element = attribute_source(dom, entry, slot->kind);
    JSValue parts[2] = {JS_NewString(ctx, "event"), JS_NewString(ctx, slot->seen)};
    if (JS_IsException(parts[0]) || JS_IsException(parts[1])) {
        JS_FreeValue(ctx, parts[0]);
        JS_FreeValue(ctx, parts[1]);
        return -1;
    }
    JSValue check = JS_CallConstructor(ctx, dom->function_ctor->value, 2, parts);
    JS_FreeValue(ctx, parts[0]);
    JS_FreeValue(ctx, parts[1]);
    if (JS_IsException(check)) {
        slot_state(dom, slot, D_SLOT_ERROR);
        return -1;
    }
    JS_FreeValue(ctx, check);
    static const char head[] = "(function(){with(arguments[0])with(arguments[1])with(arguments[2])"
                               "return function on";
    size_t name_length = os64_strlen(slot->type), body_length = os64_strlen(slot->seen);
    size_t length = sizeof(head) - 1 + name_length + 10 + body_length + 5;
    char *source = d_alloc(dom, length + 1);
    if (source == NULL) {
        d_error(ctx, "QuotaExceededError", "DOM handler quota exceeded");
        return -1;
    }
    size_t at = 0;
    os64_memcpy(source + at, head, sizeof(head) - 1); at += sizeof(head) - 1;
    os64_memcpy(source + at, slot->type, name_length); at += name_length;
    os64_memcpy(source + at, "(event){\n", 9); at += 9;
    os64_memcpy(source + at, slot->seen, body_length); at += body_length;
    os64_memcpy(source + at, "\n}})", 4); at += 4;
    source[at] = '\0';
    char name[48] = "on";
    os64_strcopy(name + 2, sizeof(name) - 2, slot->type);
    os64_strcopy(name + os64_strlen(name), sizeof(name) - os64_strlen(name), " attribute");
    JSValue factory = JS_Eval(ctx, source, at, name, JS_EVAL_TYPE_GLOBAL);
    d_free(dom, source);
    if (JS_IsException(factory)) {
        slot_state(dom, slot, D_SLOT_ERROR);
        return -1;
    }
    const os64_html_node_t *form = element != NULL ? d_form_owner(dom, element) : NULL;
    JSValue scopes[3] = {
        d_wrap(dom, ctx, document_node(dom)),
        form != NULL ? d_wrap(dom, ctx, form) : JS_NewObject(ctx),
        element != NULL ? d_wrap(dom, ctx, element) : JS_NewObject(ctx)
    };
    JSValue function = JS_EXCEPTION;
    if (!JS_IsException(scopes[0]) && !JS_IsException(scopes[1]) && !JS_IsException(scopes[2]))
        function = JS_Call(ctx, factory, JS_UNDEFINED, 3, scopes);
    for (int i = 0; i < 3; i++) JS_FreeValue(ctx, scopes[i]);
    JS_FreeValue(ctx, factory);
    if (!JS_IsException(function) && !JS_IsFunction(ctx, function)) {
        JS_FreeValue(ctx, function);
        function = JS_ThrowSyntaxError(ctx, "on%s handler is not a function body", slot->type);
    }
    if (JS_IsException(function)) {
        slot_state(dom, slot, D_SLOT_ERROR);
        return -1;
    }
    slot_state(dom, slot, D_SLOT_FUNCTION);
    set_callback(dom, slot, function);
    return 0;
}

/* The slot's function, compiling it if its attribute has not been: a new
 * reference, JS_NULL when there is none, JS_EXCEPTION when compiling threw. */
static JSValue slot_value(os64_dom_t *dom, JSContext *ctx, DValue *entry, DListener *slot)
{
    if (slot_refresh(dom, ctx, entry, slot) < 0) return JS_EXCEPTION;
    if (slot->handler == D_SLOT_UNCOMPILED && slot_compile(dom, ctx, entry, slot) < 0)
        return JS_EXCEPTION;
    return slot->handler == D_SLOT_FUNCTION ? JS_DupValue(ctx, slot->callback) : JS_NULL;
}

/* A listener is a function, or an object whose handleEvent is one. */
static JSValue call_callback(JSContext *ctx, JSValueConst callback, JSValueConst self,
                             JSValueConst event)
{
    if (JS_IsFunction(ctx, callback))
        return JS_Call(ctx, callback, self, 1, &event);
    JSValue method = JS_GetPropertyStr(ctx, callback, "handleEvent");
    if (JS_IsException(method)) return method;
    JSValue result;
    if (!JS_IsFunction(ctx, method)) result = JS_ThrowTypeError(ctx, "Listener has no handleEvent method");
    else result = JS_Call(ctx, method, callback, 1, &event);
    JS_FreeValue(ctx, method);
    return result;
}

/* Run one record against an event. The callback is taken before a `once`
 * listener is removed, so the record's own reference may go first. HTML's
 * event handler processing: a handler that returns false cancels. */
static JSValue run_record(os64_dom_t *dom, JSContext *ctx, DValue *entry, DListener *listener,
                          JSValueConst callback, JSValueConst event)
{
    JSValue held = JS_UNDEFINED;
    if (JS_IsUndefined(callback)) {
        held = slot_value(dom, ctx, entry, listener);
        if (JS_IsException(held) || JS_IsNull(held)) return held;
        callback = held;
    }
    JSValue result = call_callback(ctx, callback, entry->value, event);
    if (listener->handler != D_LISTENER && JS_VALUE_GET_TAG(result) == JS_TAG_BOOL &&
        !JS_VALUE_GET_BOOL(result)) {
        DEvent *state = JS_GetOpaque(event, dom->event_class);
        if (state != NULL) cancel(state);
    }
    JS_FreeValue(ctx, held);
    return result;
}

/* The host's dispatch enters the engine through os64_js_call on this one
 * function, so every listener runs inside the wrapper's guard and budget.
 * The record is passed in dom->invoking; a listener's own callback rides as
 * the second argument because removing a `once` listener drops the record's
 * reference before the call. */
static JSValue invoke(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                      int magic, JSValue *data)
{
    (void)self; (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DListener *listener = dom->invoking;
    DValue *entry = dom->invoking_entry;
    dom->invoking = NULL;
    if (listener == NULL || entry == NULL || argc < 2) return JS_ThrowTypeError(ctx, "Nothing to invoke");
    JSValue result = run_record(dom, ctx, entry, listener, argv[1], argv[0]);
    if (JS_IsException(result)) return result;
    JS_FreeValue(ctx, result);
    return JS_UNDEFINED;
}

static void report_pending(os64_dom_t *dom, JSContext *ctx)
{
    JSValue error = JS_GetException(ctx);
    d_report(dom, ctx, error);
    JS_FreeValue(ctx, error);
}

static bool runtime_failed(os64_dom_t *dom)
{
    os64_js_outcome_t probe;
    return os64_js_context(dom->runtime, OS64_JS_ABI_ID, &probe) == NULL;
}

static bool sticky(os64_js_status_t status)
{
    return status == OS64_JS_LIMIT || status == OS64_JS_CANCELLED ||
        status == OS64_JS_HOST_FAILURE || status == OS64_JS_FAILED_RUNTIME;
}

static void fold(os64_js_outcome_t *into, const os64_js_outcome_t *from)
{
    if (from->status == OS64_JS_OK || from->status == OS64_JS_MORE_JOBS) return;
    if (into->status == OS64_JS_OK || (sticky(from->status) && !sticky(into->status)))
        *into = *from;
}

typedef struct {
    os64_dom_t *dom;
    JSContext *ctx;
    JSValue object;
    DEvent *event;
    /* Host dispatch only: each listener is a wrapper call and a checkpoint. */
    os64_js_outcome_t *host;
    /* HTML's load at window reads the document as its target. */
    bool document_target;
} Dispatch;

/* One step of the path: the target's records whose capture flag fits the
 * phase. The list is cut where it ended when this target was reached, so a
 * listener added during the dispatch waits for the next one. -1 ends the
 * dispatch: the runtime is finished with (an exception is pending in script
 * mode). */
static int invoke_target(Dispatch *d, DValue *entry, uint8_t phase, bool capture)
{
    os64_dom_t *dom = d->dom;
    DEvent *event = d->event;
    if (entry == NULL || event->stop) return 0;
    int kind = d_event_kind(event->type);
    if (slot_attribute_pending(dom, d->ctx, entry, kind) < 0) report_pending(dom, d->ctx);
    DListener *last = entry->last_listener;
    if (last == NULL) return 0;
    JS_FreeValue(d->ctx, event->current);
    event->current = JS_DupValue(d->ctx, entry->value);
    event->phase = phase;
    int result = 0;
    for (DListener *listener = entry->listeners; listener != NULL; listener = listener->next) {
        bool final = listener == last;
        bool slot = listener->handler != D_LISTENER;
        bool fits = slot ? !capture : listener->capture == capture;
        bool runs = !listener->removed && fits && os64_strcmp(listener->type, event->type) == 0;
        /* A slot is brought up to date here, where no script runs, so an
         * empty one costs no engine entry; compiling happens in the call. */
        if (runs && slot && slot_refresh(dom, d->ctx, entry, listener) < 0) report_pending(dom, d->ctx);
        if (runs && slot) runs = listener->handler == D_SLOT_FUNCTION || listener->handler == D_SLOT_UNCOMPILED;
        if (runs) {
            JSValue callback = slot ? JS_UNDEFINED : JS_DupValue(d->ctx, listener->callback);
            if (listener->once) remove_listener(dom, entry, listener);
            if (d->host != NULL) {
                JSValue args[2] = {d->object, callback};
                os64_js_outcome_t call, point;
                dom->invoking = listener;
                dom->invoking_entry = entry;
                os64_js_call(dom->runtime, OS64_JS_ABI_ID, dom->invoke->value, entry->value, 2, args,
                             NULL, &call);
                dom->invoking = NULL;
                dom->invoking_entry = NULL;
                JS_FreeValueRT(dom->engine, callback);
                fold(d->host, &call);
                if (!sticky(call.status)) {
                    os64_js_checkpoint(dom->runtime, OS64_JS_ABI_ID, &point);
                    fold(d->host, &point);
                    if (sticky(point.status)) { result = -1; break; }
                } else { result = -1; break; }
            } else {
                JSValue value = run_record(dom, d->ctx, entry, listener, callback, d->object);
                JS_FreeValue(d->ctx, callback);
                if (JS_IsException(value)) {
                    JSValue error = JS_GetException(d->ctx);
                    if (runtime_failed(dom)) {
                        JS_Throw(d->ctx, error);
                        result = -1;
                        break;
                    }
                    d_report(dom, d->ctx, error);
                    JS_FreeValue(d->ctx, error);
                } else JS_FreeValue(d->ctx, value);
            }
            if (event->stop_now) break;
        }
        if (final) break;
    }
    return result;
}

static bool element_has_handler(const os64_html_node_t *node, int kind)
{
    return has_handler_name(kind) && node->kind == OS64_HTML_ELEMENT &&
        !(body_like(node) && window_reflecting(kind)) && handler_attribute(node, kind) != NULL;
}

/* The DOM Standard's dispatch, for one event at one target: the path is the
 * target's ancestors and, past the document, window (except for load);
 * capture down, the target, then bubble up when the event bubbles. */
static int dispatch(Dispatch *d, const os64_html_node_t *node)
{
    os64_dom_t *dom = d->dom;
    JSContext *ctx = d->ctx;
    DEvent *event = d->event;
    int kind = d_event_kind(event->type);
    size_t length = 0;
    const os64_html_node_t *top = node;
    for (const os64_html_node_t *at = node; at != NULL; at = at->parent) { length++; top = at; }
    bool to_window = node == NULL || (top == document_node(dom) && kind != D_EVENT_LOAD);
    if (to_window) length++;
    DValue **path = d_alloc(dom, length * sizeof(*path));
    if (path == NULL) {
        d_error(ctx, "QuotaExceededError", "DOM event path quota exceeded");
        return -1;
    }
    JSValue target;
    if (node == NULL)
        target = d->document_target ? d_wrap(dom, ctx, document_node(dom)) : JS_DupValue(ctx, dom->window->value);
    else target = d_wrap(dom, ctx, node);
    if (JS_IsException(target)) { d_free(dom, path); return -1; }
    size_t count = 0;
    for (const os64_html_node_t *at = node; at != NULL; at = at->parent) {
        DValue *entry = d_find(dom, at);
        if (entry == NULL && element_has_handler(at, kind)) {
            JSValue wrapper = d_wrap(dom, ctx, at);
            if (JS_IsException(wrapper)) { JS_FreeValue(ctx, target); d_free(dom, path); return -1; }
            JS_FreeValue(ctx, wrapper);
            entry = d_find(dom, at);
        }
        path[count++] = entry;
    }
    if (to_window) path[count++] = dom->window;
    JS_FreeValue(ctx, event->target);
    event->target = target;
    event->dispatching = true;
    dom->dispatching++;
    int result = 0;
    for (size_t i = count; result == 0 && i > 1; i--)
        result = invoke_target(d, path[i - 1], PHASE_CAPTURING, true);
    if (result == 0) result = invoke_target(d, path[0], PHASE_AT_TARGET, true);
    if (result == 0) result = invoke_target(d, path[0], PHASE_AT_TARGET, false);
    for (size_t i = 1; result == 0 && event->bubbles && i < count; i++)
        result = invoke_target(d, path[i], PHASE_BUBBLING, false);
    dom->dispatching--;
    event->dispatching = false;
    event->phase = PHASE_NONE;
    event->stop = event->stop_now = false;
    JS_FreeValue(ctx, event->current);
    event->current = JS_NULL;
    d_free(dom, path);
    sweep(dom);
    return result < 0 ? -1 : event->canceled ? 0 : 1;
}

int d_dispatch(os64_dom_t *dom, JSContext *ctx, JSValueConst object, const os64_html_node_t *node)
{
    Dispatch d = {dom, ctx, (JSValue)object, JS_GetOpaque(object, dom->event_class), NULL, false};
    if (d.event == NULL) { JS_ThrowTypeError(ctx, "Expected an Event"); return -1; }
    return dispatch(&d, node);
}

static bool define_number(JSContext *ctx, JSValueConst object, const char *name, double value)
{
    return JS_DefinePropertyValueStr(ctx, object, name, JS_NewFloat64(ctx, value),
                                     JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE) >= 0;
}

static bool define_bool(JSContext *ctx, JSValueConst object, const char *name, bool value)
{
    return JS_DefinePropertyValueStr(ctx, object, name, JS_NewBool(ctx, value),
                                     JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE) >= 0;
}

/* The fields a mouse or key event carries, as plain data. The old web reads
 * keyCode and which; key is the modern spelling of the same key. */
static bool event_fields(os64_dom_t *dom, JSContext *ctx, JSValueConst object, const os64_dom_event_t *ev)
{
    if (ev->kind == OS64_DOM_EVENT_PLAIN) return true;
    bool ok = define_bool(ctx, object, "shiftKey", ev->shift) && define_bool(ctx, object, "ctrlKey", ev->ctrl) &&
        define_bool(ctx, object, "altKey", ev->alt) && define_bool(ctx, object, "metaKey", ev->meta);
    if (ok && ev->kind == OS64_DOM_EVENT_MOUSE) {
        ok = define_number(ctx, object, "clientX", ev->client_x) && define_number(ctx, object, "clientY", ev->client_y) &&
            define_number(ctx, object, "pageX", ev->client_x) && define_number(ctx, object, "pageY", ev->client_y) &&
            define_number(ctx, object, "x", ev->client_x) && define_number(ctx, object, "y", ev->client_y) &&
            define_number(ctx, object, "screenX", ev->screen_x) && define_number(ctx, object, "screenY", ev->screen_y) &&
            define_number(ctx, object, "button", ev->button) && define_number(ctx, object, "which", ev->button + 1);
        if (ok) {
            JSValue related = ev->related != NULL ? d_wrap(dom, ctx, ev->related) : JS_NULL;
            ok = !JS_IsException(related) && JS_DefinePropertyValueStr(ctx, object, "relatedTarget", related,
                    JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE) >= 0;
        }
    }
    if (ok && ev->kind == OS64_DOM_EVENT_KEY) {
        JSValue key = JS_NewString(ctx, ev->key != NULL ? ev->key : "");
        ok = !JS_IsException(key) && JS_DefinePropertyValueStr(ctx, object, "key", key,
                JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE) >= 0 &&
            define_number(ctx, object, "keyCode", ev->key_code) &&
            define_number(ctx, object, "charCode", ev->char_code) &&
            define_number(ctx, object, "which", ev->key_code);
    }
    return ok;
}

static void discard(JSContext *ctx)
{
    JSValue error = JS_GetException(ctx);
    JS_FreeValue(ctx, error);
}

static os64_js_status_t finish_task(os64_dom_t *dom, os64_js_outcome_t *outcome)
{
    os64_js_outcome_t end;
    os64_js_task_end(dom->runtime, OS64_JS_ABI_ID, &end);
    fold(outcome, &end);
    if (outcome->status == OS64_JS_OK && dom->report.status != OS64_JS_OK) {
        *outcome = dom->report;
        os64_memset(&dom->report, 0, sizeof(dom->report));
    }
    return outcome->status;
}

os64_js_status_t os64_dom_dispatch(os64_dom_t *dom, const os64_html_node_t *node,
                                   const os64_dom_event_t *ev, bool *prevented,
                                   os64_js_outcome_t *outcome)
{
    if (prevented != NULL) *prevented = false;
    if (outcome == NULL) return OS64_JS_BAD_ARGUMENT;
    os64_memset(outcome, 0, sizeof(*outcome));
    if (dom == NULL || ev == NULL || ev->type == NULL ||
        (node != NULL && !os64_html_owns_node(dom->document, node)) ||
        (ev->related != NULL && !os64_html_owns_node(dom->document, ev->related))) {
        outcome->status = OS64_JS_BAD_ARGUMENT;
        return outcome->status;
    }
    if (dom->closed) {
        outcome->status = OS64_JS_FAILED_RUNTIME;
        return outcome->status;
    }
    if (!os64_dom_listens(dom, ev->type)) return OS64_JS_OK;
    char name[OS64_JS_SOURCE_NAME_CAP];
    os64_snprintf(name, sizeof(name), "%s event", ev->type);
    os64_js_outcome_t begin;
    if (os64_js_task_begin(dom->runtime, OS64_JS_ABI_ID, name, &begin) != OS64_JS_OK) {
        *outcome = begin;
        return outcome->status;
    }
    os64_js_outcome_t access;
    JSContext *ctx = os64_js_context(dom->runtime, OS64_JS_ABI_ID, &access);
    if (ctx == NULL) return finish_task(dom, outcome);
    JSValue object = d_event_new(dom, ctx, ev->type, ev->bubbles, ev->cancelable, true);
    if (JS_IsException(object)) { discard(ctx); return finish_task(dom, outcome); }
    if (!event_fields(dom, ctx, object, ev)) {
        discard(ctx);
        JS_FreeValue(ctx, object);
        return finish_task(dom, outcome);
    }
    Dispatch d = {dom, ctx, object, JS_GetOpaque(object, dom->event_class), outcome,
                  node == NULL && d_event_kind(ev->type) == D_EVENT_LOAD};
    /* Engine refusals between the wrapper's calls are latched by libjs and
     * reported by task_end; the binding's own refusal is reported here. */
    if (dispatch(&d, node) < 0 && JS_HasException(ctx)) {
        JSValue error = JS_GetException(ctx);
        d_report(dom, ctx, error);
        JS_FreeValue(ctx, error);
    }
    if (prevented != NULL) *prevented = d.event->canceled;
    JS_FreeValue(ctx, object);
    return finish_task(dom, outcome);
}

/* Attributes are counted once per tree version: a walk of the connected tree
 * for on<type> names. Listener and property counts are kept as they change. */
static uint32_t attribute_mask(os64_dom_t *dom)
{
    uint64_t version = os64_html_version(dom->document);
    if (dom->attribute_scanned && dom->attribute_version == version) return dom->attribute_mask;
    uint32_t mask = 0;
    const os64_html_node_t *root = document_node(dom);
    for (const os64_html_node_t *at = root; at != NULL;) {
        if (at->kind == OS64_HTML_ELEMENT)
            for (const os64_html_attr_t *attr = at->attrs; attr != NULL; attr = attr->next) {
                int kind = handler_kind(attr->name);
                if (kind >= 0) mask |= 1u << kind;
            }
        if (at->first_child != NULL) { at = at->first_child; continue; }
        while (at != NULL && at != root && at->next == NULL) at = at->parent;
        at = at != NULL && at != root ? at->next : NULL;
    }
    dom->attribute_mask = mask;
    dom->attribute_version = version;
    dom->attribute_scanned = true;
    return mask;
}

bool os64_dom_listens(os64_dom_t *dom, const char *type)
{
    if (dom == NULL || dom->closed || type == NULL) return false;
    int kind = d_event_kind(type);
    if (kind < 0) return true;
    return dom->listen_counts[kind] != 0 || (attribute_mask(dom) & (1u << kind)) != 0;
}

/* SCRIPT SURFACE. */

static DValue *target_entry(os64_dom_t *dom, JSContext *ctx, JSValueConst self)
{
    if (JS_IsUndefined(self) || JS_IsNull(self) ||
        (JS_IsObject(self) && JS_VALUE_GET_PTR(self) == JS_VALUE_GET_PTR(dom->window->value)))
        return dom->window;
    const os64_html_node_t *node = d_node(dom, ctx, self);
    if (node == NULL) return NULL;
    DValue *entry = d_find(dom, node);
    if (entry == NULL) JS_ThrowTypeError(ctx, "Missing node wrapper");
    return entry;
}

static int listener_options(JSContext *ctx, int argc, JSValueConst *argv, bool *capture, bool *once)
{
    *capture = *once = false;
    if (argc < 3 || JS_IsUndefined(argv[2])) return 0;
    if (!JS_IsObject(argv[2])) {
        int flag = JS_ToBool(ctx, argv[2]);
        if (flag < 0) return -1;
        *capture = flag != 0;
        return 0;
    }
    const char *names[] = {"capture", "once"};
    bool *flags[] = {capture, once};
    for (int i = 0; i < 2; i++) {
        JSValue value = JS_GetPropertyStr(ctx, argv[2], names[i]);
        if (JS_IsException(value)) return -1;
        int flag = JS_ToBool(ctx, value);
        JS_FreeValue(ctx, value);
        if (flag < 0) return -1;
        *flags[i] = flag != 0;
    }
    return 0;
}

enum { M_ADD, M_REMOVE, M_DISPATCH };

static JSValue target_method(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                             int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DValue *entry = target_entry(dom, ctx, self);
    if (entry == NULL) return JS_EXCEPTION;
    if (magic == M_DISPATCH) {
        if (argc < 1) return JS_ThrowTypeError(ctx, "dispatchEvent needs an event");
        DEvent *event = event_of(dom, ctx, argv[0]);
        if (event == NULL) return JS_EXCEPTION;
        if (event->dispatching || !event->initialized)
            return d_error(ctx, "InvalidStateError", "The event is already being dispatched or is not initialized");
        // A redispatched event stays cancelled: initEvent is the reset, and
        // a vetoed default action must not come back on the second try.
        event->trusted = false;
        int result = d_dispatch(dom, ctx, argv[0], entry == dom->window ? NULL : entry->node);
        return result < 0 ? JS_EXCEPTION : JS_NewBool(ctx, result != 0);
    }
    if (argc < 2) return JS_ThrowTypeError(ctx, "Listener methods need a type and a callback");
    if (JS_IsNull(argv[1]) || JS_IsUndefined(argv[1])) return JS_UNDEFINED;
    if (!JS_IsObject(argv[1])) return JS_ThrowTypeError(ctx, "A listener must be an object");
    bool capture, once;
    if (listener_options(ctx, argc, argv, &capture, &once) < 0) return JS_EXCEPTION;
    DString type = {0};
    if (!d_string(dom, ctx, argv[0], &type)) return JS_EXCEPTION;
    DListener *found = NULL;
    for (DListener *listener = entry->listeners; listener != NULL; listener = listener->next)
        if (listener->handler == D_LISTENER && !listener->removed && listener->capture == capture &&
            JS_VALUE_GET_PTR(listener->callback) == JS_VALUE_GET_PTR(argv[1]) &&
            os64_strcmp(listener->type, type.data) == 0) found = listener;
    JSValue result = JS_UNDEFINED;
    if (magic == M_REMOVE) {
        if (found != NULL) remove_listener(dom, entry, found);
    } else if (found == NULL) {
        for (int kind = 0; entry == dom->window && kind < D_EVENT_COUNT; kind++)
            if (window_reflecting(kind) && slot_attribute_pending(dom, ctx, entry, kind) < 0) {
                d_string_free(dom, &type);
                return JS_EXCEPTION;
            }
        DListener *listener = listener_new(dom, ctx, entry, type.data, false);
        if (listener == NULL) result = JS_EXCEPTION;
        else {
            listener->capture = capture;
            listener->once = once;
            set_callback(dom, listener, JS_DupValue(ctx, argv[1]));
            count(dom, listener, 1);
        }
    }
    d_string_free(dom, &type);
    return result;
}

/* on<type> for an element, the document or window. The body's
 * window-reflecting handlers are window's, whichever object is asked. */
static JSValue handler_property(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                                int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    int kind = magic & ~D_SET;
    DValue *entry = target_entry(dom, ctx, self);
    if (entry == NULL) return JS_EXCEPTION;
    if (entry != dom->window && body_like(entry->node) && window_reflecting(kind)) entry = dom->window;
    if (!set) {
        DListener *slot = slot_find(entry, kinds[kind]);
        if (slot == NULL) {
            if (slot_attribute_pending(dom, ctx, entry, kind) < 0) return JS_EXCEPTION;
            slot = slot_find(entry, kinds[kind]);
            if (slot == NULL) return JS_NULL;
        }
        JSValue value = slot_value(dom, ctx, entry, slot);
        if (JS_IsException(value)) {
            /* HTML reports a handler that fails to compile and answers null. */
            JSValue error = JS_GetException(ctx);
            if (runtime_failed(dom)) return JS_Throw(ctx, error);
            d_report(dom, ctx, error);
            JS_FreeValue(ctx, error);
            return JS_NULL;
        }
        return value;
    }
    DListener *slot = slot_for(dom, ctx, entry, kind, false);
    if (slot == NULL || slot_refresh(dom, ctx, entry, slot) < 0) return JS_EXCEPTION;
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (JS_IsObject(value)) {
        slot_state(dom, slot, D_SLOT_FUNCTION);
        set_callback(dom, slot, JS_DupValue(ctx, value));
    } else slot_state(dom, slot, D_SLOT_EMPTY);
    return JS_UNDEFINED;
}

enum {
    E_TYPE, E_TARGET, E_CURRENT, E_PHASE, E_BUBBLES, E_CANCELABLE, E_PREVENTED,
    E_RETURN_VALUE, E_CANCEL_BUBBLE, E_TIME, E_TRUSTED,
    E_PREVENT, E_STOP, E_STOP_NOW, E_INIT
};

static JSValue event_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                            int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DEvent *event = event_of(dom, ctx, self);
    if (event == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    if (set) {
        int flag = JS_ToBool(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
        if (flag < 0) return JS_EXCEPTION;
        if (magic == E_RETURN_VALUE && !flag) cancel(event);
        if (magic == E_CANCEL_BUBBLE && flag) event->stop = true;
        return JS_UNDEFINED;
    }
    switch (magic) {
    case E_TYPE: return JS_NewString(ctx, event->type);
    case E_TARGET: return JS_DupValue(ctx, event->target);
    case E_CURRENT: return JS_DupValue(ctx, event->current);
    case E_PHASE: return JS_NewInt32(ctx, event->phase);
    case E_BUBBLES: return JS_NewBool(ctx, event->bubbles);
    case E_CANCELABLE: return JS_NewBool(ctx, event->cancelable);
    case E_PREVENTED: return JS_NewBool(ctx, event->canceled);
    case E_RETURN_VALUE: return JS_NewBool(ctx, !event->canceled);
    case E_CANCEL_BUBBLE: return JS_NewBool(ctx, event->stop);
    case E_TIME: return JS_NewFloat64(ctx, event->time);
    case E_TRUSTED: return JS_NewBool(ctx, event->trusted);
    case E_PREVENT: cancel(event); return JS_UNDEFINED;
    case E_STOP: event->stop = true; return JS_UNDEFINED;
    case E_STOP_NOW: event->stop = event->stop_now = true; return JS_UNDEFINED;
    case E_INIT: {
        if (event->dispatching) return JS_UNDEFINED;
        if (argc < 1) return JS_ThrowTypeError(ctx, "initEvent needs a type");
        const char *type = JS_ToCString(ctx, argv[0]);
        if (type == NULL) return JS_EXCEPTION;
        char *copy = js_strdup(ctx, type);
        JS_FreeCString(ctx, type);
        if (copy == NULL) return JS_EXCEPTION;
        int bubbles = argc > 1 ? JS_ToBool(ctx, argv[1]) : 0;
        int cancelable = argc > 2 ? JS_ToBool(ctx, argv[2]) : 0;
        if (bubbles < 0 || cancelable < 0) { js_free(ctx, copy); return JS_EXCEPTION; }
        js_free(ctx, event->type);
        event->type = copy;
        event->bubbles = bubbles != 0;
        event->cancelable = cancelable != 0;
        event->initialized = true;
        event->canceled = event->stop = event->stop_now = false;
        JS_FreeValue(ctx, event->target);
        event->target = JS_NULL;
        return JS_UNDEFINED;
    }
    default: return JS_ThrowTypeError(ctx, "Unknown Event member");
    }
}

static JSValue event_construct(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                               int magic, JSValue *data)
{
    (void)self;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (magic == 1) {
        /* document.createEvent: an event initEvent must set up first. */
        JSValue object = d_event_new(dom, ctx, "", false, false, false);
        if (!JS_IsException(object)) ((DEvent *)JS_GetOpaque(object, dom->event_class))->initialized = false;
        return object;
    }
    if (argc < 1) return JS_ThrowTypeError(ctx, "Event needs a type");
    bool flags[2] = {false, false};
    if (argc > 1 && JS_IsObject(argv[1])) {
        const char *names[] = {"bubbles", "cancelable"};
        for (int i = 0; i < 2; i++) {
            JSValue value = JS_GetPropertyStr(ctx, argv[1], names[i]);
            if (JS_IsException(value)) return value;
            int flag = JS_ToBool(ctx, value);
            JS_FreeValue(ctx, value);
            if (flag < 0) return JS_EXCEPTION;
            flags[i] = flag != 0;
        }
    }
    const char *type = JS_ToCString(ctx, argv[0]);
    if (type == NULL) return JS_EXCEPTION;
    JSValue object = d_event_new(dom, ctx, type, flags[0], flags[1], false);
    JS_FreeCString(ctx, type);
    return object;
}

static int define_constants(JSContext *ctx, JSValueConst target)
{
    const char *names[] = {"NONE", "CAPTURING_PHASE", "AT_TARGET", "BUBBLING_PHASE"};
    for (int i = 0; i < 4; i++)
        if (JS_DefinePropertyValueStr(ctx, target, names[i], JS_NewInt32(ctx, i), 0) < 0) return -1;
    return 0;
}

int d_event_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    dom->event_class = os64_js_class_id(&event_slot);
    const JSClassDef definition = {.class_name = "Event", .finalizer = event_finalizer, .gc_mark = event_mark};
    if (JS_NewClass(dom->engine, dom->event_class, &definition) < 0) return -1;
    JSValue prototype = JS_NewObject(ctx);
    if (JS_IsException(prototype)) return -1;
    static const struct { const char *name; int magic; bool writable; } members[] = {
        {"type", E_TYPE, false}, {"target", E_TARGET, false}, {"srcElement", E_TARGET, false},
        {"currentTarget", E_CURRENT, false}, {"eventPhase", E_PHASE, false},
        {"bubbles", E_BUBBLES, false}, {"cancelable", E_CANCELABLE, false},
        {"defaultPrevented", E_PREVENTED, false}, {"returnValue", E_RETURN_VALUE, true},
        {"cancelBubble", E_CANCEL_BUBBLE, true}, {"timeStamp", E_TIME, false}, {"isTrusted", E_TRUSTED, false}
    };
    static const struct { const char *name; int magic, argc; } methods[] = {
        {"preventDefault", E_PREVENT, 0}, {"stopPropagation", E_STOP, 0},
        {"stopImmediatePropagation", E_STOP_NOW, 0}, {"initEvent", E_INIT, 3}
    };
    int result = define_constants(ctx, prototype);
    for (size_t i = 0; result >= 0 && i < sizeof(members) / sizeof(members[0]); i++)
        result = d_accessor(dom, ctx, prototype, members[i].name, event_member, members[i].magic, members[i].writable);
    for (size_t i = 0; result >= 0 && i < sizeof(methods) / sizeof(methods[0]); i++)
        result = d_method(dom, ctx, prototype, methods[i].name, event_member, methods[i].argc, methods[i].magic);
    JSValue constructor = result >= 0 ? JS_NewCFunctionData(ctx, event_construct, 1, 0, 1, &dom->anchor->value)
                                      : JS_EXCEPTION;
    if (JS_IsException(constructor)) {
        JS_FreeValue(ctx, prototype);
        return -1;
    }
    JS_SetConstructorBit(ctx, constructor, true);
    bool linked = define_constants(ctx, constructor) >= 0 &&
        JS_DefinePropertyValueStr(ctx, constructor, "prototype", JS_DupValue(ctx, prototype), 0) >= 0 &&
        JS_DefinePropertyValueStr(ctx, prototype, "constructor", JS_DupValue(ctx, constructor),
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) >= 0;
    /* The global's definition takes the constructor's reference either way. */
    if (!linked) JS_FreeValue(ctx, constructor);
    else linked = JS_DefinePropertyValueStr(ctx, global, "Event", constructor,
                                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) >= 0;
    if (!linked) {
        JS_FreeValue(ctx, prototype);
        return -1;
    }
    JS_SetClassProto(ctx, dom->event_class, prototype);
    /* The intrinsic Function constructor, taken before any page script can
     * replace the global, is what checks a handler body on its own. */
    JSValue function = JS_GetPropertyStr(ctx, global, "Function");
    dom->function_ctor = d_retain(dom, ctx, function);
    if (dom->function_ctor == NULL) return -1;
    dom->window = d_retain(dom, ctx, JS_DupValue(ctx, global));
    if (dom->window == NULL) return -1;
    JSValue invoker = JS_NewCFunctionData(ctx, invoke, 2, 0, 1, &dom->anchor->value);
    dom->invoke = d_retain(dom, ctx, invoker);
    if (dom->invoke == NULL) return -1;
    static const struct { const char *name; int magic, argc; } targets[] = {
        {"addEventListener", M_ADD, 2}, {"removeEventListener", M_REMOVE, 2}, {"dispatchEvent", M_DISPATCH, 1}
    };
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++)
        if (d_method(dom, ctx, dom->prototypes[D_PROTO_NODE]->value, targets[i].name, target_method,
                     targets[i].argc, targets[i].magic) < 0 ||
            d_method(dom, ctx, global, targets[i].name, target_method, targets[i].argc, targets[i].magic) < 0)
            return -1;
    JSValueConst holders[] = {dom->prototypes[D_PROTO_ELEMENT]->value, dom->prototypes[D_PROTO_DOCUMENT]->value, global};
    for (int kind = 0; kind < D_EVENT_COUNT; kind++) {
        if (!has_handler_name(kind)) continue;
        char name[32] = "on";
        os64_strcopy(name + 2, sizeof(name) - 2, kinds[kind]);
        for (size_t i = 0; i < sizeof(holders) / sizeof(holders[0]); i++)
            if (d_accessor(dom, ctx, holders[i], name, handler_property, kind, true) < 0) return -1;
    }
    return d_method(dom, ctx, dom->prototypes[D_PROTO_DOCUMENT]->value, "createEvent", event_construct, 1, 1);
}

/* Drain: every listener's callback goes with the registry. Records stay, as
 * native storage, until os64_dom_free. */
void d_event_drain(os64_dom_t *dom)
{
    for (DValue *entry = dom->values; entry != NULL; entry = entry->next)
        for (DListener *listener = entry->listeners; listener != NULL; listener = listener->next) {
            if (!JS_IsUndefined(listener->callback)) JS_FreeValueRT(dom->engine, listener->callback);
            listener->callback = JS_UNDEFINED;
        }
    dom->listeners_held = 0;
    os64_memset(dom->listen_counts, 0, sizeof(dom->listen_counts));
}

void d_event_free(os64_dom_t *dom, DValue *entry)
{
    for (DListener *listener = entry->listeners; listener != NULL;) {
        DListener *next = listener->next;
        d_free(dom, listener->type);
        d_free(dom, listener->seen);
        d_free(dom, listener);
        listener = next;
    }
    entry->listeners = entry->last_listener = NULL;
}
