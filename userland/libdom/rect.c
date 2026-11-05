#include "internal.h"
#include <math.h>

/* DOMRECTREADONLY AND DOMRECT (Geometry Interfaces 1 § 4;
 * INTERSECTION_OBSERVER.md): engine-owned values, as points are, and not
 * registry entries, since a page that reads every element's box makes one
 * a read. A rect is its four numbers; the edges are worked from them at
 * each read, with NaN in either number making NaN, as the standard's
 * min and max do. */

static JSClassID rect_slot, readonly_slot;
typedef struct { double v[4]; } DRect;
enum { R_X, R_Y, R_WIDTH, R_HEIGHT, R_TOP, R_RIGHT, R_BOTTOM, R_LEFT, R_JSON };
/* On a member DOMRect's prototype declares itself: its receiver must be a
 * DOMRect, as WebIDL's brand check asks, not any read-only rect. */
#define R_WRITABLE_ONLY 0x100
static const char *const kNames[] = {"x", "y", "width", "height", "top", "right", "bottom", "left"};

static DRect *rect_of(JSValueConst value)
{
    DRect *r = JS_GetOpaque(value, os64_js_class_id(&rect_slot));
    return r != NULL ? r : JS_GetOpaque(value, os64_js_class_id(&readonly_slot));
}

static void rect_finalizer(JSRuntime *rt, JSValue value)
{
    js_free_rt(rt, rect_of(value));
}

static double least(double a, double b)
{
    return isnan(a) || isnan(b) ? NAN : a < b ? a : b;
}

static double most(double a, double b)
{
    return isnan(a) || isnan(b) ? NAN : a > b ? a : b;
}

static double edge(const DRect *r, int which)
{
    switch (which) {
    case R_TOP: return least(r->v[R_Y], r->v[R_Y] + r->v[R_HEIGHT]);
    case R_RIGHT: return most(r->v[R_X], r->v[R_X] + r->v[R_WIDTH]);
    case R_BOTTOM: return most(r->v[R_Y], r->v[R_Y] + r->v[R_HEIGHT]);
    case R_LEFT: return least(r->v[R_X], r->v[R_X] + r->v[R_WIDTH]);
    default: return r->v[which];
    }
}

static JSValue make(JSContext *ctx, bool readonly, JSValueConst proto, const double v[4])
{
    JSClassID id = os64_js_class_id(readonly ? &readonly_slot : &rect_slot);
    JSValue object = JS_IsObject(proto) ? JS_NewObjectProtoClass(ctx, proto, id) : JS_NewObjectClass(ctx, id);
    if (JS_IsException(object)) return object;
    DRect *r = js_malloc(ctx, sizeof(*r));
    if (r == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    for (int i = 0; i < 4; i++) r->v[i] = v[i];
    JS_SetOpaque(object, r);
    return object;
}

JSValue d_rect_new(os64_dom_t *dom, JSContext *ctx, bool readonly, double x, double y, double width,
                   double height)
{
    const double v[4] = {x, y, width, height};
    if (!dom->options.modern) return JS_ThrowTypeError(ctx, "DOMRect is a Modern Web interface");
    return make(ctx, readonly, JS_UNDEFINED, v);
}

/* DOMRectInit, its members read in WebIDL's order (lexicographic). */
static int rect_dictionary(JSContext *ctx, int argc, JSValueConst *argv, double out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    int present = d_dictionary(ctx, argc, argv, 0);
    if (present <= 0) return present;
    static const int kOrder[] = {R_HEIGHT, R_WIDTH, R_X, R_Y};
    for (int j = 0; j < 4; j++) {
        int i = kOrder[j];
        JSValue v = JS_GetPropertyStr(ctx, argv[0], kNames[i]);
        if (JS_IsException(v)) return -1;
        int status = JS_IsUndefined(v) ? 0 : JS_ToFloat64(ctx, &out[i], v);
        JS_FreeValue(ctx, v);
        if (status < 0) return -1;
    }
    return 0;
}

static JSValue construct_rect(JSContext *ctx, JSValueConst target, int argc, JSValueConst *argv, bool readonly)
{
    if (d_binding(ctx) == NULL) return JS_EXCEPTION;
    double v[4] = {0, 0, 0, 0};
    for (int i = 0; i < argc && i < 4; i++)
        if (!JS_IsUndefined(argv[i]) && JS_ToFloat64(ctx, &v[i], argv[i]) < 0) return JS_EXCEPTION;
    JSValue proto = JS_GetPropertyStr(ctx, target, "prototype");
    if (JS_IsException(proto)) return proto;
    JSValue object = make(ctx, readonly, proto, v);
    JS_FreeValue(ctx, proto);
    return object;
}

static JSValue construct(JSContext *c, JSValueConst t, int n, JSValueConst *v)
{
    return construct_rect(c, t, n, v, false);
}

static JSValue construct_readonly(JSContext *c, JSValueConst t, int n, JSValueConst *v)
{
    return construct_rect(c, t, n, v, true);
}

static JSValue from_rect(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    (void)self;
    if (d_callback(ctx, data, OS64_JS_ABI_ID) == NULL) return JS_EXCEPTION;
    double v[4];
    if (rect_dictionary(ctx, argc, argv, v) < 0) return JS_EXCEPTION;
    return make(ctx, magic != 0, JS_UNDEFINED, v);
}

static JSValue member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    if (d_callback(ctx, data, OS64_JS_ABI_ID) == NULL) return JS_EXCEPTION;
    DRect *r = rect_of(self);
    if (r == NULL) return JS_ThrowTypeError(ctx, "Expected a DOMRectReadOnly");
    bool set = (magic & D_SET) != 0;
    bool writable_only = (magic & R_WRITABLE_ONLY) != 0;
    magic &= ~(D_SET | R_WRITABLE_ONLY);
    if ((set || writable_only) && JS_GetOpaque(self, os64_js_class_id(&rect_slot)) == NULL)
        return JS_ThrowTypeError(ctx, "Expected a DOMRect");
    if (set) {
        double v;
        if (JS_ToFloat64(ctx, &v, argc > 0 ? argv[0] : JS_UNDEFINED) < 0) return JS_EXCEPTION;
        r->v[magic] = v;
        return JS_UNDEFINED;
    }
    if (magic != R_JSON) return JS_NewFloat64(ctx, edge(r, magic));
    JSValue out = JS_NewObject(ctx);
    if (JS_IsException(out)) return out;
    for (int i = 0; i < R_JSON; i++)
        if (JS_DefinePropertyValueStr(ctx, out, kNames[i], JS_NewFloat64(ctx, edge(r, i)), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
    return out;
}

/* DOMRECTLIST AND getClientRects (CSSOM View § 6.1): a list made for one
 * read, its rects its own read-only indexed properties and its length
 * kept on its opaque (one more than the count, so an empty list is not a
 * NULL one). An element with a layout box answers its border box, one
 * rect for the whole element (DEBTS.md: a box split across lines answers
 * one rect, where a browser answers one per fragment); an element with
 * none (display: none, detached) answers an empty list, and so does an
 * SVG element, which the layout provider does not measure (DEBTS.md). */
static JSClassID list_slot;
enum { L_LENGTH, L_ITEM };

static JSValue list_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                           JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    uintptr_t stored = (uintptr_t)JS_GetOpaque(self, os64_js_class_id(&list_slot));
    if (stored == 0) return JS_ThrowTypeError(ctx, "Expected a DOMRectList");
    uint32_t length = (uint32_t)(stored - 1);
    if (magic == L_LENGTH) return JS_NewUint32(ctx, length);
    if (argc < 1) return JS_ThrowTypeError(ctx, "item needs an index");
    uint32_t index;
    if (JS_ToUint32(ctx, &index, argv[0]) < 0) return JS_EXCEPTION;
    return index < length ? JS_GetPropertyUint32(ctx, self, index) : JS_NULL;
}

static JSValue client_rects(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                            JSValue *data)
{
    (void)argc; (void)argv; (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *node = d_node(dom, ctx, self);
    if (node == NULL) return JS_EXCEPTION;
    if (node->kind != OS64_HTML_ELEMENT) return JS_ThrowTypeError(ctx, "Geometry requires an element");
    os64_dom_geometry_t box;
    if (!d_geometry_read(dom, ctx, node, &box)) return JS_EXCEPTION;
    JSValue list = JS_NewObjectClass(ctx, (int)os64_js_class_id(&list_slot));
    if (JS_IsException(list)) return list;
    uint32_t length = box.boxed ? 1 : 0;
    if (length != 0) {
        JSValue rect = d_rect_new(dom, ctx, false, box.x, box.y, box.width, box.height);
        if (JS_IsException(rect) || JS_DefinePropertyValueUint32(ctx, list, 0, rect, JS_PROP_ENUMERABLE) < 0) {
            JS_FreeValue(ctx, list);
            return JS_EXCEPTION;
        }
    }
    JS_SetOpaque(list, (void *)(uintptr_t)(length + 1));
    return list;
}

static int list_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSClassID id = os64_js_class_id(&list_slot);
    JSClassDef definition = {.class_name = "DOMRectList"};
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, &definition) < 0) return -1;
    JSValue proto = JS_NewObject(ctx);
    if (d_retain(dom, ctx, proto) == NULL) return -1;
    JS_SetClassProto(ctx, id, JS_DupValue(ctx, proto));
    if (d_accessor(dom, ctx, proto, "length", list_member, L_LENGTH, false) < 0 ||
        d_method(dom, ctx, proto, "item", list_member, 1, L_ITEM) < 0 || d_iterable(ctx, proto, false) < 0)
        return -1;
    JSValue constructor = d_interface_new(ctx, "DOMRectList", 0, NULL, proto, JS_UNDEFINED);
    if (JS_IsException(constructor) ||
        JS_DefinePropertyValueStr(ctx, global, "DOMRectList", constructor, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    return d_method(dom, ctx, dom->prototypes[D_PROTO_ELEMENT]->value, "getClientRects", client_rects, 0, 0);
}

/* The DOMRect row (modern.c): DOMRectReadOnly, DOMRect inheriting it,
 * whose four numbers are writable, then DOMRectList and getClientRects. */
int d_rect_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSValue parent = JS_UNDEFINED, parent_proto = JS_UNDEFINED;
    for (int i = 0; i < 2; i++) {
        bool readonly = i == 0;
        const char *name = readonly ? "DOMRectReadOnly" : "DOMRect";
        JSClassID id = os64_js_class_id(readonly ? &readonly_slot : &rect_slot);
        JSClassDef definition = {.class_name = name, .finalizer = rect_finalizer};
        if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, &definition) < 0) return -1;
        JSValue proto = readonly ? JS_NewObject(ctx) : JS_NewObjectProto(ctx, parent_proto);
        if (d_retain(dom, ctx, proto) == NULL) return -1;
        JS_SetClassProto(ctx, id, JS_DupValue(ctx, proto));
        JSValue constructor = d_interface_new(ctx, name, 0, readonly ? construct_readonly : construct, proto, parent);
        if (JS_IsException(constructor) || d_retain(dom, ctx, constructor) == NULL) return -1;
        if (JS_DefinePropertyValueStr(ctx, global, name, JS_DupValue(ctx, constructor),
                                      JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
            return -1;
        /* DOMRect redeclares the four numbers writable; the edges and
         * toJSON are inherited. */
        int members = readonly ? R_JSON : R_TOP;
        for (int j = 0; j < members; j++)
            if (d_accessor(dom, ctx, proto, kNames[j], member, readonly ? j : j | R_WRITABLE_ONLY,
                           !readonly && j < R_TOP) < 0)
                return -1;
        if (d_method_named(dom, ctx, constructor, "fromRect", from_rect, 0, readonly) < 0) return -1;
        if (readonly && d_method_named(dom, ctx, proto, "toJSON", member, 0, R_JSON) < 0) return -1;
        parent = constructor;
        parent_proto = proto;
    }
    return list_install(dom, ctx, global);
}
