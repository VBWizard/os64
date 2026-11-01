#include "internal.h"

static void hold(os64_dom_t *dom, const os64_html_node_t **slot, const os64_html_node_t *node)
{
    if (*slot == node) return;
    if (node) os64_html_hold(dom->document, node);
    if (*slot) os64_html_release(dom->document, *slot);
    *slot = node;
}
static bool connected(os64_dom_t *dom, const os64_html_node_t *node)
{
    return node && d_window_document(dom, node) && d_connected(node);
}
static void changed(os64_dom_t *dom, const os64_html_change_t *record)
{
    DPointer *p = &dom->pointer;
    // A removed previous target falls back along its old event ancestry.
    // CHILDREN supplies the old parent while the detached subtree is alive.
    if (record->kind == OS64_HTML_CHANGE_CHILDREN && record->nremoved) {
        const os64_html_node_t *parent = record->target;
        while (parent && !connected(dom, parent)) parent = parent->parent;
        if (p->effective && !connected(dom, p->effective)) {
            hold(dom, &p->effective, parent); p->over_owed = true;
        }
        if (p->legacy && !connected(dom, p->legacy)) {
            hold(dom, &p->legacy, parent); p->mouse_over_owed = true;
        }
    }
    if (p->pending && !connected(dom, p->pending)) hold(dom, &p->pending, NULL);
    if (p->capture && !connected(dom, p->capture)) hold(dom, &p->capture, dom->document->document);
    for (int i = 0; i < 3; i++)
        if (p->down[i] && !connected(dom, p->down[i])) hold(dom, &p->down[i], NULL);
}
static JSValue capture_method(JSContext *ctx, JSValueConst self, int argc,
                               JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (!dom) return JS_EXCEPTION;
    const os64_html_node_t *node = d_node(dom, ctx, self);
    if (!node) return JS_EXCEPTION;
    if (node->kind != OS64_HTML_ELEMENT) return JS_ThrowTypeError(ctx, "Expected an Element");
    if (argc < 1) return JS_ThrowTypeError(ctx, "Pointer capture needs an ID");
    int32_t id;
    if (JS_ToInt32(ctx, &id, argv[0]) < 0) return JS_EXCEPTION;
    DPointer *p = &dom->pointer;
    if (magic == 2) return JS_NewBool(ctx, id == 1 && p->available && p->pending == node);
    if (id != 1 || !p->available) return d_error(ctx, "NotFoundError", "Unknown active pointer");
    if (magic == 0) {
        if (!connected(dom, node)) return d_error(ctx, "InvalidStateError", "Capture needs a connected element");
        if (p->buttons && !p->draining && !p->canceling) hold(dom, &p->pending, node);
    } else if (p->pending == node) hold(dom, &p->pending, NULL);
    return JS_UNDEFINED;
}
void d_pointer_drain(os64_dom_t *dom)
{
    DPointer *p = &dom->pointer;
    hold(dom, &p->physical, NULL); hold(dom, &p->effective, NULL); hold(dom, &p->legacy, NULL);
    hold(dom, &p->pending, NULL); hold(dom, &p->capture, NULL);
    for (int i = 0; i < 3; i++) hold(dom, &p->down[i], NULL);
    p->buttons = 0; p->available = p->cancel_owed = p->suppress = false;
    p->over_owed = p->mouse_over_owed = false;
}
void os64_dom_pointer_cancel(os64_dom_t *dom)
{
    if (dom && !dom->closed && (dom->pointer.buttons || dom->pointer.capture || dom->pointer.pending))
        dom->pointer.cancel_owed = true;
}
bool os64_dom_pointer_active(const os64_dom_t *dom)
{
    return dom && !dom->closed && (dom->pointer.buttons || dom->pointer.draining);
}
typedef struct {
    os64_dom_t *dom;
    JSContext *ctx;
    os64_js_outcome_t *out;
    os64_dom_event_t event;
    os64_dom_pointer_default_t action;
    void *opaque;
    bool failed;
} Packet;
static bool emit(Packet *q, const char *type, const os64_html_node_t *node,
                  const os64_html_node_t *related, bool pointer, bool bubbles, bool cancelable,
                  bool native_default, bool *prevented)
{
    os64_dom_t *dom = q->dom;
    if (q->failed || dom->closed || dom->navigation.kind != OS64_DOM_NAVIGATE_NONE) return false;
    os64_dom_event_t e = q->event;
    e.type = type; e.kind = pointer ? OS64_DOM_EVENT_POINTER : OS64_DOM_EVENT_MOUSE;
    e.bubbles = bubbles; e.cancelable = cancelable; e.related = related;
    e.pointer_id = 1; e.pointer_type = "mouse"; e.is_primary = true;
    e.pressure = os64_streq(type, "pointercancel") ? q->dom->pointer.last.pressure : e.buttons ? 0.5 : 0;
    e.detail = !pointer && (os64_streq(type, "mousedown") || os64_streq(type, "mouseup")) ? 1 : 0;
    if (!pointer && os64_streq(type, "mousemove")) e.button = 0;
    if (os64_streq(type, "click") || os64_streq(type, "auxclick")) {
        e.is_primary = false; e.pressure = 0; e.detail = 1;
    }
    if (node) os64_html_hold(dom->document, node);
    if (related) os64_html_hold(dom->document, related);
    bool canceled = false;
    int result = d_event_in_task(dom, q->ctx, node, &e, &canceled, q->out);
    if (result < 0) {
        JSValue error = JS_GetException(q->ctx);
        if (d_exception_catchable(q->ctx)) d_report(dom, q->ctx, error);
        JS_FreeValue(q->ctx, error);
        q->failed = true;
    }
    d_task_settle(dom, q->out);
    if (q->out->status != OS64_JS_OK && q->out->status != OS64_JS_EXCEPTION &&
        q->out->status != OS64_JS_UNHANDLED_REJECTION) q->failed = true;
    if (!q->failed && !dom->closed && dom->navigation.kind == OS64_DOM_NAVIGATE_NONE &&
        native_default && q->action && !q->action(q->opaque, node, &e, canceled)) q->failed = true;
    if (related) os64_html_release(dom->document, related);
    if (node) os64_html_release(dom->document, node);
    if (prevented) *prevented = canceled;
    return !q->failed && !dom->closed && dom->navigation.kind == OS64_DOM_NAVIGATE_NONE &&
           (!dom->pointer.cancel_owed || dom->pointer.canceling);
}
/* Snapshot ancestor paths before boundary listeners can detach or reparent
 * them. These holds are independent of capture and event-object lifetimes. */
typedef struct { const os64_html_node_t **nodes; size_t count; } Path;
static bool path_make(os64_dom_t *dom, const os64_html_node_t *node, Path *path)
{
    size_t n = 0;
    for (const os64_html_node_t *at = node; at; at = at->parent) n++;
    path->count = 0; path->nodes = n ? d_alloc(dom, n * sizeof(*path->nodes)) : NULL;
    if (n && !path->nodes) return false;
    for (; node; node = node->parent) {
        os64_html_hold(dom->document, node);
        path->nodes[path->count++] = node;
    }
    return true;
}
static void path_drop(os64_dom_t *dom, Path *p)
{
    for (size_t i = 0; i < p->count; i++) os64_html_release(dom->document, p->nodes[i]);
    d_free(dom, p->nodes);
}
static bool boundary(Packet *q, const os64_html_node_t **slot, const os64_html_node_t *target, bool pointer)
{
    bool *owed = pointer ? &q->dom->pointer.over_owed : &q->dom->pointer.mouse_over_owed;
    bool again = *owed; *owed = false;
    if (*slot == target) return !again || !target ||
        emit(q, pointer ? "pointerover" : "mouseover", target, NULL, pointer, true, true, false, NULL);
    const os64_html_node_t *old = *slot;
    Path a = {0}, b = {0};
    bool ok = path_make(q->dom, old, &a) && path_make(q->dom, target, &b);
    if (!ok) {
        q->failed = true;
        q->out->status = OS64_JS_LIMIT;
        q->out->limit = OS64_JS_LIMIT_MEMORY;
        os64_strcopy(q->out->message, sizeof(q->out->message), "No room for pointer boundary paths");
    }
    size_t na = a.count, nb = b.count;
    while (na && nb && a.nodes[na - 1] == b.nodes[nb - 1]) { na--; nb--; }
    if (ok && old) ok = emit(q, pointer ? "pointerout" : "mouseout", old, target, pointer, true, true, false, NULL);
    for (size_t i = 0; ok && i < na; i++)
        if (a.nodes[i]->kind == OS64_HTML_ELEMENT)
            ok = emit(q, pointer ? "pointerleave" : "mouseleave", a.nodes[i], target, pointer, false, false, false, NULL);
    if (ok && target) ok = emit(q, pointer ? "pointerover" : "mouseover", target, old, pointer, true, true, false, NULL);
    for (size_t i = nb; ok && i > 0; i--)
        if (b.nodes[i - 1]->kind == OS64_HTML_ELEMENT)
            ok = emit(q, pointer ? "pointerenter" : "mouseenter", b.nodes[i - 1], old, pointer, false, false, false, NULL);
    const os64_html_node_t *kept = target;
    if (kept && !connected(q->dom, kept)) {
        kept = NULL;
        for (size_t i = 1; i < b.count; i++)
            if (connected(q->dom, b.nodes[i])) { kept = b.nodes[i]; break; }
        *owed = true;
    }
    hold(q->dom, slot, kept);
    path_drop(q->dom, &a); path_drop(q->dom, &b);
    return ok;
}
static bool capture_pending(Packet *q)
{
    DPointer *p = &q->dom->pointer;
    if (p->capture == p->pending) return true;
    if (p->capture && !emit(q, "lostpointercapture", p->capture, NULL, true, true, false, false, NULL)) return false;
    if (p->pending && !emit(q, "gotpointercapture", p->pending, NULL, true, true, false, false, NULL)) return false;
    hold(q->dom, &p->capture, p->pending);
    return true;
}
static void cancel(Packet *q)
{
    DPointer *p = &q->dom->pointer;
    p->canceling = true; p->cancel_owed = false; p->draining = true;
    q->event = p->last;
    q->event.buttons = 0;
    const os64_html_node_t *target = p->capture ? p->capture : p->effective;
    emit(q, "pointercancel", target, NULL, true, true, false, false, NULL);
    if (!p->suppress) emit(q, "mouseup", NULL, NULL, false, true, true, false, NULL);
    hold(q->dom, &p->pending, NULL);
    capture_pending(q);
    boundary(q, &p->effective, NULL, true);
    boundary(q, &p->legacy, NULL, false);
    d_pointer_drain(q->dom);
    p->canceling = false;
}
static const os64_html_node_t *common(const os64_html_node_t *a, const os64_html_node_t *b)
{
    for (; a; a = a->parent)
        for (const os64_html_node_t *n = b; n; n = n->parent)
            if (a == n) return a;
    return NULL;
}
os64_js_status_t os64_dom_pointer_dispatch(os64_dom_t *dom, const os64_dom_pointer_sample_t *s,
                                            os64_dom_pointer_default_t action, void *opaque,
                                            bool *ran, os64_js_outcome_t *out)
{
    if (ran) *ran = false;
    if (!out) return OS64_JS_BAD_ARGUMENT;
    os64_memset(out, 0, sizeof(*out));
    if (!dom || !s || (s->hit && !os64_html_owns_node(dom->document, s->hit)))
        return out->status = OS64_JS_BAD_ARGUMENT;
    DPointer *p = &dom->pointer;
    if (dom->closed || p->running) return out->status = OS64_JS_FAILED_RUNTIME;
    if (!dom->options.modern) return OS64_JS_OK;
    bool hover = s->action == OS64_DOM_POINTER_MOVE || s->action == OS64_DOM_POINTER_BOUNDARY;
    if (hover && !p->buttons && !s->event.buttons && !p->capture && !p->pending &&
        !p->draining && !p->cancel_owed) {
        bool heard = false;
        const char *types[] = {"pointermove", "pointerover", "pointerout", "pointerenter", "pointerleave",
                               "mousemove", "mouseover", "mouseout", "mouseenter", "mouseleave"};
        for (size_t i = 0; i < sizeof(types)/sizeof(types[0]) && !heard; i++) heard = os64_dom_listens(dom, types[i]);
        if (!heard) {
            const os64_html_node_t *hit = connected(dom, s->hit) ? s->hit : NULL;
            p->available = true;
            hold(dom, &p->physical, hit); hold(dom, &p->effective, hit); hold(dom, &p->legacy, hit);
            p->last = s->event;
            p->last.type = NULL; p->last.key = NULL; p->last.pointer_type = NULL; p->last.related = NULL;
            if (action && s->action == OS64_DOM_POINTER_MOVE) {
                os64_dom_event_t event = s->event;
                event.type = "mousemove"; event.kind = OS64_DOM_EVENT_MOUSE; event.button = 0;
                if (!action(opaque, hit, &event, false)) { d_pointer_drain(dom); p->draining = true; }
            }
            return OS64_JS_OK;
        }
    }
    if (ran) *ran = true;
    if (os64_js_task_begin(dom->runtime, OS64_JS_ABI_ID, "pointer input", out) != OS64_JS_OK) {
        d_pointer_drain(dom); p->draining = true; return out->status;
    }
    p->running = true;
    os64_js_outcome_t access;
    Packet q = {.dom = dom, .ctx = os64_js_context(dom->runtime, OS64_JS_ABI_ID, &access),
                .out = out, .event = s->event, .action = action, .opaque = opaque};
    const os64_html_node_t *target = NULL;
    bool captured = false;
    if (!q.ctx) { d_outcome_fold(out, &access); q.failed = true; goto done; }
    if (p->cancel_owed || s->action == OS64_DOM_POINTER_CANCEL) {
        if (p->available && (p->buttons || p->capture || p->pending)) cancel(&q);
        if (s->action == OS64_DOM_POINTER_CANCEL) goto done;
        q.event = s->event;
    }
    if (p->draining) {
        if (s->action != OS64_DOM_POINTER_BOUNDARY && s->event.buttons == 0) p->draining = false;
        goto done;
    }
    p->available = true;
    uint16_t previous = p->buttons;
    p->buttons = s->event.buttons;
    p->last = s->event;
    p->last.pressure = s->event.buttons ? 0.5 : 0;
    /* last carries value fields; borrowed caller strings/nodes must not persist. */
    p->last.type = NULL; p->last.related = NULL; p->last.key = NULL; p->last.pointer_type = NULL;
    hold(dom, &p->physical, connected(dom, s->hit) ? s->hit : NULL);
    if (!capture_pending(&q)) goto done;
    target = p->capture ? p->capture : p->physical;
    captured = p->capture != NULL;
    if (target) os64_html_hold(dom->document, target);
    if (!boundary(&q, &p->effective, target, true) || !boundary(&q, &p->legacy, target, false)) goto done;
    if (s->action == OS64_DOM_POINTER_BOUNDARY) goto done;
    int button = s->event.button;
    bool down = s->action == OS64_DOM_POINTER_DOWN, up = s->action == OS64_DOM_POINTER_UP;
    if (down && button >= 0 && button < 3) hold(dom, &p->down[button], target);
    const char *type = down && !previous ? "pointerdown" : up && !p->buttons ? "pointerup" : "pointermove";
    bool prevented = false;
    if (emit(&q, type, target, NULL, true, true, true, false, &prevented)) {
        if (down && !previous && prevented) p->suppress = true;
        if (!p->suppress) emit(&q, down ? "mousedown" : up ? "mouseup" : "mousemove",
                              target, NULL, false, true, true, true, NULL);
        if (up && !p->buttons) {
            hold(dom, &p->pending, NULL);
            capture_pending(&q);
        }
        if (up && button >= 0 && button < 3 && p->down[button] && connected(dom, target)) {
            /* Capture was in force for this release, even after implicit release. */
            const os64_html_node_t *activate = captured ? target : common(p->down[button], target);
            if (activate && connected(dom, activate))
                emit(&q, button == 0 ? "click" : "auxclick", activate, NULL, true, true, true, true, NULL);
            hold(dom, &p->down[button], NULL);
        }
        if (up && !p->buttons) {
            p->suppress = false;
            boundary(&q, &p->effective, p->physical, true);
            boundary(&q, &p->legacy, p->physical, false);
        }
    }
done:
    if (p->cancel_owed && !q.failed && !dom->closed && dom->navigation.kind == OS64_DOM_NAVIGATE_NONE) cancel(&q);
    if (target) os64_html_release(dom->document, target);
    if (q.failed || dom->closed || dom->navigation.kind != OS64_DOM_NAVIGATE_NONE) {
        bool drain = p->buttons != 0 || p->draining || s->event.buttons != 0;
        d_pointer_drain(dom); p->draining = drain;
    }
    d_task_finish(dom, out);
    p->running = false;
    return out->status;
}
int d_pointer_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    const char *names[] = {"setPointerCapture", "releasePointerCapture", "hasPointerCapture"};
    for (int i = 0; i < 3; i++)
        if (d_method(dom, ctx, dom->prototypes[D_PROTO_ELEMENT]->value, names[i], capture_method, 1, i) < 0) return -1;
    JSValue holders[] = {global, dom->prototypes[D_PROTO_ELEMENT]->value, dom->prototypes[D_PROTO_DOCUMENT]->value};
    for (int kind = D_EVENT_POINTERDOWN; kind < D_EVENT_COUNT; kind++)
        for (size_t i = 0; i < sizeof(holders) / sizeof(holders[0]); i++)
            if (d_event_handler(dom, ctx, holders[i], kind) < 0) return -1;
    d_mutation_register(dom, &dom->pointer.mutations, changed);
    return 0;
}
