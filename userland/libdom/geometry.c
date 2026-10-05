#include "internal.h"

enum {
    G_OFFSET_LEFT, G_OFFSET_TOP, G_OFFSET_WIDTH, G_OFFSET_HEIGHT, G_OFFSET_PARENT,
    G_CLIENT_LEFT, G_CLIENT_TOP, G_CLIENT_WIDTH, G_CLIENT_HEIGHT, G_RECT
};

void os64_dom_set_geometry(os64_dom_t *dom, os64_dom_geometry_provider_t provider, void *opaque)
{
    if (dom == NULL || dom->closed) return;
    dom->geometry = provider;
    dom->geometry_opaque = opaque;
}

os64_dom_geometry_stats_t os64_dom_geometry_stats(os64_dom_t *dom, bool reset)
{
    os64_dom_geometry_stats_t result = {0};
    if (dom != NULL) {
        result = dom->geometry_stats;
        if (reset) dom->geometry_stats = (os64_dom_geometry_stats_t){0};
    }
    return result;
}

static uint64_t add_count(uint64_t a, uint64_t b)
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

static bool budget(os64_dom_t *dom, JSContext *ctx)
{
    os64_js_outcome_t outcome;
    if (os64_js_check_budget(dom->runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_OK)
        return true;
    d_error(ctx, "InvalidStateError", "Geometry requires a live script budget");
    return false;
}

static JSValue geometry(JSContext *ctx, JSValueConst self, int argc,
                         JSValueConst *argv, int magic, JSValue *data)
{
    (void)argc;
    (void)argv;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *node = d_node(dom, ctx, self);
    if (node == NULL) return JS_EXCEPTION;
    if (node->kind != OS64_HTML_ELEMENT || node->ns != OS64_HTML_NS_HTML)
        return JS_ThrowTypeError(ctx, "Geometry requires an HTML element");
    if (dom->geometry == NULL)
        return d_error(ctx, "InvalidStateError", "No layout provider is installed");
    if (!budget(dom, ctx)) return JS_EXCEPTION;
    os64_dom_geometry_t snapshot = {0};
    bool ready = dom->geometry(dom->geometry_opaque, node, &snapshot);
    dom->geometry_stats.layouts = add_count(dom->geometry_stats.layouts, snapshot.layouts);
    dom->geometry_stats.elapsed_us = add_count(dom->geometry_stats.elapsed_us, snapshot.elapsed_us);
    if (!budget(dom, ctx)) return JS_EXCEPTION;
    if (!ready)
        return d_error(ctx, "InvalidStateError", "A current layout could not be produced");
    switch (magic) {
    case G_OFFSET_LEFT: return JS_NewInt32(ctx, snapshot.offset_left);
    case G_OFFSET_TOP: return JS_NewInt32(ctx, snapshot.offset_top);
    case G_OFFSET_WIDTH: return JS_NewInt32(ctx, snapshot.offset_width);
    case G_OFFSET_HEIGHT: return JS_NewInt32(ctx, snapshot.offset_height);
    case G_OFFSET_PARENT: return d_wrap(dom, ctx, snapshot.offset_parent);
    case G_CLIENT_LEFT: return JS_NewInt32(ctx, snapshot.client_left);
    case G_CLIENT_TOP: return JS_NewInt32(ctx, snapshot.client_top);
    case G_CLIENT_WIDTH: return JS_NewInt32(ctx, snapshot.client_width);
    case G_CLIENT_HEIGHT: return JS_NewInt32(ctx, snapshot.client_height);
    default: break;
    }
    JSValue rect = JS_NewObject(ctx);
    if (JS_IsException(rect)) return rect;
    const char *const names[] = {"x", "y", "width", "height", "left", "top", "right", "bottom"};
    const double values[] = {snapshot.x, snapshot.y, snapshot.width, snapshot.height,
                            snapshot.x, snapshot.y, snapshot.x + snapshot.width,
                            snapshot.y + snapshot.height};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        // Define own properties: Object.prototype setters cannot intercept a
        // native geometry result or execute script while it is assembled.
        if (JS_DefinePropertyValueStr(ctx, rect, names[i], JS_NewFloat64(ctx, values[i]),
                                     JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, rect);
            return JS_EXCEPTION;
        }
    }
    return rect;
}

int d_geometry_install(os64_dom_t *dom, JSContext *ctx)
{
    JSValueConst prototype = dom->prototypes[D_PROTO_ELEMENT]->value;
    static const char *const names[] = {
        "offsetLeft", "offsetTop", "offsetWidth", "offsetHeight", "offsetParent",
        "clientLeft", "clientTop", "clientWidth", "clientHeight"
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (d_accessor(dom, ctx, prototype, names[i], geometry, (int)i, false) < 0)
            return -1;
    return d_method(dom, ctx, prototype, "getBoundingClientRect", geometry, 0, G_RECT);
}
