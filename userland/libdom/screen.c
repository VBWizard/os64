#include "internal.h"

/* `screen` and `devicePixelRatio` (CSSOM View § 4, the Screen interface;
 * the Screen Orientation API's ScreenOrientation): the display the host
 * says the page is on, given at creation in device pixels
 * (options.screen), reported in CSS pixels at the page's zoom as it is
 * when asked, which is what a zoomed page in Chrome is told: at 125% a
 * 1920-pixel display is 1536 wide and devicePixelRatio is 1.25. The
 * orientation is the display's shape; it never changes here, so its
 * `change` event never fires, and lock() is refused, as a desktop browser
 * refuses it. screen, screen.orientation and devicePixelRatio are
 * [Replaceable]: a page may assign over them. */

static JSClassID screen_slot, orientation_slot;
static int screen_mark;

static uint32_t zoom_of(const os64_dom_t *dom)
{
    uint32_t zoom = dom->options.zoom != NULL ? dom->options.zoom(dom->options.host_opaque) : 1000;
    return zoom != 0 ? zoom : 1000;
}

/* A device length at the page's zoom, in whole CSS pixels. */
static int32_t css(const os64_dom_t *dom, int32_t device)
{
    return device > 0 ? (int32_t)((int64_t)device * 1000 / zoom_of(dom)) : 0;
}

enum {
    W_SCREEN, W_RATIO, S_WIDTH, S_HEIGHT, S_AVAIL_WIDTH, S_AVAIL_HEIGHT, S_COLOR_DEPTH, S_ORIENTATION,
    O_TYPE, O_ANGLE, O_LOCK, O_UNLOCK
};

/* A promise rejected with a NotSupportedError: lock()'s answer. */
static JSValue refused(JSContext *ctx)
{
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return promise;
    (void)d_error(ctx, "NotSupportedError", "screen.orientation.lock() is not available on this device");
    JSValue error = JS_GetException(ctx);
    JSValue done = JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, (JSValueConst *)&error);
    JS_FreeValue(ctx, error);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    if (JS_IsException(done)) {
        JS_FreeValue(ctx, promise);
        return done;
    }
    JS_FreeValue(ctx, done);
    return promise;
}

static JSValue screen_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                             JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    const os64_dom_screen_t *s = &dom->options.screen;
    if (magic == W_SCREEN || magic == W_RATIO) {
        const char *name = magic == W_SCREEN ? "screen" : "devicePixelRatio";
        /* [Replaceable]: the assignment becomes window's own property. */
        if (set) {
            JSValue global = JS_GetGlobalObject(ctx);
            int done = JS_DefinePropertyValueStr(ctx, global, name, JS_DupValue(ctx, argc > 0 ? argv[0] : JS_UNDEFINED),
                                                 JS_PROP_C_W_E);
            JS_FreeValue(ctx, global);
            return done < 0 ? JS_EXCEPTION : JS_UNDEFINED;
        }
        if (magic == W_RATIO) return JS_NewFloat64(ctx, zoom_of(dom) / 1000.0);
        return JS_DupValue(ctx, dom->screen->value);
    }
    if (magic <= S_ORIENTATION) {
        if (JS_GetOpaque(self, os64_js_class_id(&screen_slot)) == NULL)
            return JS_ThrowTypeError(ctx, "Expected a Screen");
        switch (magic) {
        case S_WIDTH: return JS_NewInt32(ctx, css(dom, s->width));
        case S_HEIGHT: return JS_NewInt32(ctx, css(dom, s->height));
        case S_AVAIL_WIDTH: return JS_NewInt32(ctx, css(dom, s->avail_width));
        case S_AVAIL_HEIGHT: return JS_NewInt32(ctx, css(dom, s->avail_height));
        case S_COLOR_DEPTH: return JS_NewUint32(ctx, s->color_depth);
        default: return JS_DupValue(ctx, dom->orientation->value);
        }
    }
    if (JS_GetOpaque(self, os64_js_class_id(&orientation_slot)) == NULL)
        return JS_ThrowTypeError(ctx, "Expected a ScreenOrientation");
    switch (magic) {
    case O_TYPE: return JS_NewString(ctx, s->width >= s->height ? "landscape-primary" : "portrait-primary");
    case O_ANGLE: return JS_NewInt32(ctx, 0);
    case O_LOCK:
        if (argc < 1) return JS_ThrowTypeError(ctx, "lock needs an orientation");
        return refused(ctx);
    default: return JS_UNDEFINED;
    }
}

/* An interface's prototype, made once, named on window with its interface
 * object; borrowed back from the registry entry that holds it. */
static int prototype_of(os64_dom_t *dom, JSContext *ctx, JSValueConst global, JSClassID id,
                        const JSClassDef *definition, JSValue *out)
{
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, definition) < 0) return -1;
    JSValue prototype = JS_NewObject(ctx);
    DValue *held = d_retain(dom, ctx, prototype);
    if (held == NULL) return -1;
    JS_SetClassProto(ctx, id, JS_DupValue(ctx, prototype));
    JSValue f = d_interface_new(ctx, definition->class_name, 0, NULL, prototype, JS_UNDEFINED);
    if (JS_IsException(f) ||
        JS_DefinePropertyValueStr(ctx, global, definition->class_name, f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    *out = held->value;
    return 0;
}

static void clear(JSRuntime *rt, JSValue value)
{
    (void)rt;
    JS_SetOpaque(value, NULL);
}

int d_screen_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef screen_class = {.class_name = "Screen", .finalizer = clear};
    static const JSClassDef orientation_class = {.class_name = "ScreenOrientation", .finalizer = clear};
    JSClassID screen_id = os64_js_class_id(&screen_slot), orientation_id = os64_js_class_id(&orientation_slot);
    JSValue screen_prototype, orientation_prototype;
    if (prototype_of(dom, ctx, global, screen_id, &screen_class, &screen_prototype) < 0 ||
        prototype_of(dom, ctx, global, orientation_id, &orientation_class, &orientation_prototype) < 0)
        return -1;
    static const struct { const char *name; int magic; } kScreen[] = {
        {"width", S_WIDTH}, {"height", S_HEIGHT}, {"availWidth", S_AVAIL_WIDTH},
        {"availHeight", S_AVAIL_HEIGHT}, {"colorDepth", S_COLOR_DEPTH}, {"pixelDepth", S_COLOR_DEPTH},
        {"orientation", S_ORIENTATION},
    };
    for (size_t i = 0; i < sizeof(kScreen) / sizeof(kScreen[0]); i++)
        if (d_accessor(dom, ctx, screen_prototype, kScreen[i].name, screen_member, kScreen[i].magic, false) < 0)
            return -1;
    if (d_accessor(dom, ctx, orientation_prototype, "type", screen_member, O_TYPE, false) < 0 ||
        d_accessor(dom, ctx, orientation_prototype, "angle", screen_member, O_ANGLE, false) < 0 ||
        d_method(dom, ctx, orientation_prototype, "lock", screen_member, 1, O_LOCK) < 0 ||
        d_method(dom, ctx, orientation_prototype, "unlock", screen_member, 0, O_UNLOCK) < 0 ||
        d_event_target(dom, ctx, orientation_prototype, orientation_id) < 0 ||
        d_event_handler(dom, ctx, orientation_prototype, D_EVENT_CHANGE) < 0)
        return -1;
    JSValue screen = JS_NewObjectClass(ctx, (int)screen_id);
    dom->screen = d_retain(dom, ctx, screen);
    if (dom->screen == NULL) return -1;
    JS_SetOpaque(screen, &screen_mark);
    /* A plain event target's opaque is its registry entry. */
    JSValue orientation = JS_NewObjectClass(ctx, (int)orientation_id);
    dom->orientation = d_retain(dom, ctx, orientation);
    if (dom->orientation == NULL) return -1;
    JS_SetOpaque(orientation, dom->orientation);
    return d_accessor(dom, ctx, global, "screen", screen_member, W_SCREEN, true) < 0 ||
           d_accessor(dom, ctx, global, "devicePixelRatio", screen_member, W_RATIO, true) < 0 ? -1 : 0;
}
