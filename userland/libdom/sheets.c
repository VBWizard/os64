#include "internal.h"

/* THE PAGE'S STYLE SHEETS AS A SCRIPT SEES THEM (CSSOM § 6.1-6.3; HTML's
 * LinkStyle): `document.styleSheets`, a live StyleSheetList (collection.c,
 * D_QUERY_SHEETS) of the sheets the document names by libpage's rule
 * (os64_page_names_sheet, the one the cascade ranks by); each a
 * CSSStyleSheet, one a `style` or `link`, made once and kept on the node's
 * record; and a `style` or `link`'s `sheet`.
 *
 * A sheet says what it is (type, href, ownerNode, title, disabled,
 * parentStyleSheet, ownerRule) and nothing of what it holds: no cssRules,
 * no media list, and `disabled` cannot be set. A page that loads late CSS
 * counts document.styleSheets to see its sheet arrive and finds it by its
 * ownerNode, and that is what this row is for. DEBTS.md lists the rest.
 *
 * One row of the Modern Web table, naming StyleSheetList, StyleSheet and
 * CSSStyleSheet on window. */

static JSClassID sheet_slot;

enum { S_TYPE, S_HREF, S_OWNER_NODE, S_PARENT, S_OWNER_RULE, S_TITLE, S_DISABLED, S_STYLE_SHEETS, S_SHEET };

static void sheet_finalizer(JSRuntime *rt, JSValue value)
{
    (void)rt;
    JS_SetOpaque(value, NULL);
}

/* Whether `node` has a sheet now: it names one by libpage's rule and is in
 * a document. A sheet whose node does not keeps its object, and says it
 * has no owner. */
static bool associated(const os64_html_node_t *node)
{
    if (!os64_page_names_sheet(node)) return false;
    const os64_html_node_t *root = node;
    while (root->parent != NULL) root = root->parent;
    return root->kind == OS64_HTML_DOCUMENT;
}

JSValue d_sheet(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
    /* The node's record holds its sheet, so it needs one. */
    JSValue wrapper = d_wrap(dom, ctx, node);
    if (JS_IsException(wrapper)) return wrapper;
    JS_FreeValue(ctx, wrapper);
    DValue *owner = d_find(dom, node);
    if (owner == NULL) return JS_ThrowTypeError(ctx, "Missing node wrapper");
    if (owner->sheet != NULL) return JS_DupValue(ctx, owner->sheet->value);
    JSValue object = JS_NewObjectClass(ctx, (int)os64_js_class_id(&sheet_slot));
    DValue *entry = d_retain(dom, ctx, object);
    if (entry == NULL) return JS_EXCEPTION;
    entry->dom = dom;
    entry->target = node;
    os64_html_hold(dom->document, node);
    JS_SetOpaque(object, entry);
    owner->sheet = entry;
    return JS_DupValue(ctx, object);
}

/* A title as CSSOM keeps one: the attribute's value, null when it is
 * missing or empty. */
static JSValue title(JSContext *ctx, const os64_html_node_t *node)
{
    const os64_html_attr_t *attr = os64_html_attr(node, "title");
    return attr != NULL && attr->value[0] != '\0' ? JS_NewString(ctx, attr->value) : JS_NULL;
}

/* A linked sheet's address, resolved as the page resolves its `href`; the
 * attribute as written when it will not resolve. Null for a `style`'s. */
static JSValue href(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
    const os64_html_attr_t *attr = node->ns == OS64_HTML_NS_HTML && node->tag == OS64_HTML_TAG_LINK
        ? os64_html_attr(node, "href") : NULL;
    if (attr == NULL) return JS_NULL;
    char *absolute = d_alloc(dom, OS64_DOM_URL_MAX);
    if (absolute == NULL) return d_error(ctx, "QuotaExceededError", "DOM address quota exceeded");
    JSValue result = os64_dom_resolve(dom->document, dom->url, attr->value, absolute, OS64_DOM_URL_MAX)
        ? JS_NewString(ctx, absolute) : JS_NewString(ctx, attr->value);
    d_free(dom, absolute);
    return result;
}

static JSValue sheet_member(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv, int magic,
                            JSValue *data)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (magic == S_STYLE_SHEETS || magic == S_SHEET) {
        const os64_html_node_t *node = d_node(dom, ctx, this_value);
        if (node == NULL) return JS_EXCEPTION;
        if (magic == S_STYLE_SHEETS) {
            if (node->kind != OS64_HTML_DOCUMENT) return JS_ThrowTypeError(ctx, "Illegal invocation");
            return d_style_sheets(dom, ctx, node);
        }
        /* LinkStyle's `sheet`: the node's, while it has one. */
        if (node->kind != OS64_HTML_ELEMENT || node->ns != OS64_HTML_NS_HTML ||
            (node->tag != OS64_HTML_TAG_STYLE && node->tag != OS64_HTML_TAG_LINK))
            return JS_ThrowTypeError(ctx, "Illegal invocation");
        return associated(node) ? d_sheet(dom, ctx, node) : JS_NULL;
    }
    DValue *entry = JS_GetOpaque(this_value, os64_js_class_id(&sheet_slot));
    if (entry == NULL || entry->target == NULL) return JS_ThrowTypeError(ctx, "Expected a CSSStyleSheet");
    const os64_html_node_t *node = entry->target;
    switch (magic) {
    case S_TYPE: return JS_NewString(ctx, "text/css");
    case S_HREF: return href(dom, ctx, node);
    case S_OWNER_NODE: return associated(node) ? d_wrap(dom, ctx, node) : JS_NULL;
    case S_TITLE: return title(ctx, node);
    case S_DISABLED: return JS_FALSE;
    default: return JS_NULL;    /* S_PARENT, S_OWNER_RULE: a top-level sheet's */
    }
}

int d_sheets_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSClassID id = os64_js_class_id(&sheet_slot);
    static const JSClassDef definition = {.class_name = "CSSStyleSheet", .finalizer = sheet_finalizer};
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, &definition) < 0) return -1;
    if (d_sheet_list_install(dom, ctx, global) < 0) return -1;
    /* StyleSheet.prototype, with every member a sheet has here, and
     * CSSStyleSheet.prototype over it. */
    JSValue base = JS_NewObject(ctx);
    if (JS_IsException(base)) return -1;
    DValue *kept = d_retain(dom, ctx, base);
    if (kept == NULL) return -1;
    static const struct { const char *name; int magic; } kMembers[] = {
        {"type", S_TYPE}, {"href", S_HREF}, {"ownerNode", S_OWNER_NODE}, {"parentStyleSheet", S_PARENT},
        {"title", S_TITLE}, {"disabled", S_DISABLED},
    };
    for (size_t i = 0; i < sizeof(kMembers) / sizeof(kMembers[0]); i++)
        if (d_accessor(dom, ctx, base, kMembers[i].name, sheet_member, kMembers[i].magic, false) < 0) return -1;
    JSValue sheet = JS_NewObjectProto(ctx, base);
    if (JS_IsException(sheet)) return -1;
    dom->sheet_prototype = d_retain(dom, ctx, sheet);
    if (dom->sheet_prototype == NULL ||
        d_accessor(dom, ctx, sheet, "ownerRule", sheet_member, S_OWNER_RULE, false) < 0)
        return -1;
    JS_SetClassProto(ctx, id, JS_DupValue(ctx, sheet));
    JSValue parent = d_interface_new(ctx, "StyleSheet", 0, NULL, base, JS_UNDEFINED);
    if (JS_IsException(parent)) return -1;
    JSValue child = d_interface_new(ctx, "CSSStyleSheet", 0, NULL, sheet, parent);
    if (JS_IsException(child)) {
        JS_FreeValue(ctx, parent);
        return -1;
    }
    if (JS_DefinePropertyValueStr(ctx, global, "StyleSheet", parent, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(ctx, child);
        return -1;
    }
    if (
        JS_DefinePropertyValueStr(ctx, global, "CSSStyleSheet", child, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    /* document.styleSheets, and LinkStyle's `sheet` on a link and a style. */
    if (d_accessor(dom, ctx, dom->prototypes[D_PROTO_DOCUMENT]->value, "styleSheets", sheet_member, S_STYLE_SHEETS,
                   false) < 0)
        return -1;
    static const int kKinds[] = {D_PROTO_LINK, D_PROTO_STYLE};
    for (size_t i = 0; i < sizeof(kKinds) / sizeof(kKinds[0]); i++)
        if (d_accessor(dom, ctx, dom->prototypes[kKinds[i]]->value, "sheet", sheet_member, S_SHEET, false) < 0)
            return -1;
    return 0;
}
