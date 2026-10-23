#include "internal.h"
#include "way/origin.h"

/* THE WINDOW AS A BROWSING CONTEXT (HTML's Window object, and CSSOM View's
 * members of it): P1-W's members of window (WINDOW_DOCUMENT.md § P1-W),
 * rows of the Modern Web table. Every yonder page is the one document of a
 * top-level window that the person opened, never a frame and never a
 * window a script opened, and each answer below is what HTML gives such a
 * window:
 *
 *   - opener reads null (no script opened it); assigning null keeps it
 *     null, and assigning anything else makes it window's own data
 *     property, as HTML's setter defines one;
 *   - closed is false, and close() does nothing: HTML lets a script close
 *     a top-level window whose session history holds one entry, and this
 *     browser lets no page close the person's window (DIVERGENCES.md);
 *   - moveTo, moveBy, resizeTo and resizeBy do nothing, as CSSOM View
 *     makes them for any window a script did not open; their arguments
 *     are still converted, as WebIDL converts them before any step;
 *   - frameElement is null (no frame holds it), crossOriginIsolated is
 *     false (yonder sends no COOP or COEP isolation), originAgentCluster
 *     is true (each page's runtime is its own, so its agent cluster is
 *     keyed by no more than its origin);
 *   - origin is the page's origin serialized, d_origin's, which storage
 *     keys on, and isSecureContext whether the page's address is
 *     potentially trustworthy, libway's one judgement of it
 *     (way_url_trustworthy): the page's own address stands for HTML's
 *     top-level creation URL, the two being one for a page that is never
 *     a frame. A binding with no address is about:blank, which that
 *     judgement does not count;
 *   - captureEvents() and releaseEvents() do nothing, as HTML defines them;
 *   - clientInformation is the navigator object again;
 *   - the six BarProps (locationbar, menubar, personalbar, scrollbars,
 *     statusbar, toolbar) are six objects whose `visible` is true: the
 *     window is not a popup;
 *   - external's AddSearchProvider() and IsSearchProviderInstalled() do
 *     nothing, as HTML's obsolete section defines them;
 *   - ononline, onoffline and onlanguagechange are window's handler slots
 *     (and the body's content attributes for them are window's). Their
 *     events are owed when navigator.onLine or the person's languages
 *     change, and yonder has neither to change, so nothing fires them;
 *     a handler that exists for an event never owed is no promise broken
 *     (WINDOW_DOCUMENT.md § The handler hazard);
 *   - queueMicrotask(callback) queues the callback on the engine's job
 *     queue, the one promise reactions are on, so the two run in the
 *     order they were queued, and whatever drains the queue calls it with
 *     `this` undefined. A callback that throws is a job that throws:
 *     reported as the failure of the task that drained it, never thrown
 *     into the script that queued it, and the jobs after it still run (a
 *     checkpoint carries on past it, and a host drains on past a script
 *     whose jobs threw, as yonder's scripts.c does).
 *
 * [Replaceable] members (clientInformation, origin, the BarProps,
 * external) take an assignment as window's own data property. */

static JSClassID bar_slot, external_slot;
static int bar_mark, external_mark;

enum {
    B_OPENER, B_CLOSED, B_CLOSE, B_CLIENT_INFORMATION, B_MOVE_TO, B_MOVE_BY, B_RESIZE_TO, B_RESIZE_BY,
    B_CAPTURE_EVENTS, B_RELEASE_EVENTS, B_QUEUE_MICROTASK, B_SECURE, B_FRAME_ELEMENT, B_ORIGIN, B_ISOLATED,
    B_AGENT_CLUSTER, B_EXTERNAL, B_SEARCH_PROVIDER, B_VISIBLE, B_BAR
};

static const char *const kBars[6] = {"locationbar", "menubar", "personalbar", "scrollbars", "statusbar",
                                     "toolbar"};

/* An assignment to a [Replaceable] member or to opener: window's own data
 * property from now on, which shadows the accessor for good. */
static JSValue own_property(JSContext *ctx, const char *name, int argc, JSValueConst *argv)
{
    JSValue global = JS_GetGlobalObject(ctx);
    int done = JS_DefinePropertyValueStr(ctx, global, name, JS_DupValue(ctx, argc > 0 ? argv[0] : JS_UNDEFINED),
                                         JS_PROP_C_W_E);
    JS_FreeValue(ctx, global);
    return done < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* The job queueMicrotask queues: its callback, called with no arguments
 * and `this` undefined. What it throws is the job's failure, which the
 * draining checkpoint reports. */
static JSValue microtask(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)argc;
    return JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
}

static const char *page_address(const os64_dom_t *dom)
{
    return dom->url != NULL ? dom->url : "about:blank";
}

static JSValue browsing_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                               JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    switch (magic) {
    case B_OPENER:
        if (!set) return JS_NULL;
        return argc > 0 && JS_IsNull(argv[0]) ? JS_UNDEFINED : own_property(ctx, "opener", argc, argv);
    case B_CLOSED: return JS_FALSE;
    case B_CLOSE: case B_CAPTURE_EVENTS: case B_RELEASE_EVENTS: return JS_UNDEFINED;
    case B_CLIENT_INFORMATION:
        return set ? own_property(ctx, "clientInformation", argc, argv) : JS_DupValue(ctx, dom->navigator->value);
    case B_MOVE_TO: case B_MOVE_BY: case B_RESIZE_TO: case B_RESIZE_BY: {
        if (argc < 2) return JS_ThrowTypeError(ctx, "This needs two numbers");
        int32_t x, y;
        if (JS_ToInt32(ctx, &x, argv[0]) < 0 || JS_ToInt32(ctx, &y, argv[1]) < 0) return JS_EXCEPTION;
        return JS_UNDEFINED;
    }
    case B_QUEUE_MICROTASK:
        if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
            return JS_ThrowTypeError(ctx, "queueMicrotask needs a function");
        return JS_EnqueueJob(ctx, microtask, 1, argv) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    case B_SECURE: return JS_NewBool(ctx, way_url_trustworthy(page_address(dom)));
    case B_FRAME_ELEMENT: return JS_NULL;
    case B_ORIGIN: {
        if (set) return own_property(ctx, "origin", argc, argv);
        char origin[WAY_ORIGIN_TEXT_MAX];
        return JS_NewString(ctx, d_origin(page_address(dom), origin, sizeof(origin)) ? origin : "null");
    }
    case B_ISOLATED: return JS_FALSE;
    case B_AGENT_CLUSTER: return JS_TRUE;
    case B_EXTERNAL:
        return set ? own_property(ctx, "external", argc, argv) : JS_DupValue(ctx, dom->external->value);
    case B_SEARCH_PROVIDER:
        if (JS_GetOpaque(self, os64_js_class_id(&external_slot)) == NULL)
            return JS_ThrowTypeError(ctx, "Expected an External");
        return JS_UNDEFINED;
    case B_VISIBLE:
        if (JS_GetOpaque(self, os64_js_class_id(&bar_slot)) == NULL)
            return JS_ThrowTypeError(ctx, "Expected a BarProp");
        return JS_TRUE;
    default: {
        int bar = magic - B_BAR;
        if (bar < 0 || bar >= 6) return JS_ThrowTypeError(ctx, "Unknown window member");
        return set ? own_property(ctx, kBars[bar], argc, argv) : JS_DupValue(ctx, dom->bars[bar]->value);
    }
    }
}

int d_browsing_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const struct { const char *name; int magic; bool writable; } kAttributes[] = {
        {"opener", B_OPENER, true}, {"closed", B_CLOSED, false},
        {"clientInformation", B_CLIENT_INFORMATION, true}, {"isSecureContext", B_SECURE, false},
        {"frameElement", B_FRAME_ELEMENT, false}, {"origin", B_ORIGIN, true},
        {"crossOriginIsolated", B_ISOLATED, false}, {"originAgentCluster", B_AGENT_CLUSTER, false},
    };
    static const struct { const char *name; int magic, length; } kMethods[] = {
        {"close", B_CLOSE, 0}, {"moveTo", B_MOVE_TO, 2}, {"moveBy", B_MOVE_BY, 2},
        {"resizeTo", B_RESIZE_TO, 2}, {"resizeBy", B_RESIZE_BY, 2}, {"captureEvents", B_CAPTURE_EVENTS, 0},
        {"releaseEvents", B_RELEASE_EVENTS, 0}, {"queueMicrotask", B_QUEUE_MICROTASK, 1},
    };
    for (size_t i = 0; i < sizeof(kAttributes) / sizeof(kAttributes[0]); i++)
        if (d_accessor(dom, ctx, global, kAttributes[i].name, browsing_member, kAttributes[i].magic,
                       kAttributes[i].writable) < 0)
            return -1;
    for (size_t i = 0; i < sizeof(kMethods) / sizeof(kMethods[0]); i++)
        if (d_method_named(dom, ctx, global, kMethods[i].name, browsing_member, kMethods[i].length,
                           kMethods[i].magic) < 0)
            return -1;
    return d_event_handler(dom, ctx, global, D_EVENT_ONLINE) < 0 ||
           d_event_handler(dom, ctx, global, D_EVENT_OFFLINE) < 0 ||
           d_event_handler(dom, ctx, global, D_EVENT_LANGUAGECHANGE) < 0 ? -1 : 0;
}

int d_bars_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef bar_class = {.class_name = "BarProp"};
    JSClassID id = os64_js_class_id(&bar_slot);
    JSValue prototype;
    if (d_interface_class(dom, ctx, global, id, &bar_class, &prototype) < 0 ||
        d_accessor(dom, ctx, prototype, "visible", browsing_member, B_VISIBLE, false) < 0)
        return -1;
    for (int i = 0; i < 6; i++) {
        JSValue bar = JS_NewObjectClass(ctx, (int)id);
        dom->bars[i] = d_retain(dom, ctx, bar);
        if (dom->bars[i] == NULL) return -1;
        JS_SetOpaque(bar, &bar_mark);
        if (d_accessor(dom, ctx, global, kBars[i], browsing_member, B_BAR + i, true) < 0) return -1;
    }
    return 0;
}

int d_external_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef external_class = {.class_name = "External"};
    JSClassID id = os64_js_class_id(&external_slot);
    JSValue prototype;
    if (d_interface_class(dom, ctx, global, id, &external_class, &prototype) < 0 ||
        d_method_named(dom, ctx, prototype, "AddSearchProvider", browsing_member, 0, B_SEARCH_PROVIDER) < 0 ||
        d_method_named(dom, ctx, prototype, "IsSearchProviderInstalled", browsing_member, 0, B_SEARCH_PROVIDER) < 0)
        return -1;
    JSValue external = JS_NewObjectClass(ctx, (int)id);
    dom->external = d_retain(dom, ctx, external);
    if (dom->external == NULL) return -1;
    JS_SetOpaque(external, &external_mark);
    return d_accessor(dom, ctx, global, "external", browsing_member, B_EXTERNAL, true);
}
