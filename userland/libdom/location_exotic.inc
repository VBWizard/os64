/* Location's private property view has no own engine slots: the backing object
 * supplies ordinary same-origin semantics without bypassing the exotic hooks.
 * DValue roots and native records survive departure until grouped teardown. */
static JSClassID location_exotic_slot;

typedef struct DLocationDescriptor {
    struct DLocationDescriptor *next;
    os64_dom_t *source;
    unsigned key;
    DValue *function;
} DLocationDescriptor;

typedef struct DLocationExotic {
    os64_dom_t *dom;
    DValue *backing;
    JSPropertyEnum *defaults;
    uint32_t default_count;
    JSAtom cross_keys[6];
    DLocationDescriptor *cache;
} DLocationExotic;

static DLocationExotic *location_exotic(JSContext *ctx, JSValueConst object, os64_dom_t **source)
{
    DLocationExotic *e = JS_GetOpaque(object, os64_js_class_id(&location_exotic_slot));
    if (!e || !d_context(e->dom, ctx, OS64_JS_ABI_ID)) return NULL;
    *source = origin_settings(e->dom, OS64_JS_SETTINGS_CURRENT);
    if (!*source) { navigation_denied(ctx); return NULL; }
    return e;
}

static bool location_same_origin(DLocationExotic *e, os64_dom_t *source)
{
    return way_origin_same(e->dom->binding_origin, source->binding_origin);
}

static bool location_default(DLocationExotic *e, JSAtom key)
{
    for (uint32_t i = 0; i < e->default_count; ++i)
        if (e->defaults[i].atom == key) return true;
    return false;
}

static JSValue location_cross_call(JSContext *ctx, JSValueConst self, int argc,
    JSValueConst *argv, int magic, JSValue *data)
{
    (void)self;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (!dom) return JS_EXCEPTION;
    /* Cross-origin descriptor functions perform the IDL operation on their
     * captured object, regardless of the receiver supplied to call/apply. */
    return location_member(dom, ctx, data[1], argc ? argv[0] : JS_UNDEFINED,
        magic == 0 ? L_HREF : L_REPLACE, magic == 0);
}

static JSValue location_cached_function(DLocationExotic *e, JSContext *ctx,
    os64_dom_t *source, unsigned key)
{
    for (DLocationDescriptor *at = e->cache; at; at = at->next)
        if (at->source == source && at->key == key) return JS_DupValue(ctx, at->function->value);
    DLocationDescriptor *entry = d_alloc(e->dom, sizeof(*entry));
    if (!entry) return d_error(ctx, "QuotaExceededError", "Location descriptor cache quota exceeded");
    os64_js_outcome_t out;
    JSContext *realm = os64_js_realm_context(source->realm, OS64_JS_ABI_ID, &out);
    if (!realm) { d_free(e->dom, entry); return navigation_denied(ctx); }
    JSValue data[] = {e->dom->anchor->value, e->dom->location->value};
    JSValue function = JS_NewCFunctionData(realm, location_cross_call, 1, (int)key, 2, data);
    if (JS_IsException(function)) { d_free(e->dom, entry); return JS_EXCEPTION; }
    DValue *held = d_retain(e->dom, ctx, function);
    if (!held) { d_free(e->dom, entry); return d_error(ctx, "QuotaExceededError", "Location descriptor hold refused"); }
    *entry = (DLocationDescriptor){e->cache, source, key, held};
    e->cache = entry;
    return JS_DupValue(ctx, held->value);
}

static int location_cross_descriptor(DLocationExotic *e, JSContext *ctx,
    os64_dom_t *source, JSPropertyDescriptor *desc, JSAtom key)
{
    unsigned index = 0;
    while (index < 6 && e->cross_keys[index] != key) ++index;
    if (index == 6) { navigation_denied(ctx); return -1; }
    JSValue function = index < 2 ? location_cached_function(e, ctx, source, index) : JS_UNDEFINED;
    if (JS_IsException(function)) return -1;
    if (desc) {
        *desc = (JSPropertyDescriptor){.flags = JS_PROP_CONFIGURABLE,
            .value = JS_UNDEFINED, .getter = JS_UNDEFINED, .setter = JS_UNDEFINED};
        if (index == 0) { desc->flags |= JS_PROP_GETSET; desc->setter = function; }
        else desc->value = function;
    } else JS_FreeValue(ctx, function);
    return 1;
}

static int location_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst object, JSAtom key)
{
    os64_dom_t *source;
    DLocationExotic *e = location_exotic(ctx, object, &source);
    if (!e) return -1;
    if (!location_same_origin(e, source)) return location_cross_descriptor(e, ctx, source, desc, key);
    int found = JS_GetOwnProperty(ctx, desc, e->backing->value, key);
    if (found > 0 && desc && location_default(e, key)) desc->flags |= JS_PROP_CONFIGURABLE;
    return found;
}

static int location_keys(JSContext *ctx, JSPropertyEnum **out, uint32_t *count, JSValueConst object)
{
    *out = NULL; *count = 0;
    os64_dom_t *source;
    DLocationExotic *e = location_exotic(ctx, object, &source);
    if (!e) return -1;
    if (location_same_origin(e, source))
        return JS_GetOwnPropertyNames(ctx, out, count, e->backing->value, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK);
    JSPropertyEnum *keys = js_malloc(ctx, 6 * sizeof(*keys));
    if (!keys) return -1;
    for (unsigned i = 0; i < 6; ++i)
        keys[i] = (JSPropertyEnum){.atom = JS_DupAtom(ctx, e->cross_keys[i]), .is_enumerable = false};
    *out = keys; *count = 6;
    return 0;
}

static JSValue location_get(JSContext *ctx, JSValueConst object, JSAtom key, JSValueConst receiver)
{
    os64_dom_t *source;
    DLocationExotic *e = location_exotic(ctx, object, &source);
    if (!e) return JS_EXCEPTION;
    if (location_same_origin(e, source))
        return JS_GetPropertyInternal(ctx, e->backing->value, key, receiver, false);
    JSPropertyDescriptor desc;
    if (location_cross_descriptor(e, ctx, source, &desc, key) < 0) return JS_EXCEPTION;
    if (!(desc.flags & JS_PROP_GETSET)) return desc.value;
    JS_FreeValue(ctx, desc.setter);
    return navigation_denied(ctx);
}

static int location_set(JSContext *ctx, JSValueConst object, JSAtom key,
    JSValueConst value, JSValueConst receiver, int flags)
{
    os64_dom_t *source;
    DLocationExotic *e = location_exotic(ctx, object, &source);
    if (!e) return -1;
    if (location_same_origin(e, source))
        return JS_SetPropertyInternal(ctx, e->backing->value, key, JS_DupValue(ctx, value), receiver, flags);
    JSPropertyDescriptor desc;
    if (location_cross_descriptor(e, ctx, source, &desc, key) < 0) return -1;
    if (!(desc.flags & JS_PROP_GETSET)) {
        JS_FreeValue(ctx, desc.value); navigation_denied(ctx); return -1;
    }
    JSValue result = JS_Call(ctx, desc.setter, receiver, 1, &value);
    JS_FreeValue(ctx, desc.setter);
    bool failed = JS_IsException(result);
    JS_FreeValue(ctx, result);
    return failed ? -1 : 1;
}

static int location_has(JSContext *ctx, JSValueConst object, JSAtom key)
{
    os64_dom_t *source;
    DLocationExotic *e = location_exotic(ctx, object, &source);
    if (!e) return -1;
    return location_same_origin(e, source) ? JS_HasProperty(ctx, e->backing->value, key)
        : location_cross_descriptor(e, ctx, source, NULL, key);
}

static int location_define(JSContext *ctx, JSValueConst object, JSAtom key,
    JSValueConst value, JSValueConst getter, JSValueConst setter, int flags)
{
    os64_dom_t *source;
    DLocationExotic *e = location_exotic(ctx, object, &source);
    if (!e) return -1;
    if (!location_same_origin(e, source)) { navigation_denied(ctx); return -1; }
    /* The nonextensible guard preserves the engine's strict/throw interpretation
     * while refusing even an identical redefinition of a default property. */
    JSValueConst target = location_default(e, key) ? e->dom->index_guard->value : e->backing->value;
    return JS_DefineProperty(ctx, target, key, value, getter, setter, flags);
}

static int location_delete(JSContext *ctx, JSValueConst object, JSAtom key)
{
    os64_dom_t *source;
    DLocationExotic *e = location_exotic(ctx, object, &source);
    if (!e) return -1;
    if (!location_same_origin(e, source)) { navigation_denied(ctx); return -1; }
    return JS_DeleteProperty(ctx, e->backing->value, key, 0);
}

static JSValue location_prototype(JSContext *ctx, JSValueConst object)
{
    os64_dom_t *source;
    DLocationExotic *e = location_exotic(ctx, object, &source);
    if (!e) return JS_EXCEPTION;
    return location_same_origin(e, source) ? JS_GetPrototype(ctx, e->backing->value) : JS_NULL;
}

static int location_set_prototype(JSContext *ctx, JSValueConst object, JSValueConst prototype)
{
    JSValue current = location_prototype(ctx, object);
    if (JS_IsException(current)) return -1;
    bool same = JS_IsNull(current) ? JS_IsNull(prototype) :
        JS_IsObject(prototype) && JS_VALUE_GET_PTR(current) == JS_VALUE_GET_PTR(prototype);
    JS_FreeValue(ctx, current);
    return same;
}

static int location_extensible(JSContext *ctx, JSValueConst object)
{
    os64_dom_t *source;
    return location_exotic(ctx, object, &source) ? 1 : -1;
}

static int location_prevent_extensions(JSContext *ctx, JSValueConst object)
{
    os64_dom_t *source;
    return location_exotic(ctx, object, &source) ? 0 : -1;
}

static JSClassExoticMethods location_methods = {
    .get_own_property = location_get_own, .get_own_property_names = location_keys,
    .get_property = location_get, .set_property = location_set, .has_property = location_has,
    .define_own_property = location_define, .delete_property = location_delete,
    .get_prototype = location_prototype, .set_prototype = location_set_prototype,
    .is_extensible = location_extensible, .prevent_extensions = location_prevent_extensions
};

int d_location_exotic_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    if (!dom->binding_origin) return 0;
    JSClassID id = os64_js_class_id(&location_exotic_slot);
    const JSClassDef definition = {.class_name = "Location", .exotic = &location_methods};
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, &definition) < 0) return -1;
    DLocationExotic *e = d_alloc(dom, sizeof(*e));
    if (!e) return -1;
    *e = (DLocationExotic){.dom = dom};
    dom->location_exotic = e;
    /* Bootstrap owns the unpublished backing object. Unforgeable storage and
     * its original key set precede replacement of the script-facing handle. */
    if (unforgeable(ctx, dom->location->value) < 0 ||
        JS_GetOwnPropertyNames(ctx, &e->defaults, &e->default_count, dom->location->value,
            JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0) return -1;
    const char *names[] = {"href", "replace", "then", "toStringTag", "hasInstance", "isConcatSpreadable"};
    JSValue symbol = JS_GetPropertyStr(ctx, global, "Symbol");
    if (JS_IsException(symbol)) return -1;
    for (unsigned i = 0; i < 6; ++i) {
        if (i < 3) e->cross_keys[i] = JS_NewAtom(ctx, names[i]);
        else {
            JSValue key = JS_GetPropertyStr(ctx, symbol, names[i]);
            e->cross_keys[i] = JS_IsException(key) ? JS_ATOM_NULL : JS_ValueToAtom(ctx, key);
            JS_FreeValue(ctx, key);
        }
        if (e->cross_keys[i] == JS_ATOM_NULL) { JS_FreeValue(ctx, symbol); return -1; }
    }
    JS_FreeValue(ctx, symbol);
    JSValue object = JS_NewObjectProtoClass(ctx, JS_NULL, id);
    if (JS_IsException(object)) return -1;
    e->backing = d_retain(dom, ctx, JS_DupValue(ctx, dom->location->value));
    if (!e->backing) { JS_FreeValue(ctx, object); return -1; }
    JS_SetOpaque(object, e);
    JS_FreeValue(ctx, dom->location->value);
    dom->location->value = object;
    return 0;
}

void d_location_exotic_drain(os64_dom_t *dom)
{
    DLocationExotic *e = dom->location_exotic;
    if (!e) return;
    for (uint32_t i = 0; i < e->default_count; ++i) JS_FreeAtomRT(dom->engine, e->defaults[i].atom);
    js_free_rt(dom->engine, e->defaults);
    e->defaults = NULL; e->default_count = 0;
    for (unsigned i = 0; i < 6; ++i) {
        JS_FreeAtomRT(dom->engine, e->cross_keys[i]); e->cross_keys[i] = JS_ATOM_NULL;
    }
}

static void location_exotic_free(os64_dom_t *dom)
{
    DLocationExotic *e = dom->location_exotic;
    if (!e) return;
    while (e->cache) {
        DLocationDescriptor *at = e->cache; e->cache = at->next; d_free(dom, at);
    }
    d_free(dom, e); dom->location_exotic = NULL;
}
