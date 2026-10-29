#include "internal.h"
#include "os64/fmt.h"

/* Events: the DOM Standard's EventTarget and Event, HTML's event handlers
 * (the on<type> properties and content attributes), and the dispatch a host
 * starts for a person's input; and the Event interfaces whose state is
 * Event's own record with a field or two more (ProgressEvent, CustomEvent)
 * and createEvent's table. LIBDOM.md § Events is the design record. */

static JSClassID event_slot;

static const char *const kinds[D_EVENT_COUNT] = {
    "click", "mousedown", "mouseup", "mouseover", "mouseout", "mousemove",
    "keydown", "keypress", "keyup", "input", "change", "submit", "reset",
    "focus", "blur", "load", "scroll", "resize", "popstate", "hashchange", "online", "offline",
    "languagechange", "error", "unhandledrejection", "rejectionhandled", "readystatechange",
    "visibilitychange", "prerenderingchange", "DOMContentLoaded", "message", "messageerror", "invalid"
};
/* A page's handler attributes are a bit per kind in a uint64_t
 * (os64_dom_handler_attributes, attribute_mask). */
_Static_assert(D_EVENT_COUNT <= 64, "the handler-attribute mask holds a bit per event kind");

enum { PHASE_NONE, PHASE_CAPTURING, PHASE_AT_TARGET, PHASE_BUBBLING };

/* The most values an Event interface keeps in its record (a MessageEvent's
 * five). */
#define D_EVENT_DETAILS 5

/* An Event's state lives in engine memory and its references are traced by
 * the collector, because a script may keep an event long after its dispatch:
 * nothing native is owned, so its finalizer frees only engine storage. */
typedef struct {
    JSValue target, current;
    char *type;
    double time;
    uint8_t phase;
    bool bubbles, cancelable, canceled, stop, stop_now, dispatching, initialized, trusted;
    /* A ProgressEvent's (XHR § 4): its three attributes. */
    bool progress, length_computable;
    double loaded, total;
    /* detail[] holds one interface's values, as each says: an ErrorEvent's,
     * a PromiseRejectionEvent's, a MessageEvent's, or a UI event's (below).
     * An ErrorEvent's (HTML § 8.1.4.6): its error, message and filename as
     * detail[0], [1] and [2] (the two strings, strings), and its lineno and
     * colno; a PromiseRejectionEvent's: its promise and reason as detail[0]
     * and [1]; a MessageEvent's (HTML § 9.3.2): its data, origin,
     * lastEventId, source and ports as detail[0] to [4] (origin and
     * lastEventId strings, ports a frozen array). Traced with the event. */
    bool error_event, rejection_event, message_event;
    uint32_t lineno, colno;
    JSValue detail[D_EVENT_DETAILS];
    /* A CustomEvent's (DOM § 2.4): its detail, null until set. Traced with
     * the event. */
    bool custom;
    JSValue custom_detail;
    /* A UI event's (UI Events; HTML § 4.10.21.3's SubmitEvent): which
     * interface it is (D_UI_*), and its attributes. Its view, its
     * relatedTarget (a SubmitEvent's submitter) and two strings (a
     * KeyboardEvent's key and code, an InputEvent's data, null or a string,
     * and inputType, a CompositionEvent's data) are detail[0] to [3]
     * (U_VIEW on), traced with the event; the rest are here. `which` is
     * UIEventInit's; `modifiers` a bit each in EventModifierInit's member
     * order (kModifiers). */
    uint8_t ui;
    int32_t ui_detail;
    uint32_t which, location, char_code, key_code;
    uint16_t modifiers, buttons;
    int16_t button;
    bool repeat, composing;
    double screen_x, screen_y, client_x, client_y, page_x, page_y;
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

/* Whether an element's on<type> content attribute is a handler for the
 * type: not for the document's own types (internal.h), which HTML gives an
 * IDL property on Document and no content attribute. */
static bool content_attribute(int kind)
{
    return has_handler_name(kind) && kind != D_EVENT_READYSTATECHANGE && kind != D_EVENT_VISIBILITYCHANGE &&
        kind != D_EVENT_PRERENDERINGCHANGE;
}

/* Whether on<type> is a handler on this page: the arc's types (SCROLL on)
 * only with the Modern Web switch on, so a content attribute for one is
 * an ordinary attribute with it off, as it was before the arc. */
static bool handler_on(const os64_dom_t *dom, int kind)
{
    return has_handler_name(kind) && (kind < D_EVENT_SCROLL || dom->options.modern);
}

/* HTML's "window-reflecting body element event handler set" and the body's
 * WindowEventHandlers, as far as the types named here reach: the body's
 * onload, onfocus and onblur are window's, and so are its onscroll,
 * onresize, onpopstate, onhashchange, ononline, onoffline,
 * onlanguagechange, onerror, onunhandledrejection, onrejectionhandled,
 * onmessage and onmessageerror (handlers only with the switch on,
 * handler_on). */
static bool window_reflecting(int kind)
{
    return kind == D_EVENT_LOAD || kind == D_EVENT_FOCUS || kind == D_EVENT_BLUR ||
        kind == D_EVENT_SCROLL || kind == D_EVENT_RESIZE || kind == D_EVENT_POPSTATE ||
        kind == D_EVENT_HASHCHANGE || kind == D_EVENT_ONLINE || kind == D_EVENT_OFFLINE ||
        kind == D_EVENT_LANGUAGECHANGE || kind == D_EVENT_ERROR || kind == D_EVENT_UNHANDLEDREJECTION ||
        kind == D_EVENT_REJECTIONHANDLED || kind == D_EVENT_MESSAGE || kind == D_EVENT_MESSAGEERROR;
}

static void event_finalizer(JSRuntime *rt, JSValue value)
{
    DEvent *event = JS_GetOpaque(value, event_slot);
    if (event == NULL) return;
    JS_FreeValueRT(rt, event->target);
    JS_FreeValueRT(rt, event->current);
    for (int i = 0; i < D_EVENT_DETAILS; i++) JS_FreeValueRT(rt, event->detail[i]);
    JS_FreeValueRT(rt, event->custom_detail);
    js_free_rt(rt, event->type);
    js_free_rt(rt, event);
}

static void event_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DEvent *event = JS_GetOpaque(value, event_slot);
    if (event == NULL) return;
    JS_MarkValue(rt, event->target, mark);
    JS_MarkValue(rt, event->current, mark);
    for (int i = 0; i < D_EVENT_DETAILS; i++) JS_MarkValue(rt, event->detail[i], mark);
    JS_MarkValue(rt, event->custom_detail, mark);
}

static uint64_t now_ms(const os64_dom_t *dom)
{
    return dom->options.now_ms != NULL ? dom->options.now_ms(dom->options.host_opaque) : 0;
}

JSValue d_event_new(os64_dom_t *dom, JSContext *ctx, const char *type, bool bubbles,
                    bool cancelable, bool trusted)
{
    return d_event_new_proto(dom, ctx, JS_UNDEFINED, type, bubbles, cancelable, trusted);
}

JSValue d_event_new_proto(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype, const char *type,
                          bool bubbles, bool cancelable, bool trusted)
{
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, dom->event_class)
                                            : JS_NewObjectClass(ctx, dom->event_class);
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
                      .cancelable = cancelable, .initialized = true, .trusted = trusted,
                      .detail = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED},
                      .custom_detail = JS_NULL};
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

static void first_report(os64_dom_t *dom, JSContext *ctx, JSValueConst error)
{
    os64_js_outcome_t *out = &dom->report;
    out->status = OS64_JS_EXCEPTION;
    os64_strcopy(out->message, sizeof(out->message), "JavaScript exception; diagnostic unavailable");
    os64_strcopy(out->source_name, sizeof(out->source_name), "event handler");
    /* Read as an event's message is (report.c's d_error_text), so writing
     * it down runs none of the page's code: no getter, no toString. */
    char text[OS64_JS_MESSAGE_CAP + 1];
    d_error_text(dom, ctx, error, text, sizeof(text));
    out->diagnostic_truncated = os64_strcopy(out->message, sizeof(out->message), text) >= sizeof(out->message);
    out->diagnostic_truncated |= d_error_stack(ctx, error, out->stack_trace, sizeof(out->stack_trace));
}

/* An exception the binding caught itself: the first is kept for the task's
 * outcome (os64_dom_take_report), and each is the window's error event
 * (d_report_error), the page's (d_report) or `script`'s (d_report_as). */
void d_report(os64_dom_t *dom, JSContext *ctx, JSValueConst error)
{
    d_report_as(dom, ctx, error, D_SCRIPT_PAGE);
}

void d_report_as(os64_dom_t *dom, JSContext *ctx, JSValueConst error, uint32_t script)
{
    if (dom->report.status == OS64_JS_OK) first_report(dom, ctx, error);
    d_report_error(dom, ctx, error, script);
}

/* LISTENER RECORDS. A callback held by a record is one of the registry's
 * values: listeners_held counts them and drain releases them. A collectable
 * plain target's are its own (DListener.engine): traced and freed with it,
 * and not counted, as the host never dispatches to such a target. */

static void listeners_changed(os64_dom_t *dom, DValue *entry);

static void count(os64_dom_t *dom, const DListener *listener, int delta)
{
    if (listener->kind >= 0 && !listener->engine) dom->listen_counts[listener->kind] += (uint32_t)delta;
}

static void set_callback(os64_dom_t *dom, DListener *listener, JSValue value)
{
    if (!JS_IsUndefined(listener->callback)) {
        JS_FreeValueRT(dom->engine, listener->callback);
        if (!listener->engine) dom->listeners_held--;
    }
    listener->callback = value;
    if (!JS_IsUndefined(value) && !listener->engine) dom->listeners_held++;
}

static void slot_state(os64_dom_t *dom, DListener *slot, uint8_t state)
{
    if (slot->handler == D_SLOT_FUNCTION && state != D_SLOT_FUNCTION) count(dom, slot, -1);
    if (slot->handler != D_SLOT_FUNCTION && state == D_SLOT_FUNCTION) count(dom, slot, 1);
    slot->handler = state;
    if (state != D_SLOT_FUNCTION) {
        set_callback(dom, slot, JS_UNDEFINED);
        slot->script = D_SCRIPT_PAGE;
    }
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
    listener->engine = entry->engine_held;
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
    if (!handler_on(dom, kind) || !content_attribute(kind)) return NULL;
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
        if (!content_attribute(kind)) continue;
        const char *a = name + 2, *b = kinds[kind];
        while (*a != '\0' && *b != '\0' && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
        if (*a == '\0' && *b == '\0') return kind;
    }
    return -1;
}

/* HTML's "determining the target of an event handler": a body's (or a
 * frameset's) window-reflecting handler is window's when the body is the
 * window's document's, and nobody's when its document has no window
 * (implementation.c). 1 for window's, 0 for the element's own, -1 for
 * nobody's. */
static int reflected(os64_dom_t *dom, const os64_html_node_t *node, int kind)
{
    if (!body_like(node) || !window_reflecting(kind)) return 0;
    return d_window_document(dom, node) ? 1 : -1;
}

int d_handler_attribute_set(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                            const char *name)
{
    int kind = handler_kind(name);
    if (kind < 0 || !handler_on(dom, kind) || node->kind != OS64_HTML_ELEMENT) return 0;
    int target = reflected(dom, node, kind);
    if (target < 0) return 0;
    DValue *entry = target > 0 ? dom->window : d_find(dom, node);
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
 * function out of the wrapper is a compile failure. Window's onerror (the
 * body's, reflected) is a function of five: event, source, lineno, colno
 * and error, which is how an error event's handler is called (run_record). */
static int slot_compile(os64_dom_t *dom, JSContext *ctx, DValue *entry, DListener *slot)
{
    const os64_html_node_t *element = attribute_source(dom, entry, slot->kind);
    const char *params = entry == dom->window && slot->kind == D_EVENT_ERROR ? "event,source,lineno,colno,error"
                                                                             : "event";
    size_t params_length = os64_strlen(params);
    JSValue parts[2] = {JS_NewString(ctx, params), JS_NewString(ctx, slot->seen)};
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
    size_t length = sizeof(head) - 1 + name_length + params_length + 4 + body_length + 4;
    char *source = d_alloc(dom, length + 1);
    if (source == NULL) {
        d_error(ctx, "QuotaExceededError", "DOM handler quota exceeded");
        return -1;
    }
    size_t at = 0;
    os64_memcpy(source + at, head, sizeof(head) - 1); at += sizeof(head) - 1;
    os64_memcpy(source + at, slot->type, name_length); at += name_length;
    source[at++] = '(';
    os64_memcpy(source + at, params, params_length); at += params_length;
    os64_memcpy(source + at, "){\n", 3); at += 3;
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
    const os64_html_node_t *form = element != NULL ? d_form_owner(element) : NULL;
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
    slot->script = D_SCRIPT_PAGE;
    return 0;
}

/* The slot's function, compiling it if its attribute has not been: a new
 * reference, JS_NULL when there is none, JS_EXCEPTION when compiling threw.
 * An attribute on an element of a document with no window is not compiled
 * (scripting is disabled for it) and reads as none, until the element is
 * the window's. */
static JSValue slot_value(os64_dom_t *dom, JSContext *ctx, DValue *entry, DListener *slot)
{
    if (slot_refresh(dom, ctx, entry, slot) < 0) return JS_EXCEPTION;
    if (slot->handler == D_SLOT_UNCOMPILED && entry != dom->window && entry->node != NULL &&
        !d_window_document(dom, entry->node))
        return JS_NULL;
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

/* HTML's special error event handling: window's onerror, given an ErrorEvent
 * named error, is called with the event's message, filename, lineno, colno
 * and error rather than with the event. */
static JSValue call_error_handler(JSContext *ctx, JSValueConst callback, JSValueConst self, const DEvent *state)
{
    JSValueConst args[5] = {
        state->detail[1], state->detail[2], JS_NewInt64(ctx, state->lineno), JS_NewInt64(ctx, state->colno),
        state->detail[0]
    };
    return JS_Call(ctx, callback, self, 5, args);
}

/* Run one record against an event. The callback is taken before a `once`
 * listener is removed, so the record's own reference may go first. HTML's
 * event handler processing: a handler that returns false cancels, except
 * window's onerror called for an error event, which cancels by returning
 * true. */
static JSValue run_record(os64_dom_t *dom, JSContext *ctx, DValue *entry, DListener *listener,
                          JSValueConst callback, JSValueConst event)
{
    JSValue held = JS_UNDEFINED;
    if (JS_IsUndefined(callback)) {
        held = slot_value(dom, ctx, entry, listener);
        if (JS_IsException(held) || JS_IsNull(held)) return held;
        callback = held;
    }
    DEvent *state = JS_GetOpaque(event, dom->event_class);
    bool special = listener->handler != D_LISTENER && entry == dom->window && state != NULL &&
        state->error_event && os64_strcmp(state->type, "error") == 0 && JS_IsFunction(ctx, callback);
    JSValue result = special ? call_error_handler(ctx, callback, entry->value, state)
                             : call_callback(ctx, callback, entry->value, event);
    if (listener->handler != D_LISTENER && state != NULL && JS_VALUE_GET_TAG(result) == JS_TAG_BOOL &&
        JS_VALUE_GET_BOOL(result) == special)
        cancel(state);
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

static bool sticky(os64_js_status_t status)
{
    return status == OS64_JS_LIMIT || status == OS64_JS_CANCELLED ||
        status == OS64_JS_HOST_FAILURE || status == OS64_JS_FAILED_RUNTIME;
}

void d_outcome_fold(os64_js_outcome_t *into, const os64_js_outcome_t *from)
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
    /* A plain target (d_event_target): the path is the target alone. */
    DValue *plain;
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
            if (listener->once) {
                remove_listener(dom, entry, listener);
                listeners_changed(dom, entry);
            }
            /* What the listener throws is its script's (report.c's MUTED
             * ERRORS), whichever call it is reported from. */
            DScripts before = d_script_enter(dom, listener->script);
            if (d->host != NULL) {
                JSValue args[2] = {d->object, callback};
                os64_js_outcome_t call, point;
                dom->invoking = listener;
                dom->invoking_entry = entry;
                os64_js_call(dom->runtime, OS64_JS_ABI_ID, dom->invoke->value, entry->value, 2, args,
                             NULL, &call);
                dom->invoking = NULL;
                dom->invoking_entry = NULL;
                d_script_leave(dom, &before, true);
                JS_FreeValueRT(dom->engine, callback);
                d_outcome_fold(d->host, &call);
                /* A listener that threw is reported before the checkpoint
                 * that cleans up after it, as HTML's callback steps do. */
                if (call.status == OS64_JS_EXCEPTION) d_errors_fire(dom, d->host);
                if (!sticky(call.status)) {
                    os64_js_checkpoint(dom->runtime, OS64_JS_ABI_ID, &point);
                    d_outcome_fold(d->host, &point);
                    if (sticky(point.status)) { result = -1; break; }
                } else { result = -1; break; }
            } else {
                uint32_t script = listener->script;
                JSValue value = run_record(dom, d->ctx, entry, listener, callback, d->object);
                d_script_leave(dom, &before, false);
                JS_FreeValue(d->ctx, callback);
                if (JS_IsException(value)) {
                    /* A runtime that must stop keeps its exception as the
                     * engine made it, uncatchable. */
                    if (!d_exception_catchable(d->ctx)) {
                        result = -1;
                        break;
                    }
                    JSValue error = JS_GetException(d->ctx);
                    d_report_as(dom, d->ctx, error, script);
                    JS_FreeValue(d->ctx, error);
                } else JS_FreeValue(d->ctx, value);
            }
            if (event->stop_now) break;
        }
        if (final) break;
    }
    return result;
}

static bool element_has_handler(const os64_dom_t *dom, const os64_html_node_t *node, int kind)
{
    return handler_on(dom, kind) && content_attribute(kind) && node->kind == OS64_HTML_ELEMENT &&
        !(body_like(node) && window_reflecting(kind)) && handler_attribute(node, kind) != NULL;
}

/* DOM § 2.9's activation target: for a click a script dispatches as a
 * MouseEvent at a node, the node when it has activation behaviour, else,
 * for a click that bubbles, its nearest ancestor that has (window.c's
 * d_activation_has); NULL for every other dispatch. A host's click is the
 * host's own to act on, after its dispatch, and a plain Event named click
 * activates nothing. */
static const os64_html_node_t *activation_target(const Dispatch *d, const os64_html_node_t *node)
{
    if (d->host != NULL || node == NULL || d->event->ui != D_UI_MOUSE || os64_strcmp(d->event->type, "click") != 0)
        return NULL;
    if (d_activation_has(node)) return node;
    for (const os64_html_node_t *at = node->parent; d->event->bubbles && at != NULL; at = at->parent)
        if (d_activation_has(at)) return at;
    return NULL;
}

/* The DOM Standard's dispatch, for one event at one target: the path is the
 * target's ancestors and, past the document, window (except for load);
 * capture down, the target, then bubble up when the event bubbles. A plain
 * target is a path of one. A click's activation (activation_target) runs
 * its first half before the listeners and its second once the dispatch is
 * over, the event's phase, current target and flags already reset, as
 * DOM's steps order them. */
static int dispatch(Dispatch *d, const os64_html_node_t *node)
{
    os64_dom_t *dom = d->dom;
    JSContext *ctx = d->ctx;
    DEvent *event = d->event;
    int kind = d_event_kind(event->type);
    size_t length = d->plain != NULL ? 1 : 0;
    const os64_html_node_t *top = node;
    if (d->plain != NULL) node = NULL;
    for (const os64_html_node_t *at = node; at != NULL; at = at->parent) { length++; top = at; }
    bool to_window = d->plain == NULL && (node == NULL || (top == document_node(dom) && kind != D_EVENT_LOAD));
    if (to_window) length++;
    DValue **path = d_alloc(dom, length * sizeof(*path));
    if (path == NULL) {
        d_error(ctx, "QuotaExceededError", "DOM event path quota exceeded");
        return -1;
    }
    JSValue target;
    if (d->plain != NULL) target = JS_DupValue(ctx, d->plain->value);
    else if (node == NULL)
        target = d->document_target ? d_wrap(dom, ctx, document_node(dom)) : JS_DupValue(ctx, dom->window->value);
    else target = d_wrap(dom, ctx, node);
    if (JS_IsException(target)) { d_free(dom, path); return -1; }
    size_t count = 0;
    if (d->plain != NULL) path[count++] = d->plain;
    for (const os64_html_node_t *at = node; at != NULL; at = at->parent) {
        DValue *entry = d_find(dom, at);
        if (entry == NULL && element_has_handler(dom, at, kind)) {
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
    DActiveEvent active = {dom->current_event, JS_DupValue(ctx,d->object)};
    dom->current_event = &active;
    event->dispatching = true;
    dom->dispatching++;
    const os64_html_node_t *activation = activation_target(d, node);
    DActivation saved = {false, false, false, false};
    int result = activation != NULL && d_activation_before(dom, ctx, activation, &saved) < 0 ? -1 : 0;
    for (size_t i = count; result == 0 && i > 1; i--)
        result = invoke_target(d, path[i - 1], PHASE_CAPTURING, true);
    if (result == 0) result = invoke_target(d, path[0], PHASE_AT_TARGET, true);
    if (result == 0) result = invoke_target(d, path[0], PHASE_AT_TARGET, false);
    for (size_t i = 1; result == 0 && event->bubbles && i < count; i++)
        result = invoke_target(d, path[i], PHASE_BUBBLING, false);
    dom->current_event = active.previous;
    JS_FreeValue(ctx,active.value);
    dom->dispatching--;
    event->dispatching = false;
    event->phase = PHASE_NONE;
    event->stop = event->stop_now = false;
    JS_FreeValue(ctx, event->current);
    event->current = JS_NULL;
    d_free(dom, path);
    sweep(dom);
    /* sweep walks the registry; a collectable target is not on it. */
    if (d->plain != NULL && d->plain->engine_held && dom->dispatching == 0) unlink_removed(dom, d->plain);
    if (activation != NULL && result == 0 &&
        d_activation_after(dom, ctx, activation, event->canceled, &saved) < 0)
        result = -1;
    return result < 0 ? -1 : event->canceled ? 0 : 1;
}

int d_dispatch(os64_dom_t *dom, JSContext *ctx, JSValueConst object, const os64_html_node_t *node)
{
    Dispatch d = {dom, ctx, (JSValue)object, JS_GetOpaque(object, dom->event_class), NULL, false, NULL};
    if (d.event == NULL) { JS_ThrowTypeError(ctx, "Expected an Event"); return -1; }
    return dispatch(&d, node);
}

int d_dispatch_plain(os64_dom_t *dom, JSContext *ctx, JSValueConst object, DValue *target)
{
    Dispatch d = {dom, ctx, (JSValue)object, JS_GetOpaque(object, dom->event_class), NULL, false, target};
    if (d.event == NULL) { JS_ThrowTypeError(ctx, "Expected an Event"); return -1; }
    return dispatch(&d, NULL);
}

int d_dispatch_plain_task(os64_dom_t *dom, JSContext *ctx, JSValueConst object, DValue *target,
                          os64_js_outcome_t *outcome)
{
    Dispatch d = {dom, ctx, (JSValue)object, JS_GetOpaque(object, dom->event_class), outcome, false, target};
    if (d.event == NULL) { JS_ThrowTypeError(ctx, "Expected an Event"); return -1; }
    return dispatch(&d, NULL);
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

/* The fields a mouse or key event carries, as plain data, with the Modern
 * Web switch off (with it on they are a MouseEvent's and a KeyboardEvent's,
 * ui_host_event). The old web reads keyCode and which; key is the modern
 * spelling of the same key. */
static bool event_fields(os64_dom_t *dom, JSContext *ctx, JSValueConst object, const os64_dom_event_t *ev)
{
    if (ev->kind != OS64_DOM_EVENT_MOUSE && ev->kind != OS64_DOM_EVENT_KEY) return true;
    bool ok = define_bool(ctx, object, "shiftKey", ev->shift) && define_bool(ctx, object, "ctrlKey", ev->ctrl) &&
        define_bool(ctx, object, "altKey", ev->alt) && define_bool(ctx, object, "metaKey", ev->meta);
    if (ok && ev->kind == OS64_DOM_EVENT_MOUSE) {
        ok = define_number(ctx, object, "clientX", ev->client_x) && define_number(ctx, object, "clientY", ev->client_y) &&
            define_number(ctx, object, "pageX", ev->page_coordinates ? ev->page_x : ev->client_x) &&
            define_number(ctx, object, "pageY", ev->page_coordinates ? ev->page_y : ev->client_y) &&
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

/* What the task left owed is settled inside it (d_task_settle), so its
 * events run under its deadline; as it closes, the binding's own report
 * and the rejections it only counted are told (d_task_close). */
os64_js_status_t d_task_finish(os64_dom_t *dom, os64_js_outcome_t *outcome)
{
    d_task_settle(dom, outcome);
    os64_js_outcome_t end;
    os64_js_task_end(dom->runtime, OS64_JS_ABI_ID, &end);
    d_outcome_fold(outcome, &end);
    d_task_close(dom, end.source_name, outcome);
    return outcome->status;
}

os64_js_status_t d_host_dispatch(os64_dom_t *dom, const os64_html_node_t *node, DValue *plain,
                                 const char *type, JSValue (*make)(os64_dom_t *, JSContext *, void *),
                                 void *arg, bool *prevented, os64_js_outcome_t *outcome)
{
    char name[OS64_JS_SOURCE_NAME_CAP];
    os64_snprintf(name, sizeof(name), "%s event", type);
    os64_js_outcome_t begin;
    if (os64_js_task_begin(dom->runtime, OS64_JS_ABI_ID, name, &begin) != OS64_JS_OK) {
        *outcome = begin;
        return outcome->status;
    }
    os64_js_outcome_t access;
    JSContext *ctx = os64_js_context(dom->runtime, OS64_JS_ABI_ID, &access);
    if (ctx == NULL) return d_task_finish(dom, outcome);
    JSValue object = make(dom, ctx, arg);
    if (JS_IsException(object)) { discard(ctx); return d_task_finish(dom, outcome); }
    Dispatch d = {dom, ctx, object, JS_GetOpaque(object, dom->event_class), outcome,
                  plain == NULL && node == NULL && d_event_kind(type) == D_EVENT_LOAD, plain};
    /* Engine refusals between the wrapper's calls are latched by libjs and
     * reported by task_end; the binding's own refusal is reported here. */
    if (dispatch(&d, node) < 0 && JS_HasException(ctx)) {
        /* A runtime that must stop is the task's failure, which task_end
         * reports; nothing is told to the page. */
        JSValue error = JS_GetException(ctx);
        if (d_exception_catchable(ctx)) d_report(dom, ctx, error);
        JS_FreeValue(ctx, error);
    }
    if (prevented != NULL) *prevented = d.event->canceled;
    JS_FreeValue(ctx, object);
    return d_task_finish(dom, outcome);
}

static int ui_kind_of(os64_dom_event_kind_t kind);
static JSValue ui_host_event(os64_dom_t *dom, JSContext *ctx, const os64_dom_event_t *ev, int kind);

/* A host event as os64_dom_event_t describes it: the UI event its kind
 * names, with the switch on; else an Event with its mouse or key fields. */
static JSValue host_event(os64_dom_t *dom, JSContext *ctx, void *arg)
{
    const os64_dom_event_t *ev = arg;
    int kind = ui_kind_of(ev->kind);
    if (kind != D_UI_NONE && dom->ui_prototypes[kind] != NULL) return ui_host_event(dom, ctx, ev, kind);
    JSValue object = d_event_new(dom, ctx, ev->type, ev->bubbles, ev->cancelable, true);
    if (!JS_IsException(object) && !event_fields(dom, ctx, object, ev)) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
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
    /* The readiness this dispatch makes comes first, and readystatechange
     * just before the event itself (document.c). */
    os64_js_outcome_t changed;
    os64_memset(&changed, 0, sizeof(changed));
    if (d_readiness_moved(dom, node, ev->type, &changed) && sticky(changed.status)) {
        *outcome = changed;
        return outcome->status;
    }
    /* A person's submit at a form: the form is being submitted (window.c's
     * submit's flag), so a submission its listener asks of it does nothing,
     * rather than send it a second time beside the person's. */
    bool submitting = node != NULL && node->kind == OS64_HTML_ELEMENT && node->ns == OS64_HTML_NS_HTML &&
        node->tag == OS64_HTML_TAG_FORM && os64_strcmp(ev->type, "submit") == 0;
    struct DClicking self = {node, dom->submitting};
    if (submitting) dom->submitting = &self;
    if (os64_dom_listens(dom, ev->type))
        d_host_dispatch(dom, node, NULL, ev->type, host_event, (void *)ev, prevented, outcome);
    if (submitting) dom->submitting = self.next;
    if (changed.status != OS64_JS_OK) {
        d_outcome_fold(&changed, outcome);
        *outcome = changed;
    }
    return outcome->status;
}

/* Attributes are counted once per tree version: a walk of the connected tree
 * for on<type> names. Listener and property counts are kept as they change. */
uint64_t os64_dom_handler_attributes(const os64_html_document_t *doc)
{
    uint64_t mask = 0;
    const os64_html_node_t *root = doc != NULL ? doc->document : NULL;
    for (const os64_html_node_t *at = root; at != NULL;) {
        if (at->kind == OS64_HTML_ELEMENT)
            for (const os64_html_attr_t *attr = at->attrs; attr != NULL; attr = attr->next) {
                int kind = handler_kind(attr->name);
                if (kind >= 0) mask |= UINT64_C(1) << kind;
            }
        if (at->first_child != NULL) { at = at->first_child; continue; }
        while (at != NULL && at != root && at->next == NULL) at = at->parent;
        at = at != NULL && at != root ? at->next : NULL;
    }
    return mask;
}

static uint64_t attribute_mask(os64_dom_t *dom)
{
    uint64_t version = os64_html_version(dom->document);
    if (dom->attribute_scanned && dom->attribute_version == version) return dom->attribute_mask;
    uint64_t mask = os64_dom_handler_attributes(dom->document);
    // With the switch off the arc's types' attributes are no handlers.
    if (!dom->options.modern)
        for (int kind = D_EVENT_SCROLL; kind < D_EVENT_COUNT; kind++) mask &= ~(UINT64_C(1) << kind);
    dom->attribute_mask = mask;
    dom->attribute_version = version;
    dom->attribute_scanned = true;
    return mask;
}

uint64_t os64_dom_handler_bit(const char *type)
{
    int kind = type != NULL ? d_event_kind(type) : -1;
    return content_attribute(kind) ? UINT64_C(1) << kind : 0;
}

bool os64_dom_listens(os64_dom_t *dom, const char *type)
{
    if (dom == NULL || dom->closed || type == NULL) return false;
    int kind = d_event_kind(type);
    if (kind < 0) return true;
    return dom->listen_counts[kind] != 0 || (attribute_mask(dom) & (UINT64_C(1) << kind)) != 0;
}

/* SCRIPT SURFACE. */

/* A plain target's entry, for an object of a class d_event_target
 * registered: its opaque, or what the class's resolver makes of it (only
 * when `make`). 1 with the entry, which is NULL for a resolver's target
 * that has none and was not to make one; 0 for any other value; -1 with an
 * exception pending. */
static int plain_entry(os64_dom_t *dom, JSContext *ctx, JSValueConst self, bool make, DValue **entry)
{
    if (!JS_IsObject(self)) return 0;
    JSClassID id = JS_GetClassID(self);
    for (unsigned i = 0; i < dom->target_class_count; i++) {
        if (dom->target_classes[i] != id) continue;
        if (dom->target_resolvers[i] != NULL) return dom->target_resolvers[i](dom, ctx, self, make, entry);
        *entry = JS_GetOpaque(self, id);
        return *entry != NULL ? 1 : 0;
    }
    return 0;
}

/* The listener record of the target `self`: window's, a node's, or a plain
 * target's. A plain target whose entry is made on demand gets one only
 * when `make` (something is added to it, or it is dispatched at); else
 * *out is NULL when it has none, and there is nothing to read or remove.
 * 0, or -1 with an exception pending. */
static int target_entry(os64_dom_t *dom, JSContext *ctx, JSValueConst self, bool make, DValue **out)
{
    *out = NULL;
    if (JS_IsUndefined(self) || JS_IsNull(self) ||
        (JS_IsObject(self) && JS_VALUE_GET_PTR(self) == JS_VALUE_GET_PTR(dom->window->value))) {
        *out = dom->window;
        return 0;
    }
    int found = plain_entry(dom, ctx, self, make, out);
    if (found != 0) return found > 0 ? 0 : -1;
    const os64_html_node_t *node = d_node(dom, ctx, self);
    if (node == NULL) return -1;
    *out = d_find(dom, node);
    if (*out != NULL) return 0;
    JS_ThrowTypeError(ctx, "Missing node wrapper");
    return -1;
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

/* A plain target's listener-change hook (DValue.listeners_changed), after
 * a listener was added or removed or a handler set. */
static void listeners_changed(os64_dom_t *dom, DValue *entry)
{
    if (entry != NULL && entry->listeners_changed != NULL) entry->listeners_changed(dom->engine, entry);
}

static JSValue target_method_on(os64_dom_t *dom, JSContext *ctx, JSValueConst self, int argc,
                                JSValueConst *argv, int magic)
{
    DValue *entry;
    if (target_entry(dom, ctx, self, magic != M_REMOVE, &entry) < 0) return JS_EXCEPTION;
    if (magic == M_DISPATCH) {
        if (argc < 1) return JS_ThrowTypeError(ctx, "dispatchEvent needs an event");
        DEvent *event = event_of(dom, ctx, argv[0]);
        if (event == NULL) return JS_EXCEPTION;
        if (event->dispatching || !event->initialized)
            return d_error(ctx, "InvalidStateError", "The event is already being dispatched or is not initialized");
        // A redispatched event stays cancelled: initEvent is the reset, and
        // a vetoed default action must not come back on the second try.
        event->trusted = false;
        int result = entry != dom->window && entry->node == NULL
            ? d_dispatch_plain(dom, ctx, argv[0], entry)
            : d_dispatch(dom, ctx, argv[0], entry == dom->window ? NULL : entry->node);
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
    for (DListener *listener = entry != NULL ? entry->listeners : NULL; listener != NULL; listener = listener->next)
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
        uint32_t script = d_callback_script(dom, ctx, argv[1]);
        DListener *listener = script == D_SCRIPT_FAILED ? NULL : listener_new(dom, ctx, entry, type.data, false);
        if (listener == NULL) result = JS_EXCEPTION;
        else {
            listener->capture = capture;
            listener->once = once;
            set_callback(dom, listener, JS_DupValue(ctx, argv[1]));
            listener->script = script;
            count(dom, listener, 1);
        }
    }
    d_string_free(dom, &type);
    listeners_changed(dom, entry);
    return result;
}

static JSValue target_method(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                             int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    return target_method_on(dom, ctx, self, argc, argv, magic);
}

/* on<type> for an element, the document, window or a plain target (a
 * MediaQueryList's or screen.orientation's onchange). The body's
 * window-reflecting handlers are window's, whichever object is asked; a
 * body of a document with no window has none, reads null and ignores an
 * assignment. */
static JSValue handler_property(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                                int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    int kind = magic & ~D_SET;
    // A plain target made on demand gets its entry for a handler set, and
    // reads null, or is set to nothing, without one.
    DValue *entry;
    if (target_entry(dom, ctx, self, set && argc > 0 && JS_IsObject(argv[0]), &entry) < 0) return JS_EXCEPTION;
    if (entry == NULL) return set ? JS_UNDEFINED : JS_NULL;
    int target = entry != dom->window ? reflected(dom, entry->node, kind) : 0;
    if (target < 0) return set ? JS_UNDEFINED : JS_NULL;
    if (target > 0) entry = dom->window;
    if (!set) {
        DListener *slot = slot_find(entry, kinds[kind]);
        if (slot == NULL) {
            if (slot_attribute_pending(dom, ctx, entry, kind) < 0) return JS_EXCEPTION;
            slot = slot_find(entry, kinds[kind]);
            if (slot == NULL) return JS_NULL;
        }
        JSValue value = slot_value(dom, ctx, entry, slot);
        if (JS_IsException(value)) {
            /* HTML reports a handler that fails to compile and answers null;
             * a runtime that must stop keeps its exception uncatchable. */
            if (!d_exception_catchable(ctx)) return JS_EXCEPTION;
            JSValue error = JS_GetException(ctx);
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
        uint32_t script = d_callback_script(dom, ctx, value);
        if (script == D_SCRIPT_FAILED) return JS_EXCEPTION;
        slot_state(dom, slot, D_SLOT_FUNCTION);
        set_callback(dom, slot, JS_DupValue(ctx, value));
        slot->script = script;
    } else slot_state(dom, slot, D_SLOT_EMPTY);
    listeners_changed(dom, entry);
    return JS_UNDEFINED;
}

enum {
    E_TYPE, E_TARGET, E_CURRENT, E_PHASE, E_BUBBLES, E_CANCELABLE, E_PREVENTED,
    E_RETURN_VALUE, E_CANCEL_BUBBLE, E_TIME, E_TRUSTED,
    E_PREVENT, E_STOP, E_STOP_NOW, E_INIT,
    /* CustomEvent's own. */
    E_DETAIL, E_INIT_CUSTOM
};

/* DOM's "initialize" an event, initEvent's and initCustomEvent's steps
 * after the dispatch check: its type (converted first, as WebIDL converts
 * the arguments in order), bubbles and cancelable, the stop-propagation,
 * stop-immediate and canceled flags cleared, isTrusted false (a host
 * event a page keeps and re-initializes is its own from then on), and no
 * target. -1 with an exception pending. Its two halves, the arguments
 * converted (event_init_read: the type, engine memory, the caller's to
 * free or hand to event_init_set) and the event changed, are for an init
 * with arguments of its own to convert between them, so a conversion that
 * throws leaves the event untouched. */
typedef struct { char *type; bool bubbles, cancelable; } DInit;

static int event_init_read(JSContext *ctx, int argc, JSValueConst *argv, const char *who, DInit *out)
{
    out->type = NULL;
    if (argc < 1) {
        JS_ThrowTypeError(ctx, "%s needs a type", who);
        return -1;
    }
    const char *type = JS_ToCString(ctx, argv[0]);
    if (type == NULL) return -1;
    char *copy = js_strdup(ctx, type);
    JS_FreeCString(ctx, type);
    if (copy == NULL) return -1;
    int bubbles = argc > 1 ? JS_ToBool(ctx, argv[1]) : 0;
    int cancelable = argc > 2 ? JS_ToBool(ctx, argv[2]) : 0;
    if (bubbles < 0 || cancelable < 0) { js_free(ctx, copy); return -1; }
    *out = (DInit){copy, bubbles != 0, cancelable != 0};
    return 0;
}

static void event_init_set(JSContext *ctx, DEvent *event, DInit *init)
{
    js_free(ctx, event->type);
    event->type = init->type;
    init->type = NULL;
    event->bubbles = init->bubbles;
    event->cancelable = init->cancelable;
    event->initialized = true;
    event->canceled = event->stop = event->stop_now = false;
    event->trusted = false;
    JS_FreeValue(ctx, event->target);
    event->target = JS_NULL;
}

static int event_init(JSContext *ctx, DEvent *event, int argc, JSValueConst *argv, const char *who)
{
    DInit init;
    if (event_init_read(ctx, argc, argv, who, &init) < 0) return -1;
    event_init_set(ctx, event, &init);
    return 0;
}

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
    case E_INIT:
        if (event->dispatching) return JS_UNDEFINED;
        return event_init(ctx, event, argc, argv, "initEvent") < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    case E_DETAIL: case E_INIT_CUSTOM:
        if (!event->custom) return JS_ThrowTypeError(ctx, "Expected a CustomEvent");
        if (magic == E_DETAIL) return JS_DupValue(ctx, event->custom_detail);
        /* DOM § 2.4: during a dispatch it does nothing, its arguments
         * converted all the same; detail's default is null. */
        if (event->dispatching) {
            if (argc < 1) return JS_ThrowTypeError(ctx, "initCustomEvent needs a type");
            const char *type = JS_ToCString(ctx, argv[0]);
            if (type == NULL) return JS_EXCEPTION;
            JS_FreeCString(ctx, type);
            return JS_UNDEFINED;
        }
        if (event_init(ctx, event, argc, argv, "initCustomEvent") < 0) return JS_EXCEPTION;
        JS_FreeValue(ctx, event->custom_detail);
        event->custom_detail = argc > 3 && !JS_IsUndefined(argv[3]) ? JS_DupValue(ctx, argv[3]) : JS_NULL;
        return JS_UNDEFINED;
    default: return JS_ThrowTypeError(ctx, "Unknown Event member");
    }
}

JSValue d_event_construct(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv,
                          JSValueConst prototype)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "Event needs a type");
    /* The arguments convert in order: the type, then the init dictionary
     * (WebIDL § 3.2.17), whose primitive other than undefined and null is
     * a TypeError here for every Event interface; one with members of its
     * own reads them after. */
    const char *type = JS_ToCString(ctx, argv[0]);
    if (type == NULL) return JS_EXCEPTION;
    bool flags[2] = {false, false};
    int init = d_dictionary(ctx, argc, argv, 1);
    for (int i = 0; init > 0 && i < 2; i++) {
        static const char *const names[] = {"bubbles", "cancelable"};
        JSValue value = JS_GetPropertyStr(ctx, argv[1], names[i]);
        int flag = JS_IsException(value) ? -1 : JS_ToBool(ctx, value);
        JS_FreeValue(ctx, value);
        if (flag < 0) init = -1;
        else flags[i] = flag != 0;
    }
    if (init < 0) {
        JS_FreeCString(ctx, type);
        return JS_EXCEPTION;
    }
    JSValue object = d_event_new_proto(dom, ctx, prototype, type, flags[0], flags[1], false);
    JS_FreeCString(ctx, type);
    return object;
}

/* DOCUMENT.CREATEEVENT (DOM § 4.5): the interface its argument names,
 * matched against DOM's table without regard to ASCII case, makes an event
 * that initEvent (or its interface's own init) must set up first. A name
 * not on the table is a NotSupportedError. A name on it is made as the
 * interface it names where this binding has that interface: Event,
 * CustomEvent, HashChangeEvent, MessageEvent, and the UI events (UIEvent,
 * MouseEvent, KeyboardEvent, FocusEvent, CompositionEvent). Every other
 * name on the table names an interface this browser has not built
 * (BeforeUnloadEvent, DragEvent, StorageEvent, TextEvent and the device
 * events), and makes a plain Event, which is what every name made before
 * the table was read: createEvent followed by initEvent is how pages have
 * long made their events, and refusing a name would break them where today
 * they work (MODERN_WEB.md § P1-Da, as built, says how many). TouchEvent is the exception: DOM puts
 * it on the table only where the legacy touch APIs are exposed, which a
 * desktop browser does not do (WINDOW_DOCUMENT.md, `ontouchstart` (e)),
 * and a page that makes one has asked "is this a phone?". With the Modern
 * Web switch off the argument is not read and every call makes a plain
 * Event, as before the arc. */
/* C_UI is a UI event, the table's `ui` naming which. */
enum { C_REFUSED, C_EVENT, C_CUSTOM, C_HASHCHANGE, C_MESSAGE, C_UNBUILT, C_UI };

static int message_defaults(os64_dom_t *dom, JSContext *ctx, DEvent *event);
static int ui_defaults(os64_dom_t *dom, JSContext *ctx, DEvent *event, int kind);

static int creatable(const char *name, int *ui)
{
    static const struct { const char *name; int as, ui; } kTable[] = {
        {"beforeunloadevent", C_UNBUILT, 0}, {"compositionevent", C_UI, D_UI_COMPOSITION},
        {"customevent", C_CUSTOM, 0}, {"devicemotionevent", C_UNBUILT, 0},
        {"deviceorientationevent", C_UNBUILT, 0}, {"dragevent", C_UNBUILT, 0}, {"event", C_EVENT, 0},
        {"events", C_EVENT, 0}, {"focusevent", C_UI, D_UI_FOCUS}, {"hashchangeevent", C_HASHCHANGE, 0},
        {"htmlevents", C_EVENT, 0}, {"keyboardevent", C_UI, D_UI_KEYBOARD}, {"messageevent", C_MESSAGE, 0},
        {"mouseevent", C_UI, D_UI_MOUSE}, {"mouseevents", C_UI, D_UI_MOUSE}, {"storageevent", C_UNBUILT, 0},
        {"svgevents", C_EVENT, 0}, {"textevent", C_UNBUILT, 0}, {"touchevent", C_REFUSED, 0},
        {"uievent", C_UI, D_UI_EVENT}, {"uievents", C_UI, D_UI_EVENT},
    };
    for (size_t i = 0; i < sizeof(kTable) / sizeof(kTable[0]); i++) {
        const char *a = name, *b = kTable[i].name;
        while (*a != '\0' && *b != '\0' && (*a >= 'A' && *a <= 'Z' ? *a + ('a' - 'A') : *a) == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') {
            *ui = kTable[i].ui;
            return kTable[i].as;
        }
    }
    return C_REFUSED;
}

static JSValue create_event(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv)
{
    int as = C_EVENT, ui = D_UI_NONE;
    if (dom->options.modern) {
        if (argc < 1) return JS_ThrowTypeError(ctx, "createEvent needs an interface's name");
        DString name = {0};
        if (!d_string(dom, ctx, argv[0], &name)) return JS_EXCEPTION;
        /* d_string makes a NUL U+FFFD, which no name on the table holds. */
        as = creatable(name.data, &ui);
        d_string_free(dom, &name);
        if (as == C_REFUSED) return d_error(ctx, "NotSupportedError", "createEvent does not know that interface");
    }
    if (as == C_UI && dom->ui_prototypes[ui] != NULL) {
        JSValue object = d_event_new_proto(dom, ctx, dom->ui_prototypes[ui]->value, "", false, false, false);
        if (JS_IsException(object)) return object;
        DEvent *event = JS_GetOpaque(object, dom->event_class);
        event->initialized = false;
        if (ui_defaults(dom, ctx, event, ui) < 0) {
            JS_FreeValue(ctx, object);
            return JS_EXCEPTION;
        }
        return object;
    }
    JSValueConst prototype = as == C_CUSTOM && dom->custom_prototype != NULL ? dom->custom_prototype->value
                           : as == C_HASHCHANGE && dom->hashchange_prototype != NULL ? dom->hashchange_prototype->value
                           : as == C_MESSAGE && dom->message_prototype != NULL ? dom->message_prototype->value
                           : JS_UNDEFINED;
    JSValue object = d_event_new_proto(dom, ctx, prototype, "", false, false, false);
    if (JS_IsException(object)) return object;
    DEvent *event = JS_GetOpaque(object, dom->event_class);
    event->initialized = false;
    event->custom = as == C_CUSTOM;
    if (as == C_MESSAGE && dom->message_prototype != NULL && message_defaults(dom, ctx, event) < 0) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    /* HashChangeEvent's members are its own data properties, as its
     * constructor makes them (window.c), here their defaults. */
    if (as == C_HASHCHANGE) {
        static const char *const kURLs[] = {"newURL", "oldURL"};
        for (int i = 0; i < 2; i++)
            if (JS_DefinePropertyValueStr(ctx, object, kURLs[i], JS_NewString(ctx, ""),
                                          JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE) < 0) {
                JS_FreeValue(ctx, object);
                return JS_EXCEPTION;
            }
    }
    return object;
}

static JSValue event_construct(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                               int magic, JSValue *data)
{
    (void)self;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (magic == 1) return create_event(dom, ctx, argc, argv);
    return d_event_construct(dom, ctx, argc, argv, JS_UNDEFINED);
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
    /* EVENTTARGET.PROTOTYPE (DOM § 2.7) holds the listener methods once,
     * and every target inherits them: Node.prototype, each plain target's
     * prototype (d_event_target), and the global through Window.prototype
     * (HTML § 7.2), which sits between them. The methods tell the target by
     * `this`, the global's included when a bare call gives none. */
    dom->event_target_prototype = d_retain(dom, ctx, JS_NewObject(ctx));
    if (dom->event_target_prototype == NULL) return -1;
    JSValueConst target = dom->event_target_prototype->value;
    static const struct { const char *name; int magic, argc; } targets[] = {
        {"addEventListener", M_ADD, 2}, {"removeEventListener", M_REMOVE, 2}, {"dispatchEvent", M_DISPATCH, 1}
    };
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++)
        if (d_method(dom, ctx, target, targets[i].name, target_method, targets[i].argc, targets[i].magic) < 0)
            return -1;
    dom->window_prototype = d_retain(dom, ctx, JS_NewObjectProto(ctx, target));
    if (dom->window_prototype == NULL ||
        JS_SetPrototype(ctx, dom->prototypes[D_PROTO_NODE]->value, target) < 0 ||
        JS_SetPrototype(ctx, global, dom->window_prototype->value) < 0)
        return -1;
    JSValueConst holders[] = {dom->prototypes[D_PROTO_ELEMENT]->value, dom->prototypes[D_PROTO_DOCUMENT]->value, global};
    for (int kind = 0; kind < D_EVENT_SCROLL; kind++) {
        if (!has_handler_name(kind)) continue;
        for (size_t i = 0; i < sizeof(holders) / sizeof(holders[0]); i++)
            if (d_event_handler(dom, ctx, holders[i], kind) < 0) return -1;
    }
    return d_method(dom, ctx, dom->prototypes[D_PROTO_DOCUMENT]->value, "createEvent", event_construct, 1, 1);
}

/* CUSTOMEVENT (DOM § 2.4): an Event that carries a detail, null by
 * default. Its state is the Event's own record, so dispatch needs nothing
 * new. `new CustomEvent(type, {detail})` reads EventInit's members, then
 * detail; the detail is whatever value the page gave, held by the event
 * and traced with it. A Modern Web row. */
static JSValue custom_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSValue object = d_event_construct(dom, ctx, argc, argv,
                                       JS_IsObject(prototype) ? (JSValueConst)prototype : dom->custom_prototype->value);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object)) return object;
    JSValue detail = argc > 1 && JS_IsObject(argv[1]) ? JS_GetPropertyStr(ctx, argv[1], "detail") : JS_UNDEFINED;
    if (JS_IsException(detail)) {
        JS_FreeValue(ctx, object);
        return detail;
    }
    DEvent *event = JS_GetOpaque(object, dom->event_class);
    event->custom = true;
    event->custom_detail = JS_IsUndefined(detail) ? JS_NULL : detail;
    return object;
}

int d_custom_event_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSValue event_prototype = JS_GetClassProto(ctx, dom->event_class);
    dom->custom_prototype = d_retain(dom, ctx, JS_NewObjectProto(ctx, event_prototype));
    JS_FreeValue(ctx, event_prototype);
    if (dom->custom_prototype == NULL) return -1;
    JSValueConst prototype = dom->custom_prototype->value;
    if (d_accessor(dom, ctx, prototype, "detail", event_member, E_DETAIL, false) < 0 ||
        d_method_named(dom, ctx, prototype, "initCustomEvent", event_member, 1, E_INIT_CUSTOM) < 0)
        return -1;
    JSValue event_interface = JS_GetPropertyStr(ctx, global, "Event");
    if (JS_IsException(event_interface)) return -1;
    JSValue interface = d_interface_new(ctx, "CustomEvent", 1, custom_constructor, prototype, event_interface);
    JS_FreeValue(ctx, event_interface);
    if (JS_IsException(interface)) return -1;
    return JS_DefinePropertyValueStr(ctx, global, "CustomEvent", interface, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

int d_event_target(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype, JSClassID class_id,
                   int (*resolve)(os64_dom_t *dom, JSContext *ctx, JSValueConst self, bool make,
                                  DValue **entry))
{
    bool known = false;
    for (unsigned i = 0; i < dom->target_class_count; i++) known = known || dom->target_classes[i] == class_id;
    if (!known) {
        if (dom->target_class_count == sizeof(dom->target_classes) / sizeof(dom->target_classes[0])) {
            JS_ThrowInternalError(ctx, "Too many kinds of plain event target");
            return -1;
        }
        dom->target_resolvers[dom->target_class_count] = resolve;
        dom->target_classes[dom->target_class_count++] = class_id;
    }
    /* The methods are EventTarget.prototype's: a prototype whose chain
     * reaches it already (Attr's, through Node's) is left as it is, and one
     * that inherits Object.prototype directly inherits EventTarget.prototype
     * instead, as every plain target's interface does. */
    if (dom->event_target_prototype == NULL) {
        JS_ThrowInternalError(ctx, "A plain event target installed before EventTarget.prototype");
        return -1;
    }
    JSValueConst target = dom->event_target_prototype->value;
    /* `new EventTarget()`'s own class: its prototype is EventTarget's. */
    if (JS_VALUE_GET_PTR(prototype) == JS_VALUE_GET_PTR(target)) return 0;
    JSValue object = JS_GetPrototype(ctx, target);      /* Object.prototype */
    if (JS_IsException(object)) return -1;
    JSValue at = JS_DupValue(ctx, prototype);
    bool reached = false, plain = false;
    for (int depth = 0; !reached && JS_IsObject(at) && depth < 32; depth++) {
        JSValue next = JS_GetPrototype(ctx, at);
        if (JS_IsException(next)) {
            JS_FreeValue(ctx, at);
            JS_FreeValue(ctx, object);
            return -1;
        }
        reached = JS_IsObject(next) && JS_VALUE_GET_PTR(next) == JS_VALUE_GET_PTR(target);
        if (depth == 0) plain = JS_IsObject(next) && JS_VALUE_GET_PTR(next) == JS_VALUE_GET_PTR(object);
        JS_FreeValue(ctx, at);
        at = next;
    }
    JS_FreeValue(ctx, at);
    JS_FreeValue(ctx, object);
    if (reached) return 0;
    if (!plain) {
        JS_ThrowInternalError(ctx, "A plain event target's prototype inherits neither Object nor EventTarget");
        return -1;
    }
    return JS_SetPrototype(ctx, prototype, target) < 0 ? -1 : 0;
}

JSValue d_listener_change(os64_dom_t *dom, JSContext *ctx, JSValueConst self, JSValueConst callback,
                          bool add)
{
    /* The legacy addListener(callback): addEventListener("change", callback),
     * a null callback doing nothing (CSSOM View § MediaQueryList). */
    if (JS_IsNull(callback) || JS_IsUndefined(callback)) return JS_UNDEFINED;
    JSValue type = JS_NewString(ctx, "change");
    if (JS_IsException(type)) return type;
    JSValueConst args[2] = {type, callback};
    JSValue result = target_method_on(dom, ctx, self, 2, args, add ? M_ADD : M_REMOVE);
    JS_FreeValue(ctx, type);
    return result;
}

int d_event_handler(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype, int kind)
{
    char name[32] = "on";
    os64_strcopy(name + 2, sizeof(name) - 2, kinds[kind]);
    return d_accessor(dom, ctx, prototype, name, handler_property, kind, true);
}

/* A PLAIN TARGET'S ENTRY IN THE OBJECT (internal.h, A PLAIN EVENT TARGET).
 * A plain target a page makes as often as it
 * likes (an AbortSignal, an XMLHttpRequest) cannot be a registry entry,
 * which holds its object until drain: its record lives in the object's own
 * state instead, and its listeners' callbacks go with the object. The
 * dispatch, the listener records and the methods are the same ones. */

void d_event_target_init(os64_dom_t *dom, DValue *entry, JSValueConst object)
{
    os64_memset(entry, 0, sizeof(*entry));
    entry->value = (JSValue)object;
    entry->dom = dom;
    entry->engine_held = true;
}

void d_event_target_mark(JSRuntime *rt, const DValue *entry, JS_MarkFunc *mark)
{
    for (const DListener *listener = entry->listeners; listener != NULL; listener = listener->next)
        JS_MarkValue(rt, listener->callback, mark);
}

void d_event_target_finalize(JSRuntime *rt, DValue *entry)
{
    if (entry->dom == NULL) return;
    for (DListener *listener = entry->listeners; listener != NULL; listener = listener->next) {
        JS_FreeValueRT(rt, listener->callback);
        listener->callback = JS_UNDEFINED;
    }
    d_event_free(entry->dom, entry);
    entry->dom = NULL;
}

/* on<type> for a plain target, for any type: data[1] is the type. */
static JSValue named_handler_property(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                                      int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    /* As handler_property: an entry made on demand is made for a handler
     * set, and its absence reads null. */
    DValue *entry = NULL;
    int found = plain_entry(dom, ctx, self, set && argc > 0 && JS_IsObject(argv[0]), &entry);
    if (found < 0) return JS_EXCEPTION;
    if (found == 0) return JS_ThrowTypeError(ctx, "Expected an event target");
    if (entry == NULL) return set ? JS_UNDEFINED : JS_NULL;
    const char *type = JS_ToCString(ctx, data[1]);
    if (type == NULL) return JS_EXCEPTION;
    JSValue result = JS_UNDEFINED;
    DListener *slot = slot_find(entry, type);
    if ((magic & D_SET) == 0) {
        result = slot != NULL && slot->handler == D_SLOT_FUNCTION ? JS_DupValue(ctx, slot->callback) : JS_NULL;
    } else {
        if (slot == NULL) {
            slot = listener_new(dom, ctx, entry, type, false);
            if (slot != NULL) slot->handler = D_SLOT_EMPTY;
        }
        JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
        uint32_t script = slot != NULL && JS_IsObject(value) ? d_callback_script(dom, ctx, value) : D_NO_SCRIPT;
        if (slot == NULL || script == D_SCRIPT_FAILED) result = JS_EXCEPTION;
        else if (JS_IsObject(value)) {
            slot_state(dom, slot, D_SLOT_FUNCTION);
            set_callback(dom, slot, JS_DupValue(ctx, value));
            slot->script = script;
        } else slot_state(dom, slot, D_SLOT_EMPTY);
        listeners_changed(dom, entry);
    }
    JS_FreeCString(ctx, type);
    return result;
}

int d_event_handler_named(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype, const char *type)
{
    char name[48] = "on";
    os64_strcopy(name + 2, sizeof(name) - 2, type);
    JSValue data[2] = {JS_DupValue(ctx, dom->anchor->value), JS_NewString(ctx, type)};
    JSAtom atom = JS_IsException(data[1]) ? JS_ATOM_NULL : JS_NewAtom(ctx, name);
    JSValue get = atom == JS_ATOM_NULL ? JS_EXCEPTION : JS_NewCFunctionData(ctx, named_handler_property, 0, 0, 2, data);
    JSValue set = JS_IsException(get) ? JS_EXCEPTION
                                      : JS_NewCFunctionData(ctx, named_handler_property, 1, D_SET, 2, data);
    JS_FreeValue(ctx, data[0]);
    JS_FreeValue(ctx, data[1]);
    if (JS_IsException(set)) {
        JS_FreeValue(ctx, get);
        if (atom != JS_ATOM_NULL) JS_FreeAtom(ctx, atom);
        return -1;
    }
    int result = JS_DefinePropertyGetSet(ctx, prototype, atom, get, set, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, atom);
    return result;
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

/* PROGRESS EVENTS (XHR § 4): an Event with three more attributes, which an
 * XMLHttpRequest fires and a page may construct. Its state is the Event's
 * own record, so dispatch needs nothing new. */

enum { P_COMPUTABLE, P_LOADED, P_TOTAL };

static JSValue progress_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                               JSValue *data)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DEvent *event = JS_GetOpaque(self, dom->event_class);
    if (event == NULL || !event->progress) return JS_ThrowTypeError(ctx, "Expected a ProgressEvent");
    if (magic == P_COMPUTABLE) return JS_NewBool(ctx, event->length_computable);
    return JS_NewFloat64(ctx, magic == P_LOADED ? event->loaded : event->total);
}

JSValue d_progress_event_new(os64_dom_t *dom, JSContext *ctx, const char *type, bool computable, double loaded,
                             double total, bool trusted)
{
    JSValue object = d_event_new_proto(dom, ctx, dom->progress_prototype != NULL ? dom->progress_prototype->value
                                                                                 : JS_UNDEFINED,
                                       type, false, false, trusted);
    if (JS_IsException(object)) return object;
    DEvent *event = JS_GetOpaque(object, dom->event_class);
    event->progress = true;
    event->length_computable = computable;
    event->loaded = loaded;
    event->total = total;
    return object;
}

/* new ProgressEvent(type, init): the dictionary read as WebIDL reads an
 * inherited one, EventInit's members first, then its own in name order;
 * loaded and total are WebIDL doubles: finite, or a TypeError. */
static JSValue progress_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "ProgressEvent needs a type");
    const char *type = JS_ToCString(ctx, argv[0]);
    if (type == NULL) return JS_EXCEPTION;
    bool flags[4] = {false, false, false, false};     /* bubbles, cancelable, composed, lengthComputable */
    double numbers[2] = {0, 0};
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (!JS_IsUndefined(init) && !JS_IsNull(init) && !JS_IsObject(init)) {
        JS_FreeCString(ctx, type);
        return JS_ThrowTypeError(ctx, "ProgressEventInit must be a dictionary");
    }
    if (JS_IsObject(init)) {
        static const char *const names[] = {"bubbles", "cancelable", "composed", "lengthComputable", "loaded", "total"};
        for (int i = 0; i < 6; i++) {
            JSValue value = JS_GetPropertyStr(ctx, init, names[i]);
            double n = 0;
            int got = JS_IsException(value) ? -1
                    : i < 4 ? JS_ToBool(ctx, value)
                    : JS_IsUndefined(value) ? 0 : JS_ToFloat64(ctx, &n, value);
            JS_FreeValue(ctx, value);
            if (got < 0) {
                JS_FreeCString(ctx, type);
                return JS_EXCEPTION;
            }
            if (i < 4) flags[i] = got != 0;
            else if (n != n || n - n != 0) {
                JS_FreeCString(ctx, type);
                return JS_ThrowTypeError(ctx, "loaded and total are finite numbers");
            } else numbers[i - 4] = n;
        }
    }
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) {
        JS_FreeCString(ctx, type);
        return prototype;
    }
    JSValue object = d_event_new_proto(dom, ctx, JS_IsObject(prototype) ? (JSValueConst)prototype
                                                 : dom->progress_prototype->value,
                                       type, flags[0], flags[1], false);
    JS_FreeValue(ctx, prototype);
    JS_FreeCString(ctx, type);
    if (JS_IsException(object)) return object;
    DEvent *event = JS_GetOpaque(object, dom->event_class);
    event->progress = true;
    event->length_computable = flags[3];
    event->loaded = numbers[0];
    event->total = numbers[1];
    return object;
}

int d_progress_event_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSValue event_prototype = JS_GetClassProto(ctx, dom->event_class);
    JSValue event_interface = JS_GetPropertyStr(ctx, global, "Event");
    dom->progress_prototype = d_retain(dom, ctx, JS_NewObjectProto(ctx, event_prototype));
    JS_FreeValue(ctx, event_prototype);
    if (dom->progress_prototype == NULL || JS_IsException(event_interface)) {
        JS_FreeValue(ctx, event_interface);
        return -1;
    }
    JSValueConst prototype = dom->progress_prototype->value;
    static const char *const names[] = {"lengthComputable", "loaded", "total"};
    for (int i = 0; i < 3; i++)
        if (d_accessor(dom, ctx, prototype, names[i], progress_member, i, false) < 0) {
            JS_FreeValue(ctx, event_interface);
            return -1;
        }
    JSValue interface = d_interface_new(ctx, "ProgressEvent", 1, progress_constructor, prototype, event_interface);
    JS_FreeValue(ctx, event_interface);
    if (JS_IsException(interface)) return -1;
    return JS_DefinePropertyValueStr(ctx, global, "ProgressEvent", interface, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

/* ERROR EVENTS (HTML § 8.1.4.6 and § 8.1.4.7.2): ErrorEvent, which the
 * window's error event is, and PromiseRejectionEvent, which its
 * unhandledrejection and rejectionhandled are. Their attributes are the
 * Event's own record, as a ProgressEvent's are (report.c fires them). */

enum { X_MESSAGE, X_FILENAME, X_LINENO, X_COLNO, X_ERROR, X_PROMISE, X_REASON };

static JSValue error_event_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                                  JSValue *data)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DEvent *event = JS_GetOpaque(self, dom->event_class);
    bool rejection = magic >= X_PROMISE;
    if (event == NULL || !(rejection ? event->rejection_event : event->error_event))
        return JS_ThrowTypeError(ctx, rejection ? "Expected a PromiseRejectionEvent" : "Expected an ErrorEvent");
    switch (magic) {
    case X_MESSAGE: return JS_DupValue(ctx, event->detail[1]);
    case X_FILENAME: return JS_DupValue(ctx, event->detail[2]);
    case X_LINENO: return JS_NewInt64(ctx, event->lineno);
    case X_COLNO: return JS_NewInt64(ctx, event->colno);
    case X_REASON: return JS_DupValue(ctx, event->detail[1]);
    default: return JS_DupValue(ctx, event->detail[0]);
    }
}

JSValue d_error_event_new(os64_dom_t *dom, JSContext *ctx, const char *message, const char *filename,
                          uint32_t lineno, uint32_t colno, JSValueConst error)
{
    JSValue object = d_event_new_proto(dom, ctx, dom->error_event_prototype->value, "error", false, true, true);
    if (JS_IsException(object)) return object;
    JSValue text[2] = {JS_NewString(ctx, message), JS_NewString(ctx, filename)};
    if (JS_IsException(text[0]) || JS_IsException(text[1])) {
        JS_FreeValue(ctx, text[0]);
        JS_FreeValue(ctx, text[1]);
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    DEvent *event = JS_GetOpaque(object, dom->event_class);
    event->error_event = true;
    event->detail[0] = JS_DupValue(ctx, error);
    event->detail[1] = text[0];
    event->detail[2] = text[1];
    event->lineno = lineno;
    event->colno = colno;
    return object;
}

JSValue d_rejection_event_new(os64_dom_t *dom, JSContext *ctx, const char *type, JSValueConst promise,
                              JSValueConst reason, bool cancelable)
{
    JSValue object = d_event_new_proto(dom, ctx, dom->rejection_event_prototype->value, type, false, cancelable,
                                       true);
    if (JS_IsException(object)) return object;
    DEvent *event = JS_GetOpaque(object, dom->event_class);
    event->rejection_event = true;
    event->detail[0] = JS_DupValue(ctx, promise);
    event->detail[1] = JS_DupValue(ctx, reason);
    return object;
}

/* An Event interface's own constructor: the Event made (EventInit's
 * members read), with new.target's prototype or `own`. */
static JSValue constructed_event(os64_dom_t *dom, JSContext *ctx, JSValueConst new_target, int argc,
                                 JSValueConst *argv, JSValueConst own)
{
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSValue object = d_event_construct(dom, ctx, argc, argv, JS_IsObject(prototype) ? (JSValueConst)prototype : own);
    JS_FreeValue(ctx, prototype);
    return object;
}

/* new ErrorEvent(type, init): ErrorEventInit's members after EventInit's,
 * in name order as WebIDL reads them: colno and lineno unsigned longs,
 * error any value, filename a USVString, message a DOMString. */
static JSValue error_event_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue object = constructed_event(dom, ctx, new_target, argc, argv, dom->error_event_prototype->value);
    if (JS_IsException(object)) return object;
    DEvent *event = JS_GetOpaque(object, dom->event_class);
    event->error_event = true;
    event->detail[1] = JS_NewString(ctx, "");
    event->detail[2] = JS_NewString(ctx, "");
    bool failed = JS_IsException(event->detail[1]) || JS_IsException(event->detail[2]);
    static const char *const names[] = {"colno", "error", "filename", "lineno", "message"};
    for (int i = 0; !failed && argc > 1 && JS_IsObject(argv[1]) && i < 5; i++) {
        JSValue value = JS_GetPropertyStr(ctx, argv[1], names[i]);
        if (JS_IsException(value)) { failed = true; break; }
        if (JS_IsUndefined(value)) continue;
        if (i == 0 || i == 3) {
            int32_t n = 0;
            failed = JS_ToInt32(ctx, &n, value) < 0;
            JS_FreeValue(ctx, value);
            if (i == 0) event->colno = (uint32_t)n;
            else event->lineno = (uint32_t)n;
        } else if (i == 1) {
            JS_FreeValue(ctx, event->detail[0]);
            event->detail[0] = value;
        } else if (i == 2) {
            DString text;
            failed = !d_string_text(dom, ctx, value, &text, true);
            JS_FreeValue(ctx, value);
            JSValue string = failed ? JS_EXCEPTION : JS_NewStringLen(ctx, text.data, text.length);
            if (!failed) d_string_free(dom, &text);
            failed = JS_IsException(string);
            if (!failed) {
                JS_FreeValue(ctx, event->detail[2]);
                event->detail[2] = string;
            }
        } else {
            JSValue string = JS_ToString(ctx, value);
            JS_FreeValue(ctx, value);
            failed = JS_IsException(string);
            if (!failed) {
                JS_FreeValue(ctx, event->detail[1]);
                event->detail[1] = string;
            }
        }
    }
    if (failed) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
}

/* new PromiseRejectionEvent(type, init): the init is required, and so is
 * its promise, an object; its reason is any value. */
static JSValue rejection_event_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(ctx, "PromiseRejectionEvent needs a type and a dictionary");
    JSValue object = constructed_event(dom, ctx, new_target, argc, argv, dom->rejection_event_prototype->value);
    if (JS_IsException(object)) return object;
    JSValue promise = JS_IsObject(argv[1]) ? JS_GetPropertyStr(ctx, argv[1], "promise") : JS_UNDEFINED;
    if (!JS_IsException(promise) && !JS_IsObject(promise)) {
        JS_FreeValue(ctx, promise);
        promise = JS_ThrowTypeError(ctx, "PromiseRejectionEventInit's promise is a required object");
    }
    JSValue reason = JS_IsException(promise) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, argv[1], "reason");
    if (JS_IsException(reason)) {
        JS_FreeValue(ctx, promise);
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    DEvent *event = JS_GetOpaque(object, dom->event_class);
    event->rejection_event = true;
    event->detail[0] = promise;
    event->detail[1] = reason;
    return object;
}

int d_error_events_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSValue event_prototype = JS_GetClassProto(ctx, dom->event_class);
    dom->error_event_prototype = d_retain(dom, ctx, JS_NewObjectProto(ctx, event_prototype));
    dom->rejection_event_prototype = dom->error_event_prototype != NULL
        ? d_retain(dom, ctx, JS_NewObjectProto(ctx, event_prototype)) : NULL;
    JS_FreeValue(ctx, event_prototype);
    if (dom->rejection_event_prototype == NULL) return -1;
    static const struct { const char *name; int magic; } members[] = {
        {"message", X_MESSAGE}, {"filename", X_FILENAME}, {"lineno", X_LINENO}, {"colno", X_COLNO},
        {"error", X_ERROR}, {"promise", X_PROMISE}, {"reason", X_REASON}
    };
    for (size_t i = 0; i < sizeof(members) / sizeof(members[0]); i++) {
        DValue *holder = members[i].magic >= X_PROMISE ? dom->rejection_event_prototype : dom->error_event_prototype;
        if (d_accessor(dom, ctx, holder->value, members[i].name, error_event_member, members[i].magic, false) < 0)
            return -1;
    }
    JSValue event_interface = JS_GetPropertyStr(ctx, global, "Event");
    if (JS_IsException(event_interface)) return -1;
    static const char *const names[] = {"ErrorEvent", "PromiseRejectionEvent"};
    JSCFunction *const constructors[] = {error_event_constructor, rejection_event_constructor};
    int result = 0;
    for (int i = 0; result == 0 && i < 2; i++) {
        JSValueConst own = i == 0 ? dom->error_event_prototype->value : dom->rejection_event_prototype->value;
        JSValue f = d_interface_new(ctx, names[i], i + 1, constructors[i], own, event_interface);
        if (JS_IsException(f) ||
            JS_DefinePropertyValueStr(ctx, global, names[i], f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
            result = -1;
    }
    JS_FreeValue(ctx, event_interface);
    return result;
}

/* MESSAGE EVENTS (HTML § 9.3.2): MessageEvent, which a posted message's
 * message and messageerror are (message.c fires them). Its attributes are
 * the Event's own record. A MessageEvent's ports are a frozen array of
 * MessagePorts, which this browser does not have, so every one is empty and
 * a non-empty list is a TypeError; its source is a window or null. */

enum { G_DATA, G_ORIGIN, G_LAST_EVENT_ID, G_SOURCE, G_PORTS, G_INIT };

/* An empty array, frozen: what Object.freeze leaves of one (its length not
 * writable, and the array not extensible). */
static JSValue frozen_empty(JSContext *ctx)
{
    JSValue array = JS_NewArray(ctx);
    if (JS_IsException(array)) return array;
    if (JS_DefinePropertyValueStr(ctx, array, "length", JS_NewInt32(ctx, 0), 0) < 0 ||
        JS_PreventExtensions(ctx, array) < 0) {
        JS_FreeValue(ctx, array);
        return JS_EXCEPTION;
    }
    return array;
}

/* A MessageEvent's record set: the values taken. */
static void message_fields(JSContext *ctx, DEvent *event, JSValue data, JSValue origin, JSValue last_event_id,
                           JSValue source, JSValue ports)
{
    JSValue fields[D_EVENT_DETAILS] = {data, origin, last_event_id, source, ports};
    event->message_event = true;
    for (int i = 0; i < D_EVENT_DETAILS; i++) {
        JS_FreeValue(ctx, event->detail[i]);
        event->detail[i] = fields[i];
    }
}

/* A MessageEvent's members as WebIDL converts them, JS_UNDEFINED being
 * each one's default; JS_EXCEPTION with an exception pending. origin is a
 * USVString and lastEventId a DOMString, "" by default. */
static JSValue message_text(os64_dom_t *dom, JSContext *ctx, JSValueConst value, bool scalar)
{
    if (JS_IsUndefined(value)) return JS_NewString(ctx, "");
    if (!scalar) return JS_ToString(ctx, value);
    DString text = {0};
    if (!d_string_text(dom, ctx, value, &text, true)) return JS_EXCEPTION;
    JSValue string = JS_NewStringLen(ctx, text.data, text.length);
    d_string_free(dom, &text);
    return string;
}

/* source: a MessageEventSource (a window, and this one is the only window
 * a page here can name; a MessagePort or a ServiceWorker, which this
 * browser does not have) or null, its default. */
static JSValue message_source(JSContext *ctx, JSValueConst value)
{
    if (JS_IsUndefined(value) || JS_IsNull(value)) return JS_NULL;
    JSValue global = JS_GetGlobalObject(ctx);
    bool window = JS_IsObject(value) && JS_VALUE_GET_PTR(global) == JS_VALUE_GET_PTR(value);
    JS_FreeValue(ctx, global);
    if (!window) return JS_ThrowTypeError(ctx, "A MessageEvent's source is a window, a MessagePort or null");
    return JS_DupValue(ctx, value);
}

/* ports: a sequence of MessagePorts, which must therefore be empty, as a
 * new frozen array. */
static JSValue message_ports(os64_dom_t *dom, JSContext *ctx, JSValueConst value)
{
    if (!JS_IsUndefined(value)) {
        JSValue *items;
        size_t count;
        if (d_sequence(dom, ctx, value, D_SEQUENCE_MAX, &items, &count) < 0) return JS_EXCEPTION;
        d_sequence_free(ctx, items, count);
        if (count != 0) return JS_ThrowTypeError(ctx, "A MessageEvent's ports are MessagePorts");
    }
    return frozen_empty(ctx);
}

/* initMessageEvent's members past data, converted in its arguments' order:
 * origin, lastEventId, source, ports, into out[] as the record orders them
 * from detail[1]. -1 with an exception pending and nothing to free. */
static int message_members(os64_dom_t *dom, JSContext *ctx, JSValueConst origin, JSValueConst last_event_id,
                           JSValueConst source, JSValueConst ports, JSValue out[4])
{
    out[0] = message_text(dom, ctx, origin, true);
    out[1] = JS_IsException(out[0]) ? JS_EXCEPTION : message_text(dom, ctx, last_event_id, false);
    out[2] = JS_IsException(out[1]) ? JS_EXCEPTION : message_source(ctx, source);
    out[3] = JS_IsException(out[2]) ? JS_EXCEPTION : message_ports(dom, ctx, ports);
    if (!JS_IsException(out[3])) return 0;
    for (int i = 0; i < 4; i++) JS_FreeValue(ctx, out[i]);
    return -1;
}

static JSValue message_event_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                                    JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DEvent *event = JS_GetOpaque(self, dom->event_class);
    if (event == NULL || !event->message_event) return JS_ThrowTypeError(ctx, "Expected a MessageEvent");
    if (magic != G_INIT) return JS_DupValue(ctx, event->detail[magic]);
    /* initMessageEvent(type, bubbles, cancelable, data, origin,
     * lastEventId, source, ports): every argument converted, in order,
     * before the event changes, so one that throws leaves it as it was;
     * during a dispatch it then does nothing. */
    JSValueConst at[8];
    for (int i = 0; i < 8; i++) at[i] = i < argc ? argv[i] : JS_UNDEFINED;
    DInit init;
    if (event_init_read(ctx, argc, argv, "initMessageEvent", &init) < 0) return JS_EXCEPTION;
    JSValue members[4];
    if (message_members(dom, ctx, at[4], at[5], at[6], at[7], members) < 0) {
        js_free(ctx, init.type);
        return JS_EXCEPTION;
    }
    if (event->dispatching) {
        js_free(ctx, init.type);
        for (int i = 0; i < 4; i++) JS_FreeValue(ctx, members[i]);
        return JS_UNDEFINED;
    }
    event_init_set(ctx, event, &init);
    message_fields(ctx, event, JS_IsUndefined(at[3]) ? JS_NULL : JS_DupValue(ctx, at[3]), members[0], members[1],
                   members[2], members[3]);
    return JS_UNDEFINED;
}

JSValue d_message_event_new(os64_dom_t *dom, JSContext *ctx, const char *type, JSValue data, const char *origin,
                            JSValueConst source)
{
    JSValue object = d_event_new_proto(dom, ctx, dom->message_prototype->value, type, false, false, true);
    JSValue text = JS_IsException(object) ? JS_EXCEPTION : JS_NewString(ctx, origin);
    JSValue empty = JS_IsException(text) ? JS_EXCEPTION : JS_NewString(ctx, "");
    JSValue ports = JS_IsException(empty) ? JS_EXCEPTION : frozen_empty(ctx);
    if (JS_IsException(ports)) {
        JS_FreeValue(ctx, data);
        JS_FreeValue(ctx, text);
        JS_FreeValue(ctx, empty);
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    message_fields(ctx, JS_GetOpaque(object, dom->event_class), data, text, empty, JS_DupValue(ctx, source), ports);
    return object;
}

/* An event createEvent('MessageEvent') makes: the record's defaults. */
static int message_defaults(os64_dom_t *dom, JSContext *ctx, DEvent *event)
{
    JSValue members[4];
    if (message_members(dom, ctx, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, members) < 0) return -1;
    message_fields(ctx, event, JS_NULL, members[0], members[1], members[2], members[3]);
    return 0;
}

/* new MessageEvent(type, init): MessageEventInit's members after
 * EventInit's, in name order as WebIDL reads them: data, lastEventId,
 * origin, ports, source. */
static JSValue message_event_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue object = constructed_event(dom, ctx, new_target, argc, argv, dom->message_prototype->value);
    if (JS_IsException(object)) return object;
    /* Each member is read and converted before the next is read. */
    static const char *const names[] = {"data", "lastEventId", "origin", "ports", "source"};
    JSValue got[5] = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
    bool failed = false;
    for (int i = 0; !failed && i < 5; i++) {
        JSValue value = argc > 1 && JS_IsObject(argv[1]) ? JS_GetPropertyStr(ctx, argv[1], names[i]) : JS_UNDEFINED;
        if (JS_IsException(value)) { failed = true; break; }
        got[i] = i == 0 ? (JS_IsUndefined(value) ? JS_NULL : JS_DupValue(ctx, value))
               : i == 1 ? message_text(dom, ctx, value, false)
               : i == 2 ? message_text(dom, ctx, value, true)
               : i == 3 ? message_ports(dom, ctx, value) : message_source(ctx, value);
        JS_FreeValue(ctx, value);
        failed = JS_IsException(got[i]);
    }
    if (failed) {
        for (int i = 0; i < 5; i++) JS_FreeValue(ctx, got[i]);
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    message_fields(ctx, JS_GetOpaque(object, dom->event_class), got[0], got[2], got[1], got[4], got[3]);
    return object;
}

int d_message_event_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSValue event_prototype = JS_GetClassProto(ctx, dom->event_class);
    dom->message_prototype = d_retain(dom, ctx, JS_NewObjectProto(ctx, event_prototype));
    JS_FreeValue(ctx, event_prototype);
    if (dom->message_prototype == NULL) return -1;
    JSValueConst prototype = dom->message_prototype->value;
    static const char *const names[] = {"data", "origin", "lastEventId", "source", "ports"};
    for (int i = 0; i < 5; i++)
        if (d_accessor(dom, ctx, prototype, names[i], message_event_member, i, false) < 0) return -1;
    if (d_method_named(dom, ctx, prototype, "initMessageEvent", message_event_member, 1, G_INIT) < 0) return -1;
    JSValue event_interface = JS_GetPropertyStr(ctx, global, "Event");
    if (JS_IsException(event_interface)) return -1;
    JSValue interface = d_interface_new(ctx, "MessageEvent", 1, message_event_constructor, prototype,
                                        event_interface);
    JS_FreeValue(ctx, event_interface);
    if (JS_IsException(interface)) return -1;
    return JS_DefinePropertyValueStr(ctx, global, "MessageEvent", interface, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

/* UI EVENTS (UI Events § 3 to 5 and its legacy § 8; HTML § 4.10.21.3's
 * SubmitEvent; CSSOM View § 12's MouseEvent, whose coordinates are doubles
 * and which has pageX and pageY): UIEvent and the five that inherit it, and
 * SubmitEvent, which inherits Event. Their attributes are the Event's own
 * record, so dispatch needs nothing new. A Modern Web row: with the switch
 * off none of them exists, and a host's mouse and key events carry their
 * fields as data (event_fields). The interfaces a person here has no way
 * to fire are absent: PointerEvent above all, whose existence makes pointer
 * libraries stop listening for mouse events (WINDOW_DOCUMENT.md § The
 * handler hazard), and WheelEvent, DragEvent and TextEvent. 0f's P8
 * (INTERFACES_FORMS.md). */

/* Where a UI event keeps its values in the record's detail[]. */
enum { U_VIEW, U_RELATED, U_TEXT, U_TEXT2 };

static const char *const kUINames[D_UI_KINDS] = {
    "", "UIEvent", "MouseEvent", "KeyboardEvent", "FocusEvent", "InputEvent", "CompositionEvent", "SubmitEvent"
};

/* EventModifierInit's members in its order, which is the order WebIDL reads
 * them in, each with the key getModifierState names it by. Bit i of
 * DEvent.modifiers is the table's row i. */
static const struct { const char *init, *key; } kModifiers[] = {
    {"altKey", "Alt"}, {"ctrlKey", "Control"}, {"metaKey", "Meta"}, {"modifierAltGraph", "AltGraph"},
    {"modifierCapsLock", "CapsLock"}, {"modifierFn", "Fn"}, {"modifierFnLock", "FnLock"},
    {"modifierHyper", "Hyper"}, {"modifierNumLock", "NumLock"}, {"modifierScrollLock", "ScrollLock"},
    {"modifierSuper", "Super"}, {"modifierSymbol", "Symbol"}, {"modifierSymbolLock", "SymbolLock"},
    {"shiftKey", "Shift"}
};
_Static_assert(sizeof(kModifiers) / sizeof(kModifiers[0]) <= 16, "the modifiers are a uint16_t's bits");
/* The four the old spellings (ctrlKey and the rest) read: their rows. */
enum { MOD_ALT = 0, MOD_CTRL = 1, MOD_META = 2, MOD_SHIFT = 13 };

/* The members, each on its interface's prototype; a getter or method's
 * magic is its interface (D_UI_*) times 256 plus the member. */
enum {
    UM_VIEW, UM_DETAIL, UM_WHICH, UM_INIT_UI,
    UM_SCREEN_X, UM_SCREEN_Y, UM_CLIENT_X, UM_CLIENT_Y, UM_PAGE_X, UM_PAGE_Y, UM_BUTTON, UM_BUTTONS,
    UM_RELATED, UM_ALT, UM_CTRL, UM_META, UM_SHIFT, UM_MODIFIER_STATE, UM_INIT_MOUSE,
    UM_KEY, UM_CODE, UM_LOCATION, UM_REPEAT, UM_COMPOSING, UM_CHAR_CODE, UM_KEY_CODE, UM_INIT_KEYBOARD,
    UM_DATA, UM_INPUT_TYPE, UM_DATA_TRANSFER, UM_TARGET_RANGES, UM_INIT_COMPOSITION
};

/* What a dictionary member or an init method's argument sets, each
 * converted as its IDL type says. The _LONG coordinates are
 * initMouseEvent's, longs where the dictionary's are doubles. */
enum {
    F_DETAIL, F_VIEW, F_WHICH, F_MODIFIER, F_BUTTON, F_BUTTONS, F_CLIENT_X, F_CLIENT_Y, F_RELATED,
    F_SCREEN_X, F_SCREEN_Y, F_SCREEN_X_LONG, F_SCREEN_Y_LONG, F_CLIENT_X_LONG, F_CLIENT_Y_LONG,
    F_CHAR_CODE, F_CODE, F_COMPOSING, F_KEY, F_KEY_CODE, F_LOCATION, F_REPEAT,
    F_INPUT_DATA, F_DATA_TRANSFER, F_INPUT_TYPE, F_TARGET_RANGES, F_DATA, F_SUBMITTER
};
typedef struct { const char *name; uint8_t field, bit; } DUIMember;

/* Each init dictionary's own members, in WebIDL's reading order (by name;
 * a legacy partial dictionary's members join theirs). UIEventInit's come
 * before an inheriting one's, and EventModifierInit's (kModifiers) between
 * them for a MouseEventInit or a KeyboardEventInit. */
static const DUIMember kUIInit[] = {{"detail", F_DETAIL, 0}, {"view", F_VIEW, 0}, {"which", F_WHICH, 0}};
static const DUIMember kMouseInit[] = {
    {"button", F_BUTTON, 0}, {"buttons", F_BUTTONS, 0}, {"clientX", F_CLIENT_X, 0}, {"clientY", F_CLIENT_Y, 0},
    {"relatedTarget", F_RELATED, 0}, {"screenX", F_SCREEN_X, 0}, {"screenY", F_SCREEN_Y, 0}
};
static const DUIMember kKeyboardInit[] = {
    {"charCode", F_CHAR_CODE, 0}, {"code", F_CODE, 0}, {"isComposing", F_COMPOSING, 0}, {"key", F_KEY, 0},
    {"keyCode", F_KEY_CODE, 0}, {"location", F_LOCATION, 0}, {"repeat", F_REPEAT, 0}
};
static const DUIMember kFocusInit[] = {{"relatedTarget", F_RELATED, 0}};
static const DUIMember kInputInit[] = {
    {"data", F_INPUT_DATA, 0}, {"dataTransfer", F_DATA_TRANSFER, 0}, {"inputType", F_INPUT_TYPE, 0},
    {"isComposing", F_COMPOSING, 0}, {"targetRanges", F_TARGET_RANGES, 0}
};
static const DUIMember kCompositionInit[] = {{"data", F_DATA, 0}};
static const DUIMember kSubmitInit[] = {{"submitter", F_SUBMITTER, 0}};

/* The legacy init methods' arguments after cancelableArg, in order. */
static const DUIMember kUIArgs[] = {{NULL, F_VIEW, 0}, {NULL, F_DETAIL, 0}};
static const DUIMember kMouseArgs[] = {
    {NULL, F_VIEW, 0}, {NULL, F_DETAIL, 0}, {NULL, F_SCREEN_X_LONG, 0}, {NULL, F_SCREEN_Y_LONG, 0},
    {NULL, F_CLIENT_X_LONG, 0}, {NULL, F_CLIENT_Y_LONG, 0}, {NULL, F_MODIFIER, MOD_CTRL},
    {NULL, F_MODIFIER, MOD_ALT}, {NULL, F_MODIFIER, MOD_SHIFT}, {NULL, F_MODIFIER, MOD_META},
    {NULL, F_BUTTON, 0}, {NULL, F_RELATED, 0}
};
static const DUIMember kKeyboardArgs[] = {
    {NULL, F_VIEW, 0}, {NULL, F_KEY, 0}, {NULL, F_LOCATION, 0}, {NULL, F_MODIFIER, MOD_CTRL},
    {NULL, F_MODIFIER, MOD_ALT}, {NULL, F_MODIFIER, MOD_SHIFT}, {NULL, F_MODIFIER, MOD_META}
};
static const DUIMember kCompositionArgs[] = {{NULL, F_VIEW, 0}, {NULL, F_DATA, 0}};

/* Whether `event` is a `kind`: a UIEvent is any of the five that inherit
 * it too, D_UI_EVENT to D_UI_COMPOSITION in internal.h's order. */
static bool ui_implements(const DEvent *event, int kind)
{
    if (kind == D_UI_EVENT) return event->ui >= D_UI_EVENT && event->ui <= D_UI_COMPOSITION;
    return event->ui == kind;
}

static void ui_set(JSContext *ctx, DEvent *event, int slot, JSValue value)
{
    JS_FreeValue(ctx, event->detail[slot]);
    event->detail[slot] = value;
}

/* A UI event's record as `kind`'s defaults leave it: its view and
 * relatedTarget null, its strings "" (an InputEvent's data null). -1 with
 * an exception pending. */
static int ui_defaults(os64_dom_t *dom, JSContext *ctx, DEvent *event, int kind)
{
    (void)dom;
    bool texts = kind == D_UI_KEYBOARD || kind == D_UI_INPUT || kind == D_UI_COMPOSITION;
    JSValue first = !texts ? JS_UNDEFINED : kind == D_UI_INPUT ? JS_NULL : JS_NewString(ctx, "");
    JSValue second = kind == D_UI_KEYBOARD || kind == D_UI_INPUT ? JS_NewString(ctx, "") : JS_UNDEFINED;
    if (JS_IsException(first) || JS_IsException(second)) {
        JS_FreeValue(ctx, first);
        JS_FreeValue(ctx, second);
        return -1;
    }
    event->ui = (uint8_t)kind;
    ui_set(ctx, event, U_VIEW, JS_NULL);
    ui_set(ctx, event, U_RELATED, JS_NULL);
    ui_set(ctx, event, U_TEXT, first);
    ui_set(ctx, event, U_TEXT2, second);
    return 0;
}

/* Window? (a view): the window, or null for undefined and null. */
static JSValue ui_view(os64_dom_t *dom, JSContext *ctx, JSValueConst value)
{
    if (JS_IsUndefined(value) || JS_IsNull(value)) return JS_NULL;
    if (JS_IsObject(value) && JS_VALUE_GET_PTR(value) == JS_VALUE_GET_PTR(dom->window->value))
        return JS_DupValue(ctx, value);
    return JS_ThrowTypeError(ctx, "A view is a Window or null");
}

/* EventTarget? (a relatedTarget): the window, a node or an Attr, a plain
 * target, or null. */
static JSValue ui_target(os64_dom_t *dom, JSContext *ctx, JSValueConst value)
{
    if (JS_IsUndefined(value) || JS_IsNull(value)) return JS_NULL;
    DValue *entry = NULL;
    int plain = plain_entry(dom, ctx, value, false, &entry);
    if (plain < 0) return JS_EXCEPTION;
    bool target = plain > 0 ||
        (JS_IsObject(value) && (JS_VALUE_GET_PTR(value) == JS_VALUE_GET_PTR(dom->window->value) ||
                                JS_GetOpaque(value, dom->node_class) != NULL || d_is_attr(dom, value)));
    if (!target) return JS_ThrowTypeError(ctx, "A relatedTarget is an EventTarget or null");
    return JS_DupValue(ctx, value);
}

/* HTMLElement? (a submitter): an element in HTML's namespace, or null. */
static JSValue ui_submitter(os64_dom_t *dom, JSContext *ctx, JSValueConst value)
{
    if (JS_IsUndefined(value) || JS_IsNull(value)) return JS_NULL;
    DValue *entry = JS_IsObject(value) ? JS_GetOpaque(value, dom->node_class) : NULL;
    const os64_html_node_t *node = entry != NULL ? entry->node : NULL;
    if (node == NULL || node->kind != OS64_HTML_ELEMENT || node->ns != OS64_HTML_NS_HTML)
        return JS_ThrowTypeError(ctx, "A submitter is an HTML element or null");
    return JS_DupValue(ctx, value);
}

/* A restricted double: finite, or a TypeError. */
static int ui_double(JSContext *ctx, JSValueConst value, double *out)
{
    if (JS_ToFloat64(ctx, out, value) < 0) return -1;
    if (*out != *out || *out - *out != 0) {
        JS_ThrowTypeError(ctx, "A coordinate is a finite number");
        return -1;
    }
    return 0;
}

/* One member or argument, `value`, converted as `field`'s IDL type says
 * and set on `event` (`bit`: F_MODIFIER's row). -1 with an exception
 * pending and the event as it was. */
static int ui_field(os64_dom_t *dom, JSContext *ctx, DEvent *event, int field, int bit, JSValueConst value)
{
    int32_t n = 0;
    int slot = U_TEXT;
    JSValue string;
    switch (field) {
    case F_DETAIL: return JS_ToInt32(ctx, &event->ui_detail, value);
    case F_WHICH: return JS_ToUint32(ctx, &event->which, value);
    case F_CHAR_CODE: return JS_ToUint32(ctx, &event->char_code, value);
    case F_KEY_CODE: return JS_ToUint32(ctx, &event->key_code, value);
    case F_LOCATION: return JS_ToUint32(ctx, &event->location, value);
    case F_BUTTON: case F_BUTTONS:
    case F_SCREEN_X_LONG: case F_SCREEN_Y_LONG: case F_CLIENT_X_LONG: case F_CLIENT_Y_LONG:
        if (JS_ToInt32(ctx, &n, value) < 0) return -1;
        if (field == F_BUTTON) event->button = (int16_t)n;
        else if (field == F_BUTTONS) event->buttons = (uint16_t)n;
        else if (field == F_SCREEN_X_LONG) event->screen_x = n;
        else if (field == F_SCREEN_Y_LONG) event->screen_y = n;
        else if (field == F_CLIENT_X_LONG) event->client_x = n;
        else event->client_y = n;
        return 0;
    case F_SCREEN_X: return ui_double(ctx, value, &event->screen_x);
    case F_SCREEN_Y: return ui_double(ctx, value, &event->screen_y);
    case F_CLIENT_X: return ui_double(ctx, value, &event->client_x);
    case F_CLIENT_Y: return ui_double(ctx, value, &event->client_y);
    case F_MODIFIER: case F_COMPOSING: case F_REPEAT: {
        int flag = JS_ToBool(ctx, value);
        if (flag < 0) return -1;
        if (field == F_COMPOSING) event->composing = flag != 0;
        else if (field == F_REPEAT) event->repeat = flag != 0;
        else event->modifiers = (uint16_t)(flag ? event->modifiers | 1u << bit : event->modifiers & ~(1u << bit));
        return 0;
    }
    case F_DATA_TRANSFER:
        if (JS_IsNull(value)) return 0;
        JS_ThrowTypeError(ctx, "An InputEvent's dataTransfer is a DataTransfer, which this browser does not have");
        return -1;
    case F_TARGET_RANGES: {
        JSValue *items;
        size_t count;
        if (d_sequence(dom, ctx, value, D_SEQUENCE_MAX, &items, &count) < 0) return -1;
        d_sequence_free(ctx, items, count);
        if (count == 0) return 0;
        JS_ThrowTypeError(ctx, "An InputEvent's targetRanges are StaticRanges, which this browser does not have");
        return -1;
    }
    case F_VIEW: string = ui_view(dom, ctx, value); slot = U_VIEW; break;
    case F_RELATED: string = ui_target(dom, ctx, value); slot = U_RELATED; break;
    case F_SUBMITTER: string = ui_submitter(dom, ctx, value); slot = U_RELATED; break;
    case F_INPUT_DATA: string = JS_IsNull(value) ? JS_NULL : JS_ToString(ctx, value); break;
    case F_CODE: case F_INPUT_TYPE: string = JS_ToString(ctx, value); slot = U_TEXT2; break;
    default: string = JS_ToString(ctx, value); break;     /* F_KEY, F_DATA */
    }
    if (JS_IsException(string)) return -1;
    ui_set(ctx, event, slot, string);
    return 0;
}

/* `init`'s members in `members`, each read and converted before the next
 * is read; one undefined keeps its default. */
static int ui_read(os64_dom_t *dom, JSContext *ctx, DEvent *event, JSValueConst init, const DUIMember *members,
                   size_t count)
{
    for (size_t i = 0; i < count; i++) {
        JSValue value = JS_GetPropertyStr(ctx, init, members[i].name);
        if (JS_IsException(value)) return -1;
        int result = JS_IsUndefined(value) ? 0 : ui_field(dom, ctx, event, members[i].field, members[i].bit, value);
        JS_FreeValue(ctx, value);
        if (result < 0) return -1;
    }
    return 0;
}

/* A UI event's init dictionary past EventInit, as WebIDL reads an inherited
 * one: UIEventInit's members, then EventModifierInit's for a mouse or key
 * event, then its own. */
static int ui_read_init(os64_dom_t *dom, JSContext *ctx, DEvent *event, JSValueConst init, int kind)
{
    if (kind != D_UI_SUBMIT && ui_read(dom, ctx, event, init, kUIInit, sizeof(kUIInit) / sizeof(kUIInit[0])) < 0)
        return -1;
    for (size_t i = 0; (kind == D_UI_MOUSE || kind == D_UI_KEYBOARD) && i < sizeof(kModifiers) / sizeof(kModifiers[0]);
         i++) {
        const DUIMember modifier = {kModifiers[i].init, F_MODIFIER, (uint8_t)i};
        if (ui_read(dom, ctx, event, init, &modifier, 1) < 0) return -1;
    }
    switch (kind) {
    case D_UI_MOUSE: return ui_read(dom, ctx, event, init, kMouseInit, sizeof(kMouseInit) / sizeof(kMouseInit[0]));
    case D_UI_KEYBOARD:
        return ui_read(dom, ctx, event, init, kKeyboardInit, sizeof(kKeyboardInit) / sizeof(kKeyboardInit[0]));
    case D_UI_FOCUS: return ui_read(dom, ctx, event, init, kFocusInit, 1);
    case D_UI_INPUT: return ui_read(dom, ctx, event, init, kInputInit, sizeof(kInputInit) / sizeof(kInputInit[0]));
    case D_UI_COMPOSITION: return ui_read(dom, ctx, event, init, kCompositionInit, 1);
    case D_UI_SUBMIT: return ui_read(dom, ctx, event, init, kSubmitInit, 1);
    default: return 0;
    }
}

/* new UIEvent(type, init) and the rest: the Event made (EventInit's
 * members read), then the record's defaults, then the rest of the
 * dictionary. pageX and pageY are clientX and clientY: CSSOM View adds the
 * view's scroll offset outside a dispatch, which this binding leaves out
 * (DEBTS.md). */
static JSValue ui_construct(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv, int kind)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue object = constructed_event(dom, ctx, new_target, argc, argv, dom->ui_prototypes[kind]->value);
    if (JS_IsException(object)) return object;
    DEvent *event = JS_GetOpaque(object, dom->event_class);
    int result = ui_defaults(dom, ctx, event, kind);
    if (result == 0 && argc > 1 && JS_IsObject(argv[1])) result = ui_read_init(dom, ctx, event, argv[1], kind);
    if (result < 0) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    event->page_x = event->client_x;
    event->page_y = event->client_y;
    return object;
}

static JSValue ui_event_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    return ui_construct(ctx, new_target, argc, argv, D_UI_EVENT);
}
static JSValue mouse_event_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    return ui_construct(ctx, new_target, argc, argv, D_UI_MOUSE);
}
static JSValue keyboard_event_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    return ui_construct(ctx, new_target, argc, argv, D_UI_KEYBOARD);
}
static JSValue focus_event_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    return ui_construct(ctx, new_target, argc, argv, D_UI_FOCUS);
}
static JSValue input_event_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    return ui_construct(ctx, new_target, argc, argv, D_UI_INPUT);
}
static JSValue composition_event_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    return ui_construct(ctx, new_target, argc, argv, D_UI_COMPOSITION);
}
static JSValue submit_event_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    return ui_construct(ctx, new_target, argc, argv, D_UI_SUBMIT);
}

/* One of an init method's arguments, converted into `from`, moved onto
 * `to`. */
static void ui_take(JSContext *ctx, DEvent *to, DEvent *from, int field, int bit)
{
    int slot = -1;
    switch (field) {
    case F_VIEW: slot = U_VIEW; break;
    case F_RELATED: slot = U_RELATED; break;
    case F_KEY: case F_DATA: slot = U_TEXT; break;
    case F_DETAIL: to->ui_detail = from->ui_detail; break;
    case F_SCREEN_X_LONG: to->screen_x = from->screen_x; break;
    case F_SCREEN_Y_LONG: to->screen_y = from->screen_y; break;
    case F_CLIENT_X_LONG: to->client_x = from->client_x; break;
    case F_CLIENT_Y_LONG: to->client_y = from->client_y; break;
    case F_BUTTON: to->button = from->button; break;
    case F_LOCATION: to->location = from->location; break;
    case F_MODIFIER:
        to->modifiers = (uint16_t)((to->modifiers & ~(1u << bit)) | (from->modifiers & 1u << bit));
        break;
    default: break;
    }
    if (slot < 0) return;
    ui_set(ctx, to, slot, from->detail[slot]);
    from->detail[slot] = JS_UNDEFINED;
}

/* initUIEvent, initMouseEvent, initKeyboardEvent, initCompositionEvent
 * (UI Events § 8): every argument converted, in order, an argument left
 * out taking its default (a view and relatedTarget null, a string "",
 * zero, false), before the event changes, so one that throws leaves it as
 * it was; during a dispatch it then does nothing. Each sets what its
 * arguments name and nothing else, initMouseEvent's pageX and pageY being
 * its clientX and clientY. */
static JSValue ui_init(os64_dom_t *dom, JSContext *ctx, DEvent *event, int argc, JSValueConst *argv, int member)
{
    const DUIMember *args = member == UM_INIT_MOUSE ? kMouseArgs
                          : member == UM_INIT_KEYBOARD ? kKeyboardArgs
                          : member == UM_INIT_COMPOSITION ? kCompositionArgs : kUIArgs;
    size_t count = member == UM_INIT_MOUSE ? sizeof(kMouseArgs) / sizeof(kMouseArgs[0])
                 : member == UM_INIT_KEYBOARD ? sizeof(kKeyboardArgs) / sizeof(kKeyboardArgs[0])
                 : member == UM_INIT_COMPOSITION ? sizeof(kCompositionArgs) / sizeof(kCompositionArgs[0])
                 : sizeof(kUIArgs) / sizeof(kUIArgs[0]);
    const char *who = member == UM_INIT_MOUSE ? "initMouseEvent" : member == UM_INIT_KEYBOARD ? "initKeyboardEvent"
                    : member == UM_INIT_COMPOSITION ? "initCompositionEvent" : "initUIEvent";
    DInit init;
    if (event_init_read(ctx, argc, argv, who, &init) < 0) return JS_EXCEPTION;
    DEvent next;
    os64_memset(&next, 0, sizeof(next));
    for (int i = 0; i < D_EVENT_DETAILS; i++) next.detail[i] = JS_NULL;
    next.detail[U_TEXT] = JS_NewString(ctx, "");
    bool failed = JS_IsException(next.detail[U_TEXT]);
    for (size_t i = 0; !failed && i < count; i++) {
        int at = 3 + (int)i;
        if (at < argc && !JS_IsUndefined(argv[at]))
            failed = ui_field(dom, ctx, &next, args[i].field, args[i].bit, argv[at]) < 0;
    }
    if (!failed && !event->dispatching) {
        event_init_set(ctx, event, &init);
        for (size_t i = 0; i < count; i++) ui_take(ctx, event, &next, args[i].field, args[i].bit);
        if (member == UM_INIT_MOUSE) {
            event->page_x = event->client_x;
            event->page_y = event->client_y;
        }
    }
    js_free(ctx, init.type);
    for (int i = 0; i < D_EVENT_DETAILS; i++) JS_FreeValue(ctx, next.detail[i]);
    return failed ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue ui_modifier_state(JSContext *ctx, const DEvent *event, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "getModifierState needs a key");
    size_t length;
    const char *key = JS_ToCStringLen(ctx, &length, argv[0]);
    if (key == NULL) return JS_EXCEPTION;
    bool on = false;
    for (size_t i = 0; i < sizeof(kModifiers) / sizeof(kModifiers[0]); i++)
        if (os64_strlen(kModifiers[i].key) == length && os64_strcmp(key, kModifiers[i].key) == 0)
            on = (event->modifiers & 1u << i) != 0;
    JS_FreeCString(ctx, key);
    return JS_NewBool(ctx, on);
}

static JSValue ui_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    int kind = magic >> 8, member = magic & 0xff;
    DEvent *event = JS_GetOpaque(self, dom->event_class);
    if (event == NULL || !ui_implements(event, kind)) return JS_ThrowTypeError(ctx, "Expected a %s", kUINames[kind]);
    switch (member) {
    case UM_VIEW: return JS_DupValue(ctx, event->detail[U_VIEW]);
    case UM_DETAIL: return JS_NewInt32(ctx, event->ui_detail);
    case UM_WHICH: return JS_NewUint32(ctx, event->which);
    case UM_SCREEN_X: return JS_NewFloat64(ctx, event->screen_x);
    case UM_SCREEN_Y: return JS_NewFloat64(ctx, event->screen_y);
    case UM_CLIENT_X: return JS_NewFloat64(ctx, event->client_x);
    case UM_CLIENT_Y: return JS_NewFloat64(ctx, event->client_y);
    case UM_PAGE_X: return JS_NewFloat64(ctx, event->page_x);
    case UM_PAGE_Y: return JS_NewFloat64(ctx, event->page_y);
    case UM_BUTTON: return JS_NewInt32(ctx, event->button);
    case UM_BUTTONS: return JS_NewInt32(ctx, event->buttons);
    case UM_RELATED: return JS_DupValue(ctx, event->detail[U_RELATED]);
    case UM_ALT: return JS_NewBool(ctx, (event->modifiers & 1u << MOD_ALT) != 0);
    case UM_CTRL: return JS_NewBool(ctx, (event->modifiers & 1u << MOD_CTRL) != 0);
    case UM_META: return JS_NewBool(ctx, (event->modifiers & 1u << MOD_META) != 0);
    case UM_SHIFT: return JS_NewBool(ctx, (event->modifiers & 1u << MOD_SHIFT) != 0);
    case UM_MODIFIER_STATE: return ui_modifier_state(ctx, event, argc, argv);
    case UM_KEY: case UM_DATA: return JS_DupValue(ctx, event->detail[U_TEXT]);
    case UM_CODE: case UM_INPUT_TYPE: return JS_DupValue(ctx, event->detail[U_TEXT2]);
    case UM_LOCATION: return JS_NewUint32(ctx, event->location);
    case UM_REPEAT: return JS_NewBool(ctx, event->repeat);
    case UM_COMPOSING: return JS_NewBool(ctx, event->composing);
    case UM_CHAR_CODE: return JS_NewUint32(ctx, event->char_code);
    case UM_KEY_CODE: return JS_NewUint32(ctx, event->key_code);
    case UM_DATA_TRANSFER: return JS_NULL;
    case UM_TARGET_RANGES: return JS_NewArray(ctx);
    case UM_INIT_UI: case UM_INIT_MOUSE: case UM_INIT_KEYBOARD: case UM_INIT_COMPOSITION:
        return ui_init(dom, ctx, event, argc, argv, member);
    default: return JS_ThrowTypeError(ctx, "Unknown UI event member");
    }
}

static int ui_kind_of(os64_dom_event_kind_t kind)
{
    switch (kind) {
    case OS64_DOM_EVENT_MOUSE: return D_UI_MOUSE;
    case OS64_DOM_EVENT_KEY: return D_UI_KEYBOARD;
    case OS64_DOM_EVENT_FOCUS: return D_UI_FOCUS;
    case OS64_DOM_EVENT_INPUT: return D_UI_INPUT;
    case OS64_DOM_EVENT_SUBMIT: return D_UI_SUBMIT;
    default: return D_UI_NONE;
    }
}

/* A host event as the UI event its kind names, trusted: its view the
 * window (a SubmitEvent has none), its other node its relatedTarget or
 * submitter. A mouse event's `which` is its button's, one up, and a key
 * event's its keyCode, as Blink answers (UI Events § 8.3 leaves both to
 * the browser). An InputEvent's data is null and its inputType "": the
 * host does not say what the edit was (DEBTS.md), and a KeyboardEvent's
 * code is "" for the same reason. */
static JSValue ui_host_event(os64_dom_t *dom, JSContext *ctx, const os64_dom_event_t *ev, int kind)
{
    JSValue object = d_event_new_proto(dom, ctx, dom->ui_prototypes[kind]->value, ev->type, ev->bubbles,
                                       ev->cancelable, true);
    if (JS_IsException(object)) return object;
    DEvent *event = JS_GetOpaque(object, dom->event_class);
    JSValue related = JS_NULL, key = JS_UNDEFINED;
    bool failed = ui_defaults(dom, ctx, event, kind) < 0;
    if (!failed && ev->related != NULL) related = d_wrap(dom, ctx, ev->related);
    if (!failed && kind == D_UI_KEYBOARD) key = JS_NewString(ctx, ev->key != NULL ? ev->key : "");
    if (failed || JS_IsException(related) || JS_IsException(key)) {
        JS_FreeValue(ctx, related);
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    if (kind != D_UI_SUBMIT) ui_set(ctx, event, U_VIEW, JS_DupValue(ctx, dom->window->value));
    ui_set(ctx, event, U_RELATED, related);
    event->ui_detail = ev->detail;
    if (kind == D_UI_MOUSE || kind == D_UI_KEYBOARD)
        event->modifiers = (uint16_t)((ev->alt ? 1u << MOD_ALT : 0) | (ev->ctrl ? 1u << MOD_CTRL : 0) |
                                      (ev->meta ? 1u << MOD_META : 0) | (ev->shift ? 1u << MOD_SHIFT : 0));
    if (kind == D_UI_MOUSE) {
        event->screen_x = ev->screen_x;
        event->screen_y = ev->screen_y;
        event->client_x = ev->client_x;
        event->client_y = ev->client_y;
        event->page_x = ev->page_coordinates ? ev->page_x : ev->client_x;
        event->page_y = ev->page_coordinates ? ev->page_y : ev->client_y;
        event->button = (int16_t)ev->button;
        event->buttons = ev->buttons;
        event->which = (uint32_t)(ev->button + 1);
    }
    if (kind == D_UI_KEYBOARD) {
        ui_set(ctx, event, U_TEXT, key);
        event->key_code = ev->key_code;
        event->char_code = ev->char_code;
        event->which = ev->key_code;
    }
    return object;
}

JSValue d_click_event_new(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
    if (dom->ui_prototypes[D_UI_MOUSE] == NULL) return d_event_new(dom, ctx, "click", true, true, false);
    JSValue object = d_event_new_proto(dom, ctx, dom->ui_prototypes[D_UI_MOUSE]->value, "click", true, true, false);
    if (JS_IsException(object)) return object;
    DEvent *event = JS_GetOpaque(object, dom->event_class);
    if (ui_defaults(dom, ctx, event, D_UI_MOUSE) < 0) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    if (d_window_document(dom, node)) ui_set(ctx, event, U_VIEW, JS_DupValue(ctx, dom->window->value));
    return object;
}

JSValue d_submit_event_new(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *submitter)
{
    if (dom->ui_prototypes[D_UI_SUBMIT] == NULL) return d_event_new(dom, ctx, "submit", true, true, false);
    JSValue object = d_event_new_proto(dom, ctx, dom->ui_prototypes[D_UI_SUBMIT]->value, "submit", true, true,
                                       false);
    if (JS_IsException(object)) return object;
    DEvent *event = JS_GetOpaque(object, dom->event_class);
    JSValue by = submitter != NULL ? d_wrap(dom, ctx, submitter) : JS_NULL;
    if (JS_IsException(by) || ui_defaults(dom, ctx, event, D_UI_SUBMIT) < 0) {
        JS_FreeValue(ctx, by);
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    ui_set(ctx, event, U_RELATED, by);
    return object;
}

int d_ui_events_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const struct { uint8_t kind, member; const char *name; int8_t argc; } kMembers[] = {
        /* argc -1: an attribute. */
        {D_UI_EVENT, UM_VIEW, "view", -1}, {D_UI_EVENT, UM_DETAIL, "detail", -1},
        {D_UI_EVENT, UM_WHICH, "which", -1}, {D_UI_EVENT, UM_INIT_UI, "initUIEvent", 1},
        {D_UI_MOUSE, UM_SCREEN_X, "screenX", -1}, {D_UI_MOUSE, UM_SCREEN_Y, "screenY", -1},
        {D_UI_MOUSE, UM_CLIENT_X, "clientX", -1}, {D_UI_MOUSE, UM_CLIENT_Y, "clientY", -1},
        {D_UI_MOUSE, UM_CLIENT_X, "x", -1}, {D_UI_MOUSE, UM_CLIENT_Y, "y", -1},
        {D_UI_MOUSE, UM_PAGE_X, "pageX", -1}, {D_UI_MOUSE, UM_PAGE_Y, "pageY", -1},
        {D_UI_MOUSE, UM_CTRL, "ctrlKey", -1}, {D_UI_MOUSE, UM_SHIFT, "shiftKey", -1},
        {D_UI_MOUSE, UM_ALT, "altKey", -1}, {D_UI_MOUSE, UM_META, "metaKey", -1},
        {D_UI_MOUSE, UM_BUTTON, "button", -1}, {D_UI_MOUSE, UM_BUTTONS, "buttons", -1},
        {D_UI_MOUSE, UM_RELATED, "relatedTarget", -1}, {D_UI_MOUSE, UM_MODIFIER_STATE, "getModifierState", 1},
        {D_UI_MOUSE, UM_INIT_MOUSE, "initMouseEvent", 1},
        {D_UI_KEYBOARD, UM_KEY, "key", -1}, {D_UI_KEYBOARD, UM_CODE, "code", -1},
        {D_UI_KEYBOARD, UM_LOCATION, "location", -1}, {D_UI_KEYBOARD, UM_CTRL, "ctrlKey", -1},
        {D_UI_KEYBOARD, UM_SHIFT, "shiftKey", -1}, {D_UI_KEYBOARD, UM_ALT, "altKey", -1},
        {D_UI_KEYBOARD, UM_META, "metaKey", -1}, {D_UI_KEYBOARD, UM_REPEAT, "repeat", -1},
        {D_UI_KEYBOARD, UM_COMPOSING, "isComposing", -1}, {D_UI_KEYBOARD, UM_CHAR_CODE, "charCode", -1},
        {D_UI_KEYBOARD, UM_KEY_CODE, "keyCode", -1}, {D_UI_KEYBOARD, UM_MODIFIER_STATE, "getModifierState", 1},
        {D_UI_KEYBOARD, UM_INIT_KEYBOARD, "initKeyboardEvent", 1},
        {D_UI_FOCUS, UM_RELATED, "relatedTarget", -1},
        {D_UI_INPUT, UM_DATA, "data", -1}, {D_UI_INPUT, UM_COMPOSING, "isComposing", -1},
        {D_UI_INPUT, UM_INPUT_TYPE, "inputType", -1}, {D_UI_INPUT, UM_DATA_TRANSFER, "dataTransfer", -1},
        {D_UI_INPUT, UM_TARGET_RANGES, "getTargetRanges", 0},
        {D_UI_COMPOSITION, UM_DATA, "data", -1}, {D_UI_COMPOSITION, UM_INIT_COMPOSITION, "initCompositionEvent", 1},
        {D_UI_SUBMIT, UM_RELATED, "submitter", -1},
    };
    static JSCFunction *const kConstructors[D_UI_KINDS] = {
        NULL, ui_event_constructor, mouse_event_constructor, keyboard_event_constructor, focus_event_constructor,
        input_event_constructor, composition_event_constructor, submit_event_constructor
    };
    JSValue event_prototype = JS_GetClassProto(ctx, dom->event_class);
    for (int kind = D_UI_EVENT; kind < D_UI_KINDS; kind++) {
        JSValueConst parent = kind == D_UI_EVENT || kind == D_UI_SUBMIT ? (JSValueConst)event_prototype
                                                                          : dom->ui_prototypes[D_UI_EVENT]->value;
        dom->ui_prototypes[kind] = d_retain(dom, ctx, JS_NewObjectProto(ctx, parent));
        if (dom->ui_prototypes[kind] == NULL) {
            JS_FreeValue(ctx, event_prototype);
            return -1;
        }
    }
    JS_FreeValue(ctx, event_prototype);
    for (size_t i = 0; i < sizeof(kMembers) / sizeof(kMembers[0]); i++) {
        JSValueConst prototype = dom->ui_prototypes[kMembers[i].kind]->value;
        int magic = kMembers[i].kind << 8 | kMembers[i].member;
        int result = kMembers[i].argc < 0
            ? d_accessor(dom, ctx, prototype, kMembers[i].name, ui_member, magic, false)
            : d_method_named(dom, ctx, prototype, kMembers[i].name, ui_member, kMembers[i].argc, magic);
        if (result < 0) return -1;
    }
    /* interfaces[D_UI_NONE] is Event's, which UIEvent and SubmitEvent
     * inherit. */
    JSValue interfaces[D_UI_KINDS];
    for (int kind = 0; kind < D_UI_KINDS; kind++) interfaces[kind] = JS_UNDEFINED;
    interfaces[D_UI_NONE] = JS_GetPropertyStr(ctx, global, "Event");
    int result = JS_IsException(interfaces[D_UI_NONE]) ? -1 : 0;
    for (int kind = D_UI_EVENT; result == 0 && kind < D_UI_KINDS; kind++) {
        JSValueConst parent = kind == D_UI_EVENT || kind == D_UI_SUBMIT ? interfaces[D_UI_NONE] : interfaces[D_UI_EVENT];
        interfaces[kind] = d_interface_new(ctx, kUINames[kind], 1, kConstructors[kind],
                                           dom->ui_prototypes[kind]->value, parent);
        if (JS_IsException(interfaces[kind]) ||
            JS_DefinePropertyValueStr(ctx, global, kUINames[kind], JS_DupValue(ctx, interfaces[kind]),
                                      JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
            result = -1;
    }
    /* KeyboardEvent's key locations, on its interface and its prototype. */
    static const char *const kLocations[] = {
        "DOM_KEY_LOCATION_STANDARD", "DOM_KEY_LOCATION_LEFT", "DOM_KEY_LOCATION_RIGHT", "DOM_KEY_LOCATION_NUMPAD"
    };
    JSValueConst holders[] = {interfaces[D_UI_KEYBOARD], dom->ui_prototypes[D_UI_KEYBOARD]->value};
    for (int i = 0; result == 0 && i < 4; i++)
        for (int h = 0; result == 0 && h < 2; h++)
            if (JS_DefinePropertyValueStr(ctx, holders[h], kLocations[i], JS_NewInt32(ctx, i), JS_PROP_ENUMERABLE) < 0)
                result = -1;
    for (int kind = 0; kind < D_UI_KINDS; kind++) JS_FreeValue(ctx, interfaces[kind]);
    return result;
}

/* ── EventTarget and Window, their names (0f's P1b) ─────────────────────
 * The levels are d_event_install's, there with the Modern Web surface on
 * or off; this row names them. `new EventTarget()` (and a subclass's
 * `super()`) makes a plain target whose entry is in the object, as an
 * AbortSignal's is; Window is an interface a page cannot construct. And an
 * interface object follows its prototype (WebIDL § 3.7.1): every global
 * interface whose prototype inherits EventTarget.prototype directly comes
 * to inherit EventTarget, Node's and the plain targets' alike, so the row
 * runs last, when they are all there. */

static JSClassID target_slot;

typedef struct {
    DValue target;              /* first: the plain target's entry, in the object */
} DTarget;

static void target_finalizer(JSRuntime *rt, JSValue value)
{
    DTarget *t = JS_GetOpaque(value, os64_js_class_id(&target_slot));
    if (t == NULL) return;
    d_event_target_finalize(rt, &t->target);
    js_free_rt(rt, t);
}

static void target_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DTarget *t = JS_GetOpaque(value, os64_js_class_id(&target_slot));
    if (t != NULL) d_event_target_mark(rt, &t->target, mark);
}

static JSValue target_construct(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSValue object = JS_NewObjectProtoClass(ctx, JS_IsObject(prototype) ? (JSValueConst)prototype
                                                                       : dom->event_target_prototype->value,
                                            os64_js_class_id(&target_slot));
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object)) return object;
    DTarget *t = js_mallocz(ctx, sizeof(*t));
    if (t == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    d_event_target_init(dom, &t->target, object);
    JS_SetOpaque(object, t);
    return object;
}

/* Each global interface whose `prototype` inherits EventTarget.prototype
 * directly, and which itself still inherits Function.prototype, comes to
 * inherit `parent` (EventTarget). Only data properties are read: a getter
 * on the global is a page's to run, not the binding's. */
static int adopt_interfaces(os64_dom_t *dom, JSContext *ctx, JSValueConst global, JSValueConst parent)
{
    JSValue function = JS_GetPrototype(ctx, parent);   /* Function.prototype */
    if (JS_IsException(function)) return -1;
    JSPropertyEnum *names = NULL;
    uint32_t count = 0;
    int result = JS_GetOwnPropertyNames(ctx, &names, &count, global, JS_GPN_STRING_MASK) < 0 ? -1 : 0;
    for (uint32_t i = 0; result == 0 && i < count; i++) {
        JSPropertyDescriptor desc;
        int found = JS_GetOwnProperty(ctx, &desc, global, names[i].atom);
        if (found < 0) { result = -1; break; }
        if (found == 0) continue;
        JSValue value = desc.value;
        JS_FreeValue(ctx, desc.getter);
        JS_FreeValue(ctx, desc.setter);
        if ((desc.flags & JS_PROP_GETSET) == 0 && JS_IsFunction(ctx, value) &&
            JS_VALUE_GET_PTR(value) != JS_VALUE_GET_PTR(parent)) {
            JSValue own = JS_GetPrototype(ctx, value);
            JSValue prototype = JS_IsException(own) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, value, "prototype");
            JSValue above = JS_IsObject(prototype) ? JS_GetPrototype(ctx, prototype) : JS_UNDEFINED;
            if (JS_IsException(own) || JS_IsException(prototype) || JS_IsException(above))
                result = -1;
            else if (JS_IsObject(above) && JS_VALUE_GET_PTR(above) == JS_VALUE_GET_PTR(dom->event_target_prototype->value) &&
                     JS_VALUE_GET_PTR(own) == JS_VALUE_GET_PTR(function) && JS_SetPrototype(ctx, value, parent) < 0)
                result = -1;
            JS_FreeValue(ctx, own);
            JS_FreeValue(ctx, prototype);
            JS_FreeValue(ctx, above);
        }
        JS_FreeValue(ctx, value);
    }
    if (names != NULL) JS_FreePropertyEnum(ctx, names, count);
    JS_FreeValue(ctx, function);
    return result;
}

/* The engine gives its global object an own `[Symbol.toStringTag]` of
 * "global", which would hide Window.prototype's; a window has none of its
 * own, so `String(window)` is "[object Window]". */
static int untag_global(JSContext *ctx, JSValueConst global)
{
    JSValue symbol = JS_GetPropertyStr(ctx, global, "Symbol");
    JSValue tag = JS_IsException(symbol) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, symbol, "toStringTag");
    JS_FreeValue(ctx, symbol);
    if (!JS_IsSymbol(tag)) {
        JS_FreeValue(ctx, tag);
        return -1;
    }
    JSAtom key = JS_ValueToAtom(ctx, tag);
    JS_FreeValue(ctx, tag);
    if (key == JS_ATOM_NULL) return -1;
    int deleted = JS_DeleteProperty(ctx, global, key, 0);
    JS_FreeAtom(ctx, key);
    return deleted < 0 ? -1 : 0;
}

int d_targets_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef definition = {.class_name = "EventTarget", .finalizer = target_finalizer,
                                          .gc_mark = target_mark};
    JSClassID id = os64_js_class_id(&target_slot);
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, &definition) < 0) return -1;
    JSValueConst target = dom->event_target_prototype->value;
    JS_SetClassProto(ctx, id, JS_DupValue(ctx, target));
    if (d_event_target(dom, ctx, target, id, NULL) < 0) return -1;
    JSValue event_target = d_interface_new(ctx, "EventTarget", 0, target_construct, target, JS_UNDEFINED);
    if (JS_IsException(event_target)) return -1;
    JSValue window = d_interface_new(ctx, "Window", 0, NULL, dom->window_prototype->value, event_target);
    if (JS_IsException(window) ||
        JS_DefinePropertyValueStr(ctx, global, "Window", window, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(ctx, event_target);
        return -1;
    }
    if (JS_DefinePropertyValueStr(ctx, global, "EventTarget", JS_DupValue(ctx, event_target),
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(ctx, event_target);
        return -1;
    }
    int adopted = adopt_interfaces(dom, ctx, global, event_target);
    JS_FreeValue(ctx, event_target);
    return adopted < 0 ? -1 : untag_global(ctx, global);
}
