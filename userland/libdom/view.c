#include <float.h>

#include "internal.h"
#include "garb/cascade.h"
#include "garb/values.h"
#include "os64/fmt.h"

/* THE WINDOW'S VIEW (CSSOM View § Extensions to the Window interface;
 * MODERN_WEB.md § Slice 1b): the scroll and size members, the document's
 * scrollingElement, matchMedia and its MediaQueryLists, getComputedStyle,
 * and the events the view owes a page when it moves (os64_dom_view_step).
 * The host answers through two providers, the view's and the computed
 * style's (dom.h); every name here is a row of the Modern Web table
 * (modern.c). */

static JSClassID media_slot, computed_slot;

static bool budget(os64_dom_t *dom, JSContext *ctx)
{
    os64_js_outcome_t outcome;
    if (os64_js_check_budget(dom->runtime, OS64_JS_ABI_ID, &outcome) == OS64_JS_OK)
        return true;
    d_error(ctx, "InvalidStateError", "The view requires a live script budget");
    return false;
}

static uint64_t add_count(uint64_t a, uint64_t b)
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

static void charge(os64_dom_t *dom, uint64_t layouts, uint64_t elapsed_us)
{
    dom->geometry_stats.layouts = add_count(dom->geometry_stats.layouts, layouts);
    dom->geometry_stats.elapsed_us = add_count(dom->geometry_stats.elapsed_us, elapsed_us);
}

// An interface object (d_interface_new) named on window.
static int expose(JSContext *ctx, JSValueConst global, const char *name, int length, JSCFunction *constructor,
                  JSValueConst prototype, JSValueConst parent)
{
    JSValue f = d_interface_new(ctx, name, length, constructor, prototype, parent);
    if (JS_IsException(f)) return -1;
    return JS_DefinePropertyValueStr(ctx, global, name, f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0 ? -1 : 0;
}

// ── The view ────────────────────────────────────────────────────────────

static os64_dom_view_t view_now(const os64_dom_t *dom)
{
    os64_dom_view_t v;
    os64_memset(&v, 0, sizeof(v));
    if (dom->view.read != NULL)
        dom->view.read(dom->view.opaque, &v);
    return v;
}

void os64_dom_set_view(os64_dom_t *dom, const os64_dom_view_provider_t *view)
{
    if (dom == NULL || dom->closed) return;
    if (view != NULL) dom->view = *view;
    else os64_memset(&dom->view, 0, sizeof(dom->view));
    dom->view_seen = view_now(dom);
    dom->media_judged = dom->view_seen;
}

/* The members, by magic: each accessor's name, which a page's assignment
 * replaces with a property of its own ([Replaceable]). */
enum {
    V_SCROLL_X, V_PAGE_X_OFFSET, V_SCROLL_Y, V_PAGE_Y_OFFSET, V_INNER_WIDTH, V_INNER_HEIGHT,
    V_OUTER_WIDTH, V_OUTER_HEIGHT, V_ACCESSORS,
    V_SCROLL_TO = V_ACCESSORS, V_SCROLL_BY, V_SCROLLING_ELEMENT, V_FOCUS
};
static const char *const kViewNames[V_ACCESSORS] = {
    "scrollX", "pageXOffset", "scrollY", "pageYOffset", "innerWidth", "innerHeight",
    "outerWidth", "outerHeight"
};

// An unrestricted double as CSSOM View normalises one: NaN and the
// infinities are 0.
static int finite_number(JSContext *ctx, JSValueConst value, double *out)
{
    if (JS_ToFloat64(ctx, out, value) < 0) return -1;
    if (!(*out == *out) || *out > DBL_MAX || *out < -DBL_MAX) *out = 0;
    return 0;
}

// A ScrollToOptions dictionary (CSSOM View § The ScrollToOptions
// dictionary), its members read in WebIDL's order: `behavior` (one of the
// enum's words, which all scroll at once here), then `left` and `top`.
static int scroll_options(JSContext *ctx, JSValueConst options, bool *has_left, double *left,
                          bool *has_top, double *top)
{
    *has_left = *has_top = false;
    if (JS_IsUndefined(options) || JS_IsNull(options)) return 0;
    if (!JS_IsObject(options)) {
        JS_ThrowTypeError(ctx, "ScrollToOptions must be an object");
        return -1;
    }
    JSValue behavior = JS_GetPropertyStr(ctx, options, "behavior");
    if (JS_IsException(behavior)) return -1;
    if (!JS_IsUndefined(behavior)) {
        const char *word = JS_ToCString(ctx, behavior);
        JS_FreeValue(ctx, behavior);
        if (word == NULL) return -1;
        bool known = os64_strcmp(word, "auto") == 0 || os64_strcmp(word, "instant") == 0 ||
                     os64_strcmp(word, "smooth") == 0;
        JS_FreeCString(ctx, word);
        if (!known) {
            JS_ThrowTypeError(ctx, "behavior must be auto, instant or smooth");
            return -1;
        }
    }
    const char *names[2] = {"left", "top"};
    bool *has[2] = {has_left, has_top};
    double *at[2] = {left, top};
    for (int i = 0; i < 2; i++) {
        JSValue value = JS_GetPropertyStr(ctx, options, names[i]);
        if (JS_IsException(value)) return -1;
        if (!JS_IsUndefined(value)) {
            int read = finite_number(ctx, value, at[i]);
            JS_FreeValue(ctx, value);
            if (read < 0) return -1;
            *has[i] = true;
        }
    }
    return 0;
}

// scroll()/scrollTo() and scrollBy(), with two numbers or one dictionary
// (CSSOM View § The Window's scroll methods). One argument that is not a
// dictionary is a TypeError, as WebIDL's overloads make it. A smooth
// scroll is made at once.
static JSValue scroll_method(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv, bool by)
{
    os64_dom_view_t now = view_now(dom);
    double x = by ? 0 : now.scroll_x, y = by ? 0 : now.scroll_y;
    if (argc >= 2) {
        if (finite_number(ctx, argv[0], &x) < 0 || finite_number(ctx, argv[1], &y) < 0)
            return JS_EXCEPTION;
    } else {
        bool has_left, has_top;
        double left = 0, top = 0;
        if (scroll_options(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &has_left, &left, &has_top, &top) < 0)
            return JS_EXCEPTION;
        if (has_left) x = left;
        if (has_top) y = top;
    }
    if (by) {
        x += now.scroll_x;
        y += now.scroll_y;
    }
    if (dom->view.scroll == NULL) return JS_UNDEFINED;
    if (!budget(dom, ctx)) return JS_EXCEPTION;
    os64_dom_geometry_stats_t cost = {0};
    bool ready = dom->view.scroll(dom->view.opaque, x, y, &cost);
    charge(dom, cost.layouts, cost.elapsed_us);
    if (!budget(dom, ctx)) return JS_EXCEPTION;
    if (!ready) return d_error(ctx, "InvalidStateError", "A current layout could not be produced");
    return JS_UNDEFINED;
}

// ── The computed style ──────────────────────────────────────────────────
//
// The provider writes an element's every property into the snapshot, one
// text after another in one buffer the binding keeps and grows; a read
// asks again, so a declaration a script holds answers for the element as
// it is now.

struct os64_dom_computed {
    os64_dom_t *dom;
    char *text;
    size_t used, cap;
    size_t at[GARB_NPROPS];
    size_t length[GARB_NPROPS];
    bool put[GARB_NPROPS];
    bool refused;
    uint64_t layouts, elapsed_us;
};

bool os64_dom_computed_put(os64_dom_computed_t *out, int32_t prop, const char *value, size_t length)
{
    if (out == NULL || out->refused || prop < 0 || prop >= GARB_NPROPS || (value == NULL && length != 0))
        return false;
    if (length > SIZE_MAX - 1 - out->used) {
        out->refused = true;
        return false;
    }
    size_t need = out->used + length + 1;
    if (need > out->cap) {
        size_t cap = out->cap > 0 ? out->cap : 4096;
        while (cap < need) cap = cap <= SIZE_MAX / 2 ? cap * 2 : need;
        char *grown = d_alloc(out->dom, cap);
        if (grown == NULL) {
            out->refused = true;
            return false;
        }
        if (out->used > 0) os64_memcpy(grown, out->text, out->used);
        d_free(out->dom, out->text);
        out->text = grown;
        out->cap = cap;
    }
    if (length > 0) os64_memcpy(out->text + out->used, value, length);
    out->text[out->used + length] = '\0';
    out->at[prop] = out->used;
    out->length[prop] = length;
    out->put[prop] = true;
    out->used += length + 1;
    return true;
}

void os64_dom_computed_cost(os64_dom_computed_t *out, uint64_t layouts, uint64_t elapsed_us)
{
    if (out == NULL) return;
    out->layouts = add_count(out->layouts, layouts);
    out->elapsed_us = add_count(out->elapsed_us, elapsed_us);
}

void os64_dom_set_computed(os64_dom_t *dom, os64_dom_computed_provider_t provider, void *opaque)
{
    if (dom == NULL || dom->closed) return;
    dom->computed = provider;
    dom->computed_opaque = opaque;
}

// The provider's answer for `element` in the snapshot: 1 when it made
// one, 0 when it could not make the style current, -1 with an exception
// pending (the budget, or no room for the snapshot).
static int snapshot_of(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *element)
{
    if (dom->snapshot == NULL) {
        dom->snapshot = d_alloc(dom, sizeof(*dom->snapshot));
        if (dom->snapshot == NULL) {
            d_error(ctx, "QuotaExceededError", "No room for a computed style");
            return -1;
        }
        dom->snapshot->dom = dom;
    }
    os64_dom_computed_t *s = dom->snapshot;
    s->used = 0;
    s->refused = false;
    s->layouts = s->elapsed_us = 0;
    os64_memset(s->put, 0, sizeof(s->put));
    if (!budget(dom, ctx)) return -1;
    bool ready = dom->computed(dom->computed_opaque, element, s);
    charge(dom, s->layouts, s->elapsed_us);
    if (!budget(dom, ctx)) return -1;
    if (s->refused) {
        d_error(ctx, "QuotaExceededError", "No room for a computed style");
        return -1;
    }
    return ready ? 1 : 0;
}

static const char *snapshot_value(const os64_dom_t *dom, int32_t prop, size_t *length)
{
    const os64_dom_computed_t *s = dom->snapshot;
    if (s == NULL || prop < 0 || prop >= GARB_NPROPS || !s->put[prop]) {
        *length = 0;
        return "";
    }
    *length = s->length[prop];
    return s->text + s->at[prop];
}

/* A declaration getComputedStyle made: the element it reads (its wrapper,
 * which holds the node), and `empty` for a pseudo-element's, which answers
 * nothing until generated content is laid out. Engine memory, traced by
 * the collector, so a page that asks in a loop holds only what it keeps. */
typedef struct {
    JSValue element;
    bool empty;
} DComputed;

static void computed_finalizer(JSRuntime *rt, JSValue value)
{
    DComputed *c = JS_GetOpaque(value, JS_GetClassID(value));
    if (c == NULL) return;
    JS_FreeValueRT(rt, c->element);
    js_free_rt(rt, c);
}

static void computed_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DComputed *c = JS_GetOpaque(value, JS_GetClassID(value));
    if (c != NULL) JS_MarkValue(rt, c->element, mark);
}

/* The declaration's properties: libgarb's table, its names sorted as
 * CSSOM lists them (lexicographically, a vendor's prefixed names after the
 * rest), made once at install. */
static int32_t kOrder[GARB_NPROPS];

static int name_order(int32_t a, int32_t b)
{
    const char *x = garb_prop_name((garb_prop_t)a), *y = garb_prop_name((garb_prop_t)b);
    if ((x[0] == '-') != (y[0] == '-')) return x[0] == '-' ? 1 : -1;
    return os64_strcmp(x, y);
}

static void order_properties(void)
{
    for (int32_t i = 0; i < GARB_NPROPS; i++) kOrder[i] = i;
    for (int32_t i = 1; i < GARB_NPROPS; i++)
        for (int32_t k = i; k > 0 && name_order(kOrder[k - 1], kOrder[k]) > 0; k--) {
            int32_t t = kOrder[k];
            kOrder[k] = kOrder[k - 1];
            kOrder[k - 1] = t;
        }
}

// A property's index in libgarb's table by its name, ASCII case folded
// (a custom property's `--` name is not in the table); -1 for none.
static int32_t property_named(const char *name)
{
    for (int32_t i = 0; i < GARB_NPROPS; i++) {
        const char *p = garb_prop_name((garb_prop_t)i), *n = name;
        while (*p != '\0' && *n != '\0' &&
               (*n >= 'A' && *n <= 'Z' ? *n + ('a' - 'A') : *n) == *p) { p++; n++; }
        if (*p == '\0' && *n == '\0') return i;
    }
    return -1;
}

enum {
    C_TEXT = GARB_NPROPS, C_LENGTH, C_ITEM, C_GET, C_PRIORITY, C_SET, C_REMOVE, C_PARENT_RULE
};

#define READ_ONLY "This declaration is a computed style, which a script cannot change"

/* Whether a declaration answers nothing: a pseudo-element's, or an
 * element's that is not in the document now (CSSOM: its declarations are
 * then an empty list). */
static bool computed_empty(os64_dom_t *dom, JSContext *ctx, const DComputed *c)
{
    if (c->empty) return true;
    const os64_html_node_t *node = d_node(dom, ctx, c->element);
    return node == NULL || !d_connected(dom, node);
}

static JSValue computed_value(os64_dom_t *dom, JSContext *ctx, const DComputed *c, int32_t prop)
{
    if (computed_empty(dom, ctx, c)) return JS_NewString(ctx, "");
    if (dom->computed == NULL)
        return d_error(ctx, "InvalidStateError", "No style provider is installed");
    const os64_html_node_t *node = d_node(dom, ctx, c->element);
    if (node == NULL) return JS_EXCEPTION;
    int made = snapshot_of(dom, ctx, node);
    if (made < 0) return JS_EXCEPTION;
    if (made == 0) return d_error(ctx, "InvalidStateError", "A current style could not be produced");
    size_t length;
    const char *text = snapshot_value(dom, prop, &length);
    return JS_NewStringLen(ctx, text, length);
}

static JSValue computed_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                               int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DComputed *c = JS_GetOpaque(self, dom->computed_class);
    if (c == NULL) return JS_ThrowTypeError(ctx, "Expected a computed CSSStyleDeclaration");
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    if (set || magic == C_SET || magic == C_REMOVE)
        return d_error(ctx, "NoModificationAllowedError", READ_ONLY);
    if (magic < GARB_NPROPS) return computed_value(dom, ctx, c, magic);
    switch (magic) {
    case C_TEXT: return JS_NewString(ctx, "");
    case C_LENGTH: return JS_NewInt32(ctx, computed_empty(dom, ctx, c) ? 0 : GARB_NPROPS);
    case C_PARENT_RULE: return JS_NULL;
    case C_PRIORITY: return JS_NewString(ctx, "");
    case C_ITEM: {
        uint32_t index = 0;
        if (argc < 1) return JS_ThrowTypeError(ctx, "item needs an index");
        if (JS_ToUint32(ctx, &index, argv[0]) < 0) return JS_EXCEPTION;
        if (index >= GARB_NPROPS || computed_empty(dom, ctx, c)) return JS_NewString(ctx, "");
        return JS_NewString(ctx, garb_prop_name((garb_prop_t)kOrder[index]));
    }
    case C_GET: {
        if (argc < 1) return JS_ThrowTypeError(ctx, "getPropertyValue needs a property name");
        DString name = {0};
        if (!d_string(dom, ctx, argv[0], &name)) return JS_EXCEPTION;
        int32_t prop = property_named(name.data);
        d_string_free(dom, &name);
        return prop >= 0 ? computed_value(dom, ctx, c, prop) : JS_NewString(ctx, "");
    }
    default: return JS_ThrowTypeError(ctx, "Unknown CSSStyleDeclaration member");
    }
}

static JSValue get_computed_style(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                                  int magic, JSValue *data)
{
    (void)self; (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "getComputedStyle needs an element");
    const os64_html_node_t *node = d_node(dom, ctx, argv[0]);
    if (node == NULL) return JS_EXCEPTION;
    if (node->kind != OS64_HTML_ELEMENT) return JS_ThrowTypeError(ctx, "getComputedStyle needs an element");
    // A pseudo-element, which starts with a colon, has no box here until
    // generated content does (MODERN_WEB.md, slice 8): its declaration is
    // empty. Anything else is the element's own.
    bool empty = false;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        DString pseudo = {0};
        if (!d_string(dom, ctx, argv[1], &pseudo)) return JS_EXCEPTION;
        empty = pseudo.length > 0 && pseudo.data[0] == ':';
        d_string_free(dom, &pseudo);
    }
    JSValue object = JS_NewObjectClass(ctx, dom->computed_class);
    if (JS_IsException(object)) return object;
    DComputed *c = js_mallocz(ctx, sizeof(*c));
    if (c == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    c->element = JS_DupValue(ctx, argv[0]);
    c->empty = empty;
    JS_SetOpaque(object, c);
    return object;
}

// A property's camel-cased attribute (`background-color` is
// `backgroundColor`, `-webkit-text-fill-color` is `WebkitTextFillColor`),
// or with `webkit` its webkit-cased one (`webkitTextFillColor`).
static void camel(const char *name, char *out, size_t cap, bool webkit)
{
    size_t at = 0;
    size_t j = 0;
    if (webkit) {
        os64_strcopy(out, cap, "webkit");
        at = 6;
        j = 7;              // past "-webkit"
    }
    for (; name[j] != '\0' && at + 1 < cap; j++) {
        if (name[j] == '-' && name[j + 1] != '\0') out[at++] = (char)(name[++j] - ('a' - 'A'));
        else out[at++] = name[j];
    }
    out[at] = '\0';
}

// document.defaultView: the window, which is where a page that reaches
// getComputedStyle through its document finds it.
static JSValue default_view(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                            int magic, JSValue *data)
{
    (void)argc; (void)argv; (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *node = d_node(dom, ctx, self);
    if (node == NULL) return JS_EXCEPTION;
    if (node != dom->document->document) return JS_ThrowTypeError(ctx, "Expected the document");
    return JS_DupValue(ctx, dom->window->value);
}

int d_computed_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    dom->computed_class = os64_js_class_id(&computed_slot);
    const JSClassDef definition = {.class_name = "CSSStyleDeclaration", .finalizer = computed_finalizer,
                                   .gc_mark = computed_mark};
    if (JS_NewClass(dom->engine, dom->computed_class, &definition) < 0) return -1;
    /* A computed declaration and an element's style are one interface,
     * CSSStyleDeclaration (CSSOM), whose interface object names the style
     * object's prototype: the computed one inherits it, shadowing every
     * member with its read-only own, so `instanceof` answers truly for
     * both. */
    JSValue prototype = JS_NewObjectProto(ctx, dom->style_prototype->value);
    dom->computed_prototype = d_retain(dom, ctx, prototype);
    if (dom->computed_prototype == NULL) return -1;
    JS_SetClassProto(ctx, dom->computed_class, JS_DupValue(ctx, prototype));
    order_properties();
    for (int32_t i = 0; i < GARB_NPROPS; i++) {
        const char *name = garb_prop_name((garb_prop_t)i);
        char attribute[128];
        camel(name, attribute, sizeof(attribute), false);
        if (d_accessor(dom, ctx, prototype, attribute, computed_member, i, true) < 0) return -1;
        if (os64_strchr(name, '-') != NULL && d_accessor(dom, ctx, prototype, name, computed_member, i, true) < 0)
            return -1;
        if (os64_strlen(name) > 8 && os64_memcmp(name, "-webkit-", 8) == 0) {
            camel(name, attribute, sizeof(attribute), true);
            if (d_accessor(dom, ctx, prototype, attribute, computed_member, i, true) < 0) return -1;
        }
        if (os64_strcmp(name, "float") == 0 &&
            d_accessor(dom, ctx, prototype, "cssFloat", computed_member, i, true) < 0) return -1;
        // Its index, as a read-only property of the prototype: every
        // computed declaration lists the same names in the same order.
        JSValue listed = JS_NewString(ctx, garb_prop_name((garb_prop_t)kOrder[i]));
        if (JS_IsException(listed) ||
            JS_DefinePropertyValueUint32(ctx, prototype, (uint32_t)i, listed, JS_PROP_ENUMERABLE) < 0)
            return -1;
    }
    if (d_accessor(dom, ctx, prototype, "cssText", computed_member, C_TEXT, true) < 0 ||
        d_accessor(dom, ctx, prototype, "length", computed_member, C_LENGTH, false) < 0 ||
        d_accessor(dom, ctx, prototype, "parentRule", computed_member, C_PARENT_RULE, false) < 0 ||
        d_method(dom, ctx, prototype, "item", computed_member, 1, C_ITEM) < 0 ||
        d_method(dom, ctx, prototype, "getPropertyValue", computed_member, 1, C_GET) < 0 ||
        d_method(dom, ctx, prototype, "getPropertyPriority", computed_member, 1, C_PRIORITY) < 0 ||
        d_method(dom, ctx, prototype, "setProperty", computed_member, 2, C_SET) < 0 ||
        d_method(dom, ctx, prototype, "removeProperty", computed_member, 1, C_REMOVE) < 0 ||
        d_iterable(ctx, prototype, false) < 0)
        return -1;
    if (d_accessor(dom, ctx, dom->prototypes[D_PROTO_DOCUMENT]->value, "defaultView", default_view, 0, false) < 0 ||
        expose(ctx, global, "CSSStyleDeclaration", 0, NULL, dom->style_prototype->value, JS_UNDEFINED) < 0)
        return -1;
    return d_method_named(dom, ctx, global, "getComputedStyle", get_computed_style, 1, 0);
}

// ── scrollingElement ────────────────────────────────────────────────────

static bool html_tag(const os64_html_node_t *n, os64_html_tag_t tag)
{
    return n != NULL && n->kind == OS64_HTML_ELEMENT && n->ns == OS64_HTML_NS_HTML && n->tag == tag;
}

static const os64_html_node_t *root_element(const os64_dom_t *dom)
{
    for (const os64_html_node_t *n = dom->document->document->first_child; n != NULL; n = n->next)
        if (n->kind == OS64_HTML_ELEMENT) return n;
    return NULL;
}

// HTML's "the body element": the root html element's first child that is
// a body or a frameset.
static const os64_html_node_t *body_element(const os64_dom_t *dom)
{
    const os64_html_node_t *root = root_element(dom);
    if (!html_tag(root, OS64_HTML_TAG_HTML)) return NULL;
    for (const os64_html_node_t *n = root->first_child; n != NULL; n = n->next)
        if (html_tag(n, OS64_HTML_TAG_BODY) || html_tag(n, OS64_HTML_TAG_FRAMESET)) return n;
    return NULL;
}

// Whether a snapshot's overflow-x or overflow-y is one that scrolls:
// neither visible, nor (unless `clip_scrolls`) clip.
static bool overflows(const os64_dom_t *dom, bool clip_scrolls)
{
    const int32_t props[2] = {GARB_OVERFLOW_X, GARB_OVERFLOW_Y};
    for (int i = 0; i < 2; i++) {
        size_t length;
        const char *v = snapshot_value(dom, props[i], &length);
        if (length > 0 && os64_strcmp(v, "visible") != 0 && (clip_scrolls || os64_strcmp(v, "clip") != 0))
            return true;
    }
    return false;
}

// Whether the snapshot's element generates no box of its own: `display`
// none or contents (or no style at all).
static bool boxless(const os64_dom_t *dom)
{
    size_t length;
    const char *display = snapshot_value(dom, GARB_DISPLAY, &length);
    return length == 0 || os64_strcmp(display, "none") == 0 || os64_strcmp(display, "contents") == 0;
}

// CSSOM View's "potentially scrollable" for the body (as its
// scrollingElement steps): the body has an associated box, and its parent's
// and its own overflow-x or overflow-y are each neither visible nor clip —
// the parent's `clip` counting as `hidden`, as the WPT holds it. The body
// has no box when it is display none or contents, or when its parent (the
// root, whose own computed display is not the body's) generates none, as
// `html { display: none }` does. 1, 0, or -1 with an exception pending.
// With no style provider nothing is known to scroll.
static int body_scrolls(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *body)
{
    if (dom->computed == NULL || body->parent == NULL) return 0;
    int made = snapshot_of(dom, ctx, body);
    if (made <= 0) return made;
    if (boxless(dom) || !overflows(dom, false)) return 0;
    made = snapshot_of(dom, ctx, body->parent);
    if (made <= 0) return made;
    size_t length;
    const char *display = snapshot_value(dom, GARB_DISPLAY, &length);
    if (length == 0 || os64_strcmp(display, "none") == 0) return 0;
    return overflows(dom, true) ? 1 : 0;
}

static JSValue scrolling_element(os64_dom_t *dom, JSContext *ctx)
{
    if (dom->document->quirks != OS64_HTML_QUIRKS) {
        const os64_html_node_t *root = root_element(dom);
        return root != NULL ? d_wrap(dom, ctx, root) : JS_NULL;
    }
    const os64_html_node_t *body = body_element(dom);
    if (body == NULL) return JS_NULL;
    int scrolls = body_scrolls(dom, ctx, body);
    if (scrolls < 0) return JS_EXCEPTION;
    return scrolls ? JS_NULL : d_wrap(dom, ctx, body);
}

static JSValue view_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                           int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    if (set) {
        // [Replaceable]: the page's own property takes the name's place.
        JSValue global = JS_GetGlobalObject(ctx);
        int done = JS_DefinePropertyValueStr(ctx, global, kViewNames[magic],
                                             JS_DupValue(ctx, argc > 0 ? argv[0] : JS_UNDEFINED),
                                             JS_PROP_C_W_E);
        JS_FreeValue(ctx, global);
        return done < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    }
    if (magic == V_SCROLL_TO || magic == V_SCROLL_BY)
        return scroll_method(dom, ctx, argc, argv, magic == V_SCROLL_BY);
    /* window.focus() and blur(): the window a page is shown in is the
     * person's to raise or leave, so both do nothing here (HTML: blur()'s
     * steps are to do nothing; focus() on a top-level window only
     * encourages the browser to bring it forward). */
    if (magic == V_FOCUS) return JS_UNDEFINED;
    if (magic == V_SCROLLING_ELEMENT) {
        const os64_html_node_t *node = d_node(dom, ctx, self);
        if (node == NULL) return JS_EXCEPTION;
        if (node != dom->document->document) return JS_ThrowTypeError(ctx, "Expected the document");
        return scrolling_element(dom, ctx);
    }
    os64_dom_view_t v = view_now(dom);
    switch (magic) {
    case V_SCROLL_X: case V_PAGE_X_OFFSET: return JS_NewFloat64(ctx, v.scroll_x);
    case V_SCROLL_Y: case V_PAGE_Y_OFFSET: return JS_NewFloat64(ctx, v.scroll_y);
    case V_INNER_WIDTH: return JS_NewInt32(ctx, v.width);
    case V_INNER_HEIGHT: return JS_NewInt32(ctx, v.height);
    case V_OUTER_WIDTH: return JS_NewInt32(ctx, v.outer_width);
    case V_OUTER_HEIGHT: return JS_NewInt32(ctx, v.outer_height);
    default: return JS_ThrowTypeError(ctx, "Unknown view member");
    }
}

int d_view_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    for (int i = 0; i < V_ACCESSORS; i++)
        if (d_accessor(dom, ctx, global, kViewNames[i], view_member, i, true) < 0) return -1;
    JSValueConst document = dom->prototypes[D_PROTO_DOCUMENT]->value;
    JSValueConst element = dom->prototypes[D_PROTO_ELEMENT]->value;
    if (d_method_named(dom, ctx, global, "scroll", view_member, 0, V_SCROLL_TO) < 0 ||
        d_method_named(dom, ctx, global, "scrollTo", view_member, 0, V_SCROLL_TO) < 0 ||
        d_method_named(dom, ctx, global, "scrollBy", view_member, 0, V_SCROLL_BY) < 0 ||
        d_method_named(dom, ctx, global, "focus", view_member, 0, V_FOCUS) < 0 ||
        d_method_named(dom, ctx, global, "blur", view_member, 0, V_FOCUS) < 0 ||
        d_accessor(dom, ctx, document, "scrollingElement", view_member, V_SCROLLING_ELEMENT, false) < 0)
        return -1;
    // onscroll and onresize are every element's, the document's and
    // window's (HTML's GlobalEventHandlers); the body's are window's.
    JSValueConst holders[] = {element, document, global};
    for (size_t i = 0; i < sizeof(holders) / sizeof(holders[0]); i++)
        if (d_event_handler(dom, ctx, holders[i], D_EVENT_SCROLL) < 0 ||
            d_event_handler(dom, ctx, holders[i], D_EVENT_RESIZE) < 0)
            return -1;
    return 0;
}

// ── matchMedia ──────────────────────────────────────────────────────────

/* One MediaQueryList the page made, in the binding's ledger: its list as
 * the page wrote it and as CSSOM serialises it, what `matches` last
 * answered to a step (and whether that change is still owed an event),
 * and its target record once a listener or onchange was asked for, which
 * holds the object from then on. Until then the object may be collected,
 * and its finalizer only marks the record, which matchMedia frees. */
typedef struct DMedia {
    struct DMedia *next;
    char *query, *media;
    DValue *entry;
    bool matches, owed, collected;
    /* libgarb could not read the list whole (it nests past GARB_DEPTH_MAX,
     * or memory ran short reading or writing it): what was read is a
     * different list, so
     * it is `not all`, which never matches, rather than a refusal, since
     * matchMedia answers every string with a list. */
    bool unread;
} DMedia;

static void media_finalizer(JSRuntime *rt, JSValue value)
{
    (void)rt;
    DMedia *m = JS_GetOpaque(value, JS_GetClassID(value));
    if (m != NULL) m->collected = true;
}

// libgarb's grown buffer in the binding's ledger (garb_grow_t).
static void *media_grow(void *ctx, void *old, size_t old_size, size_t new_size)
{
    os64_dom_t *dom = ctx;
    char *grown = new_size > 0 ? d_alloc(dom, new_size) : NULL;
    if (new_size > 0 && grown == NULL) return NULL;
    if (grown != NULL && old != NULL) os64_memcpy(grown, old, old_size < new_size ? old_size : new_size);
    d_free(dom, old);
    return grown;
}

static bool media_matches(const os64_dom_t *dom, const DMedia *m, const os64_dom_view_t *v)
{
    garb_env_t env = {.width = v->width, .height = v->height, .dark = v->dark,
                      .reduced_motion = v->reduced_motion, .scripting = dom->options.scripting};
    d_screen_size(dom, &env.device_width, &env.device_height);
    return !m->unread && garb_media_text_matches(m->query, env);
}

static void media_free(os64_dom_t *dom, DMedia *m)
{
    d_free(dom, m->query);
    d_free(dom, m->media);
    d_free(dom, m);
}

// The records whose objects are gone and that nothing holds.
static void media_sweep(os64_dom_t *dom)
{
    DMedia **link = &dom->media;
    dom->media_last = NULL;
    while (*link != NULL) {
        DMedia *m = *link;
        if (m->collected && m->entry == NULL) {
            *link = m->next;
            media_free(dom, m);
        } else {
            dom->media_last = m;
            link = &m->next;
        }
    }
}

// A MediaQueryList's event target entry (d_event_target's resolver), made
// the first time it is asked for, so a list nothing listens to is not held.
static int media_entry(os64_dom_t *dom, JSContext *ctx, JSValueConst self, DValue **entry)
{
    if (dom->media_class == 0 || !JS_IsObject(self) || JS_GetClassID(self) != dom->media_class) return 0;
    DMedia *m = JS_GetOpaque(self, dom->media_class);
    if (m == NULL) {
        JS_ThrowTypeError(ctx, "This MediaQueryList belongs to no page");
        return -1;
    }
    if (m->entry == NULL && (m->entry = d_retain(dom, ctx, JS_DupValue(ctx, self))) == NULL) return -1;
    *entry = m->entry;
    return 1;
}

static DMedia *media_of(os64_dom_t *dom, JSContext *ctx, JSValueConst self)
{
    DMedia *m = dom->media_class != 0 ? JS_GetOpaque(self, dom->media_class) : NULL;
    if (m == NULL) JS_ThrowTypeError(ctx, "Expected a MediaQueryList");
    return m;
}

enum { M_MEDIA, M_MATCHES, M_ADD_LISTENER, M_REMOVE_LISTENER, M_MATCH_MEDIA };

static JSValue media_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                            int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (magic == M_MATCH_MEDIA) {
        if (argc < 1) return JS_ThrowTypeError(ctx, "matchMedia needs a query");
        DString query = {0};
        if (!d_string(dom, ctx, argv[0], &query)) return JS_EXCEPTION;
        media_sweep(dom);
        // Serialised once, into the binding's own storage (media_grow);
        // NULL is a list libgarb could not read whole.
        size_t length = 0;
        char *media = garb_media_text_serialized(query.data, &length, media_grow, dom);
        bool unread = media == NULL;
        if (unread) {
            length = os64_strlen("not all");
            media = d_alloc(dom, length + 1);
            if (media != NULL) os64_strcopy(media, length + 1, "not all");
        }
        DMedia *m = media != NULL ? d_alloc(dom, sizeof(*m)) : NULL;
        JSValue object = m != NULL ? JS_NewObjectClass(ctx, dom->media_class) : JS_UNDEFINED;
        if (m == NULL || JS_IsException(object)) {
            d_free(dom, media);
            d_free(dom, m);
            d_string_free(dom, &query);
            return JS_IsException(object) ? object
                                          : d_error(ctx, "QuotaExceededError", "No room for a MediaQueryList");
        }
        m->unread = unread;
        m->query = query.data;         // the DString's storage is the binding's, and moves here
        m->media = media;
        os64_dom_view_t v = view_now(dom);
        m->matches = media_matches(dom, m, &v);
        if (dom->media_last != NULL) dom->media_last->next = m;
        else dom->media = m;
        dom->media_last = m;
        JS_SetOpaque(object, m);
        return object;
    }
    DMedia *m = media_of(dom, ctx, self);
    if (m == NULL) return JS_EXCEPTION;
    switch (magic) {
    case M_MEDIA: return JS_NewString(ctx, m->media);
    case M_MATCHES: {
        os64_dom_view_t v = view_now(dom);
        return JS_NewBool(ctx, media_matches(dom, m, &v));
    }
    case M_ADD_LISTENER: case M_REMOVE_LISTENER:
        return d_listener_change(dom, ctx, self, argc > 0 ? argv[0] : JS_UNDEFINED, magic == M_ADD_LISTENER);
    default: return JS_ThrowTypeError(ctx, "Unknown MediaQueryList member");
    }
}

// Defines a read-only own data property, as the binding gives an event's
// fields (event.c's event_fields).
static int define_field(JSContext *ctx, JSValueConst object, const char *name, JSValue value)
{
    if (JS_IsException(value)) return -1;
    return JS_DefinePropertyValueStr(ctx, object, name, value, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
}

// `new MediaQueryListEvent(type, {media, matches, bubbles, cancelable})`.
static JSValue media_event_construct(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSValue event = d_event_construct(dom, ctx, argc, argv,
                                      JS_IsObject(prototype) ? prototype : dom->media_event_prototype->value);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(event)) return event;
    JSValue matches = JS_FALSE, media = JS_UNDEFINED;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue value = JS_GetPropertyStr(ctx, argv[1], "matches");
        int flag = JS_IsException(value) ? -1 : JS_ToBool(ctx, value);
        JS_FreeValue(ctx, value);
        if (flag < 0) goto fail;
        matches = JS_NewBool(ctx, flag != 0);
        value = JS_GetPropertyStr(ctx, argv[1], "media");
        if (JS_IsException(value)) goto fail;
        if (!JS_IsUndefined(value)) {
            media = JS_ToString(ctx, value);
            JS_FreeValue(ctx, value);
            if (JS_IsException(media)) goto fail;
        }
    }
    if (JS_IsUndefined(media)) media = JS_NewString(ctx, "");
    if (define_field(ctx, event, "media", media) < 0 || define_field(ctx, event, "matches", matches) < 0) {
        JS_FreeValue(ctx, event);
        return JS_EXCEPTION;
    }
    return event;
fail:
    JS_FreeValue(ctx, event);
    return JS_EXCEPTION;
}

int d_media_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    dom->media_class = os64_js_class_id(&media_slot);
    const JSClassDef definition = {.class_name = "MediaQueryList", .finalizer = media_finalizer};
    if (JS_NewClass(dom->engine, dom->media_class, &definition) < 0) return -1;
    JSValue prototype = JS_NewObject(ctx);
    dom->media_prototype = d_retain(dom, ctx, prototype);
    if (dom->media_prototype == NULL) return -1;
    JS_SetClassProto(ctx, dom->media_class, JS_DupValue(ctx, prototype));
    if (d_accessor(dom, ctx, prototype, "media", media_member, M_MEDIA, false) < 0 ||
        d_accessor(dom, ctx, prototype, "matches", media_member, M_MATCHES, false) < 0 ||
        d_method_named(dom, ctx, prototype, "addListener", media_member, 1, M_ADD_LISTENER) < 0 ||
        d_method_named(dom, ctx, prototype, "removeListener", media_member, 1, M_REMOVE_LISTENER) < 0 ||
        d_event_handler(dom, ctx, prototype, D_EVENT_CHANGE) < 0 ||
        d_event_target(dom, ctx, prototype, dom->media_class, media_entry) < 0 ||
        expose(ctx, global, "MediaQueryList", 0, NULL, prototype, JS_UNDEFINED) < 0)
        return -1;
    JSValue event_prototype = JS_GetClassProto(ctx, dom->event_class);
    JSValue own = JS_NewObjectProto(ctx, event_prototype);
    JS_FreeValue(ctx, event_prototype);
    dom->media_event_prototype = d_retain(dom, ctx, own);
    if (dom->media_event_prototype == NULL) return -1;
    JSValue event = JS_GetPropertyStr(ctx, global, "Event");
    if (JS_IsException(event)) return -1;
    int made = expose(ctx, global, "MediaQueryListEvent", 1, media_event_construct, own, event);
    JS_FreeValue(ctx, event);
    if (made < 0) return -1;
    return d_method_named(dom, ctx, global, "matchMedia", media_member, 1, M_MATCH_MEDIA);
}

// ── The view's events ───────────────────────────────────────────────────

static JSValue plain_event(os64_dom_t *dom, JSContext *ctx, void *arg)
{
    const char *type = arg;
    return d_event_new(dom, ctx, type, os64_strcmp(type, "scroll") == 0, false, true);
}

static JSValue change_event(os64_dom_t *dom, JSContext *ctx, void *arg)
{
    const DMedia *m = arg;
    JSValue event = d_event_new_proto(dom, ctx, dom->media_event_prototype->value, "change", false, false, true);
    if (JS_IsException(event)) return event;
    if (define_field(ctx, event, "media", JS_NewString(ctx, m->media)) < 0 ||
        define_field(ctx, event, "matches", JS_NewBool(ctx, m->matches)) < 0) {
        JS_FreeValue(ctx, event);
        return JS_EXCEPTION;
    }
    return event;
}

// Whether a list's target record has a `change` listener or handler.
static bool hears_change(const DValue *entry)
{
    for (const DListener *l = entry != NULL ? entry->listeners : NULL; l != NULL; l = l->next)
        if (!l->removed && os64_strcmp(l->type, "change") == 0 &&
            (l->handler == D_LISTENER || l->handler == D_SLOT_FUNCTION || l->handler == D_SLOT_UNCOMPILED))
            return true;
    return false;
}

bool os64_dom_view_step(os64_dom_t *dom, os64_js_outcome_t *outcome)
{
    if (outcome != NULL) os64_memset(outcome, 0, sizeof(*outcome));
    if (dom == NULL || outcome == NULL || dom->closed || !dom->options.modern || dom->view.read == NULL)
        return false;
    os64_dom_view_t now = view_now(dom), *seen = &dom->view_seen;
    // The resize steps, then the scroll steps (HTML § Update the rendering).
    if (now.width != seen->width || now.height != seen->height) {
        seen->width = now.width;
        seen->height = now.height;
        if (os64_dom_listens(dom, "resize")) {
            d_host_dispatch(dom, NULL, NULL, "resize", plain_event, (void *)"resize", NULL, outcome);
            return true;
        }
    }
    if (now.scroll_x != seen->scroll_x || now.scroll_y != seen->scroll_y) {
        seen->scroll_x = now.scroll_x;
        seen->scroll_y = now.scroll_y;
        if (os64_dom_listens(dom, "scroll")) {
            d_host_dispatch(dom, dom->document->document, NULL, "scroll", plain_event, (void *)"scroll",
                            NULL, outcome);
            return true;
        }
    }
    // Every list is judged again when what a query reads moved since the
    // lists were last judged; each one whose answer moved is owed an event,
    // and they are dispatched one a call, in the order the page made them.
    os64_dom_view_t *judged = &dom->media_judged;
    if (now.width != judged->width || now.height != judged->height || now.dark != judged->dark ||
        now.reduced_motion != judged->reduced_motion) {
        *judged = now;
        for (DMedia *m = dom->media; m != NULL; m = m->next) {
            if (m->collected && m->entry == NULL) continue;
            bool matches = media_matches(dom, m, &now);
            if (matches != m->matches) {
                m->matches = matches;
                m->owed = !m->owed;     // a change and its undoing owe nothing
            }
        }
    }
    for (DMedia *m = dom->media; m != NULL; m = m->next) {
        if (!m->owed) continue;
        m->owed = false;
        if (hears_change(m->entry)) {
            d_host_dispatch(dom, NULL, m->entry, "change", change_event, m, NULL, outcome);
            return true;
        }
    }
    return false;
}

void d_view_free(os64_dom_t *dom)
{
    while (dom->media != NULL) {
        DMedia *m = dom->media;
        dom->media = m->next;
        media_free(dom, m);
    }
    dom->media_last = NULL;
    if (dom->snapshot != NULL) d_free(dom, dom->snapshot->text);
    d_free(dom, dom->snapshot);
    dom->snapshot = NULL;
}
