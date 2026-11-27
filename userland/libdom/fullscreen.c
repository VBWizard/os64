/* fullscreen.c — the Fullscreen API (WHATWG Fullscreen; VIDEO.md § 9, the
 * design state's Part B), libdom's half: dom/fullscreen.h is the host's.
 *
 * ONE FULLSCREEN ELEMENT A DOCUMENT, and a top layer of that one: the
 * standard keeps a stack (a second request pushes, an exit pops back to
 * the one before); here a request for another element replaces the first,
 * and an exit leaves full screen entirely. The element carries libhtml's
 * OS64_HTML_STATE_FULLSCREEN, which `:fullscreen` matches and libflow's
 * user-agent rules read; the host paints and hit-tests the top layer.
 *
 * THE STATES, from the fields: NONE (no element, nothing pending), ENTERING
 * (`pending`: asked of the host, its answer not yet in), FULL (`element`),
 * LEAVING (`element` and `leaving`: the host asked to bring the window
 * back). A request while ENTERING or LEAVING is refused (the standard lets
 * them race; a departure), and so is an exit unless the state is FULL.
 * The drain ends in NONE, the element's bit cleared, since the document
 * may outlive the binding.
 *
 * EVERY EVENT AND EVERY SETTLED PROMISE IS A TASK, queued here and fired
 * one a call from the view step (HTML's "update the rendering", after its
 * resize and scroll steps), never from inside a member or a host call: the
 * host's answers arrive on the window thread between tasks, and script
 * must not run inside them. fullscreenchange and fullscreenerror bubble
 * and are composed; they go to the element, or to the document when the
 * element is no longer in it. The prefixed webkitfullscreenchange and
 * webkitfullscreenerror follow each in the same task, when anything
 * listens for them.
 *
 * Removal: the element leaving its document asks the host to bring the
 * window back (the standard's removing steps fully exit), seen by the
 * mutation consumer and acted on at the next view step, since a consumer
 * may not call out with the tree mid-change. */

#include "internal.h"
#include "os64/fmt.h"
#include <dom/fullscreen.h>

typedef struct {
    JSValue promise, resolve, reject;   /* promise is JS_UNDEFINED when none */
} DFsPromise;

typedef struct DFsTask {
    struct DFsTask *next;
    const os64_html_node_t *node;       /* the event's target, or NULL: the document */
    JSValue wrapper;                    /* holds `node` until the task has run */
    bool event;                         /* an event (else the promise alone) */
    bool error;                         /* fullscreenerror, else fullscreenchange */
    DFsPromise promise;
    const char *refusal;                /* NULL: resolve; else reject, a TypeError with this message */
} DFsTask;

struct DFullscreen {
    os64_dom_fullscreen_provider_t provider;
    bool has_provider;
    const os64_html_node_t *element, *pending;
    JSValue element_wrapper, pending_wrapper;
    bool leaving;
    bool check;                         /* a removal: is the element still in its document? */
    DFsPromise enter, leave;
    DFsTask *head, *tail;
    DMutationConsumer mutations;
};

enum {
    F_REQUEST, F_REQUEST_PREFIXED, F_EXIT, F_EXIT_PREFIXED,
    F_ELEMENT, F_ENABLED, F_FULLSCREEN,
};

static const DFsPromise kNoPromise = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};

static void promise_release(os64_dom_t *dom, DFsPromise *p)
{
    JS_FreeValueRT(dom->engine, p->promise);
    JS_FreeValueRT(dom->engine, p->resolve);
    JS_FreeValueRT(dom->engine, p->reject);
    *p = kNoPromise;
}

static void queue(os64_dom_t *dom, const os64_html_node_t *node, JSValue wrapper, bool event, bool error,
                  DFsPromise promise, const char *refusal)
{
    struct DFullscreen *f = dom->fullscreen;
    DFsTask *t = d_alloc(dom, sizeof(*t));
    if (t == NULL) {
        /* Out of the binding's budget: the promise is let go unsettled
         * rather than leaked, as media's queue does. */
        JS_FreeValueRT(dom->engine, wrapper);
        promise_release(dom, &promise);
        return;
    }
    *t = (DFsTask){NULL, node, wrapper, event, error, promise, refusal};
    if (f->tail != NULL)
        f->tail->next = t;
    else
        f->head = t;
    f->tail = t;
}

static JSValue dup_rt(os64_dom_t *dom, JSValue v)
{
    return JS_DupValueRT(dom->engine, v);
}

static void set_bit(os64_dom_t *dom, const os64_html_node_t *node, bool on)
{
    uint8_t state = on ? (uint8_t)(node->state | OS64_HTML_STATE_FULLSCREEN)
                       : (uint8_t)(node->state & ~OS64_HTML_STATE_FULLSCREEN);
    (void)os64_html_set_state(dom->document, (os64_html_node_t *)node, state);
}

/* An element the standard lets be full screen: in its document, and an
 * HTML element or an outermost <svg> (Fullscreen's "fullscreen element
 * ready check", the parts that apply: no modal dialog here, no frames). */
static bool may_be_fullscreen(const os64_html_node_t *node)
{
    if (node->kind != OS64_HTML_ELEMENT || !d_connected(node))
        return false;
    if (node->ns == OS64_HTML_NS_HTML)
        return true;
    return node->ns == OS64_HTML_NS_SVG && node->name != NULL && os64_strcmp(node->name, "svg") == 0 &&
           (node->parent == NULL || node->parent->ns != OS64_HTML_NS_SVG);
}

/* ── The host's side ─────────────────────────────────────────────────── */

void os64_dom_set_fullscreen(os64_dom_t *dom, const os64_dom_fullscreen_provider_t *provider)
{
    if (dom == NULL || dom->fullscreen == NULL)
        return;
    dom->fullscreen->has_provider = provider != NULL;
    dom->fullscreen->provider = provider != NULL ? *provider : (os64_dom_fullscreen_provider_t){0};
}

const os64_html_node_t *os64_dom_fullscreen_element(const os64_dom_t *dom)
{
    return dom != NULL && dom->fullscreen != NULL ? dom->fullscreen->element : NULL;
}

/* The fullscreen element leaves: its bit cleared, a change event at it (or
 * at the document, once it is out of it), and the exit promise settled
 * with it when one waits. */
static void unfullscreen(os64_dom_t *dom)
{
    struct DFullscreen *f = dom->fullscreen;
    const os64_html_node_t *gone = f->element;
    JSValue wrapper = f->element_wrapper;
    f->element = NULL;
    f->element_wrapper = JS_UNDEFINED;
    set_bit(dom, gone, false);
    queue(dom, gone, wrapper, true, false, f->leave, NULL);
    f->leave = kNoPromise;
}

void os64_dom_fullscreen_settled(os64_dom_t *dom, bool entered)
{
    struct DFullscreen *f = dom != NULL ? dom->fullscreen : NULL;
    if (f == NULL || f->pending == NULL)
        return;
    const os64_html_node_t *node = f->pending;
    JSValue wrapper = f->pending_wrapper;
    f->pending = NULL;
    f->pending_wrapper = JS_UNDEFINED;
    if (!entered || !may_be_fullscreen(node)) {
        /* Refused, or the element left its document while the host was
         * answering: the request fails. A window that did go over the
         * screen is brought back, with nothing to change for the page. */
        queue(dom, node, wrapper, true, true, f->enter, "The window could not go full screen");
        f->enter = kNoPromise;
        if (entered && f->element == NULL && f->has_provider && f->provider.leave != NULL) {
            f->leaving = true;
            f->provider.leave(f->provider.opaque);
        }
        return;
    }
    if (f->element != NULL && f->element != node)
        unfullscreen(dom);      /* replaced: the old element's change comes first */
    else if (f->element == node)
        JS_FreeValueRT(dom->engine, f->element_wrapper);
    f->element = node;
    f->element_wrapper = dup_rt(dom, wrapper);
    set_bit(dom, node, true);
    queue(dom, node, wrapper, true, false, f->enter, NULL);
    f->enter = kNoPromise;
}

void os64_dom_fullscreen_exit(os64_dom_t *dom)
{
    struct DFullscreen *f = dom != NULL ? dom->fullscreen : NULL;
    if (f == NULL)
        return;
    f->leaving = false;
    if (f->pending != NULL) {
        queue(dom, f->pending, f->pending_wrapper, true, true, f->enter,
              "Full screen was left before it was entered");
        f->pending = NULL;
        f->pending_wrapper = JS_UNDEFINED;
        f->enter = kNoPromise;
    }
    if (f->element != NULL) {
        unfullscreen(dom);
    } else if (!JS_IsUndefined(f->leave.promise)) {
        queue(dom, NULL, JS_UNDEFINED, false, false, f->leave, NULL);
        f->leave = kNoPromise;
    }
}

/* The removing steps, at the view step: an element gone from its document
 * takes the window out of full screen (the host's exit then clears it). */
static void check_connected(os64_dom_t *dom)
{
    struct DFullscreen *f = dom->fullscreen;
    if (!f->check)
        return;
    f->check = false;
    if (f->element != NULL && !f->leaving && !d_connected(f->element) && f->has_provider &&
        f->provider.leave != NULL) {
        f->leaving = true;
        f->provider.leave(f->provider.opaque);
    }
}

static void removed(os64_dom_t *dom, const os64_html_change_t *change)
{
    struct DFullscreen *f = dom->fullscreen;
    if (f != NULL && change->kind == OS64_HTML_CHANGE_CHILDREN && change->nremoved > 0 &&
        (f->element != NULL || f->pending != NULL))
        f->check = true;
}

/* ── The tasks ──────────────────────────────────────────────────────── */

static void dispatch(os64_dom_t *dom, JSContext *ctx, const char *type, const os64_html_node_t *node,
                     os64_js_outcome_t *outcome)
{
    JSValue event = d_event_new(dom, ctx, type, true, false, true);
    if (JS_IsException(event)) {
        JSValue error = JS_GetException(ctx);
        JS_FreeValue(ctx, error);
        return;
    }
    d_event_set_composed(dom, event);
    d_dispatch_node_task(dom, ctx, event, node, outcome);
    JS_FreeValue(ctx, event);
}

static void settle(JSContext *ctx, DFsTask *t)
{
    if (JS_IsUndefined(t->promise.promise))
        return;
    JSValue arg = JS_UNDEFINED;
    if (t->refusal != NULL) {
        JSValue thrown = JS_ThrowTypeError(ctx, "%s", t->refusal);
        JS_FreeValue(ctx, thrown);
        arg = JS_GetException(ctx);
    }
    JSValue r = JS_Call(ctx, t->refusal != NULL ? t->promise.reject : t->promise.resolve, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, arg);
}

bool d_fullscreen_step(os64_dom_t *dom, os64_js_outcome_t *outcome)
{
    struct DFullscreen *f = dom->fullscreen;
    if (f == NULL)
        return false;
    check_connected(dom);
    DFsTask *t = f->head;
    if (t == NULL)
        return false;
    f->head = t->next;
    if (f->head == NULL)
        f->tail = NULL;
    const char *type = t->error ? "fullscreenerror" : "fullscreenchange";
    char name[OS64_JS_SOURCE_NAME_CAP];
    os64_snprintf(name, sizeof(name), "%.*s#%s", (int)(sizeof(name) - 40),
                  dom->url != NULL ? dom->url : "about:blank", t->event ? type : "fullscreen-settle");
    if (os64_js_realm_task_begin(dom->realm, OS64_JS_ABI_ID, name, outcome) == OS64_JS_OK) {
        os64_js_outcome_t access;
        JSContext *ctx = os64_js_realm_context(dom->realm, OS64_JS_ABI_ID, &access);
        if (ctx == NULL)
            d_outcome_fold(outcome, &access);
        if (ctx != NULL && t->event) {
            /* At the element while it is in the document, else at it. */
            const os64_html_node_t *at = t->node != NULL && d_connected(t->node) ? t->node
                                                                                  : dom->document->document;
            dispatch(dom, ctx, type, at, outcome);
            const char *prefixed = t->error ? "webkitfullscreenerror" : "webkitfullscreenchange";
            if (d_delivery_active(dom) && os64_dom_listens(dom, prefixed))
                dispatch(dom, ctx, prefixed, at, outcome);
        }
        if (ctx != NULL && d_delivery_active(dom))
            settle(ctx, t);
        d_task_finish(dom, outcome);
    }
    d_task_close(dom, name, outcome);
    promise_release(dom, &t->promise);
    JS_FreeValueRT(dom->engine, t->wrapper);
    d_free(dom, t);
    return true;
}

void d_fullscreen_drain(os64_dom_t *dom)
{
    struct DFullscreen *f = dom->fullscreen;
    if (f == NULL)
        return;
    while (f->head != NULL) {
        DFsTask *t = f->head;
        f->head = t->next;
        promise_release(dom, &t->promise);
        JS_FreeValueRT(dom->engine, t->wrapper);
        d_free(dom, t);
    }
    f->tail = NULL;
    promise_release(dom, &f->enter);
    promise_release(dom, &f->leave);
    /* The document may outlive the binding (a retired host's page stays on
     * screen), so the element stops matching :fullscreen and loses the
     * user-agent rules here, while its wrapper still holds it, and nothing
     * is full screen in this document from now on. The listener is already
     * stopped, so the change reaches no consumer. */
    if (f->element != NULL)
        set_bit(dom, f->element, false);
    f->element = f->pending = NULL;
    JS_FreeValueRT(dom->engine, f->element_wrapper);
    JS_FreeValueRT(dom->engine, f->pending_wrapper);
    f->element_wrapper = f->pending_wrapper = JS_UNDEFINED;
    f->has_provider = f->leaving = f->check = false;
}

/* The record itself goes with the binding's other records: a child whose
 * installation was abandoned is freed without a drain. */
void d_fullscreen_free(os64_dom_t *dom)
{
    if (dom->fullscreen == NULL)
        return;
    d_free(dom, dom->fullscreen);
    dom->fullscreen = NULL;
}

/* ── The members ────────────────────────────────────────────────────── */

static JSValue promise_new(JSContext *ctx, DFsPromise *out)
{
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise))
        return promise;
    *out = (DFsPromise){JS_DupValue(ctx, promise), resolving[0], resolving[1]};
    return promise;
}

/* The answer a member hands back: the promise, or undefined for the
 * prefixed forms, which return nothing (and so cannot leave a rejection
 * unhandled). */
static JSValue answer(JSContext *ctx, bool prefixed, DFsPromise *held)
{
    if (prefixed) {
        *held = kNoPromise;
        return JS_UNDEFINED;
    }
    return promise_new(ctx, held);
}

static JSValue request(os64_dom_t *dom, JSContext *ctx, JSValueConst self, bool prefixed)
{
    const os64_html_node_t *node = d_node(dom, ctx, self);
    if (node == NULL)
        return JS_EXCEPTION;
    if (node->kind != OS64_HTML_ELEMENT)
        return JS_ThrowTypeError(ctx, "Expected an element");
    os64_dom_t *owner = d_owner(dom, node);
    struct DFullscreen *f = owner->fullscreen;
    DFsPromise held;
    JSValue result = answer(ctx, prefixed, &held);
    if (JS_IsException(result))
        return result;
    const char *refusal = NULL;
    if (f == NULL || !f->has_provider)
        refusal = "Fullscreen is not enabled in this document";
    else if (!may_be_fullscreen(node))
        refusal = "The element cannot be made fullscreen";
    else if (f->pending != NULL || f->leaving)
        refusal = "A fullscreen change is already under way";
    else if (f->element == node) {
        queue(owner, node, JS_DupValue(ctx, self), false, false, held, NULL);
        return result;
    } else if (f->provider.consume_activation == NULL || !f->provider.consume_activation(f->provider.opaque))
        refusal = "The request was not made in response to a user's action";
    if (refusal != NULL) {
        if (f == NULL) {
            /* No queue to wait in (the Modern Web is off in that document):
             * refused now. */
            promise_release(dom, &held);
            if (prefixed)
                return JS_UNDEFINED;
            JS_FreeValue(ctx, result);
            JSValue thrown = JS_ThrowTypeError(ctx, "%s", refusal);
            JS_FreeValue(ctx, thrown);
            JSValue reason = JS_GetException(ctx);
            DFsPromise p;
            JSValue rejected = promise_new(ctx, &p);
            if (!JS_IsException(rejected)) {
                JSValue r = JS_Call(ctx, p.reject, JS_UNDEFINED, 1, &reason);
                JS_FreeValue(ctx, r);
                promise_release(dom, &p);
            }
            JS_FreeValue(ctx, reason);
            return rejected;
        }
        queue(owner, node, JS_DupValue(ctx, self), true, true, held, refusal);
        return result;
    }
    f->pending = node;
    f->pending_wrapper = JS_DupValue(ctx, self);
    f->enter = held;
    if (f->provider.enter != NULL)
        f->provider.enter(f->provider.opaque, node);
    return result;
}

static JSValue exit_fullscreen(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *document, bool prefixed)
{
    os64_dom_t *owner = d_owner(dom, document);
    struct DFullscreen *f = owner->fullscreen;
    DFsPromise held;
    JSValue result = answer(ctx, prefixed, &held);
    if (JS_IsException(result))
        return result;
    if (f == NULL || f->element == NULL || f->leaving || f->pending != NULL ||
        os64_html_node_document(f->element) != document) {
        if (f != NULL) {
            queue(owner, NULL, JS_UNDEFINED, false, false, held, "The document is not in fullscreen");
        } else {
            promise_release(dom, &held);
            if (!prefixed) {
                JS_FreeValue(ctx, result);
                return JS_ThrowTypeError(ctx, "The document is not in fullscreen");
            }
        }
        return result;
    }
    f->leaving = true;
    f->leave = held;
    if (f->provider.leave != NULL)
        f->provider.leave(f->provider.opaque);
    return result;
}

static JSValue fs_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    (void)argc;
    (void)argv;   /* requestFullscreen's options (navigationUI, screen) are read by nobody here */
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL)
        return JS_EXCEPTION;
    if (magic == F_REQUEST || magic == F_REQUEST_PREFIXED)
        return request(dom, ctx, self, magic == F_REQUEST_PREFIXED);
    const os64_html_node_t *document = d_node(dom, ctx, self);
    if (document == NULL)
        return JS_EXCEPTION;
    if (document->kind != OS64_HTML_DOCUMENT)
        return JS_ThrowTypeError(ctx, "Expected a document");
    os64_dom_t *owner = d_owner(dom, document);
    struct DFullscreen *f = owner->fullscreen;
    const os64_html_node_t *element =
        f != NULL && f->element != NULL && os64_html_node_document(f->element) == document ? f->element : NULL;
    switch (magic) {
    case F_EXIT:
    case F_EXIT_PREFIXED:
        return exit_fullscreen(dom, ctx, document, magic == F_EXIT_PREFIXED);
    case F_ELEMENT:
        return element != NULL ? d_wrap(owner, ctx, element) : JS_NULL;
    case F_ENABLED:
        return JS_NewBool(ctx, f != NULL && f->has_provider);
    case F_FULLSCREEN:
        return JS_NewBool(ctx, element != NULL);
    default:
        return JS_ThrowTypeError(ctx, "Unknown fullscreen member");
    }
}

int d_fullscreen_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    (void)global;
    struct DFullscreen *f = d_alloc(dom, sizeof(*f));
    if (f == NULL)
        return -1;
    *f = (struct DFullscreen){.element_wrapper = JS_UNDEFINED, .pending_wrapper = JS_UNDEFINED,
                              .enter = kNoPromise, .leave = kNoPromise};
    dom->fullscreen = f;
    JSValueConst element = dom->prototypes[D_PROTO_ELEMENT]->value;
    JSValueConst document = dom->prototypes[D_PROTO_DOCUMENT]->value;
    /* The standard's names, then the prefixed ones Chrome and Safari still
     * answer to (the design state's corpus count: a page in four that asks
     * for full screen asks only by them), the same members underneath. */
    static const struct { const char *name; int magic; } kElement[] = {
        {"requestFullscreen", F_REQUEST},
        {"webkitRequestFullscreen", F_REQUEST_PREFIXED},
        {"webkitRequestFullScreen", F_REQUEST_PREFIXED},
    };
    static const struct { const char *name; int magic; } kDocumentMethods[] = {
        {"exitFullscreen", F_EXIT},
        {"webkitExitFullscreen", F_EXIT_PREFIXED},
        {"webkitCancelFullScreen", F_EXIT_PREFIXED},
    };
    static const struct { const char *name; int magic; } kDocumentReads[] = {
        {"fullscreenElement", F_ELEMENT}, {"fullscreenEnabled", F_ENABLED}, {"fullscreen", F_FULLSCREEN},
        {"webkitFullscreenElement", F_ELEMENT}, {"webkitCurrentFullScreenElement", F_ELEMENT},
        {"webkitFullscreenEnabled", F_ENABLED}, {"webkitIsFullScreen", F_FULLSCREEN},
    };
    for (size_t i = 0; i < sizeof(kElement) / sizeof(kElement[0]); i++)
        if (d_method_named(dom, ctx, element, kElement[i].name, fs_member, 0, kElement[i].magic) < 0)
            return -1;
    for (size_t i = 0; i < sizeof(kDocumentMethods) / sizeof(kDocumentMethods[0]); i++)
        if (d_method_named(dom, ctx, document, kDocumentMethods[i].name, fs_member, 0,
                           kDocumentMethods[i].magic) < 0)
            return -1;
    for (size_t i = 0; i < sizeof(kDocumentReads) / sizeof(kDocumentReads[0]); i++)
        if (d_accessor(dom, ctx, document, kDocumentReads[i].name, fs_member, kDocumentReads[i].magic, false) < 0)
            return -1;
    /* onfullscreenchange and onfullscreenerror: on Element (an HTML
     * element's content attribute too) and on Document, as the standard
     * puts them. */
    JSValueConst holders[] = {element, document};
    for (size_t i = 0; i < sizeof(holders) / sizeof(holders[0]); i++)
        if (d_event_handler(dom, ctx, holders[i], D_EVENT_FULLSCREENCHANGE) < 0 ||
            d_event_handler(dom, ctx, holders[i], D_EVENT_FULLSCREENERROR) < 0)
            return -1;
    d_mutation_register(dom, &f->mutations, removed);
    return 0;
}
