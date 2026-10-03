#include "internal.h"

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

static bool matches(const DQuery *query, const os64_html_node_t *node)
{
    if (query->elements && node->kind != OS64_HTML_ELEMENT) return false;
    if (query->name == NULL || os64_strcmp(query->name, "*") == 0) return true;
    return node->kind == OS64_HTML_ELEMENT && node->name != NULL &&
        os64_strcmp(node->name, node->ns == OS64_HTML_NS_HTML ? query->folded : query->name) == 0;
}

/* Never replace an answer until its full successor fits. D6 must introduce
 * node holds for registry/query keys before it permits node reclamation. */
static bool refresh(DQuery *query, JSContext *ctx)
{
    if (!d_context(query->dom, ctx, OS64_JS_ABI_ID)) return false;
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
        if (matches(query, at)) nodes[index++] = at;
    const os64_html_node_t **old = query->nodes;
    query->nodes = nodes;
    query->count = count;
    query->version = version;
    d_free(query->dom, old);
    return true;
quota:
    d_error(ctx, "QuotaExceededError", "DOM collection quota exceeded");
    return false;
}

static void query_free(os64_dom_t *dom, DQuery *query)
{
    d_free(dom, query->nodes);
    d_free(dom, query->folded);
    d_free(dom, query->name);
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
    query->descendants = descendants;
    query->elements = elements;
    if (name != NULL) {
        size_t size = os64_strlen(name) + 1;
        query->name = d_alloc(dom, size);
        query->folded = d_alloc(dom, size);
        if (query->name == NULL || query->folded == NULL) {
            query_free(dom, query);
            return d_error(ctx, "QuotaExceededError", "DOM query-name quota exceeded");
        }
        os64_memcpy(query->name, name, size);
        os64_memcpy(query->folded, name, size);
        d_fold(query->folded);
    }
    JSValue object = JS_NewObjectClass(ctx, dom->collection_class);
    if (JS_IsException(object)) { query_free(dom, query); return object; }
    /* Set opaque only after native registry storage exists; a failed registry
     * allocation frees the engine object without a dangling query pointer. */
    DValue *entry = d_retain(dom, ctx, object);
    if (entry == NULL) { query_free(dom, query); return JS_EXCEPTION; }
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
static int index_key(JSContext *ctx, JSAtom atom, uint32_t *result)
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
    int numeric = index_key(ctx, atom, &index);
    if (numeric <= 0) return numeric;
    if (!refresh(query, ctx)) return -1;
    if (index >= query->count) return 0;
    if (desc != NULL) {
        JSValue value = d_wrap(query->dom, ctx, query->nodes[index]);
        if (JS_IsException(value)) return -1;
        desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
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
    int numeric = index_key(ctx, atom, &index);
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
    int numeric = index_key(ctx, atom, &index);
    if (numeric < 0) return -1;
    if (numeric == 0) return 1;
    if (!refresh(query, ctx)) return -1;
    return index < query->count ? 0 : 1;
}

static int prevent_extensions(JSContext *ctx, JSValueConst object)
{
    return query_receiver(ctx, object) != NULL ? 0 : -1;
}

static int own_names(JSContext *ctx, JSPropertyEnum **table, uint32_t *length, JSValueConst object)
{
    DQuery *query = query_receiver(ctx, object);
    if (query == NULL) return -1;
    if (!refresh(query, ctx)) return -1;
    if (query->count > UINT32_MAX || query->count > SIZE_MAX / sizeof(**table)) {
        d_error(ctx, "QuotaExceededError", "Too many collection entries");
        return -1;
    }
    JSPropertyEnum *entries = query->count != 0 ? js_malloc(ctx, query->count * sizeof(*entries)) : NULL;
    if (query->count != 0 && entries == NULL) return -1;
    uint32_t count = (uint32_t)query->count;
    for (uint32_t i = 0; i < count; i++) {
        entries[i].is_enumerable = true;
        entries[i].atom = JS_NewAtomUInt32(ctx, i);
        if (entries[i].atom == JS_ATOM_NULL) {
            while (i != 0) JS_FreeAtom(ctx, entries[--i].atom);
            js_free(ctx, entries);
            return -1;
        }
    }
    *table = entries;
    *length = count;
    return 0;
}

JSClassExoticMethods d_collection_exotic = {
    .get_own_property = own_property, .get_own_property_names = own_names,
    .define_own_property = define_property, .delete_property = delete_property,
    .prevent_extensions = prevent_extensions
};

int d_collection_install(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype)
{
    if (!d_context(dom, ctx, OS64_JS_ABI_ID)) return -1;
    if (d_accessor(dom, ctx, prototype, "length", collection_method, 0, false) < 0) return -1;
    return d_method(dom, ctx, prototype, "item", collection_method, 1, 1);
}
