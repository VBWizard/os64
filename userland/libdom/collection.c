#include "internal.h"
#include "garb/garb.h"
#include "garb/select.h"
#include "os64/fmt.h"

static const os64_html_node_t *query_next(const DQuery *query, const os64_html_node_t *node)
{
    if (!query->descendants) return node->next;
    if (node->first_child != NULL) return node->first_child;
    while (node != query->root) {
        if (node->next != NULL) return node->next;
        node = node->parent;
    }
    return NULL;
}

/* Whether `node`'s class attribute holds every class in `wanted`, a list
 * split at ASCII whitespace (DOM § getElementsByClassName); a quirks-mode
 * document compares them without regard to ASCII case. */
static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }
static bool same_class(const char *a, const char *b, size_t n, bool fold)
{
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (fold && x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (fold && y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return true;
}
bool d_has_token(const char *list, const char *token, size_t length, bool fold)
{
    for (const char *at = list; *at != '\0';) {
        while (is_space(*at)) at++;
        const char *end = at;
        while (*end != '\0' && !is_space(*end)) end++;
        if ((size_t)(end - at) == length && length != 0 && same_class(at, token, length, fold)) return true;
        at = end;
    }
    return false;
}
static bool has_classes(const os64_dom_t *dom, const os64_html_node_t *node, const char *wanted)
{
    const os64_html_attr_t *attr = node->kind == OS64_HTML_ELEMENT ? os64_html_attr(node, "class") : NULL;
    bool fold = dom->document->quirks == OS64_HTML_QUIRKS, any = false;
    for (const char *at = wanted; *at != '\0';) {
        while (is_space(*at)) at++;
        const char *end = at;
        while (*end != '\0' && !is_space(*end)) end++;
        if (end != at) {
            if (attr == NULL || !d_has_token(attr->value, at, (size_t)(end - at), fold)) return false;
            any = true;
        }
        at = end;
    }
    return any;
}

static bool matches(const DQuery *query, const os64_html_node_t *node)
{
    if (query->elements && node->kind != OS64_HTML_ELEMENT) return false;
    if (query->kind == D_QUERY_CLASSES) return has_classes(query->dom, node, query->named);
    if (query->kind == D_QUERY_NAME) {
        const os64_html_attr_t *name = node->kind == OS64_HTML_ELEMENT && node->ns == OS64_HTML_NS_HTML
            ? os64_html_attr(node, "name") : NULL;
        return name != NULL && os64_streq(name->value, query->named);
    }
    if (query->kind != D_QUERY_GENERIC && !d_classic_match(query->dom, query->owner != NULL ? query->owner : query->root, node, query->kind)) return false;
    if (query->named != NULL && !d_named_match(node, query->named)) return false;
    if (query->name == NULL || os64_strcmp(query->name, "*") == 0) return true;
    return node->kind == OS64_HTML_ELEMENT && node->name != NULL &&
        os64_strcmp(node->name, node->ns == OS64_HTML_NS_HTML ? query->folded : query->name) == 0;
}

/* Build and hold the full successor before releasing the previous answer:
 * a live query can retain detached nodes until its next successful refresh. */
static bool refresh(DQuery *query, JSContext *ctx)
{
    if (!d_context(query->dom, ctx, OS64_JS_ABI_ID)) return false;
    if (query->kind == D_QUERY_CONTROLS && query->owner != NULL) {
        const os64_html_node_t *root = query->owner;
        while (root->parent != NULL) root = root->parent;
        if (root != query->dom->document->document) root = query->owner;
        if (root != query->root) {
            os64_html_hold(query->dom->document, root);
            os64_html_release(query->dom->document, query->root);
            query->root = root;
            query->version = 0;
        }
    }
    uint64_t version = os64_html_version(query->dom->document);
    if (query->version == version) return true;
    size_t count = 0;
    for (const os64_html_node_t *at = query->root->first_child; at != NULL; at = query_next(query, at))
        if (matches(query, at)) count++;
    if (count > SIZE_MAX / sizeof(*query->nodes)) goto quota;
    const os64_html_node_t **nodes = count != 0 ? d_alloc(query->dom, count * sizeof(*nodes)) : NULL;
    if (count != 0 && nodes == NULL) goto quota;
    size_t index = 0;
    for (const os64_html_node_t *at = query->root->first_child; at != NULL; at = query_next(query, at))
        if (matches(query, at)) {
            os64_html_hold(query->dom->document, at);
            nodes[index++] = at;
        }
    const os64_html_node_t **old = query->nodes;
    size_t old_count = query->count;
    query->nodes = nodes;
    query->count = count;
    query->version = version;
    for (size_t i = 0; i < old_count; i++)
        os64_html_release(query->dom->document, old[i]);
    d_free(query->dom, old);
    return true;
quota:
    d_error(ctx, "QuotaExceededError", "DOM collection quota exceeded");
    return false;
}

void d_query_free(os64_dom_t *dom, DQuery *query)
{
    for (size_t i = 0; i < query->count; i++)
        os64_html_release(dom->document, query->nodes[i]);
    os64_html_release(dom->document, query->root);
    d_free(dom, query->nodes);
    d_free(dom, query->folded);
    d_free(dom, query->name);
    d_free(dom, query->named);
    d_free(dom, query);
}

JSValue d_collection(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *root,
                     bool descendants, bool elements, const char *name)
{
    if (!d_context(dom, ctx, OS64_JS_ABI_ID)) return JS_EXCEPTION;
    DQuery *query = d_alloc(dom, sizeof(*query));
    if (query == NULL) return d_error(ctx, "QuotaExceededError", "DOM query quota exceeded");
    query->dom = dom;
    query->root = root;
    os64_html_hold(dom->document, root);
    query->descendants = descendants;
    query->elements = elements;
    if (name != NULL) {
        size_t size = os64_strlen(name) + 1;
        query->name = d_alloc(dom, size);
        query->folded = d_alloc(dom, size);
        if (query->name == NULL || query->folded == NULL) {
            d_query_free(dom, query);
            return d_error(ctx, "QuotaExceededError", "DOM query-name quota exceeded");
        }
        os64_memcpy(query->name, name, size);
        os64_memcpy(query->folded, name, size);
        d_fold(query->folded);
    }
    JSValue object = JS_NewObjectClass(ctx, dom->collection_class);
    if (JS_IsException(object)) { d_query_free(dom, query); return object; }
    /* Set opaque only after native registry storage exists; a failed registry
     * allocation frees the engine object without a dangling query pointer. */
    DValue *entry = d_retain(dom, ctx, object);
    if (entry == NULL) { d_query_free(dom, query); return JS_EXCEPTION; }
    JS_SetOpaque(object, query);
    query->entry = entry;
    query->next = dom->queries;
    dom->queries = query;
    return JS_DupValue(ctx, object);
}

JSValue d_children(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *root,
                   bool elements)
{
    if (!d_context(dom, ctx, OS64_JS_ABI_ID)) return JS_EXCEPTION;
    DValue *entry = d_find(dom, root);
    if (entry == NULL) return JS_ThrowTypeError(ctx, "Missing node wrapper");
    DQuery *query = elements ? entry->children : entry->child_nodes;
    if (query != NULL) return JS_DupValue(ctx, query->entry->value);
    JSValue value = d_collection(dom, ctx, root, false, elements, NULL);
    if (JS_IsException(value)) return value;
    query = JS_GetOpaque(value, dom->collection_class);
    if (elements) entry->children = query;
    else entry->child_nodes = query;
    return value;
}

static JSValue collection_method(JSContext *ctx, JSValueConst self, int argc,
                                 JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DQuery *query = JS_GetOpaque(self, dom->collection_class);
    if (query == NULL) return JS_ThrowTypeError(ctx, "Expected a collection");
    if (magic == 2) {
        DString name = {0};
        if (argc < 1) return JS_ThrowTypeError(ctx, "Missing collection name");
        if (!d_string(dom, ctx, argv[0], &name)) return JS_EXCEPTION;
        JSValue result = JS_NULL;
        if (!refresh(query, ctx)) result = JS_EXCEPTION;
        else if (name.length != 0) for (size_t i = 0; i < query->count; i++)
            if (d_named_match(query->nodes[i], name.data)) { result = d_wrap(dom, ctx, query->nodes[i]); break; }
        d_string_free(dom, &name);
        return result;
    }
    uint32_t index = 0;
    if (magic != 0 && (argc < 1 || JS_ToUint32(ctx, &index, argv[0]) < 0))
        return argc < 1 ? JS_ThrowTypeError(ctx, "Missing collection index") : JS_EXCEPTION;
    if (!refresh(query, ctx)) return JS_EXCEPTION;
    if (magic == 0) return JS_NewFloat64(ctx, (double)query->count);
    return index < query->count ? d_wrap(dom, ctx, query->nodes[index]) : JS_NULL;
}

static DQuery *query_receiver(JSContext *ctx, JSValueConst object)
{
    JSClassID id;
    DQuery *query = JS_GetAnyOpaque(object, &id);
    if (query == NULL) { d_error(ctx, "InvalidStateError", "DOM collection is closed"); return NULL; }
    return d_context(query->dom, ctx, OS64_JS_ABI_ID) ? query : NULL;
}

/* Array-index names are canonical decimal integers below 2^32-1. Symbols,
 * signed numbers and leading-zero strings remain ordinary expando keys. */
int d_index_key(JSContext *ctx, JSAtom atom, uint32_t *result)
{
    JSValue key = JS_AtomToValue(ctx, atom);
    if (JS_IsException(key)) return -1;
    if (JS_IsSymbol(key)) { JS_FreeValue(ctx, key); return 0; }
    size_t length;
    const char *name = JS_ToCStringLen(ctx, &length, key);
    JS_FreeValue(ctx, key);
    if (name == NULL) return -1;
    uint64_t index = 0;
    bool numeric = length != 0 && !(name[0] == '0' && length != 1);
    for (size_t i = 0; numeric && i < length; i++) {
        if (name[i] < '0' || name[i] > '9' || index > (UINT32_MAX - 1u) / 10u) numeric = false;
        else {
            index = index * 10 + (unsigned)(name[i] - '0');
            if (index >= UINT32_MAX) numeric = false;
        }
    }
    JS_FreeCString(ctx, name);
    if (numeric) *result = (uint32_t)index;
    return numeric ? 1 : 0;
}

static int own_property(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst object, JSAtom atom)
{
    DQuery *query = query_receiver(ctx, object);
    if (query == NULL) return -1;
    uint32_t index;
    int numeric = d_index_key(ctx, atom, &index);
    if (numeric < 0) return -1;
    if (!refresh(query, ctx)) return -1;
    if (!numeric) {
        if (!query->elements) return 0;
        JSValue prototype = JS_GetPrototype(ctx, object);
        if (JS_IsException(prototype)) return -1;
        int reserved = JS_IsNull(prototype) ? 0 : JS_HasProperty(ctx, prototype, atom);
        JS_FreeValue(ctx, prototype);
        if (reserved != 0) return reserved < 0 ? -1 : 0;
        JSValue key = JS_AtomToValue(ctx, atom);
        if (JS_IsException(key)) return -1;
        if (!JS_IsString(key)) { JS_FreeValue(ctx, key); return 0; }
        const char *name = JS_ToCString(ctx, key);
        JS_FreeValue(ctx, key);
        if (name == NULL) return -1;
        index = (uint32_t)query->count;
        if (*name != '\0' && os64_strcmp(name,"length") && os64_strcmp(name,"item") && os64_strcmp(name,"namedItem"))
            for (size_t i = 0; i < query->count; i++) if (d_named_match(query->nodes[i], name)) { index = (uint32_t)i; break; }
        JS_FreeCString(ctx, name);
    }
    if (index >= query->count) return 0;
    if (desc != NULL) {
        JSValue value = d_wrap(query->dom, ctx, query->nodes[index]);
        if (JS_IsException(value)) return -1;
        desc->flags = JS_PROP_CONFIGURABLE | (numeric ? JS_PROP_ENUMERABLE : 0);
        desc->value = value;
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
    }
    return 1;
}

static int define_property(JSContext *ctx, JSValueConst object, JSAtom atom,
                            JSValueConst value, JSValueConst getter, JSValueConst setter, int flags)
{
    DQuery *query = query_receiver(ctx, object);
    if (query == NULL) return -1;
    uint32_t index;
    int numeric = d_index_key(ctx, atom, &index);
    if (numeric < 0) return -1;
    if (numeric != 0) {
        /* Let the engine interpret THROW/THROW_STRICT against an ordinary
         * nonextensible object, without creating a shadow of a live index. */
        return JS_DefineProperty(ctx, query->dom->index_guard->value, atom,
                                  value, getter, setter, flags | JS_PROP_NO_EXOTIC);
    }
    return JS_DefineProperty(ctx, object, atom, value, getter, setter, flags | JS_PROP_NO_EXOTIC);
}

static int delete_property(JSContext *ctx, JSValueConst object, JSAtom atom)
{
    DQuery *query = query_receiver(ctx, object);
    if (query == NULL) return -1;
    uint32_t index;
    int numeric = d_index_key(ctx, atom, &index);
    if (numeric < 0) return -1;
    if (numeric == 0) return 1;
    if (!refresh(query, ctx)) return -1;
    return index < query->count ? 0 : 1;
}

static int prevent_extensions(JSContext *ctx, JSValueConst object)
{
    return query_receiver(ctx, object) != NULL ? 0 : -1;
}

/* A live list's own names: its indices, 0 to `count`. */
int d_index_names(JSContext *ctx, size_t count, JSPropertyEnum **table, uint32_t *length)
{
    if (count > UINT32_MAX || count > SIZE_MAX / sizeof(**table)) {
        d_error(ctx, "QuotaExceededError", "Too many collection entries");
        return -1;
    }
    JSPropertyEnum *entries = count != 0 ? js_malloc(ctx, count * sizeof(*entries)) : NULL;
    if (count != 0 && entries == NULL) return -1;
    for (uint32_t i = 0; i < (uint32_t)count; i++) {
        entries[i].is_enumerable = true;
        entries[i].atom = JS_NewAtomUInt32(ctx, i);
        if (entries[i].atom == JS_ATOM_NULL) {
            while (i != 0) JS_FreeAtom(ctx, entries[--i].atom);
            js_free(ctx, entries);
            return -1;
        }
    }
    *table = entries;
    *length = (uint32_t)count;
    return 0;
}

static int own_names(JSContext *ctx, JSPropertyEnum **table, uint32_t *length, JSValueConst object)
{
    DQuery *query = query_receiver(ctx, object);
    if (query == NULL) return -1;
    if (!refresh(query, ctx)) return -1;
    return d_index_names(ctx, query->count, table, length);
}

JSClassExoticMethods d_collection_exotic = {
    .get_own_property = own_property, .get_own_property_names = own_names,
    .define_own_property = define_property, .delete_property = delete_property,
    .prevent_extensions = prevent_extensions
};

/* WebIDL's indexed iterables iterate as arrays do: `[Symbol.iterator]` IS
 * Array.prototype.values, and a NodeList's forEach, entries, keys and values
 * are Array.prototype's own. They read `length` and the indices, which every
 * list here answers. */
int d_iterable(JSContext *ctx, JSValueConst prototype, bool node_list)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue array = JS_GetPropertyStr(ctx, global, "Array");
    JSValue symbol = JS_GetPropertyStr(ctx, global, "Symbol");
    JSValue methods = JS_GetPropertyStr(ctx, array, "prototype");
    JSValue iterator = JS_GetPropertyStr(ctx, symbol, "iterator");
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, array);
    JS_FreeValue(ctx, symbol);
    int result = JS_IsException(methods) || JS_IsException(iterator) ? -1 : 0;
    JSAtom key = result == 0 ? JS_ValueToAtom(ctx, iterator) : JS_ATOM_NULL;
    JSValue values = result == 0 ? JS_GetPropertyStr(ctx, methods, "values") : JS_UNDEFINED;
    if (key == JS_ATOM_NULL || JS_IsException(values)) result = -1;
    if (result == 0)
        result = JS_DefinePropertyValue(ctx, prototype, key, JS_DupValue(ctx, values),
                                        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    static const char *const from_array[] = {"forEach", "entries", "keys", "values"};
    for (size_t i = 0; node_list && result >= 0 && i < sizeof(from_array) / sizeof(from_array[0]); i++) {
        JSValue method = JS_GetPropertyStr(ctx, methods, from_array[i]);
        result = JS_IsException(method) ? -1 :
            JS_DefinePropertyValueStr(ctx, prototype, from_array[i], method,
                                      JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    }
    if (key != JS_ATOM_NULL) JS_FreeAtom(ctx, key);
    JS_FreeValue(ctx, values);
    JS_FreeValue(ctx, iterator);
    JS_FreeValue(ctx, methods);
    return result < 0 ? -1 : 0;
}

int d_collection_install(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype)
{
    if (!d_context(dom, ctx, OS64_JS_ABI_ID)) return -1;
    if (d_accessor(dom, ctx, prototype, "length", collection_method, 0, false) < 0) return -1;
    if (d_method(dom, ctx, prototype, "item", collection_method, 1, 1) < 0) return -1;
    if (d_iterable(ctx, prototype, false) < 0) return -1;
    return d_method(dom, ctx, prototype, "namedItem", collection_method, 1, 2);
}

/* A static NodeList's item(): its own index, or null. */
static JSValue static_item(JSContext *ctx, JSValueConst self, int argc,
                           JSValueConst *argv, int magic, JSValue *data)
{
    (void)magic;
    if (d_callback(ctx, data, OS64_JS_ABI_ID) == NULL) return JS_EXCEPTION;
    uint32_t index = 0, length = 0;
    if (argc < 1) return JS_ThrowTypeError(ctx, "Missing list index");
    JSValue count = JS_GetPropertyStr(ctx, self, "length");
    if (JS_IsException(count) || JS_ToUint32(ctx, &length, count) < 0 ||
        JS_ToUint32(ctx, &index, argv[0]) < 0) {
        JS_FreeValue(ctx, count);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, count);
    return index < length ? JS_GetPropertyUint32(ctx, self, index) : JS_NULL;
}

/* THE STATIC NODELIST querySelectorAll answers with: what matched when it
 * was asked, never refreshed. So it is an ordinary object, its elements
 * fixed own indices and its `length` fixed, owned by the engine like any
 * value a script makes. Kept in the registry as a live collection is, a
 * page asking in a loop would hold every answer it ever had until it left. */
int d_static_list_install(os64_dom_t *dom, JSContext *ctx)
{
    if (!d_context(dom, ctx, OS64_JS_ABI_ID)) return -1;
    JSValue prototype = JS_NewObject(ctx);
    dom->static_list = d_retain(dom, ctx, prototype);
    if (dom->static_list == NULL) return -1;
    if (d_method(dom, ctx, prototype, "item", static_item, 1, 0) < 0) return -1;
    return d_iterable(ctx, prototype, true);
}

/* Selectors API (DOM § ParentNode): the elements among `root`'s descendants,
 * in tree order, that some selector in the list matches, `root` being the
 * scoping root `:scope` names. A selector for a pseudo-element matches no
 * element. The first, or (`all`) a static NodeList of every one. */
/* A selector list from a script's string, into `parsed` (the caller frees
 * it). NULL with the exception thrown: a SyntaxError naming the text. */
static garb_selectors_t *selectors_of(os64_dom_t *dom, JSContext *ctx, JSValueConst text,
                                      garb_parsed_t *parsed)
{
    DString string = {0};
    if (!d_string(dom, ctx, text, &string)) return NULL;
    garb_status_t status = garb_parse_values(string.data, string.length, parsed);
    garb_selectors_t *list = status == GARB_OK && !parsed->incomplete
        ? garb_selectors_parse(parsed, parsed->values, parsed->nvalues, dom->document->quirks) : NULL;
    if (list == NULL) {
        bool short_of_memory = status != GARB_OK || parsed->incomplete;
        /* The message names the selector, so a page's record does. */
        size_t size = string.length + 32;
        char *message = short_of_memory ? NULL : d_alloc(dom, size);
        if (message != NULL)
            os64_snprintf(message, size, "'%s' is not a valid selector", string.data);
        if (message != NULL) d_error(ctx, "SyntaxError", message);
        else d_error(ctx, "QuotaExceededError", "selector quota exceeded");
        d_free(dom, message);
    }
    d_string_free(dom, &string);
    return list;
}

/* Whether some selector in the list, none for a pseudo-element, matches
 * `element` with `scope` as `:scope`. */
static bool any_matches(const garb_selectors_t *list, const os64_html_node_t *element,
                        const os64_html_node_t *scope)
{
    if (element->kind != OS64_HTML_ELEMENT) return false;
    for (int32_t i = 0; i < garb_selectors_count(list); i++)
        if (garb_selector_pseudo(list, i) == GARB_PSEUDO_NONE &&
            garb_selector_matches_in(list, i, element, scope)) return true;
    return false;
}

/* Element.matches (`closest` false): whether the element matches, as its
 * own scope. Element.closest: the element or its nearest ancestor element
 * that matches, `:scope` still the element asked; null if none does. */
JSValue d_selector_test(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *element,
                        JSValueConst text, bool closest)
{
    garb_parsed_t parsed = {0};
    garb_selectors_t *list = selectors_of(dom, ctx, text, &parsed);
    if (list == NULL) { garb_free(&parsed); return JS_EXCEPTION; }
    JSValue result = JS_FALSE;
    if (!closest) result = JS_NewBool(ctx, any_matches(list, element, element));
    else {
        result = JS_NULL;
        for (const os64_html_node_t *at = element; at != NULL && at->kind == OS64_HTML_ELEMENT; at = at->parent)
            if (any_matches(list, at, element)) { result = d_wrap(dom, ctx, at); break; }
    }
    garb_free(&parsed);
    return result;
}

JSValue d_select(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *root,
                 JSValueConst text, bool all)
{
    garb_parsed_t parsed = {0};
    garb_selectors_t *list = selectors_of(dom, ctx, text, &parsed);
    if (list == NULL) { garb_free(&parsed); return JS_EXCEPTION; }
    const os64_html_node_t *scope = root->kind == OS64_HTML_DOCUMENT ? NULL : root;
    JSValue result = all ? JS_NewObjectProto(ctx, dom->static_list->value) : JS_NULL;
    uint32_t found = 0;
    for (const os64_html_node_t *at = root->first_child; at != NULL && !JS_IsException(result);) {
        bool matched = any_matches(list, at, scope);
        if (matched && !all) {
            result = d_wrap(dom, ctx, at);
            break;
        }
        if (matched) {
            JSValue element = d_wrap(dom, ctx, at);
            if (JS_IsException(element) ||
                JS_DefinePropertyValueUint32(ctx, result, found++, element, JS_PROP_ENUMERABLE) < 0) {
                JS_FreeValue(ctx, result);
                result = JS_EXCEPTION;
                break;
            }
        }
        if (at->first_child != NULL) { at = at->first_child; continue; }
        while (at != root && at->next == NULL) at = at->parent;
        at = at == root ? NULL : at->next;
    }
    garb_free(&parsed);
    if (all && !JS_IsException(result) &&
        JS_DefinePropertyValueStr(ctx, result, "length", JS_NewUint32(ctx, found), 0) < 0) {
        JS_FreeValue(ctx, result);
        result = JS_EXCEPTION;
    }
    return result;
}

JSValue d_special_collection(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *root,
                             unsigned kind, const char *named)
{
    char *copy = NULL;
    if (named != NULL) {
        size_t len = os64_strlen(named) + 1;
        copy = d_alloc(dom, len);
        if (copy == NULL) return d_error(ctx,"QuotaExceededError","DOM named collection quota exceeded");
        os64_memcpy(copy, named, len);
    }
    JSValue result = d_collection(dom, ctx, root, true, true, NULL);
    if (JS_IsException(result)) { d_free(dom, copy); return result; }
    DQuery *query = JS_GetOpaque(result, dom->collection_class);
    query->kind = kind;
    query->named = copy;
    return result;
}

/* getElementsByClassName (`by_name` false) and getElementsByName: live
 * collections of the descendants whose class attribute holds every class
 * the string names, or whose name attribute is the string. */
JSValue d_attribute_collection(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *root,
                               JSValueConst text, bool by_name)
{
    DString string = {0};
    if (!d_string(dom, ctx, text, &string)) return JS_EXCEPTION;
    JSValue result = d_special_collection(dom, ctx, root, by_name ? D_QUERY_NAME : D_QUERY_CLASSES,
                                          string.data);
    d_string_free(dom, &string);
    return result;
}
