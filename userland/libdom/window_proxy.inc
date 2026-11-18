/* Private native WindowProxy surface. The context keeps the last Window on
 * discard; document replacement changes that Window, not the proxy identity.
 * Roots and native records belong to the retained cluster, not JS finalizers. */
static JSClassID window_proxy_slot;

typedef struct DWindowDescriptor {
    struct DWindowDescriptor *next;
    os64_dom_t *source, *target;
    unsigned key;
    DValue *parts;
} DWindowDescriptor;

typedef struct DWindowProxy {
    os64_dom_t *owner;
    DBrowsingContext *context;
    JSAtom keys[17];
    DWindowDescriptor *cache;
} DWindowProxy;

static const char *const window_cross_names[] = {
    "window", "self", "location", "close", "closed", "focus", "blur", "frames",
    "length", "top", "opener", "parent", "postMessage", "then",
    "toStringTag", "hasInstance", "isConcatSpreadable"
};

os64_dom_t *d_window_proxy_target(JSValueConst value)
{
    DWindowProxy *proxy = JS_GetOpaque(value, os64_js_class_id(&window_proxy_slot));
    return proxy ? proxy->context->window : NULL;
}

JSValue d_window_identity(os64_dom_t *dom, JSContext *ctx)
{
    DBrowsingContext *context = dom->browsing_context;
    return context && context->proxy ? JS_DupValue(ctx, context->proxy->value) : d_binding_global(dom, ctx);
}

static JSValue window_denied(JSContext *ctx)
{
    return d_error(ctx, "SecurityError", "Window property settings do not permit access");
}

static DWindowProxy *window_proxy_access(JSContext *ctx, JSValueConst value, os64_dom_t **source)
{
    DWindowProxy *proxy = JS_GetOpaque(value, os64_js_class_id(&window_proxy_slot));
    if (!proxy || !d_context(proxy->owner, ctx, OS64_JS_ABI_ID)) return NULL;
    *source = origin_settings(proxy->owner, OS64_JS_SETTINGS_CURRENT);
    if (!*source) { window_denied(ctx); return NULL; }
    return proxy;
}

static bool window_same_origin(DWindowProxy *proxy, os64_dom_t *source)
{
    return way_origin_same(proxy->context->window->binding_origin, source->binding_origin);
}

static DBrowsingContext *window_document_context(os64_dom_t *dom)
{
    DBrowsingContext *context = dom->browsing_context;
    return context && !context->discarded && context->active == dom ? context : NULL;
}

static size_t window_child_count(DBrowsingContext *context)
{
    size_t count = 0;
    for (DBrowsingContext *at = context && !context->discarded ? context->first_child : NULL; at; at = at->next_sibling) ++count;
    return count;
}

/* These functions perform the original IDL operation, not an author replacement
 * or an invocation on the supplied receiver. The anchor fixes relevant Window. */
static JSValue window_cross_call(JSContext *ctx, JSValueConst self, int argc,
    JSValueConst *argv, int magic, JSValue *data)
{
    (void)self;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (!dom || !d_member_access(dom, ctx, true)) return JS_EXCEPTION;
    DBrowsingContext *context = window_document_context(dom);
    if (magic & D_SET)
        return location_member(dom, ctx, dom->location->value, argc ? argv[0] : JS_UNDEFINED, L_HREF, true);
    switch (magic) {
    case 0: case 1: case 7: return d_window_identity(dom, ctx);
    case 2: return JS_DupValue(ctx, dom->location->value);
    case 3: case 5: case 6: return JS_UNDEFINED; /* Existing no-close/no-focus policy. */
    case 4: return JS_NewBool(ctx, !context);
    case 8: return JS_NewUint32(ctx, (uint32_t)window_child_count(context));
    case 9: return context ? d_window_proxy(ctx, d_browsing_context_top(context)) : JS_NULL;
    case 10: return JS_NULL; /* No script-created windows/opener relation. */
    case 11: return context ? d_window_proxy(ctx, context->parent ? context->parent : context) : JS_NULL;
    case 12: return d_error(ctx, "NotSupportedError", "Cross-realm messaging requires the frame message adapter");
    default: return JS_ThrowInternalError(ctx, "Unknown Window descriptor");
    }
}

static bool window_method(unsigned key)
{
    return key == 3 || key == 5 || key == 6 || key == 12;
}

static void window_descriptor_free(JSContext *ctx, JSPropertyDescriptor *desc)
{
    JS_FreeValue(ctx, desc->value); JS_FreeValue(ctx, desc->getter); JS_FreeValue(ctx, desc->setter);
}

static int window_cross_descriptor(DWindowProxy *proxy, JSContext *ctx, os64_dom_t *source,
    unsigned key, JSPropertyDescriptor *desc)
{
    os64_dom_t *target = proxy->context->window;
    DWindowDescriptor *entry = proxy->cache;
    while (entry && !(entry->source == source && entry->target == target && entry->key == key)) entry = entry->next;
    if (!entry) {
        entry = d_alloc(proxy->owner, sizeof(*entry));
        if (!entry) { d_error(ctx, "QuotaExceededError", "Window descriptor quota exceeded"); return -1; }
        os64_js_outcome_t out;
        JSContext *realm = os64_js_realm_context(source->realm, OS64_JS_ABI_ID, &out);
        if (!realm) { d_free(proxy->owner, entry); window_denied(ctx); return -1; }
        JSValue parts = JS_NewArray(ctx);
        if (JS_IsException(parts)) { d_free(proxy->owner, entry); return -1; }
        for (unsigned i = 0; i < 3; ++i) {
            JSValue value = JS_UNDEFINED;
            bool method = window_method(key);
            if ((method && i == 0) || (!method && i == 1) || (key == 2 && i == 2)) {
                if (method) {
                    JSPropertyDescriptor original;
                    int found = JS_GetOwnProperty(ctx, &original, target->window->value, proxy->keys[key]);
                    if (found < 0) goto fail;
                    if (found) {
                        value = JS_DupValue(ctx, original.value);
                        window_descriptor_free(ctx, &original);
                    }
                }
                if (!method || JS_IsFunction(ctx, value)) {
                    JS_FreeValue(ctx, value);
                    value = JS_NewCFunctionData(realm, window_cross_call,
                        key == 12 || i == 2 ? 1 : 0, (int)key | (i == 2 ? D_SET : 0), 1, &target->anchor->value);
                }
            }
            if (JS_IsException(value)) goto fail;
            if (JS_DefinePropertyValueUint32(ctx, parts, i, value, JS_PROP_C_W_E) < 0) goto fail;
        }
        DValue *held = d_retain(proxy->owner, ctx, parts);
        if (!held) { d_free(proxy->owner, entry); return -1; }
        *entry = (DWindowDescriptor){proxy->cache, source, target, key, held};
        proxy->cache = entry;
        goto ready;
fail:
        JS_FreeValue(ctx, parts); d_free(proxy->owner, entry); return -1;
    }
ready:
    if (desc) {
        *desc = (JSPropertyDescriptor){.flags = JS_PROP_CONFIGURABLE | (window_method(key) ? 0 : JS_PROP_GETSET),
            .value = JS_GetPropertyUint32(ctx, entry->parts->value, 0),
            .getter = JS_GetPropertyUint32(ctx, entry->parts->value, 1),
            .setter = JS_GetPropertyUint32(ctx, entry->parts->value, 2)};
    }
    return 1;
}

static int window_child_descriptor(JSContext *ctx, JSPropertyDescriptor *desc,
    DBrowsingContext *child, bool enumerable)
{
    JSValue value = d_window_proxy(ctx, child);
    if (JS_IsException(value)) return -1;
    if (desc) *desc = (JSPropertyDescriptor){.flags = JS_PROP_CONFIGURABLE | (enumerable ? JS_PROP_ENUMERABLE : 0),
        .value = value, .getter = JS_UNDEFINED, .setter = JS_UNDEFINED};
    else JS_FreeValue(ctx, value);
    return 1;
}

static int window_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst object, JSAtom key)
{
    os64_dom_t *source;
    DWindowProxy *proxy = window_proxy_access(ctx, object, &source);
    if (!proxy) return -1;
    bool same = window_same_origin(proxy, source);
    uint32_t index;
    int numeric = d_index_key(ctx, key, &index);
    if (numeric < 0) return -1;
    if (numeric) {
        DBrowsingContext *child = d_browsing_context_child(proxy->context, index);
        if (child) return window_child_descriptor(ctx, desc, child, true);
        if (same) return 0;
        window_denied(ctx); return -1;
    }
    if (same) return JS_GetOwnProperty(ctx, desc, proxy->context->window->window->value, key);
    for (unsigned i = 0; i < 13; ++i)
        if (proxy->keys[i] == key) return window_cross_descriptor(proxy, ctx, source, i, desc);
    /* A disallowed first duplicate suppresses later same-origin children.
     * This compares target names only, never document IDs or arbitrary nodes. */
    for (DBrowsingContext *child = proxy->context->first_child; child; child = child->next_sibling) {
        if (!child->name[0]) continue;
        JSAtom name = JS_NewAtom(ctx, child->name);
        if (name == JS_ATOM_NULL) return -1;
        bool match = name == key;
        JS_FreeAtom(ctx, name);
        if (!match) continue;
        if (way_origin_same(child->window->binding_origin, proxy->context->window->binding_origin))
            return window_child_descriptor(ctx, desc, child, false);
        break;
    }
    for (unsigned i = 13; i < 17; ++i) if (proxy->keys[i] == key) {
        if (desc) *desc = (JSPropertyDescriptor){.flags = JS_PROP_CONFIGURABLE,
            .value = JS_UNDEFINED, .getter = JS_UNDEFINED, .setter = JS_UNDEFINED};
        return 1;
    }
    window_denied(ctx); return -1;
}

static JSValue window_proxy_get(JSContext *ctx, JSValueConst object, JSAtom key, JSValueConst receiver)
{
    JSPropertyDescriptor desc;
    int found = window_get_own(ctx, &desc, object, key);
    if (found < 0) return JS_EXCEPTION;
    if (found) {
        if (!(desc.flags & JS_PROP_GETSET)) return desc.value;
        JSValue result = JS_IsUndefined(desc.getter) ? JS_UNDEFINED : JS_Call(ctx, desc.getter, receiver, 0, NULL);
        window_descriptor_free(ctx, &desc);
        return result;
    }
    DWindowProxy *proxy = JS_GetOpaque(object, os64_js_class_id(&window_proxy_slot));
    JSValue proto = JS_GetPrototype(ctx, proxy->context->window->window->value);
    if (JS_IsException(proto)) return proto;
    JSValue result = JS_IsNull(proto) ? JS_UNDEFINED : JS_GetPropertyInternal(ctx, proto, key, receiver, false);
    JS_FreeValue(ctx, proto);
    return result;
}

static int window_proxy_has(JSContext *ctx, JSValueConst object, JSAtom key)
{
    int found = window_get_own(ctx, NULL, object, key);
    if (found) return found;
    DWindowProxy *proxy = JS_GetOpaque(object, os64_js_class_id(&window_proxy_slot));
    JSValue proto = JS_GetPrototype(ctx, proxy->context->window->window->value);
    if (JS_IsException(proto)) return -1;
    int result = JS_IsNull(proto) ? 0 : JS_HasProperty(ctx, proto, key);
    JS_FreeValue(ctx, proto);
    return result;
}

static int window_proxy_set(JSContext *ctx, JSValueConst object, JSAtom key,
    JSValueConst value, JSValueConst receiver, int flags)
{
    os64_dom_t *source;
    DWindowProxy *proxy = window_proxy_access(ctx, object, &source);
    if (!proxy) return -1;
    if (window_same_origin(proxy, source)) {
        uint32_t index;
        int numeric = d_index_key(ctx, key, &index);
        if (numeric < 0) return -1;
        if (numeric) {
            /* Use the engine's ordinary refusal path for strict assignment. */
            return JS_SetPropertyInternal(ctx, proxy->owner->index_guard->value, key,
                JS_DupValue(ctx, value), proxy->owner->index_guard->value, flags);
        }
        return JS_SetPropertyInternal(ctx, proxy->context->window->window->value, key, JS_DupValue(ctx, value), receiver, flags);
    }
    JSPropertyDescriptor desc;
    if (window_get_own(ctx, &desc, object, key) < 0) return -1;
    if (JS_IsUndefined(desc.setter)) { window_descriptor_free(ctx, &desc); window_denied(ctx); return -1; }
    JSValue result = JS_Call(ctx, desc.setter, receiver, 1, &value);
    window_descriptor_free(ctx, &desc);
    bool failed = JS_IsException(result); JS_FreeValue(ctx, result);
    return failed ? -1 : 1;
}

static int window_proxy_define(JSContext *ctx, JSValueConst object, JSAtom key,
    JSValueConst value, JSValueConst getter, JSValueConst setter, int flags)
{
    os64_dom_t *source;
    DWindowProxy *proxy = window_proxy_access(ctx, object, &source);
    if (!proxy) return -1;
    if (!window_same_origin(proxy, source)) { window_denied(ctx); return -1; }
    uint32_t index;
    int numeric = d_index_key(ctx, key, &index);
    if (numeric < 0) return -1;
    return JS_DefineProperty(ctx, numeric ? proxy->owner->index_guard->value : proxy->context->window->window->value,
        key, value, getter, setter, flags);
}

static int window_proxy_delete(JSContext *ctx, JSValueConst object, JSAtom key)
{
    os64_dom_t *source;
    DWindowProxy *proxy = window_proxy_access(ctx, object, &source);
    if (!proxy) return -1;
    if (!window_same_origin(proxy, source)) { window_denied(ctx); return -1; }
    uint32_t index;
    int numeric = d_index_key(ctx, key, &index);
    if (numeric < 0) return -1;
    return numeric ? !d_browsing_context_child(proxy->context, index)
        : JS_DeleteProperty(ctx, proxy->context->window->window->value, key, 0);
}

static JSValue window_proxy_prototype(JSContext *ctx, JSValueConst object)
{
    os64_dom_t *source;
    DWindowProxy *proxy = window_proxy_access(ctx, object, &source);
    if (!proxy) return JS_EXCEPTION;
    return window_same_origin(proxy, source) ? JS_GetPrototype(ctx, proxy->context->window->window->value) : JS_NULL;
}

static int window_proxy_set_prototype(JSContext *ctx, JSValueConst object, JSValueConst value)
{
    JSValue current = window_proxy_prototype(ctx, object);
    if (JS_IsException(current)) return -1;
    bool same = JS_IsNull(current) ? JS_IsNull(value) : JS_IsObject(value) && JS_VALUE_GET_PTR(current) == JS_VALUE_GET_PTR(value);
    JS_FreeValue(ctx, current); return same;
}

static int window_proxy_extensible(JSContext *ctx, JSValueConst object)
{
    os64_dom_t *source; return window_proxy_access(ctx, object, &source) ? 1 : -1;
}
static int window_proxy_prevent(JSContext *ctx, JSValueConst object)
{
    os64_dom_t *source; return window_proxy_access(ctx, object, &source) ? 0 : -1;
}

static int window_proxy_keys(JSContext *ctx, JSPropertyEnum **out, uint32_t *count, JSValueConst object)
{
    *out = NULL; *count = 0;
    os64_dom_t *source;
    DWindowProxy *proxy = window_proxy_access(ctx, object, &source);
    if (!proxy) return -1;
    JSPropertyEnum *own = NULL; uint32_t own_count = 0;
    bool same = window_same_origin(proxy, source);
    if (same && JS_GetOwnPropertyNames(ctx, &own, &own_count, proxy->context->window->window->value,
        JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0) return -1;
    size_t children = window_child_count(proxy->context), total = children + (same ? own_count : 17);
    JSPropertyEnum *keys = js_malloc(ctx, (total ? total : 1) * sizeof(*keys));
    if (!keys) goto fail;
    for (size_t i = 0; i < children; ++i) {
        JSAtom atom = JS_NewAtomUInt32(ctx, (uint32_t)i);
        if (atom == JS_ATOM_NULL) {
            for (size_t j = 0; j < i; ++j) JS_FreeAtom(ctx, keys[j].atom);
            js_free(ctx, keys); goto fail;
        }
        keys[i] = (JSPropertyEnum){.atom = atom, .is_enumerable = true};
    }
    if (same) for (uint32_t i = 0; i < own_count; ++i) keys[children + i] = own[i];
    else for (unsigned i = 0; i < 17; ++i) keys[children + i] = (JSPropertyEnum){.atom = JS_DupAtom(ctx, proxy->keys[i])};
    js_free(ctx, own); *out = keys; *count = (uint32_t)total; return 0;
fail:
    for (uint32_t i = 0; i < own_count; ++i) JS_FreeAtom(ctx, own[i].atom);
    js_free(ctx, own); return -1;
}

static JSClassExoticMethods window_proxy_methods = {
    .get_own_property = window_get_own, .get_own_property_names = window_proxy_keys,
    .get_property = window_proxy_get, .set_property = window_proxy_set, .has_property = window_proxy_has,
    .define_own_property = window_proxy_define, .delete_property = window_proxy_delete,
    .get_prototype = window_proxy_prototype, .set_prototype = window_proxy_set_prototype,
    .is_extensible = window_proxy_extensible, .prevent_extensions = window_proxy_prevent
};

JSValue d_window_proxy(JSContext *ctx, DBrowsingContext *context)
{
    if (!context) return JS_ThrowTypeError(ctx, "Expected a private browsing context");
    if (!d_context(context->window, ctx, OS64_JS_ABI_ID)) return JS_EXCEPTION;
    if (context->proxy) return JS_DupValue(ctx, context->proxy->value);
    os64_dom_t *owner = context->window;
    JSClassID id = os64_js_class_id(&window_proxy_slot);
    const JSClassDef definition = {.class_name = "Window", .exotic = &window_proxy_methods};
    if (!JS_IsRegisteredClass(owner->engine, id) && JS_NewClass(owner->engine, id, &definition) < 0) return JS_ThrowOutOfMemory(ctx);
    DWindowProxy *proxy = d_alloc(owner, sizeof(*proxy));
    if (!proxy) return d_error(ctx, "QuotaExceededError", "Window proxy quota exceeded");
    *proxy = (DWindowProxy){.owner = owner, .context = context};
    /* Reuse bootstrap-captured well-known symbols; lazy child proxy creation
     * must not consult a mutable global Symbol or run author getters. */
    for (unsigned i = 0; i < 17; ++i) {
        proxy->keys[i] = i < 14 ? JS_NewAtom(ctx, window_cross_names[i])
            : JS_DupAtom(ctx, owner->location_exotic->cross_keys[i - 11]);
        if (proxy->keys[i] == JS_ATOM_NULL) goto fail;
    }
    JSValue object = JS_NewObjectProtoClass(ctx, JS_NULL, id);
    if (JS_IsException(object)) goto fail;
    JS_SetOpaque(object, proxy);
    DValue *held = d_retain(owner, ctx, object);
    if (!held) goto fail;
    owner->window_proxy_state = proxy; context->proxy = held;
    return JS_DupValue(ctx, held->value);
fail:
    for (unsigned i = 0; i < 17; ++i) JS_FreeAtom(ctx, proxy->keys[i]);
    d_free(owner, proxy); return JS_EXCEPTION;
}

void d_window_proxy_drain(os64_dom_t *dom)
{
    DWindowProxy *proxy = dom->window_proxy_state;
    if (!proxy) return;
    for (unsigned i = 0; i < 17; ++i) { JS_FreeAtomRT(dom->engine, proxy->keys[i]); proxy->keys[i] = JS_ATOM_NULL; }
}

static void window_proxy_free(os64_dom_t *dom)
{
    DWindowProxy *proxy = dom->window_proxy_state;
    if (!proxy) return;
    while (proxy->cache) {
        DWindowDescriptor *at = proxy->cache; proxy->cache = at->next; d_free(dom, at);
    }
    d_free(dom, proxy); dom->window_proxy_state = NULL;
}
