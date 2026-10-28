#include "internal.h"

typedef struct { size_t size; } DBlock;
static JSClassID anchor_slot, node_slot, collection_slot, style_slot, token_slot, map_slot;

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

int d_dictionary(JSContext *ctx, int argc, JSValueConst *argv, int at)
{
    if (argc <= at || JS_IsUndefined(argv[at]) || JS_IsNull(argv[at])) return 0;
    if (!JS_IsObject(argv[at])) {
        JS_ThrowTypeError(ctx, "A dictionary argument must be an object");
        return -1;
    }
    return 1;
}

/* ONE DOMEXCEPTION FOR THE BINDING. A refusal by a WebIDL error name is a
 * TypeError or a RangeError thrown as the engine's own; any other name is a
 * DOMException with that name and its legacy code (exception.c), once the
 * DOMException row has installed it; with the modern switch off there is
 * no DOMException to make, and it is an Error carrying the name. The
 * binding is reached through its anchor without d_context, which itself
 * throws through here. */
JSValue d_error(JSContext *ctx, const char *name, const char *message)
{
    if (os64_streq(name, "TypeError")) return JS_ThrowTypeError(ctx, "%s", message);
    if (os64_streq(name, "RangeError")) return JS_ThrowRangeError(ctx, "%s", message);
    JSClassID id = os64_js_class_id(&anchor_slot);
    JSValue anchor = JS_GetClassProto(ctx, id);
    os64_dom_t *dom = JS_GetOpaque(anchor, id);
    JS_FreeValue(ctx, anchor);
    if (dom != NULL && !dom->closed && dom->exception_prototype != NULL)
        return d_dom_exception(dom, ctx, name, message);
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

bool d_exception_catchable(JSContext *ctx)
{
    JSClassID id = os64_js_class_id(&anchor_slot);
    JSValue anchor = JS_GetClassProto(ctx, id);
    os64_dom_t *dom = JS_GetOpaque(anchor, id);
    JS_FreeValue(ctx, anchor);
    if (dom == NULL) return true;
    /* Inside a page's call the budget is asked, which also sees a memory
     * limit the engine has not yet raised to the runtime; anywhere else,
     * whether the runtime has failed. */
    os64_js_outcome_t probe;
    os64_js_status_t status = os64_js_check_budget(dom->runtime, OS64_JS_ABI_ID, &probe);
    if (status == OS64_JS_BAD_ARGUMENT) return os64_js_context(dom->runtime, OS64_JS_ABI_ID, &probe) != NULL;
    return status == OS64_JS_OK;
}

os64_dom_t *d_binding(JSContext *ctx)
{
    JSClassID id = os64_js_class_id(&anchor_slot);
    JSValue anchor = JS_GetClassProto(ctx, id);
    os64_dom_t *dom = JS_GetOpaque(anchor, id);
    JS_FreeValue(ctx, anchor);
    return d_context(dom, ctx, OS64_JS_ABI_ID) ? dom : NULL;
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

DValue *d_intrinsic(os64_dom_t *dom, JSContext *ctx, JSValueConst from, const char *name)
{
    JSValue value = JS_GetPropertyStr(ctx, from, name);
    if (JS_IsException(value)) return NULL;
    if (!JS_IsFunction(ctx, value)) {
        JS_FreeValue(ctx, value);
        JS_ThrowTypeError(ctx, "Missing intrinsic %s", name);
        return NULL;
    }
    return d_retain(dom, ctx, value);
}

int d_method(os64_dom_t *dom, JSContext *ctx, JSValueConst target,
             const char *name, JSCFunctionData *fn, int argc, int magic)
{
    JSValue function = JS_NewCFunctionData(ctx, fn, argc, magic, 1, &dom->anchor->value);
    if (JS_IsException(function)) return -1;
    return JS_DefinePropertyValueStr(ctx, target, name, function, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

int d_method_named(os64_dom_t *dom, JSContext *ctx, JSValueConst target,
                   const char *name, JSCFunctionData *fn, int argc, int magic)
{
    JSValue function = JS_NewCFunctionData(ctx, fn, argc, magic, 1, &dom->anchor->value);
    if (JS_IsException(function)) return -1;
    JSValue text = JS_NewString(ctx, name);
    if (JS_IsException(text) ||
        JS_DefinePropertyValueStr(ctx, function, "name", text, JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(ctx, function);
        return -1;
    }
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

/* window.window and window.document are [LegacyUnforgeable] (HTML
 * § Window): own accessors of the global with a getter and no setter,
 * neither configurable, so no page can delete or redefine them, a `let
 * document` at a script's top level is the SyntaxError a browser throws,
 * and an assignment is ignored outside strict mode (a TypeError in it), as
 * browsers ignore one. This is the binding's one definition of each.
 *
 * They are read on nearly every line of a page's script, so each getter is
 * as short as a getter can be: window's answers the global, and
 * document's answers the document's wrapper it holds (d_held_getter). */
static JSValue window_get(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    (void)argc;
    (void)argv;
    return JS_GetGlobalObject(ctx);
}

static int unforgeable_window(JSContext *ctx, JSValueConst global)
{
    JSAtom atom = JS_NewAtom(ctx, "window");
    if (atom == JS_ATOM_NULL) return -1;
    JSValue get = JS_NewCFunction2(ctx, window_get, "get window", 0, JS_CFUNC_generic, 0);
    int result = JS_IsException(get)
        ? -1 : JS_DefinePropertyGetSet(ctx, global, atom, get, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, atom);
    return result;
}

/* A getter that answers the value it holds (its data[1]) once the binding
 * is open, the one check every binding member makes first. It reads no
 * native state and enters nothing, so d_context's ABI identity, which
 * guards an entry into the engine, is not asked. The value is held by
 * the function, so it lives as long as the property does; after drain the
 * closed binding refuses it, as d_context refuses every member. */
static JSValue held_get(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                        JSValue *data)
{
    (void)self;
    (void)argc;
    (void)argv;
    (void)magic;
    JSClassID id;
    os64_dom_t *dom = JS_GetAnyOpaque(data[0], &id);
    if (dom == NULL || dom->closed) return d_error(ctx, "InvalidStateError", "DOM binding is closed");
    return JS_DupValue(ctx, data[1]);
}

int d_held_getter(os64_dom_t *dom, JSContext *ctx, JSValueConst target, const char *name, JSValueConst held,
                  int flags)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL) return -1;
    JSValueConst data[2] = {dom->anchor->value, held};
    JSValue get = JS_NewCFunctionData(ctx, held_get, 0, 0, 2, data);
    int result = JS_IsException(get) ? -1 : JS_DefinePropertyGetSet(ctx, target, atom, get, JS_UNDEFINED, flags);
    JS_FreeAtom(ctx, atom);
    return result;
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
    dom->token_class = os64_js_class_id(&token_slot);
    dom->map_class = os64_js_class_id(&map_slot);
    if (JS_IsRegisteredClass(dom->engine, dom->anchor_class)) {
        outcome->status = OS64_JS_BAD_ARGUMENT;
        os64_free(dom);
        return NULL;
    }
    const JSClassDef anchor_definition = {.class_name = "DOMBinding", .finalizer = clear_opaque};
    const JSClassDef node_definition = {.class_name = "Node", .finalizer = clear_opaque, .exotic = &d_node_exotic};
    const JSClassDef style_definition = {.class_name = "CSSStyleDeclaration", .finalizer = clear_opaque};
    const JSClassDef token_definition = {
        .class_name = "DOMTokenList", .finalizer = clear_opaque, .exotic = &d_token_exotic
    };
    const JSClassDef map_definition = {
        .class_name = "DOMStringMap", .finalizer = clear_opaque, .exotic = &d_map_exotic
    };
    const JSClassDef collection_definition = {
        .class_name = "HTMLCollection", .finalizer = clear_opaque, .exotic = &d_collection_exotic
    };
    if (JS_NewClass(dom->engine, dom->anchor_class, &anchor_definition) < 0 ||
        JS_NewClass(dom->engine, dom->node_class, &node_definition) < 0 ||
        JS_NewClass(dom->engine, dom->collection_class, &collection_definition) < 0 ||
        JS_NewClass(dom->engine, dom->style_class, &style_definition) < 0 ||
        JS_NewClass(dom->engine, dom->token_class, &token_definition) < 0 ||
        JS_NewClass(dom->engine, dom->map_class, &map_definition) < 0) goto fail;
    JSValue anchor = JS_NewObjectClass(ctx, dom->anchor_class);
    if (JS_IsException(anchor)) goto fail;
    JS_SetOpaque(anchor, dom);
    dom->anchor = d_retain(dom, ctx, anchor);
    if (dom->anchor == NULL) goto fail;
    /* The anchor is also its own class's prototype in this context, where
     * no page reaches it: that is how a function made without data finds
     * its binding (d_binding). */
    JS_SetClassProto(ctx, dom->anchor_class, JS_DupValue(ctx, dom->anchor->value));
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
    if (d_static_list_install(dom, ctx) < 0 || d_mixin_install(dom, ctx) < 0) goto fail;
    if (selected.url != NULL) {
        size_t size = os64_strlen(selected.url) + 1;
        dom->url = d_alloc(dom, size);
        if (dom->url == NULL) goto fail;
        os64_memcpy(dom->url, selected.url, size);
    }
    /* The page's address is the binding's own copy from here on, and so is
     * its referrer (document.c). */
    dom->options.url = NULL;
    if (d_document_setup(dom) < 0) goto fail;
    JSValue global = JS_GetGlobalObject(ctx);
    if (JS_IsException(global)) goto fail;
    /* The document's wrapper is made now and held by `document`'s getter:
     * the wrapper table keeps one wrapper per node for the page's life, so
     * the one made here is the one every read answers. */
    JSValue doc = d_wrap(dom, ctx, document->document);
    int installed = JS_IsException(doc) ? -1 : unforgeable_window(ctx, global);
    if (installed >= 0) installed = d_held_getter(dom, ctx, global, "document", doc, JS_PROP_ENUMERABLE);
    JS_FreeValue(ctx, doc);
    /* The window is its own self and frames (HTML § Window), both
     * [Replaceable], so a page may assign over them. Its top, parent and
     * length are its browsing context's answers, asked as P1-W's members
     * are (browsing.c, d_browsing_classic): for a window with no frames,
     * itself, itself and 0. */
    if (installed >= 0) installed = JS_DefinePropertyValueStr(ctx, global, "self", JS_DupValue(ctx, global), JS_PROP_C_W_E);
    if (installed >= 0) installed = JS_DefinePropertyValueStr(ctx, global, "frames", JS_DupValue(ctx, global), JS_PROP_C_W_E);
    if (installed >= 0) installed = d_browsing_classic(dom, ctx, global);
    if (installed >= 0) installed = d_method(dom, ctx, global, "alert", dialog, 1, 0);
    if (installed >= 0) installed = d_method(dom, ctx, global, "confirm", dialog, 1, 1);
    if (installed >= 0) installed = d_method(dom, ctx, global, "prompt", dialog, 2, 2);
    if (installed >= 0) installed = d_event_install(dom, ctx, global);
    if (installed >= 0) installed = d_timer_install(dom, ctx, global);
    if (installed >= 0) installed = d_window_install(dom, ctx, global);
    if (installed >= 0) installed = d_modern_install(dom, ctx, global);
    JS_FreeValue(ctx, global);
    /* Last: from here the runtime hands the binding its script's failures
     * (report.c), which a binding that failed to be made never sees. */
    if (installed < 0 || !d_report_host(dom)) goto fail;
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
        if (id == dom->anchor_class || id == dom->node_class || id == dom->collection_class ||
            id == dom->style_class || id == dom->token_class || id == dom->map_class)
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
    /* What the page changed in its storage and has not committed goes to
     * the store before the binding is gone: the last of its commits. */
    d_storage_drain(dom);
    d_report_drain(dom);
    d_event_drain(dom);
    d_timer_drain(dom);
    d_message_drain(dom);
    d_idle_drain(dom);
    /* The requests out are cancelled with the host and forgotten. */
    d_fetch_drain(dom);
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
    d_idle_free(dom);
    d_fetch_free(dom);
    d_window_free(dom);
    d_document_free(dom);
    d_view_free(dom);
    d_storage_free(dom);
    d_attr_table_free(dom);
    d_report_free(dom);
    for (DValue *entry = dom->values; entry != NULL;) {
        DValue *next = entry->next;
        d_event_free(dom, entry);
        d_attr_free(dom, entry);
        d_free(dom, entry->output_default);
        if (entry->node != NULL)
            os64_html_release(dom->document, entry->node);
        if (entry->target != NULL)
            os64_html_release(dom->document, entry->target);
        d_free(dom, entry);
        entry = next;
    }
    os64_free(dom);
}

size_t os64_dom_bytes(const os64_dom_t *dom) { return dom != NULL ? dom->bytes : 0; }
/* The seven registries: wrappers and other held values, listener callbacks,
 * timer callbacks with their arguments, what the requests out hold, the
 * failures waiting for their events (report.c), the windows that posted
 * the messages waiting (message.c), and the idle callbacks (idle.c). */
size_t os64_dom_registry_count(const os64_dom_t *dom)
{
    return dom != NULL ? dom->retained + dom->listeners_held + dom->timers_held + dom->requests_held +
        dom->pending_held + dom->messages_held + dom->idle_live : 0;
}
