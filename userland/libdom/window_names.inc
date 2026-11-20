/* Window's named properties live between Window.prototype and EventTarget's
 * prototype (WebIDL § named properties object), not on the raw global. The
 * opaque DValue holds the ordinary class-string backing and its document
 * owner through grouped teardown; the exotic itself has no own engine slots. */
static JSClassID window_names_slot, global_prototype_slot;

bool d_window_named_match(const os64_html_node_t *node, const char *name)
{
    if (node->kind != OS64_HTML_ELEMENT || !*name) return false;
    const os64_html_attr_t *attr = os64_html_attr(node, "id");
    if (attr && os64_streq(attr->value, name)) return true;
    if (node->ns != OS64_HTML_NS_HTML ||
        (node->tag != OS64_HTML_TAG_EMBED && node->tag != OS64_HTML_TAG_FORM &&
         node->tag != OS64_HTML_TAG_IMG && node->tag != OS64_HTML_TAG_OBJECT)) return false;
    attr = os64_html_attr(node, "name");
    return attr && os64_streq(attr->value, name);
}

static int window_names_poll(os64_dom_t *dom, JSContext *ctx, size_t *steps)
{
    if (++*steps % 64) return 0;
    os64_js_outcome_t out;
    if (os64_js_check_budget(dom->runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK) return 0;
    d_error(ctx, "InvalidStateError", "Window name lookup requires a live script budget");
    return -1;
}

/* The first child with a name decides whether that name is supported. A
 * later same-origin duplicate cannot make an earlier foreign child visible. */
static DBrowsingContext *window_named_child(os64_dom_t *dom, const char *name, size_t length)
{
    DBrowsingContext *context = window_document_context(dom);
    for (DBrowsingContext *at = context ? context->first_child : NULL; at; at = at->next_sibling)
        if (at->name_length == length && !os64_memcmp(at->name, name, length)) return at;
    return NULL;
}

static int window_named_elements(os64_dom_t *dom, JSContext *ctx, const char *name, size_t length,
    const os64_html_node_t **first, bool *multiple)
{
    const os64_html_node_t *root = dom->document->document;
    size_t steps = 0;
    *first = NULL; *multiple = false;
    /* Native DOM attribute storage is NUL-terminated. A key with an embedded
     * NUL may match a context name, but must not alias an attribute prefix. */
    if (os64_strlen(name) != length) return 0;
    for (const os64_html_node_t *at = root->first_child; at; at = d_walk_next(root, at)) {
        if (window_names_poll(dom, ctx, &steps) < 0) return -1;
        if (!d_window_named_match(at, name)) continue;
        if (*first) { *multiple = true; break; }
        *first = at;
    }
    return 0;
}

/* Visibility inspects own descriptors, never getters. Named-properties objects
 * on the prototype chain are skipped; asking them again would recurse through
 * this same algorithm. Retain each prototype while a host-injected Proxy may
 * reenter, mutate the document or replace a browsing context. */
static int window_name_visible(os64_dom_t *dom, JSContext *ctx, JSAtom key)
{
    int own = JS_GetOwnProperty(ctx, NULL, dom->window->value, key);
    if (own != 0) return own < 0 ? -1 : 0;
    JSValue at = JS_GetPrototype(ctx, dom->window->value);
    size_t steps = 0;
    while (!JS_IsNull(at)) {
        if (JS_IsException(at)) return -1;
        if (window_names_poll(dom, ctx, &steps) < 0) { JS_FreeValue(ctx, at); return -1; }
        if (!JS_GetOpaque(at, os64_js_class_id(&window_names_slot))) {
            own = JS_GetOwnProperty(ctx, NULL, at, key);
            if (own != 0) { JS_FreeValue(ctx, at); return own < 0 ? -1 : 0; }
        }
        JSValue next = JS_GetPrototype(ctx, at);
        JS_FreeValue(ctx, at); at = next;
    }
    return 1;
}

static int window_names_get_own(JSContext *ctx, JSPropertyDescriptor *desc,
    JSValueConst object, JSAtom key)
{
    DValue *backing = JS_GetOpaque(object, os64_js_class_id(&window_names_slot));
    if (!backing || !d_context(backing->dom, ctx, OS64_JS_ABI_ID)) return -1;
    int own = JS_GetOwnProperty(ctx, desc, backing->value, key);
    if (own != 0) return own;
    JSValue text = JS_AtomToValue(ctx, key);
    if (JS_IsException(text)) return -1;
    if (JS_IsSymbol(text)) { JS_FreeValue(ctx, text); return 0; }
    size_t length;
    const char *name = JS_ToCStringLen(ctx, &length, text);
    JS_FreeValue(ctx, text);
    if (!name) return -1;
    if (!length) { JS_FreeCString(ctx, name); return 0; }
    os64_dom_t *dom = backing->dom;
    const os64_html_node_t *first;
    bool multiple;
    int result = window_named_elements(dom, ctx, name, length, &first, &multiple);
    if (result < 0) goto done;
    DBrowsingContext *child = window_named_child(dom, name, length);
    if (!first && (!child || !way_origin_same(dom->binding_origin, child->window->binding_origin))) goto done;
    result = window_name_visible(dom, ctx, key);
    if (result <= 0) goto done;
    /* Descriptor/prototype traps can change membership. Re-resolve before
     * wrapping a node or returning a context's stable proxy. */
    result = window_named_elements(dom, ctx, name, length, &first, &multiple);
    if (result < 0) goto done;
    child = window_named_child(dom, name, length);
    if (!first && (!child || !way_origin_same(dom->binding_origin, child->window->binding_origin))) goto done;
    result = 1;
    if (desc) {
        JSValue value = child ? d_window_proxy(ctx, child) : multiple
            ? d_special_collection(dom, ctx, dom->document->document, D_QUERY_WINDOW_NAMES, name)
            : d_wrap(dom, ctx, first);
        if (JS_IsException(value)) { result = -1; goto done; }
        *desc = (JSPropertyDescriptor){.flags = JS_PROP_C_W_E,
            .value = value, .getter = JS_UNDEFINED, .setter = JS_UNDEFINED};
    }
done:
    JS_FreeCString(ctx, name);
    return result;
}

static int window_names_keys(JSContext *ctx, JSPropertyEnum **keys, uint32_t *count, JSValueConst object)
{
    DValue *backing = JS_GetOpaque(object, os64_js_class_id(&window_names_slot));
    if (!backing || !d_context(backing->dom, ctx, OS64_JS_ABI_ID)) return -1;
    /* OrdinaryOwnPropertyKeys sees the class string, not virtual DOM names. */
    return JS_GetOwnPropertyNames(ctx, keys, count, backing->value, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK);
}

static int window_names_define(JSContext *ctx, JSValueConst object, JSAtom key,
    JSValueConst value, JSValueConst getter, JSValueConst setter, int flags)
{
    DValue *backing = JS_GetOpaque(object, os64_js_class_id(&window_names_slot));
    if (!backing || !d_context(backing->dom, ctx, OS64_JS_ABI_ID)) return -1;
    /* Delegate refusal to the empty nonextensible guard so the engine honors
     * Object.defineProperty's throw flag and strict/sloppy assignment modes. */
    return JS_DefineProperty(ctx, backing->dom->index_guard->value, key, value, getter, setter, flags);
}

static int window_names_delete(JSContext *ctx, JSValueConst object, JSAtom key)
{
    (void)ctx; (void)object; (void)key;
    return 0;
}

static int window_names_prevent(JSContext *ctx, JSValueConst object)
{
    (void)ctx; (void)object;
    return 0;
}

static int global_immutable_prototype(JSContext *ctx, JSValueConst object, JSValueConst prototype)
{
    JSValue current = JS_GetPrototype(ctx, object);
    if (JS_IsException(current)) return -1;
    bool same = JS_IsNull(current) ? JS_IsNull(prototype) :
        JS_IsObject(prototype) && JS_VALUE_GET_PTR(current) == JS_VALUE_GET_PTR(prototype);
    JS_FreeValue(ctx, current);
    return same;
}

static JSClassExoticMethods window_names_methods = {
    .get_own_property = window_names_get_own, .get_own_property_names = window_names_keys,
    .define_own_property = window_names_define, .delete_property = window_names_delete,
    .set_prototype = global_immutable_prototype, .prevent_extensions = window_names_prevent
};
static JSClassExoticMethods global_prototype_methods = {.set_prototype = global_immutable_prototype};

JSValue d_global_prototype(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype)
{
    if (!dom->binding_origin)
        return JS_IsUndefined(prototype) ? JS_NewObject(ctx) : JS_NewObjectProto(ctx, prototype);
    JSClassID id = os64_js_class_id(&global_prototype_slot);
    const JSClassDef definition = {.class_name = "Object", .exotic = &global_prototype_methods};
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, &definition) < 0)
        return JS_ThrowOutOfMemory(ctx);
    if (!JS_IsUndefined(prototype)) return JS_NewObjectProtoClass(ctx, prototype, id);
    JSValue ordinary = JS_NewObject(ctx);
    if (JS_IsException(ordinary)) return ordinary;
    JSValue parent = JS_GetPrototype(ctx, ordinary);
    JS_FreeValue(ctx, ordinary);
    JSValue result = JS_NewObjectProtoClass(ctx, parent, id);
    JS_FreeValue(ctx, parent);
    return result;
}

JSValue d_window_named_prototype(os64_dom_t *dom, JSContext *ctx, JSValueConst parent)
{
    if (!dom->binding_origin) return JS_NewObjectProto(ctx, parent);
    JSClassID id = os64_js_class_id(&window_names_slot);
    const JSClassDef definition = {.class_name = "WindowProperties", .exotic = &window_names_methods};
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, &definition) < 0)
        return JS_ThrowOutOfMemory(ctx);
    JSValue ordinary = JS_NewObjectProto(ctx, JS_NULL);
    if (JS_IsException(ordinary)) return ordinary;
    if (d_string_tag(ctx, ordinary, "WindowProperties") < 0) { JS_FreeValue(ctx, ordinary); return JS_EXCEPTION; }
    DValue *backing = d_retain(dom, ctx, ordinary);
    if (!backing) return JS_EXCEPTION;
    JSValue names = JS_NewObjectProtoClass(ctx, parent, id);
    if (JS_IsException(names)) return names;
    JS_SetOpaque(names, backing);
    JSValue result = d_global_prototype(dom, ctx, names);
    JS_FreeValue(ctx, names);
    return result;
}
