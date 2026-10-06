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

static int node_prototype_kind(const os64_html_node_t *node)
{
    if (node->kind == OS64_HTML_DOCUMENT) return D_PROTO_DOCUMENT;
    if (node->kind == OS64_HTML_FRAGMENT) return D_PROTO_FRAGMENT;
    if (node->kind == OS64_HTML_TEXT || node->kind == OS64_HTML_COMMENT)
        return D_PROTO_CHARACTER_DATA;
    if (node->kind != OS64_HTML_ELEMENT) return D_PROTO_NODE;
    if (node->ns == OS64_HTML_NS_HTML) {
        if (node->tag == OS64_HTML_TAG_INPUT) return D_PROTO_INPUT;
        if (node->tag == OS64_HTML_TAG_TEXTAREA) return D_PROTO_TEXTAREA;
        if (node->tag == OS64_HTML_TAG_SELECT) return D_PROTO_SELECT;
        if (node->tag == OS64_HTML_TAG_BUTTON) return D_PROTO_BUTTON;
        if (node->tag == OS64_HTML_TAG_FORM) return D_PROTO_FORM;
        if (node->tag == OS64_HTML_TAG_IMG) return D_PROTO_IMAGE;
    }
    return D_PROTO_ELEMENT;
}

static DValue *wrapper_prepare(os64_dom_t *dom, JSContext *ctx,
                                const os64_html_node_t *node)
{
    DValue *entry = d_alloc(dom, sizeof(*entry));
    if (entry == NULL) { d_error(ctx, "QuotaExceededError", "DOM wrapper quota exceeded"); return NULL; }
    JSValue value = JS_NewObjectProtoClass(ctx,
        dom->prototypes[node_prototype_kind(node)]->value, dom->node_class);
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
    if (!d_context(dom, ctx, OS64_JS_ABI_ID)) return JS_EXCEPTION;
    if (node == NULL) return JS_NULL;
    DValue *entry = d_find(dom, node);
    if (entry != NULL) return JS_DupValue(ctx, entry->value);
    entry = wrapper_prepare(dom, ctx, node);
    return entry != NULL ? wrapper_attach(dom, ctx, entry, node) : JS_EXCEPTION;
}

const os64_html_node_t *d_node(os64_dom_t *dom, JSContext *ctx, JSValueConst value)
{
    DValue *entry = JS_GetOpaque(value, dom->node_class);
    const os64_html_node_t *node = entry != NULL ? entry->node : NULL;
    if (node == NULL || !os64_html_owns_node(dom->document, node)) {
        JS_ThrowTypeError(ctx, "Expected a node from this document");
        return NULL;
    }
    return node;
}

int64_t d_insert(os64_dom_t *dom, os64_html_node_t *parent, os64_html_node_t *node,
                  os64_html_node_t *before)
{
    return os64_page_node_insert(dom->state, parent, node, before);
}
int64_t d_remove(os64_dom_t *dom, os64_html_node_t *node)
{
    return os64_page_node_remove(dom->state, node);
}
int64_t d_replace(os64_dom_t *dom, os64_html_node_t *parent, os64_html_node_t *node,
                   os64_html_node_t *old)
{
    return os64_page_node_replace(dom->state, parent, node, old);
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

static const os64_html_node_t *document_part(os64_dom_t *dom, bool body)
{
    const os64_html_node_t *html = dom->document->html;
    if (html == NULL || html->parent != dom->document->document ||
        html->ns != OS64_HTML_NS_HTML || html->tag != OS64_HTML_TAG_HTML)
        return NULL;
    for (const os64_html_node_t *at = html->first_child; at != NULL; at = at->next)
        if (at->kind == OS64_HTML_ELEMENT && at->ns == OS64_HTML_NS_HTML &&
            (body ? at->tag == OS64_HTML_TAG_BODY || at->tag == OS64_HTML_TAG_FRAMESET :
                    at->tag == OS64_HTML_TAG_HEAD)) return at;
    return NULL;
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
        int64_t status = os64_page_node_set_attr(dom->state, node, name, string.data, string.length, false);
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
    case D_OWNER: return d_wrap(dom, ctx, node->kind == OS64_HTML_DOCUMENT ? NULL : dom->document->document);
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
        return d_wrap(dom, ctx, magic == D_DOCUMENT_ELEMENT ?
                               (dom->document->html != NULL && dom->document->html->parent == dom->document->document ?
                                dom->document->html : NULL) :
                               document_part(dom, magic == D_BODY));
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
        int64_t status = os64_page_node_set_attr(dom->state, node, name.data, value.data,
                                                value.length, magic == D_REMOVE_ATTR);
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
        /* Scripts the move may connect are found before it, while a fragment
         * still holds its children, and handed to the host after it. */
        const os64_html_node_t *few[8], **scripts = few;
        size_t found = 0;
        if (magic != D_REMOVE && dom->options.script_connected != NULL) {
            found = d_script_collect(child, few, 8);
            if (found > 8) {
                scripts = d_alloc(dom, found * sizeof(*scripts));
                if (scripts == NULL) {
                    JS_FreeValue(ctx, result);
                    return d_error(ctx, "QuotaExceededError", "DOM script list quota exceeded");
                }
                d_script_collect(child, scripts, found);
            }
            for (size_t i = 0; i < found; i++) os64_html_hold(dom->document, scripts[i]);
        }
        int64_t status = magic == D_REMOVE ? d_remove(dom, (os64_html_node_t *)child) :
            magic == D_REPLACE ? d_replace(dom, (os64_html_node_t *)node, (os64_html_node_t *)child,
                                           (os64_html_node_t *)other) :
            d_insert(dom, (os64_html_node_t *)node, (os64_html_node_t *)child, (os64_html_node_t *)other);
        if (status >= 0) d_script_connected(dom, scripts, found);
        for (size_t i = 0; i < found; i++) os64_html_release(dom->document, scripts[i]);
        if (scripts != few) d_free(dom, scripts);
        if (status < 0) { JS_FreeValue(ctx, result); return d_html_error(ctx, status); }
        return result;
    }
    if (magic == D_CLONE) {
        int deep = argc > 0 ? JS_ToBool(ctx, argv[0]) : 0;
        if (deep < 0) return JS_EXCEPTION;
        int64_t status;
        DValue *entry = wrapper_prepare(dom, ctx, node);
        if (entry == NULL) return JS_EXCEPTION;
        os64_html_node_t *copy = os64_page_node_clone(dom->state, node, deep != 0, &status);
        /* HTML's cloning steps carry a script's ALREADY STARTED to its copy. */
        if (copy != NULL && d_script_copy_marks(dom, node, copy) >= 0)
            return wrapper_attach(dom, ctx, entry, copy);
        if (copy != NULL) status = OS64_HTML_NO_MEMORY;
        dom->values = entry->next;
        dom->retained--;
        JS_FreeValue(ctx, entry->value);
        d_free(dom, entry);
        return d_html_error(ctx, status);
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
    if (magic == D_CREATE_ELEMENT) {
        if (!d_element_name_valid(string.data)) {
            d_string_free(dom, &string);
            return d_error(ctx, "InvalidCharacterError", "Invalid element name");
        }
        d_fold(string.data);
    }
    int64_t status;
    os64_html_node_t *created = magic == D_CREATE_ELEMENT ?
        os64_html_create_element(dom->document, OS64_HTML_NS_HTML, string.data, &status) :
        magic == D_CREATE_TEXT ? os64_html_create_text(dom->document, string.data, string.length, &status) :
        magic == D_CREATE_COMMENT ? os64_html_create_comment(dom->document, string.data, string.length, &status) :
        os64_html_create_fragment(dom->document, &status);
    d_string_free(dom, &string);
    return created != NULL ? d_wrap(dom, ctx, created) : d_html_error(ctx, status);
}

static unsigned property_prototypes(int magic)
{
    switch (magic) {
    case D_CHILDREN: case D_CHILD_ELEMENT_COUNT: case D_FIRST_ELEMENT: case D_LAST_ELEMENT:
        return (1u << D_PROTO_DOCUMENT) | (1u << D_PROTO_ELEMENT) | (1u << D_PROTO_FRAGMENT);
    case D_PREV_ELEMENT: case D_NEXT_ELEMENT:
        return (1u << D_PROTO_ELEMENT) | (1u << D_PROTO_CHARACTER_DATA);
    case D_TAG: case D_ID: case D_CLASS: case D_INNER_HTML: case D_OUTER_HTML:
        return 1u << D_PROTO_ELEMENT;
    case D_DOCUMENT_ELEMENT: case D_HEAD: case D_BODY:
        return 1u << D_PROTO_DOCUMENT;
    case D_DATA:
        return 1u << D_PROTO_CHARACTER_DATA;
    case D_VALUE:
        return (1u << D_PROTO_INPUT) | (1u << D_PROTO_TEXTAREA) |
            (1u << D_PROTO_SELECT) | (1u << D_PROTO_BUTTON);
    case D_CHECKED:
        return 1u << D_PROTO_INPUT;
    case D_SELECTED_INDEX:
        return 1u << D_PROTO_SELECT;
    default:
        return 1u << D_PROTO_NODE;
    }
}

static unsigned method_prototypes(int magic)
{
    switch (magic) {
    case D_GET_ID: return (1u << D_PROTO_DOCUMENT) | (1u << D_PROTO_FRAGMENT);
    case D_GET_TAG: return (1u << D_PROTO_DOCUMENT) | (1u << D_PROTO_ELEMENT);
    case D_CREATE_ELEMENT: case D_CREATE_TEXT: case D_CREATE_COMMENT: case D_CREATE_FRAGMENT:
        return 1u << D_PROTO_DOCUMENT;
    case D_GET_ATTR: case D_SET_ATTR: case D_REMOVE_ATTR: case D_HAS_ATTR:
        return 1u << D_PROTO_ELEMENT;
    default: return 1u << D_PROTO_NODE;
    }
}

int d_node_install(os64_dom_t *dom, JSContext *ctx)
{
    if (!d_context(dom, ctx, OS64_JS_ABI_ID)) return -1;
    // Kind prototypes inherit Node; HTML-specific prototypes inherit Element.
    // The registry retains these engine values through the same drain as wrappers.
    for (int kind = 0; kind < D_PROTO_COUNT; kind++) {
        int parent = kind >= D_PROTO_INPUT ? D_PROTO_ELEMENT : D_PROTO_NODE;
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
        {"documentElement", D_DOCUMENT_ELEMENT, false}, {"head", D_HEAD, false}, {"body", D_BODY, false},
        {"textContent", D_TEXT_CONTENT, true}, {"nodeValue", D_NODE_VALUE, true}, {"data", D_DATA, true},
        {"innerHTML", D_INNER_HTML, true}, {"outerHTML", D_OUTER_HTML, false},
        {"id", D_ID, true}, {"className", D_CLASS, true}, {"value", D_VALUE, true},
        {"checked", D_CHECKED, true}, {"selectedIndex", D_SELECTED_INDEX, true}
    };
    static const struct { const char *name; int magic, argc; } methods[] = {
        {"appendChild", D_APPEND, 1}, {"insertBefore", D_INSERT, 2},
        {"removeChild", D_REMOVE, 1}, {"replaceChild", D_REPLACE, 2}, {"cloneNode", D_CLONE, 0},
        {"getElementById", D_GET_ID, 1}, {"getElementsByTagName", D_GET_TAG, 1},
        {"createElement", D_CREATE_ELEMENT, 1}, {"createTextNode", D_CREATE_TEXT, 1},
        {"createComment", D_CREATE_COMMENT, 1}, {"createDocumentFragment", D_CREATE_FRAGMENT, 0},
        {"getAttribute", D_GET_ATTR, 1}, {"setAttribute", D_SET_ATTR, 2},
        {"removeAttribute", D_REMOVE_ATTR, 1}, {"hasAttribute", D_HAS_ATTR, 1},
        {"hasChildNodes", D_HAS_CHILDREN, 0}
    };
    for (int kind = 0; kind < D_PROTO_COUNT; kind++) {
        JSValueConst prototype = dom->prototypes[kind]->value;
        for (size_t i = 0; i < sizeof(properties) / sizeof(properties[0]); i++)
            if ((property_prototypes(properties[i].magic) & (1u << kind)) != 0 &&
                d_accessor(dom, ctx, prototype, properties[i].name, property,
                           properties[i].magic, properties[i].writable) < 0) return -1;
        for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
            if ((method_prototypes(methods[i].magic) & (1u << kind)) != 0 &&
                d_method(dom, ctx, prototype, methods[i].name, method, methods[i].argc, methods[i].magic) < 0) return -1;
    }
    return d_geometry_install(dom, ctx);
}
