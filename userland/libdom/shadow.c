#include "internal.h"

/* SHADOW TREES' SCRIPT SURFACE (docs/design/pending/SHADOW_DOM.md § 7).
 *
 * libhtml holds the trees (a shadow root is a fragment its host owns,
 * assignment, the flat tree); this is what a page reaches of them: Element's
 * attachShadow, shadowRoot, slot and assignedSlot, Text's assignedSlot,
 * ShadowRoot's members (onslotchange among them) and HTMLSlotElement's,
 * Event's composed, and the two interfaces on window. The event path is
 * event.c's and slotchange's firing observer.c's. All of it is installed
 * only with options.shadow_trees (and the
 * Modern Web's switch), as one switch: a page decides once from several
 * of these names together (Lit's test reads ShadowRoot, adoptedStyleSheets
 * and CSSStyleSheet's replace), so they come and go as one.
 *
 * The two prototypes are made with the binding when the surface is on,
 * and a wrapper takes them by refinement (d_node_prototype) rather than by a
 * kind of their own: a shadow root keeps DocumentFragment's kind and a
 * slot HTMLElement's, so every member and receiver check those kinds have
 * applies to them unchanged. */

static const char *const kShadowNames[] = {"ShadowRoot", "HTMLSlotElement", NULL};

const char *d_shadow_name(size_t index)
{
    return index < sizeof(kShadowNames) / sizeof(kShadowNames[0]) ? kShadowNames[index] : NULL;
}

static bool is_slot(const os64_html_node_t *n)
{
    return n->kind == OS64_HTML_ELEMENT && n->ns == OS64_HTML_NS_HTML && n->tag == OS64_HTML_TAG_SLOT;
}

int d_shadow_prototypes(os64_dom_t *dom, JSContext *ctx)
{
    /* With the surface off a slot's wrapper keeps HTMLElement's prototype
     * itself, so no link a page can see appears without the names. */
    if (!dom->options.shadow_trees || !dom->options.modern) return 0;
    dom->shadow_root_proto = d_retain(dom, ctx, JS_NewObjectProto(ctx, dom->prototypes[D_PROTO_FRAGMENT]->value));
    if (dom->shadow_root_proto == NULL) return -1;
    dom->slot_proto = d_retain(dom, ctx, JS_NewObjectProto(ctx, dom->prototypes[D_PROTO_HTML_ELEMENT]->value));
    return dom->slot_proto != NULL ? 0 : -1;
}

JSValueConst d_node_prototype(os64_dom_t *dom, const os64_html_node_t *n)
{
    if (dom->shadow_root_proto != NULL && os64_html_shadow_host(n) != NULL) return dom->shadow_root_proto->value;
    if (dom->slot_proto != NULL && is_slot(n)) return dom->slot_proto->value;
    return d_svg_prototype(dom, n);
}

/* The shadow root a node's tree is, or NULL for a tree that is none. */
static const os64_html_node_t *tree_shadow_root(const os64_html_node_t *n)
{
    while (n->parent != NULL) n = n->parent;
    return os64_html_shadow_host(n) != NULL ? n : NULL;
}

enum {
    S_ATTACH, S_SHADOW_ROOT, S_SLOT, S_ASSIGNED_SLOT,
    S_HOST, S_MODE, S_DELEGATES_FOCUS, S_CLONABLE, S_SERIALIZABLE, S_SLOT_ASSIGNMENT, S_ACTIVE_ELEMENT,
    S_INNER_HTML,
    S_NAME, S_ASSIGNED_NODES, S_ASSIGNED_ELEMENTS, S_ASSIGN
};

static const DReflection kSlotAttribute = {.property = "slot", .attribute = "slot", .kind = K_STRING};
static const DReflection kNameAttribute = {.property = "name", .attribute = "name", .kind = K_STRING};

/* A dictionary member, read once, in the dictionary's order: undefined
 * when absent. JS_EXCEPTION when the read threw. */
static JSValue member(JSContext *ctx, JSValueConst init, const char *name)
{
    return JS_IsObject(init) ? JS_GetPropertyStr(ctx, init, name) : JS_UNDEFINED;
}
static int boolean_member(JSContext *ctx, JSValueConst init, const char *name)
{
    JSValue v = member(ctx, init, name);
    if (JS_IsException(v)) return -1;
    int truth = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return truth;
}
/* An enum member: the index of its value among `values`, `missing` when
 * absent; -1 with a TypeError when it is none of them. */
static int enum_member(os64_dom_t *dom, JSContext *ctx, JSValueConst init, const char *name,
                       const char *const *values, int missing)
{
    JSValue v = member(ctx, init, name);
    if (JS_IsException(v)) return -1;
    if (JS_IsUndefined(v)) return missing;
    DString s = {0};
    bool ok = d_string(dom, ctx, v, &s);
    JS_FreeValue(ctx, v);
    if (!ok) return -1;
    int found = -1;
    for (int i = 0; values[i] != NULL && found < 0; i++)
        if (os64_streq(s.data, values[i])) found = i;
    d_string_free(dom, &s);
    if (found < 0) JS_ThrowTypeError(ctx, "%s is not one of the values it may be", name);
    return found;
}

/* attachShadow(init) (DOM's "attach a shadow root"): ShadowRootInit's
 * members in WebIDL's order, mode required. A definition that disables
 * shadows, a host that is not a valid one or already has a root, and a
 * manual slotAssignment (manual slots are not built, DEBTS.md) are
 * NotSupportedError. */
static JSValue attach_shadow(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node, int argc,
                             JSValueConst *argv)
{
    if (node->kind != OS64_HTML_ELEMENT) return JS_ThrowTypeError(ctx, "attachShadow needs an element");
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "attachShadow needs a ShadowRootInit");
    static const char *const kModes[] = {"open", "closed", NULL};
    static const char *const kAssignments[] = {"named", "manual", NULL};
    int clonable = boolean_member(ctx, argv[0], "clonable");
    if (clonable < 0) return JS_EXCEPTION;
    int delegates = boolean_member(ctx, argv[0], "delegatesFocus");
    if (delegates < 0) return JS_EXCEPTION;
    int mode = enum_member(dom, ctx, argv[0], "mode", kModes, -2);
    if (mode == -1) return JS_EXCEPTION;
    if (mode == -2) return JS_ThrowTypeError(ctx, "ShadowRootInit needs a mode");
    int serializable = boolean_member(ctx, argv[0], "serializable");
    if (serializable < 0) return JS_EXCEPTION;
    int assignment = enum_member(dom, ctx, argv[0], "slotAssignment", kAssignments, 0);
    if (assignment < 0) return JS_EXCEPTION;
    if (assignment == 1) return d_error(ctx, "NotSupportedError", "Manual slot assignment is not supported");
    if (d_custom_disables_shadow(dom, node))
        return d_error(ctx, "NotSupportedError", "This element's definition disables shadow roots");
    uint8_t options = (mode == 0 ? OS64_HTML_SHADOW_OPEN : OS64_HTML_SHADOW_CLOSED) |
                      (delegates ? OS64_HTML_SHADOW_DELEGATES_FOCUS : 0) | (clonable ? OS64_HTML_SHADOW_CLONABLE : 0) |
                      (serializable ? OS64_HTML_SHADOW_SERIALIZABLE : 0);
    int64_t status = OS64_HTML_OK;
    os64_html_node_t *root = os64_html_attach_shadow(dom->document, (os64_html_node_t *)node, options, &status);
    return root != NULL ? d_wrap(dom, ctx, root) : d_html_error(ctx, status);
}

/* A slot's assigned nodes, or with `flatten` DOM's "find flattened
 * slottables": a slot among them, in a shadow tree, gives its own in its
 * place, and a slot with nothing assigned its children. As a new Array,
 * elements only when `elements`. */
static int flattened(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *slot, bool flatten, bool elements,
                     JSValueConst array, uint32_t *at)
{
    size_t count = os64_html_slot_assigned_count(dom->document, slot);
    bool fallback = flatten && count == 0 && tree_shadow_root(slot) != NULL;
    const os64_html_node_t *n = fallback ? slot->first_child : os64_html_slot_assigned(dom->document, slot, 0);
    for (size_t i = 0; n != NULL;) {
        if (flatten && is_slot(n) && tree_shadow_root(n) != NULL) {
            if (flattened(dom, ctx, n, true, elements, array, at) < 0) return -1;
        } else if ((!fallback || n->kind == OS64_HTML_ELEMENT || n->kind == OS64_HTML_TEXT) &&
                   (!elements || n->kind == OS64_HTML_ELEMENT)) {
            JSValue w = d_wrap(dom, ctx, n);
            if (JS_IsException(w) || JS_DefinePropertyValueUint32(ctx, array, (*at)++, w, JS_PROP_C_W_E) < 0)
                return -1;
        }
        n = fallback ? n->next : os64_html_slot_assigned(dom->document, slot, ++i);
    }
    return 0;
}

static JSValue shadow_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                             JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    const os64_html_node_t *node = d_node(dom, ctx, self);
    if (node == NULL) return JS_EXCEPTION;
    const os64_html_node_t *host = os64_html_shadow_host(node);
    bool root_member = magic >= S_HOST && magic <= S_INNER_HTML;
    bool slot_member = magic >= S_NAME;
    if (root_member && host == NULL) return JS_ThrowTypeError(ctx, "Expected a ShadowRoot");
    if (slot_member && !is_slot(node)) return JS_ThrowTypeError(ctx, "Expected an HTMLSlotElement");
    switch (magic) {
    case S_ATTACH: return attach_shadow(dom, ctx, node, argc, argv);
    case S_SHADOW_ROOT:
        return node->shadow_root != NULL && (node->shadow_root->shadow & OS64_HTML_SHADOW_OPEN)
            ? d_wrap(dom, ctx, node->shadow_root) : JS_NULL;
    case S_SLOT:
        if (node->kind != OS64_HTML_ELEMENT) return JS_ThrowTypeError(ctx, "Expected an element");
        return set ? d_reflection_set(dom, ctx, node, &kSlotAttribute, argc > 0 ? argv[0] : JS_UNDEFINED)
                   : d_reflection_get(dom, ctx, node, &kSlotAttribute);
    case S_ASSIGNED_SLOT: {
        /* DOM's "find a slot" with the open flag: a slot in a closed tree
         * is not handed out. */
        const os64_html_node_t *slot = os64_html_assigned_slot(dom->document, node);
        const os64_html_node_t *root = slot != NULL ? tree_shadow_root(slot) : NULL;
        return root != NULL && (root->shadow & OS64_HTML_SHADOW_OPEN) ? d_wrap(dom, ctx, slot) : JS_NULL;
    }
    case S_HOST: return d_wrap(dom, ctx, host);
    case S_MODE: return JS_NewString(ctx, node->shadow & OS64_HTML_SHADOW_OPEN ? "open" : "closed");
    case S_DELEGATES_FOCUS: return JS_NewBool(ctx, (node->shadow & OS64_HTML_SHADOW_DELEGATES_FOCUS) != 0);
    case S_CLONABLE: return JS_NewBool(ctx, (node->shadow & OS64_HTML_SHADOW_CLONABLE) != 0);
    case S_SERIALIZABLE: return JS_NewBool(ctx, (node->shadow & OS64_HTML_SHADOW_SERIALIZABLE) != 0);
    case S_SLOT_ASSIGNMENT: return JS_NewString(ctx, "named");
    case S_ACTIVE_ELEMENT: {
        /* DocumentOrShadowRoot's: the focused element retargeted against
         * this root, when that lands in this tree. */
        const os64_html_node_t *focused = d_focused(dom);
        const os64_html_node_t *candidate = focused != NULL ? d_retarget(focused, node) : NULL;
        return candidate != NULL && tree_shadow_root(candidate) == node ? d_wrap(dom, ctx, candidate) : JS_NULL;
    }
    case S_INNER_HTML:
        return set ? d_content_set(dom, ctx, node, D_INNER_HTML, argc > 0 ? argv[0] : JS_UNDEFINED)
                   : d_content_get(dom, ctx, node, D_INNER_HTML);
    case S_NAME:
        return set ? d_reflection_set(dom, ctx, node, &kNameAttribute, argc > 0 ? argv[0] : JS_UNDEFINED)
                   : d_reflection_get(dom, ctx, node, &kNameAttribute);
    case S_ASSIGNED_NODES: case S_ASSIGNED_ELEMENTS: {
        JSValueConst options = argc > 0 ? argv[0] : JS_UNDEFINED;
        if (!JS_IsUndefined(options) && !JS_IsNull(options) && !JS_IsObject(options))
            return JS_ThrowTypeError(ctx, "AssignedNodesOptions must be an object");
        int flatten = boolean_member(ctx, options, "flatten");
        if (flatten < 0) return JS_EXCEPTION;
        JSValue array = JS_NewArray(ctx);
        uint32_t at = 0;
        if (JS_IsException(array) ||
            flattened(dom, ctx, node, flatten != 0, magic == S_ASSIGNED_ELEMENTS, array, &at) < 0) {
            JS_FreeValue(ctx, array);
            return JS_EXCEPTION;
        }
        return array;
    }
    case S_ASSIGN: return d_error(ctx, "NotSupportedError", "Manual slot assignment is not supported");
    default: return JS_ThrowTypeError(ctx, "Unknown shadow member");
    }
}

/* The surface, installed by d_modern_install once the Modern Web's rows
 * have made the interfaces it inherits (DocumentFragment, HTMLElement). */
int d_shadow_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSValueConst element = dom->prototypes[D_PROTO_ELEMENT]->value;
    JSValueConst text = dom->prototypes[D_PROTO_TEXT]->value;
    JSValueConst root = dom->shadow_root_proto->value, slot = dom->slot_proto->value;
    if (d_method_named(dom, ctx, element, "attachShadow", shadow_member, 1, S_ATTACH) < 0 ||
        d_accessor(dom, ctx, element, "shadowRoot", shadow_member, S_SHADOW_ROOT, false) < 0 ||
        d_accessor(dom, ctx, element, "slot", shadow_member, S_SLOT, true) < 0 ||
        d_accessor(dom, ctx, element, "assignedSlot", shadow_member, S_ASSIGNED_SLOT, false) < 0 ||
        d_accessor(dom, ctx, text, "assignedSlot", shadow_member, S_ASSIGNED_SLOT, false) < 0 ||
        d_accessor(dom, ctx, root, "host", shadow_member, S_HOST, false) < 0 ||
        d_accessor(dom, ctx, root, "mode", shadow_member, S_MODE, false) < 0 ||
        d_accessor(dom, ctx, root, "delegatesFocus", shadow_member, S_DELEGATES_FOCUS, false) < 0 ||
        d_accessor(dom, ctx, root, "clonable", shadow_member, S_CLONABLE, false) < 0 ||
        d_accessor(dom, ctx, root, "serializable", shadow_member, S_SERIALIZABLE, false) < 0 ||
        d_accessor(dom, ctx, root, "slotAssignment", shadow_member, S_SLOT_ASSIGNMENT, false) < 0 ||
        d_accessor(dom, ctx, root, "activeElement", shadow_member, S_ACTIVE_ELEMENT, false) < 0 ||
        d_event_handler_named(dom, ctx, root, "slotchange") < 0 || d_event_composed_install(dom, ctx) < 0 ||
        d_accessor(dom, ctx, root, "innerHTML", shadow_member, S_INNER_HTML, true) < 0 ||
        d_accessor(dom, ctx, slot, "name", shadow_member, S_NAME, true) < 0 ||
        d_method_named(dom, ctx, slot, "assignedNodes", shadow_member, 0, S_ASSIGNED_NODES) < 0 ||
        d_method_named(dom, ctx, slot, "assignedElements", shadow_member, 0, S_ASSIGNED_ELEMENTS) < 0 ||
        d_method_named(dom, ctx, slot, "assign", shadow_member, 0, S_ASSIGN) < 0)
        return -1;
    static const struct { const char *name, *parent; int which; } kInterfaces[] = {
        {"ShadowRoot", "DocumentFragment", 0}, {"HTMLSlotElement", "HTMLElement", 1}};
    for (size_t i = 0; i < sizeof(kInterfaces) / sizeof(kInterfaces[0]); i++) {
        JSValue parent = JS_GetPropertyStr(ctx, global, kInterfaces[i].parent);
        if (JS_IsException(parent)) return -1;
        JSValue f = d_interface_new(ctx, kInterfaces[i].name, 0, NULL, kInterfaces[i].which ? slot : root, parent);
        JS_FreeValue(ctx, parent);
        if (JS_IsException(f) ||
            JS_DefinePropertyValueStr(ctx, global, kInterfaces[i].name, f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
            return -1;
    }
    return 0;
}
