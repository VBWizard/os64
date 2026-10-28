#include "internal.h"

static size_t bucket(const os64_html_node_t *node)
{
    uintptr_t key = (uintptr_t)node;
    key ^= key >> 17;
    key ^= key >> 9;
    return (key >> 3) % D_BUCKETS;
}

DValue *d_find(os64_dom_t *dom, const os64_html_node_t *node)
{
    for (DValue *entry = dom->buckets[bucket(node)]; entry != NULL; entry = entry->hash_next)
        if (entry->node == node) return entry;
    return NULL;
}

/* HTML's element interface for a tag libhtml names (§ 3.2.3.1), as the
 * binding's kinds; 0 (Node's, which no element is) for HTMLElement's,
 * whether HTML gives the name that interface or one the binding does not
 * have. The obsolete names HTML gives HTMLUnknownElement are UNKNOWN, and
 * so are menuitem, and math and svg in HTML's namespace, which HTML gives
 * no interface and which are no custom element names. */
static const uint8_t kTagKinds[OS64_HTML_TAG_COUNT] = {
    [OS64_HTML_TAG_INPUT] = D_PROTO_INPUT, [OS64_HTML_TAG_TEXTAREA] = D_PROTO_TEXTAREA,
    [OS64_HTML_TAG_SELECT] = D_PROTO_SELECT, [OS64_HTML_TAG_BUTTON] = D_PROTO_BUTTON,
    [OS64_HTML_TAG_FORM] = D_PROTO_FORM, [OS64_HTML_TAG_IMG] = D_PROTO_IMAGE,
    [OS64_HTML_TAG_SCRIPT] = D_PROTO_SCRIPT, [OS64_HTML_TAG_A] = D_PROTO_ANCHOR,
    [OS64_HTML_TAG_LINK] = D_PROTO_LINK, [OS64_HTML_TAG_IFRAME] = D_PROTO_IFRAME,
    [OS64_HTML_TAG_OPTION] = D_PROTO_OPTION, [OS64_HTML_TAG_OPTGROUP] = D_PROTO_OPTGROUP,
    [OS64_HTML_TAG_TEMPLATE] = D_PROTO_TEMPLATE, [OS64_HTML_TAG_LABEL] = D_PROTO_LABEL,
    [OS64_HTML_TAG_FIELDSET] = D_PROTO_FIELDSET, [OS64_HTML_TAG_LEGEND] = D_PROTO_LEGEND,
    [OS64_HTML_TAG_OUTPUT] = D_PROTO_OUTPUT, [OS64_HTML_TAG_STYLE] = D_PROTO_STYLE,
    [OS64_HTML_TAG_META] = D_PROTO_META, [OS64_HTML_TAG_CANVAS] = D_PROTO_CANVAS,
    [OS64_HTML_TAG_TIME] = D_PROTO_TIME, [OS64_HTML_TAG_DIV] = D_PROTO_DIV, [OS64_HTML_TAG_BODY] = D_PROTO_BODY,
    [OS64_HTML_TAG_OL] = D_PROTO_OLIST, [OS64_HTML_TAG_UL] = D_PROTO_ULIST, [OS64_HTML_TAG_DL] = D_PROTO_DLIST,
    [OS64_HTML_TAG_LI] = D_PROTO_LI, [OS64_HTML_TAG_HEAD] = D_PROTO_HEAD, [OS64_HTML_TAG_DATA] = D_PROTO_DATA,
    [OS64_HTML_TAG_AREA] = D_PROTO_AREA, [OS64_HTML_TAG_BASE] = D_PROTO_BASE, [OS64_HTML_TAG_MAP] = D_PROTO_MAP,
    [OS64_HTML_TAG_INS] = D_PROTO_MOD, [OS64_HTML_TAG_DEL] = D_PROTO_MOD,
    [OS64_HTML_TAG_Q] = D_PROTO_QUOTE, [OS64_HTML_TAG_BLOCKQUOTE] = D_PROTO_QUOTE,
    [OS64_HTML_TAG_TD] = D_PROTO_TABLE_CELL, [OS64_HTML_TAG_TH] = D_PROTO_TABLE_CELL,
    [OS64_HTML_TAG_COL] = D_PROTO_TABLE_COL, [OS64_HTML_TAG_COLGROUP] = D_PROTO_TABLE_COL,
    [OS64_HTML_TAG_TABLE] = D_PROTO_TABLE, [OS64_HTML_TAG_TR] = D_PROTO_TABLE_ROW,
    [OS64_HTML_TAG_THEAD] = D_PROTO_TABLE_SECTION, [OS64_HTML_TAG_TBODY] = D_PROTO_TABLE_SECTION,
    [OS64_HTML_TAG_TFOOT] = D_PROTO_TABLE_SECTION, [OS64_HTML_TAG_CAPTION] = D_PROTO_TABLE_CAPTION,
    [OS64_HTML_TAG_BR] = D_PROTO_BR, [OS64_HTML_TAG_DIR] = D_PROTO_DIRECTORY, [OS64_HTML_TAG_FONT] = D_PROTO_FONT,
    [OS64_HTML_TAG_HR] = D_PROTO_HR, [OS64_HTML_TAG_H1] = D_PROTO_HEADING, [OS64_HTML_TAG_H2] = D_PROTO_HEADING,
    [OS64_HTML_TAG_H3] = D_PROTO_HEADING, [OS64_HTML_TAG_H4] = D_PROTO_HEADING, [OS64_HTML_TAG_H5] = D_PROTO_HEADING,
    [OS64_HTML_TAG_H6] = D_PROTO_HEADING, [OS64_HTML_TAG_HTML] = D_PROTO_HTML, [OS64_HTML_TAG_MENU] = D_PROTO_MENU,
    [OS64_HTML_TAG_P] = D_PROTO_PARAGRAPH, [OS64_HTML_TAG_PRE] = D_PROTO_PRE, [OS64_HTML_TAG_LISTING] = D_PROTO_PRE,
    [OS64_HTML_TAG_XMP] = D_PROTO_PRE, [OS64_HTML_TAG_SPAN] = D_PROTO_SPAN, [OS64_HTML_TAG_TITLE] = D_PROTO_TITLE,
    [OS64_HTML_TAG_APPLET] = D_PROTO_UNKNOWN, [OS64_HTML_TAG_BGSOUND] = D_PROTO_UNKNOWN,
    [OS64_HTML_TAG_ISINDEX] = D_PROTO_UNKNOWN, [OS64_HTML_TAG_KEYGEN] = D_PROTO_UNKNOWN,
    [OS64_HTML_TAG_MENUITEM] = D_PROTO_UNKNOWN, [OS64_HTML_TAG_MATH] = D_PROTO_UNKNOWN,
    [OS64_HTML_TAG_SVG] = D_PROTO_UNKNOWN,
};

/* A valid custom element name (HTML § 4.13.2): a lowercase ASCII letter,
 * then name characters with a hyphen among them, and none of the names
 * SVG and MathML already use. A byte past ASCII is taken as one of the
 * name characters, which nearly every code point past ASCII is. */
static bool custom_element_name(const char *name)
{
    static const char *const kReserved[] = {"annotation-xml", "color-profile", "font-face", "font-face-src",
                                            "font-face-uri", "font-face-format", "font-face-name", "missing-glyph"};
    if (name == NULL || name[0] < 'a' || name[0] > 'z') return false;
    bool hyphen = false;
    for (const unsigned char *c = (const unsigned char *)name; *c != '\0'; c++) {
        hyphen = hyphen || *c == '-';
        if (!(*c >= 0x80 || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '.' ||
              *c == '_'))
            return false;
    }
    for (size_t i = 0; hyphen && i < sizeof(kReserved) / sizeof(kReserved[0]); i++)
        if (os64_streq(name, kReserved[i])) return false;
    return hyphen;
}

int d_node_kind(const os64_html_node_t *node)
{
    if (node->kind == OS64_HTML_DOCUMENT) return D_PROTO_DOCUMENT;
    if (node->kind == OS64_HTML_FRAGMENT) return D_PROTO_FRAGMENT;
    if (node->kind == OS64_HTML_TEXT) return D_PROTO_TEXT;
    if (node->kind == OS64_HTML_COMMENT) return D_PROTO_COMMENT;
    if (node->kind == OS64_HTML_DOCTYPE) return D_PROTO_DOCTYPE;
    if (node->kind != OS64_HTML_ELEMENT) return D_PROTO_NODE;
    if (node->ns == OS64_HTML_NS_HTML) {
        /* A name libhtml does not know is HTML's to judge: a custom
         * element's is HTMLElement's, any other HTMLUnknownElement's. */
        if (node->tag == OS64_HTML_TAG_UNKNOWN)
            return custom_element_name(node->name) ? D_PROTO_HTML_ELEMENT : D_PROTO_UNKNOWN;
        int kind = node->tag < OS64_HTML_TAG_COUNT ? kTagKinds[node->tag] : 0;
        return kind != 0 ? kind : D_PROTO_HTML_ELEMENT;
    }
    if (node->ns == OS64_HTML_NS_SVG) return D_PROTO_SVG;
    return D_PROTO_ELEMENT;
}

/* A wrapper for a node of `node`'s kind, not yet attached: its prototype is
 * the kind's, or `prototype` when that is an object (a constructor's
 * new.target's, d_wrap_constructed). */
static DValue *wrapper_prepare(os64_dom_t *dom, JSContext *ctx,
                                const os64_html_node_t *node, JSValueConst prototype)
{
    DValue *entry = d_alloc(dom, sizeof(*entry));
    if (entry == NULL) { d_error(ctx, "QuotaExceededError", "DOM wrapper quota exceeded"); return NULL; }
    JSValue value = JS_NewObjectProtoClass(ctx, JS_IsObject(prototype) ? prototype :
        dom->prototypes[d_node_kind(node)]->value, dom->node_class);
    if (JS_IsException(value)) { d_free(dom, entry); return NULL; }
    entry->value = value;
    entry->next = dom->values;
    dom->values = entry;
    dom->retained++;
    return entry;
}

static JSValue wrapper_attach(os64_dom_t *dom, JSContext *ctx, DValue *entry,
                                const os64_html_node_t *node)
{
    entry->dom = dom;
    JS_SetOpaque(entry->value, entry);
    entry->node = node;
    os64_html_hold(dom->document, node);
    size_t slot = bucket(node);
    entry->hash_next = dom->buckets[slot];
    dom->buckets[slot] = entry;
    return JS_DupValue(ctx, entry->value);
}

JSValue d_wrap(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
    return d_wrap_constructed(dom, ctx, node, JS_UNDEFINED);
}

JSValue d_wrap_constructed(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                           JSValueConst prototype)
{
    if (!d_context(dom, ctx, OS64_JS_ABI_ID)) return JS_EXCEPTION;
    if (node == NULL) return JS_NULL;
    DValue *entry = d_find(dom, node);
    if (entry != NULL) return JS_DupValue(ctx, entry->value);
    entry = wrapper_prepare(dom, ctx, node, prototype);
    return entry != NULL ? wrapper_attach(dom, ctx, entry, node) : JS_EXCEPTION;
}

const os64_html_node_t *d_node(os64_dom_t *dom, JSContext *ctx, JSValueConst value)
{
    DValue *entry = JS_GetOpaque(value, dom->node_class);
    const os64_html_node_t *node = entry != NULL ? entry->node : NULL;
    if (node == NULL && d_is_attr(dom, value)) {
        JS_ThrowTypeError(ctx, "An Attr is a Node with no place in the tree");
        return NULL;
    }
    if (node == NULL || !os64_html_owns_node(dom->document, node)) {
        JS_ThrowTypeError(ctx, "Expected a node from this document");
        return NULL;
    }
    return node;
}

/* The binding's tree changes. A node one of them takes out of its tree
 * records first the document it leaves (d_leaving), whether or not the
 * change then happens: the record is read only while the node is a root. */
int64_t d_insert(os64_dom_t *dom, os64_html_node_t *parent, os64_html_node_t *node,
                  os64_html_node_t *before)
{
    return os64_page_node_insert(dom->state, parent, node, before);
}
int64_t d_remove(os64_dom_t *dom, os64_html_node_t *node)
{
    d_leaving(dom, node);
    return os64_page_node_remove(dom->state, node);
}
int64_t d_replace(os64_dom_t *dom, os64_html_node_t *parent, os64_html_node_t *node,
                   os64_html_node_t *old)
{
    d_leaving(dom, old);
    return os64_page_node_replace(dom->state, parent, node, old);
}

/* A tree change a script asked for: D_APPEND/D_INSERT put `child` under
 * `parent` before `other` (NULL: last), D_REPLACE puts it where `other` is,
 * D_REPLACE_CHILDREN puts it in place of all `parent`'s children, D_REMOVE
 * takes it out. Scripts the move may connect are found before it,
 * while a fragment still holds its children, and handed to the host after
 * it. 0, or -1 with the exception thrown. */
int d_tree_change(os64_dom_t *dom, JSContext *ctx, int op, const os64_html_node_t *parent,
            const os64_html_node_t *child, const os64_html_node_t *other)
{
    const os64_html_node_t *few[8], **scripts = few;
    size_t found = 0;
    if (op != D_REMOVE && dom->options.script_connected != NULL) {
        found = d_script_collect(child, few, 8);
        if (found > 8) {
            scripts = d_alloc(dom, found * sizeof(*scripts));
            if (scripts == NULL) {
                d_error(ctx, "QuotaExceededError", "DOM script list quota exceeded");
                return -1;
            }
            d_script_collect(child, scripts, found);
        }
        for (size_t i = 0; i < found; i++) os64_html_hold(dom->document, scripts[i]);
    }
    int64_t status = op == D_REMOVE ? d_remove(dom, (os64_html_node_t *)child) :
        op == D_REPLACE_CHILDREN ? d_replace_content(dom, (os64_html_node_t *)parent, (os64_html_node_t *)child) :
        op == D_REPLACE ? d_replace(dom, (os64_html_node_t *)parent, (os64_html_node_t *)child,
                                    (os64_html_node_t *)other) :
        d_insert(dom, (os64_html_node_t *)parent, (os64_html_node_t *)child, (os64_html_node_t *)other);
    if (status >= 0) d_script_connected(dom, scripts, found);
    for (size_t i = 0; i < found; i++) os64_html_release(dom->document, scripts[i]);
    if (scripts != few) d_free(dom, scripts);
    if (status < 0) { d_html_error(ctx, status); return -1; }
    return 0;
}

static JSValue node_name(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node)
{
    const char *name;
    switch (node->kind) {
    case OS64_HTML_DOCUMENT: name = "#document"; break;
    case OS64_HTML_FRAGMENT: name = "#document-fragment"; break;
    case OS64_HTML_TEXT: name = "#text"; break;
    case OS64_HTML_COMMENT: name = "#comment"; break;
    default: name = node->name != NULL ? node->name : ""; break;
    }
    if (node->kind != OS64_HTML_ELEMENT || node->ns != OS64_HTML_NS_HTML)
        return JS_NewString(ctx, name);
    size_t length = os64_strlen(name);
    char *upper = d_alloc(dom, length + 1);
    if (upper == NULL) return d_error(ctx, "QuotaExceededError", "DOM name quota exceeded");
    for (size_t i = 0; i <= length; i++)
        upper[i] = name[i] >= 'a' && name[i] <= 'z' ? name[i] - ('a' - 'A') : name[i];
    JSValue value = JS_NewStringLen(ctx, upper, length);
    d_free(dom, upper);
    return value;
}

static const os64_html_node_t *element_sibling(const os64_html_node_t *at, bool forward)
{
    while (at != NULL && at->kind != OS64_HTML_ELEMENT) at = forward ? at->next : at->prev;
    return at;
}

static bool filename_input(const os64_html_node_t *node)
{
    if (node->kind != OS64_HTML_ELEMENT || node->ns != OS64_HTML_NS_HTML ||
        node->tag != OS64_HTML_TAG_INPUT) return false;
    const os64_html_attr_t *type = os64_html_attr(node, "type");
    if (type == NULL) return false;
    const char *value = type->value;
    const char *file = "file";
    for (size_t i = 0; i < 4; i++) {
        char c = value[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != file[i]) return false;
    }
    return value[4] == '\0';
}

/* A document's element: its element child, which is the window's
 * document's `html` once the parser has seated it. Read from the tree
 * either way, so a document of its own has one too. */
const os64_html_node_t *d_document_element(const os64_html_node_t *document)
{
    const os64_html_node_t *at = document->first_child;
    while (at != NULL && at->kind != OS64_HTML_ELEMENT) at = at->next;
    return at;
}

/* Its head or body (HTML § 3.1): the first such child of its element, when
 * that element is an HTML `html`; a frameset is a body. */
const os64_html_node_t *d_document_part(const os64_html_node_t *document, bool body)
{
    const os64_html_node_t *html = d_document_element(document);
    if (html == NULL || html->ns != OS64_HTML_NS_HTML || html->tag != OS64_HTML_TAG_HTML)
        return NULL;
    for (const os64_html_node_t *at = html->first_child; at != NULL; at = at->next)
        if (at->kind == OS64_HTML_ELEMENT && at->ns == OS64_HTML_NS_HTML &&
            (body ? at->tag == OS64_HTML_TAG_BODY || at->tag == OS64_HTML_TAG_FRAMESET :
                    at->tag == OS64_HTML_TAG_HEAD)) return at;
    return NULL;
}

/* A control's `type` (HTML § 4.10): an input's state, the keyword its
 * attribute names in any case or "text"; a button's, "submit" unless the
 * attribute says "reset" or "button"; a select's, by its `multiple`; a
 * textarea's, always "textarea". */
static JSValue control_type(JSContext *ctx, const os64_html_node_t *node)
{
    static const char *const kInput[] = {
        "hidden", "text", "search", "tel", "url", "email", "password", "date", "month", "week", "time",
        "datetime-local", "number", "range", "color", "checkbox", "radio", "file", "submit", "image",
        "reset", "button", NULL
    };
    static const char *const kButton[] = {"submit", "reset", "button", NULL};
    if (node->tag == OS64_HTML_TAG_TEXTAREA) return JS_NewString(ctx, "textarea");
    if (node->tag == OS64_HTML_TAG_SELECT)
        return JS_NewString(ctx, os64_html_attr(node, "multiple") != NULL ? "select-multiple" : "select-one");
    const char *const *keywords = node->tag == OS64_HTML_TAG_INPUT ? kInput : kButton;
    const os64_html_attr_t *type = os64_html_attr(node, "type");
    for (size_t i = 0; type != NULL && keywords[i] != NULL; i++) {
        const char *a = type->value, *b = keywords[i];
        while (*a != '\0' && *b != '\0' && (*a >= 'A' && *a <= 'Z' ? *a + ('a' - 'A') : *a) == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return JS_NewString(ctx, keywords[i]);
    }
    return JS_NewString(ctx, keywords == kInput ? "text" : "submit");
}

/* Setting it sets the attribute, as a reflected attribute does; a select's
 * and a textarea's are read-only and ignore it. */
static JSValue control_type_set(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                                JSValueConst value)
{
    if (node->tag == OS64_HTML_TAG_TEXTAREA || node->tag == OS64_HTML_TAG_SELECT) return JS_UNDEFINED;
    DString string = {0};
    if (!d_string(dom, ctx, value, &string)) return JS_EXCEPTION;
    int64_t status = d_set_attr(dom, node, "type", string.data, string.length, false);
    d_string_free(dom, &string);
    return status < 0 ? d_html_error(ctx, status) : JS_UNDEFINED;
}

static JSValue property(JSContext *ctx, JSValueConst self, int argc,
                        JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *node = d_node(dom, ctx, self);
    if (node == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (magic >= D_TEXT_CONTENT && magic <= D_OUTER_HTML)
        return set ? d_content_set(dom, ctx, node, magic, value) : d_content_get(dom, ctx, node, magic);
    if (magic == D_ID || magic == D_CLASS) {
        if (node->kind != OS64_HTML_ELEMENT) return JS_ThrowTypeError(ctx, "Attribute property requires an element");
        const char *name = magic == D_ID ? "id" : "class";
        if (!set) {
            const os64_html_attr_t *attr = os64_html_attr(node, name);
            return JS_NewString(ctx, attr != NULL ? attr->value : "");
        }
        DString string = {0};
        if (!d_string(dom, ctx, value, &string)) return JS_EXCEPTION;
        int64_t status = d_set_attr(dom, node, name, string.data, string.length, false);
        d_string_free(dom, &string);
        return status < 0 ? d_html_error(ctx, status) : JS_UNDEFINED;
    }
    if (magic == D_VALUE) {
        if (set) {
            DString string = {0};
            JSValue empty = JS_UNDEFINED;
            if (JS_IsNull(value) && node->kind == OS64_HTML_ELEMENT && node->ns == OS64_HTML_NS_HTML &&
                (node->tag == OS64_HTML_TAG_INPUT || node->tag == OS64_HTML_TAG_TEXTAREA)) {
                empty = JS_NewString(ctx, "");
                if (JS_IsException(empty)) return empty;
                value = empty;
            }
            bool converted = d_string(dom, ctx, value, &string);
            JS_FreeValue(ctx, empty);
            if (!converted) return JS_EXCEPTION;
            if (string.length != 0 && filename_input(node)) {
                d_string_free(dom, &string);
                return d_error(ctx, "InvalidStateError", "A filename input accepts only an empty value");
            }
            int64_t status = os64_page_node_set_value(dom->state, node, string.data, string.length);
            d_string_free(dom, &string);
            return status < 0 ? d_page_error(ctx, status) : JS_UNDEFINED;
        }
        const char *text; size_t length;
        int64_t status = os64_page_node_value(dom->state, node, &text, &length);
        return status < 0 ? d_page_error(ctx, status) : JS_NewStringLen(ctx, text, length);
    }
    if (magic == D_CHECKED) {
        bool on;
        int64_t status;
        if (set) {
            int boolean = JS_ToBool(ctx, value);
            if (boolean < 0) return JS_EXCEPTION;
            status = os64_page_node_set_checked(dom->state, node, boolean != 0);
        } else status = os64_page_node_checked(dom->state, node, &on);
        return status < 0 ? d_page_error(ctx, status) : set ? JS_UNDEFINED : JS_NewBool(ctx, on);
    }
    if (magic == D_CONTROL_TYPE) {
        if (node->kind != OS64_HTML_ELEMENT || node->ns != OS64_HTML_NS_HTML ||
            (node->tag != OS64_HTML_TAG_INPUT && node->tag != OS64_HTML_TAG_BUTTON &&
             node->tag != OS64_HTML_TAG_SELECT && node->tag != OS64_HTML_TAG_TEXTAREA))
            return JS_ThrowTypeError(ctx, "type requires a form control");
        return set ? control_type_set(dom, ctx, node, value) : control_type(ctx, node);
    }
    if (magic == D_SELECTED_INDEX) {
        int32_t index;
        int64_t status;
        if (set) {
            if (JS_ToInt32(ctx, &index, value) < 0) return JS_EXCEPTION;
            status = os64_page_node_set_selected_index(dom->state, node, index);
        } else status = os64_page_node_selected_index(dom->state, node, &index);
        return status < 0 ? d_page_error(ctx, status) : set ? JS_UNDEFINED : JS_NewInt32(ctx, index);
    }
    switch (magic) {
    case D_PARENT: return d_wrap(dom, ctx, node->parent);
    case D_PARENT_ELEMENT: return d_wrap(dom, ctx, node->parent != NULL &&
                                        node->parent->kind == OS64_HTML_ELEMENT ? node->parent : NULL);
    case D_FIRST: return d_wrap(dom, ctx, node->first_child);
    case D_LAST: return d_wrap(dom, ctx, node->last_child);
    case D_PREV: return d_wrap(dom, ctx, node->prev);
    case D_NEXT: return d_wrap(dom, ctx, node->next);
    case D_OWNER: return d_wrap(dom, ctx, node->kind == OS64_HTML_DOCUMENT ? NULL : d_node_document(dom, node));
    case D_CHILD_NODES: return d_children(dom, ctx, node, false);
    case D_CHILDREN: return d_children(dom, ctx, node, true);
    case D_FIRST_ELEMENT: return d_wrap(dom, ctx, element_sibling(node->first_child, true));
    case D_LAST_ELEMENT: return d_wrap(dom, ctx, element_sibling(node->last_child, false));
    case D_PREV_ELEMENT: return d_wrap(dom, ctx, element_sibling(node->prev, false));
    case D_NEXT_ELEMENT: return d_wrap(dom, ctx, element_sibling(node->next, true));
    case D_CHILD_ELEMENT_COUNT: {
        uint32_t count = 0;
        for (const os64_html_node_t *at = node->first_child; at != NULL; at = at->next)
            if (at->kind == OS64_HTML_ELEMENT) count++;
        return JS_NewUint32(ctx, count);
    }
    case D_TYPE: {
        static const int types[] = {9, 11, 10, 1, 3, 8};
        return JS_NewInt32(ctx, types[node->kind]);
    }
    case D_NAME: return node_name(dom, ctx, node);
    case D_TAG:
        if (node->kind != OS64_HTML_ELEMENT) return JS_ThrowTypeError(ctx, "tagName requires an element");
        return node_name(dom, ctx, node);
    case D_DOCUMENT_ELEMENT: case D_HEAD: case D_BODY:
        if (node->kind != OS64_HTML_DOCUMENT) return JS_ThrowTypeError(ctx, "Document getter requires a document");
        if (set) return d_body_set(dom, ctx, node, value);
        return d_wrap(dom, ctx, magic == D_DOCUMENT_ELEMENT ? d_document_element(node) :
                               d_document_part(node, magic == D_BODY));
    default: return JS_ThrowTypeError(ctx, "Unknown DOM property");
    }
}

static const os64_html_node_t *walk_next(const os64_html_node_t *root,
                                         const os64_html_node_t *node)
{
    if (node->first_child != NULL) return node->first_child;
    while (node != root) {
        if (node->next != NULL) return node->next;
        node = node->parent;
    }
    return NULL;
}

static JSValue attribute(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *node,
                          int argc, JSValueConst *argv, int magic)
{
    if (node->kind != OS64_HTML_ELEMENT || argc < (magic == D_SET_ATTR ? 2 : 1))
        return JS_ThrowTypeError(ctx, "Attribute method requires an element and arguments");
    DString name = {0}, value = {0};
    if (!d_string(dom, ctx, argv[0], &name)) return JS_EXCEPTION;
    if (node->ns == OS64_HTML_NS_HTML) d_fold(name.data);
    if (magic == D_SET_ATTR && !d_string(dom, ctx, argv[1], &value)) {
        d_string_free(dom, &name);
        return JS_EXCEPTION;
    }
    if (magic == D_SET_ATTR && !d_attribute_name_valid(name.data)) {
        d_string_free(dom, &value);
        d_string_free(dom, &name);
        return d_error(ctx, "InvalidCharacterError", "Invalid attribute name");
    }
    JSValue result;
    if (magic == D_GET_ATTR || magic == D_HAS_ATTR) {
        const os64_html_attr_t *attr = os64_html_attr(node, name.data);
        result = magic == D_HAS_ATTR ? JS_NewBool(ctx, attr != NULL) :
                 attr != NULL ? JS_NewString(ctx, attr->value) : JS_NULL;
    } else if (magic == D_REMOVE_ATTR && os64_html_attr(node, name.data) == NULL) {
        /* Reading/removing an absent qualified name does not validate it. */
        result = JS_UNDEFINED;
    } else if (d_handler_attribute_set(dom, ctx, node, name.data) < 0) {
        /* An on<type> attribute's handler slot takes its place among the
         * listeners now, before the attribute it will read exists. */
        result = JS_EXCEPTION;
    } else {
        int64_t status = d_set_attr(dom, node, name.data, value.data, value.length, magic == D_REMOVE_ATTR);
        result = status < 0 ? d_html_error(ctx, status) : JS_UNDEFINED;
    }
    d_string_free(dom, &value);
    d_string_free(dom, &name);
    return result;
}

static JSValue method(JSContext *ctx, JSValueConst self, int argc,
                      JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *node = d_node(dom, ctx, self);
    if (node == NULL) return JS_EXCEPTION;
    if (magic >= D_GET_ATTR && magic <= D_HAS_ATTR)
        return attribute(dom, ctx, node, argc, argv, magic);
    if (magic == D_HAS_CHILDREN) return JS_NewBool(ctx, node->first_child != NULL);
    if (magic <= D_REPLACE) {
        int needed = magic == D_INSERT || magic == D_REPLACE ? 2 : 1;
        if (argc < needed) return JS_ThrowTypeError(ctx, "Missing node argument");
        /* An Attr is a Node no tree holds (attr.c). Both arguments are
         * WebIDL's Nodes, insertBefore's reference a nullable one; then
         * DOM's steps: a reference that is not this parent's child (an Attr
         * never is) is NotFoundError, as is removing one, and an Attr to be
         * inserted is HierarchyRequestError. */
        if (d_is_attr(dom, argv[0]) || (needed == 2 && d_is_attr(dom, argv[1]))) {
            if (!d_is_attr(dom, argv[0]) && d_node(dom, ctx, argv[0]) == NULL) return JS_EXCEPTION;
            const os64_html_node_t *reference = NULL;
            if (needed == 2 && !d_is_attr(dom, argv[1]) &&
                !(magic == D_INSERT && (JS_IsNull(argv[1]) || JS_IsUndefined(argv[1])))) {
                reference = d_node(dom, ctx, argv[1]);
                if (reference == NULL) return JS_EXCEPTION;
            }
            if (magic == D_REMOVE || (needed == 2 && (d_is_attr(dom, argv[1]) ||
                                                      (reference != NULL && reference->parent != node))))
                return d_error(ctx, "NotFoundError", "Node is not a child of this parent");
            return d_error(ctx, "HierarchyRequestError", "An Attr has no place in the tree");
        }
        const os64_html_node_t *child = d_node(dom, ctx, argv[0]);
        if (child == NULL) return JS_EXCEPTION;
        const os64_html_node_t *other = NULL;
        if (needed == 2 && !(magic == D_INSERT && (JS_IsNull(argv[1]) || JS_IsUndefined(argv[1])))) {
            other = d_node(dom, ctx, argv[1]);
            if (other == NULL) return JS_EXCEPTION;
        }
        if (magic == D_REMOVE && child->parent != node)
            return d_error(ctx, "NotFoundError", "Node is not a child of this parent");
        JSValue result = d_wrap(dom, ctx, magic == D_REPLACE ? other : child);
        if (JS_IsException(result)) return result;
        if (d_tree_change(dom, ctx, magic, node, child, other) < 0) { JS_FreeValue(ctx, result); return JS_EXCEPTION; }
        return result;
    }
    if (magic == D_CLONE) {
        int deep = argc > 0 ? JS_ToBool(ctx, argv[0]) : 0;
        if (deep < 0) return JS_EXCEPTION;
        int64_t status;
        DValue *entry = wrapper_prepare(dom, ctx, node, JS_UNDEFINED);
        if (entry == NULL) return JS_EXCEPTION;
        os64_html_node_t *copy = os64_page_node_clone(dom->state, node, deep != 0, &status);
        /* HTML's cloning steps carry a script's ALREADY STARTED to its copy,
         * and DOM's give the copy the original's node document. */
        if (copy != NULL && d_script_copy_marks(dom, node, copy) >= 0) {
            JSValue made = wrapper_attach(dom, ctx, entry, copy);
            d_made_in(dom, copy, d_node_document(dom, node));
            return made;
        }
        if (copy != NULL) status = OS64_HTML_NO_MEMORY;
        dom->values = entry->next;
        dom->retained--;
        JS_FreeValue(ctx, entry->value);
        d_free(dom, entry);
        return d_html_error(ctx, status);
    }
    if (magic == D_QUERY_ONE || magic == D_QUERY_ALL) {
        if (argc < 1 || (node->kind != OS64_HTML_DOCUMENT && node->kind != OS64_HTML_ELEMENT &&
                         node->kind != OS64_HTML_FRAGMENT))
            return JS_ThrowTypeError(ctx, "Query requires a supported receiver and argument");
        return d_select(dom, ctx, node, argv[0], magic == D_QUERY_ALL);
    }
    if (magic == D_GET_TAG || magic == D_GET_ID) {
        if (argc < 1 || (magic == D_GET_TAG ?
            node->kind != OS64_HTML_DOCUMENT && node->kind != OS64_HTML_ELEMENT :
            node->kind != OS64_HTML_DOCUMENT && node->kind != OS64_HTML_FRAGMENT))
            return JS_ThrowTypeError(ctx, "Query requires a supported receiver and argument");
        DString string = {0};
        if (!d_string(dom, ctx, argv[0], &string)) return JS_EXCEPTION;
        JSValue result = JS_NULL;
        if (magic == D_GET_TAG) result = d_collection(dom, ctx, node, true, true, string.data);
        else if (string.length != 0) {
            for (const os64_html_node_t *at = node->first_child; at != NULL; at = walk_next(node, at)) {
                const os64_html_attr_t *id = os64_html_attr(at, "id");
                if (at->kind == OS64_HTML_ELEMENT && id != NULL && os64_strcmp(id->value, string.data) == 0) {
                    result = d_wrap(dom, ctx, at);
                    break;
                }
            }
        }
        d_string_free(dom, &string);
        return result;
    }
    if (node->kind != OS64_HTML_DOCUMENT) return JS_ThrowTypeError(ctx, "Creation requires a document");
    if (magic != D_CREATE_FRAGMENT && argc < 1) return JS_ThrowTypeError(ctx, "Missing creation argument");
    DString string = {0};
    if (magic != D_CREATE_FRAGMENT && !d_string(dom, ctx, argv[0], &string)) return JS_EXCEPTION;
    /* An HTML document folds an element's name; an XML document keeps it,
     * and makes HTML elements only when it was made for that namespace
     * (DOM § 4.5's createElement). One that was not would make an element
     * in no namespace, which this tree cannot hold. */
    int type = d_document_type(dom, node);
    if (magic == D_CREATE_ELEMENT) {
        if (!d_element_name_valid(string.data)) {
            d_string_free(dom, &string);
            return d_error(ctx, "InvalidCharacterError", "Invalid element name");
        }
        if (type == D_DOCUMENT_XML || type == D_DOCUMENT_SVG) {
            d_string_free(dom, &string);
            return d_error(ctx, "NotSupportedError",
                           "This XML document's createElement would make an element in no namespace, "
                           "which this browser's documents cannot hold");
        }
        if (type == D_DOCUMENT_HTML) d_fold(string.data);
    }
    int64_t status;
    os64_html_node_t *created = magic == D_CREATE_ELEMENT ?
        os64_html_create_element(dom->document, OS64_HTML_NS_HTML, string.data, &status) :
        magic == D_CREATE_TEXT ? os64_html_create_text(dom->document, string.data, string.length, &status) :
        magic == D_CREATE_COMMENT ? os64_html_create_comment(dom->document, string.data, string.length, &status) :
        os64_html_create_fragment(dom->document, &status);
    d_string_free(dom, &string);
    if (created == NULL) return d_html_error(ctx, status);
    JSValue value = d_wrap(dom, ctx, created);
    if (!JS_IsException(value)) d_made_in(dom, created, node);
    return value;
}

static uint64_t property_prototypes(int magic)
{
    switch (magic) {
    case D_CHILDREN: case D_CHILD_ELEMENT_COUNT: case D_FIRST_ELEMENT: case D_LAST_ELEMENT:
        return D_KIND(D_PROTO_DOCUMENT) | D_KIND(D_PROTO_ELEMENT) | D_KIND(D_PROTO_FRAGMENT);
    case D_PREV_ELEMENT: case D_NEXT_ELEMENT:
        return D_KIND(D_PROTO_ELEMENT) | D_KIND(D_PROTO_CHARACTER_DATA);
    case D_TAG: case D_ID: case D_CLASS: case D_INNER_HTML: case D_OUTER_HTML:
        return D_KIND(D_PROTO_ELEMENT);
    case D_DOCUMENT_ELEMENT: case D_HEAD: case D_BODY:
        return D_KIND(D_PROTO_DOCUMENT);
    case D_DATA:
        return D_KIND(D_PROTO_CHARACTER_DATA);
    case D_VALUE:
        return D_KIND(D_PROTO_INPUT) | D_KIND(D_PROTO_TEXTAREA) | D_KIND(D_PROTO_SELECT) | D_KIND(D_PROTO_BUTTON);
    case D_CHECKED:
        return D_KIND(D_PROTO_INPUT);
    case D_SELECTED_INDEX:
        return D_KIND(D_PROTO_SELECT);
    case D_CONTROL_TYPE:
        return D_KIND(D_PROTO_INPUT) | D_KIND(D_PROTO_TEXTAREA) | D_KIND(D_PROTO_SELECT) | D_KIND(D_PROTO_BUTTON);
    default:
        return D_KIND(D_PROTO_NODE);
    }
}

static uint64_t method_prototypes(int magic)
{
    switch (magic) {
    case D_GET_ID: return D_KIND(D_PROTO_DOCUMENT) | D_KIND(D_PROTO_FRAGMENT);
    case D_GET_TAG: return D_KIND(D_PROTO_DOCUMENT) | D_KIND(D_PROTO_ELEMENT);
    case D_QUERY_ONE: case D_QUERY_ALL:
        return D_KIND(D_PROTO_DOCUMENT) | D_KIND(D_PROTO_ELEMENT) | D_KIND(D_PROTO_FRAGMENT);
    case D_CREATE_ELEMENT: case D_CREATE_TEXT: case D_CREATE_COMMENT: case D_CREATE_FRAGMENT:
        return D_KIND(D_PROTO_DOCUMENT);
    case D_GET_ATTR: case D_SET_ATTR: case D_REMOVE_ATTR: case D_HAS_ATTR:
        return D_KIND(D_PROTO_ELEMENT);
    default: return D_KIND(D_PROTO_NODE);
    }
}

int d_proto_parent(int kind)
{
    switch (kind) {
    case D_PROTO_NODE: return -1;
    case D_PROTO_DOCUMENT: case D_PROTO_ELEMENT: case D_PROTO_CHARACTER_DATA:
    case D_PROTO_FRAGMENT: case D_PROTO_DOCTYPE: case D_PROTO_ATTR:
        return D_PROTO_NODE;
    case D_PROTO_TEXT: case D_PROTO_COMMENT: return D_PROTO_CHARACTER_DATA;
    case D_PROTO_HTML_ELEMENT: case D_PROTO_SVG: return D_PROTO_ELEMENT;
    default: return D_PROTO_HTML_ELEMENT;
    }
}

int d_node_install(os64_dom_t *dom, JSContext *ctx)
{
    if (!d_context(dom, ctx, OS64_JS_ABI_ID)) return -1;
    // Each prototype is made after the one it inherits (d_proto_parent).
    // The registry retains these engine values through the same drain as
    // wrappers.
    for (int kind = 0; kind < D_PROTO_COUNT; kind++) {
        int parent = d_proto_parent(kind);
        JSValue value = kind == D_PROTO_NODE ? JS_NewObject(ctx) :
            JS_NewObjectProto(ctx, dom->prototypes[parent]->value);
        if (JS_IsException(value)) return -1;
        dom->prototypes[kind] = d_retain(dom, ctx, value);
        if (dom->prototypes[kind] == NULL) return -1;
    }
    static const struct { const char *name; int magic; bool writable; } properties[] = {
        {"parentNode", D_PARENT, false}, {"parentElement", D_PARENT_ELEMENT, false},
        {"firstChild", D_FIRST, false}, {"lastChild", D_LAST, false},
        {"previousSibling", D_PREV, false}, {"nextSibling", D_NEXT, false},
        {"ownerDocument", D_OWNER, false}, {"childNodes", D_CHILD_NODES, false},
        {"children", D_CHILDREN, false}, {"childElementCount", D_CHILD_ELEMENT_COUNT, false},
        {"firstElementChild", D_FIRST_ELEMENT, false}, {"lastElementChild", D_LAST_ELEMENT, false},
        {"previousElementSibling", D_PREV_ELEMENT, false}, {"nextElementSibling", D_NEXT_ELEMENT, false},
        {"nodeType", D_TYPE, false}, {"nodeName", D_NAME, false}, {"tagName", D_TAG, false},
        {"documentElement", D_DOCUMENT_ELEMENT, false}, {"head", D_HEAD, false}, {"body", D_BODY, true},
        {"textContent", D_TEXT_CONTENT, true}, {"nodeValue", D_NODE_VALUE, true}, {"data", D_DATA, true},
        {"innerHTML", D_INNER_HTML, true}, {"outerHTML", D_OUTER_HTML, false},
        {"id", D_ID, true}, {"className", D_CLASS, true}, {"value", D_VALUE, true},
        {"checked", D_CHECKED, true}, {"selectedIndex", D_SELECTED_INDEX, true},
        {"type", D_CONTROL_TYPE, true}
    };
    static const struct { const char *name; int magic, argc; } methods[] = {
        {"appendChild", D_APPEND, 1}, {"insertBefore", D_INSERT, 2},
        {"removeChild", D_REMOVE, 1}, {"replaceChild", D_REPLACE, 2}, {"cloneNode", D_CLONE, 0},
        {"getElementById", D_GET_ID, 1}, {"getElementsByTagName", D_GET_TAG, 1},
        {"querySelector", D_QUERY_ONE, 1}, {"querySelectorAll", D_QUERY_ALL, 1},
        {"createElement", D_CREATE_ELEMENT, 1}, {"createTextNode", D_CREATE_TEXT, 1},
        {"createComment", D_CREATE_COMMENT, 1}, {"createDocumentFragment", D_CREATE_FRAGMENT, 0},
        {"getAttribute", D_GET_ATTR, 1}, {"setAttribute", D_SET_ATTR, 2},
        {"removeAttribute", D_REMOVE_ATTR, 1}, {"hasAttribute", D_HAS_ATTR, 1},
        {"hasChildNodes", D_HAS_CHILDREN, 0}
    };
    for (int kind = 0; kind < D_PROTO_COUNT; kind++) {
        JSValueConst prototype = dom->prototypes[kind]->value;
        for (size_t i = 0; i < sizeof(properties) / sizeof(properties[0]); i++)
            if ((property_prototypes(properties[i].magic) & D_KIND(kind)) != 0 &&
                d_accessor(dom, ctx, prototype, properties[i].name, property,
                           properties[i].magic, properties[i].writable) < 0) return -1;
        for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
            if ((method_prototypes(methods[i].magic) & D_KIND(kind)) != 0 &&
                d_method(dom, ctx, prototype, methods[i].name, method, methods[i].argc, methods[i].magic) < 0) return -1;
    }
    return d_geometry_install(dom, ctx);
}
