#include "internal.h"

typedef struct { size_t size; } DBlock;
static JSClassID anchor_slot, node_slot, collection_slot, style_slot;

void *d_alloc(os64_dom_t *dom, size_t size)
{
    if (size > SIZE_MAX - sizeof(DBlock)) return NULL;
    size_t total = sizeof(DBlock) + size;
    if (total > dom->options.max_bytes - dom->bytes) return NULL;
    DBlock *block = os64_malloc(total);
    if (block == NULL) return NULL;
    block->size = total;
    dom->bytes += total;
    os64_memset(block + 1, 0, size);
    return block + 1;
}

void d_free(os64_dom_t *dom, void *ptr)
{
    if (ptr == NULL) return;
    DBlock *block = (DBlock *)ptr - 1;
    dom->bytes -= block->size;
    os64_free(block);
}

JSValue d_error(JSContext *ctx, const char *name, const char *message)
{
    JSValue error = JS_NewError(ctx);
    if (JS_IsException(error)) return error;
    JSValue text = JS_NewString(ctx, name);
    if (JS_IsException(text)) { JS_FreeValue(ctx, error); return JS_EXCEPTION; }
    if (JS_DefinePropertyValueStr(ctx, error, "name", text, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(ctx, error);
        return JS_EXCEPTION;
    }
    text = JS_NewString(ctx, message);
    if (JS_IsException(text)) { JS_FreeValue(ctx, error); return JS_EXCEPTION; }
    if (JS_DefinePropertyValueStr(ctx, error, "message", text, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(ctx, error);
        return JS_EXCEPTION;
    }
    return JS_Throw(ctx, error);
}

JSValue d_html_error(JSContext *ctx, int64_t status)
{
    const char *name = "TypeError";
    if (status == OS64_HTML_HIERARCHY || status == OS64_HTML_ROOT_REQUIRED)
        name = "HierarchyRequestError";
    else if (status == OS64_HTML_NOT_FOUND) name = "NotFoundError";
    else if (status <= OS64_HTML_TOO_LARGE && status >= OS64_HTML_WORK_EXHAUSTED)
        name = "QuotaExceededError";
    return d_error(ctx, name, os64_html_status_name(status));
}

JSValue d_page_error(JSContext *ctx, int64_t status)
{
    return d_error(ctx, status == -OS64_PAGE_REASON_NO_MEMORY ? "QuotaExceededError" : "TypeError",
                   "Control property refused");
}

bool d_context(os64_dom_t *dom, JSContext *ctx, const char *abi)
{
    if (dom == NULL || dom->closed) {
        d_error(ctx, "InvalidStateError", "DOM binding is closed");
        return false;
    }
    os64_js_outcome_t outcome;
    if (os64_js_context(dom->runtime, abi, &outcome) != ctx) {
        d_error(ctx, "InvalidStateError", "DOM runtime is unavailable");
        return false;
    }
    return true;
}

os64_dom_t *d_callback(JSContext *ctx, JSValueConst *data, const char *abi)
{
    JSClassID id;
    os64_dom_t *dom = JS_GetAnyOpaque(data[0], &id);
    if (!d_context(dom, ctx, abi)) return NULL;
    return dom;
}

DValue *d_retain(os64_dom_t *dom, JSContext *ctx, JSValue value)
{
    if (JS_IsException(value)) return NULL;
    DValue *entry = d_alloc(dom, sizeof(*entry));
    if (entry == NULL) {
        JS_FreeValue(ctx, value);
        d_error(ctx, "QuotaExceededError", "DOM registry quota exceeded");
        return NULL;
    }
    entry->value = value;
    entry->next = dom->values;
    dom->values = entry;
    dom->retained++;
    return entry;
}

int d_method(os64_dom_t *dom, JSContext *ctx, JSValueConst target,
             const char *name, JSCFunctionData *fn, int argc, int magic)
{
    JSValue function = JS_NewCFunctionData(ctx, fn, argc, magic, 1, &dom->anchor->value);
    if (JS_IsException(function)) return -1;
    return JS_DefinePropertyValueStr(ctx, target, name, function, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

int d_accessor(os64_dom_t *dom, JSContext *ctx, JSValueConst target,
               const char *name, JSCFunctionData *fn, int magic, bool writable)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL) return -1;
    JSValue get = JS_NewCFunctionData(ctx, fn, 0, magic, 1, &dom->anchor->value);
    JSValue set = JS_UNDEFINED;
    if (!JS_IsException(get) && writable)
        set = JS_NewCFunctionData(ctx, fn, 1, magic | D_SET, 1, &dom->anchor->value);
    if (JS_IsException(get) || JS_IsException(set)) {
        JS_FreeValue(ctx, get);
        JS_FreeValue(ctx, set);
        JS_FreeAtom(ctx, atom);
        return -1;
    }
    int result = JS_DefinePropertyGetSet(ctx, target, atom, get, set,
                                       JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, atom);
    return result;
}

/* Finalizers detach engine slots; the host ledger owns native storage and
 * remains alive through JS runtime destruction. */
static void clear_opaque(JSRuntime *rt, JSValue value)
{
    (void)rt;
    JS_SetOpaque(value, NULL);
}

static JSValue dialog(JSContext *ctx, JSValueConst self, int argc,
                      JSValueConst *argv, int magic, JSValue *data)
{
    (void)self;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DString message = {0};
    if (argc > 0 && !d_string(dom, ctx, argv[0], &message)) return JS_EXCEPTION;
    if (magic == 2 && argc > 1) {
        DString initial = {0};
        if (!d_string(dom, ctx, argv[1], &initial)) {
            d_string_free(dom, &message);
            return JS_EXCEPTION;
        }
        d_string_free(dom, &initial);
    }
    if (magic == 0 && dom->options.alert != NULL)
        dom->options.alert(dom->options.alert_opaque, message.data != NULL ? message.data : "", message.length);
    d_string_free(dom, &message);
    return magic == 1 ? JS_FALSE : magic == 2 ? JS_NULL : JS_UNDEFINED;
}

static void construction_error(JSContext *ctx, os64_js_outcome_t *outcome)
{
    JSValue error = JS_GetException(ctx);
    bool present = !JS_IsNull(error) && !JS_IsUninitialized(error);
    outcome->status = present ? OS64_JS_EXCEPTION : OS64_JS_HOST_FAILURE;
    const char *fallback = "DOM construction failed; retire this runtime";
    size_t length = os64_strlen(fallback);
    os64_memcpy(outcome->message, fallback, length + 1);
    if (present) {
        JSValue message = JS_GetPropertyStr(ctx, error, "message");
        if (!JS_IsException(message) && JS_IsString(message)) {
            const char *text = JS_ToCStringLen(ctx, &length, message);
            if (text != NULL) {
                size_t copied = length < sizeof(outcome->message) - 1 ? length : sizeof(outcome->message) - 1;
                os64_memcpy(outcome->message, text, copied);
                outcome->message[copied] = '\0';
                outcome->diagnostic_truncated = copied != length;
                JS_FreeCString(ctx, text);
            }
        }
        JS_FreeValue(ctx, message);
        /* Diagnostic allocation can itself fail; consume that exception too. */
        JSValue secondary = JS_GetException(ctx);
        JS_FreeValue(ctx, secondary);
    }
    JS_FreeValue(ctx, error);
}

os64_dom_t *os64_dom_create(os64_js_runtime_t *runtime, os64_html_document_t *document,
                            os64_page_state_t *state, const os64_dom_options_t *options,
                            os64_js_outcome_t *outcome)
{
    if (outcome == NULL) return NULL;
    JSContext *ctx = os64_js_context(runtime, OS64_JS_ABI_ID, outcome);
    if (ctx == NULL) return NULL;
    if (document == NULL || state == NULL || os64_page_state_document(state) != document) {
        outcome->status = OS64_JS_BAD_ARGUMENT;
        return NULL;
    }
    os64_dom_options_t selected = options != NULL ? *options : os64_dom_default_options();
    if (selected.max_bytes < sizeof(os64_dom_t)) {
        outcome->status = OS64_JS_HOST_FAILURE;
        return NULL;
    }
    os64_dom_t *dom = os64_calloc(1, sizeof(*dom));
    if (dom == NULL) { outcome->status = OS64_JS_HOST_FAILURE; return NULL; }
    dom->runtime = runtime;
    dom->engine = JS_GetRuntime(ctx);
    dom->document = document;
    dom->state = state;
    dom->options = selected;
    dom->bytes = sizeof(*dom);
    dom->anchor_class = os64_js_class_id(&anchor_slot);
    dom->node_class = os64_js_class_id(&node_slot);
    dom->collection_class = os64_js_class_id(&collection_slot);
    dom->style_class = os64_js_class_id(&style_slot);
    if (JS_IsRegisteredClass(dom->engine, dom->anchor_class)) {
        outcome->status = OS64_JS_BAD_ARGUMENT;
        os64_free(dom);
        return NULL;
    }
    const JSClassDef anchor_definition = {.class_name = "DOMBinding", .finalizer = clear_opaque};
    const JSClassDef node_definition = {.class_name = "Node", .finalizer = clear_opaque, .exotic = &d_node_exotic};
    const JSClassDef style_definition = {.class_name = "CSSStyleDeclaration", .finalizer = clear_opaque};
    const JSClassDef collection_definition = {
        .class_name = "HTMLCollection", .finalizer = clear_opaque, .exotic = &d_collection_exotic
    };
    if (JS_NewClass(dom->engine, dom->anchor_class, &anchor_definition) < 0 ||
        JS_NewClass(dom->engine, dom->node_class, &node_definition) < 0 ||
        JS_NewClass(dom->engine, dom->collection_class, &collection_definition) < 0 ||
        JS_NewClass(dom->engine, dom->style_class, &style_definition) < 0) goto fail;
    JSValue anchor = JS_NewObjectClass(ctx, dom->anchor_class);
    if (JS_IsException(anchor)) goto fail;
    JS_SetOpaque(anchor, dom);
    dom->anchor = d_retain(dom, ctx, anchor);
    if (dom->anchor == NULL) goto fail;
    JSValue guard = JS_NewObject(ctx);
    if (JS_IsException(guard)) goto fail;
    if (JS_PreventExtensions(ctx, guard) < 0) { JS_FreeValue(ctx, guard); goto fail; }
    dom->index_guard = d_retain(dom, ctx, guard);
    if (dom->index_guard == NULL) goto fail;
    if (d_node_install(dom, ctx) < 0) goto fail;
    JSValue collection_prototype = JS_NewObject(ctx);
    if (JS_IsException(collection_prototype)) goto fail;
    if (d_collection_install(dom, ctx, collection_prototype) < 0) {
        JS_FreeValue(ctx, collection_prototype);
        goto fail;
    }
    JS_SetClassProto(ctx, dom->collection_class, collection_prototype);
    if (selected.url != NULL) {
        size_t size = os64_strlen(selected.url) + 1;
        dom->url = d_alloc(dom, size);
        if (dom->url == NULL) goto fail;
        os64_memcpy(dom->url, selected.url, size);
    }
    /* The page's address is the binding's own copy from here on. */
    dom->options.url = NULL;
    JSValue global = JS_GetGlobalObject(ctx);
    if (JS_IsException(global)) goto fail;
    JSValue doc = d_wrap(dom, ctx, document->document);
    int installed = JS_IsException(doc) ? -1 : JS_SetPropertyStr(ctx, global, "document", doc);
    if (installed >= 0) installed = JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));
    if (installed >= 0) installed = d_method(dom, ctx, global, "alert", dialog, 1, 0);
    if (installed >= 0) installed = d_method(dom, ctx, global, "confirm", dialog, 1, 1);
    if (installed >= 0) installed = d_method(dom, ctx, global, "prompt", dialog, 2, 2);
    if (installed >= 0) installed = d_event_install(dom, ctx, global);
    if (installed >= 0) installed = d_timer_install(dom, ctx, global);
    if (installed >= 0) installed = d_window_install(dom, ctx, global);
    JS_FreeValue(ctx, global);
    if (installed < 0) goto fail;
    outcome->status = OS64_JS_OK;
    return dom;
fail:
    /* Partially installed functions may outlive construction. Clear their
     * anchor before releasing native storage; the host retires this runtime.
     * Only the binding's own classes carry an opaque slot: JS_SetOpaque writes
     * any object's union, and the registry also holds engine functions. */
    construction_error(ctx, outcome);
    for (DValue *entry = dom->values; entry != NULL; entry = entry->next) {
        JSClassID id = JS_GetClassID(entry->value);
        if (id == dom->anchor_class || id == dom->node_class || id == dom->collection_class || id == dom->style_class)
            JS_SetOpaque(entry->value, NULL);
    }
    os64_dom_drain(dom);
    os64_dom_free(dom);
    return NULL;
}

void os64_dom_drain(os64_dom_t *dom)
{
    if (dom == NULL || dom->closed) return;
    dom->closed = true;
    d_event_drain(dom);
    d_timer_drain(dom);
    for (DValue *entry = dom->values; entry != NULL; entry = entry->next) {
        JS_FreeValueRT(dom->engine, entry->value);
        entry->value = JS_UNDEFINED;
    }
    dom->retained = 0;
}

void os64_dom_free(os64_dom_t *dom)
{
    if (dom == NULL) return;
    /* The public teardown order drains before engine destruction. No engine
     * call belongs here because its runtime has already been destroyed. */
    for (DQuery *query = dom->queries; query != NULL;) {
        DQuery *next = query->next;
        d_query_free(dom, query);
        query = next;
    }
    d_timer_free(dom);
    d_window_free(dom);
    for (DValue *entry = dom->values; entry != NULL;) {
        DValue *next = entry->next;
        d_event_free(dom, entry);
        if (entry->node != NULL)
            os64_html_release(dom->document, entry->node);
        if (entry->style_target != NULL)
            os64_html_release(dom->document, entry->style_target);
        d_free(dom, entry);
        entry = next;
    }
    os64_free(dom);
}

size_t os64_dom_bytes(const os64_dom_t *dom) { return dom != NULL ? dom->bytes : 0; }
/* The three registries: wrappers and other held values, listener callbacks,
 * and timer callbacks with their arguments. */
size_t os64_dom_registry_count(const os64_dom_t *dom)
{
    return dom != NULL ? dom->retained + dom->listeners_held + dom->timers_held : 0;
}
