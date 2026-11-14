#include "internal.h"
#include "os64/proc.h"

/* Allocation ownership survives cross-document operations. The packed word
 * stores the charged size and census kind; the owner releases that charge. */
typedef struct { size_t word; os64_dom_t *owner; } DBlock;
#define D_BLOCK_KIND_SHIFT 56
#define D_BLOCK_SIZE_MASK (((size_t)1 << D_BLOCK_KIND_SHIFT) - 1)
_Static_assert(OS64_DOM_MEM_KINDS <= 256, "a block's kind is its header's top byte");
static JSClassID cluster_slot, anchor_slot, node_slot, collection_slot, style_slot, token_slot, map_slot;

static const char *const kMemKindNames[OS64_DOM_MEM_KINDS] = {
    [OS64_DOM_MEM_OTHER] = "other", [OS64_DOM_MEM_WRAPPER] = "wrapper", [OS64_DOM_MEM_RETAINED] = "retained",
    [OS64_DOM_MEM_EXTRA] = "extra",
    [OS64_DOM_MEM_LISTENER] = "listener", [OS64_DOM_MEM_ATTR] = "attr", [OS64_DOM_MEM_COLLECTION] = "collection",
    [OS64_DOM_MEM_MUTATION] = "mutation", [OS64_DOM_MEM_INTERSECTION] = "intersection",
    [OS64_DOM_MEM_RESIZE] = "resize", [OS64_DOM_MEM_CUSTOM] = "custom", [OS64_DOM_MEM_RANGE] = "range",
    [OS64_DOM_MEM_TIMER] = "timer", [OS64_DOM_MEM_URL] = "url", [OS64_DOM_MEM_TEXT] = "text",
};

void *d_alloc_kind(os64_dom_t *dom, size_t size, os64_dom_mem_kind_t kind)
{
    if ((unsigned)kind >= OS64_DOM_MEM_KINDS) kind = OS64_DOM_MEM_OTHER;
    if (size > D_BLOCK_SIZE_MASK - sizeof(DBlock)) return NULL;
    size_t total = sizeof(DBlock) + size;
    if (total > dom->options.max_bytes - dom->bytes ||
        (dom->cluster && total > dom->cluster->limits.dom_bytes - dom->cluster->bytes)) return NULL;
    DBlock *block = os64_malloc(total);
    if (block == NULL) return NULL;
    block->word = total | (size_t)kind << D_BLOCK_KIND_SHIFT;
    block->owner = dom;
    if (dom->cluster) dom->cluster->bytes += total;
    dom->bytes += total;
    os64_dom_mem_t *m = &dom->memory[kind];
    m->bytes += total;
    m->blocks++;
    m->made++;
    os64_memset(block + 1, 0, size);
    return block + 1;
}

void *d_alloc(os64_dom_t *dom, size_t size)
{
    return d_alloc_kind(dom, size, OS64_DOM_MEM_OTHER);
}

void d_free(os64_dom_t *dom, void *ptr)
{
    if (ptr == NULL) return;
    DBlock *block = (DBlock *)ptr - 1;
    dom = block->owner;
    size_t total = block->word & D_BLOCK_SIZE_MASK;
    if (dom->cluster) dom->cluster->bytes -= total;
    os64_dom_mem_t *m = &dom->memory[block->word >> D_BLOCK_KIND_SHIFT];
    dom->bytes -= total;
    m->bytes -= total;
    m->blocks--;
    os64_free(block);
}

const char *os64_dom_mem_kind_name(os64_dom_mem_kind_t kind)
{
    return (unsigned)kind < OS64_DOM_MEM_KINDS ? kMemKindNames[kind] : "?";
}

os64_dom_mem_t os64_dom_memory(const os64_dom_t *dom, os64_dom_mem_kind_t kind)
{
    os64_dom_mem_t none = {0};
    return dom != NULL && (unsigned)kind < OS64_DOM_MEM_KINDS ? dom->memory[kind] : none;
}

size_t os64_dom_canvas_bytes(const os64_dom_t *dom) { return dom != NULL ? dom->canvas_bytes : 0; }

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
    else if (status == OS64_HTML_NOT_SUPPORTED) name = "NotSupportedError";
    else if (status == OS64_HTML_INVALID_NODE_TYPE) name = "InvalidNodeTypeError";
    else if (status == OS64_HTML_INDEX_SIZE) name = "IndexSizeError";
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
    if (dom == NULL || dom->closed || (dom->cluster && (dom->cluster->poisoned || dom->cluster->drained))) {
        d_error(ctx, "InvalidStateError", "DOM binding is closed");
        return false;
    }
    os64_js_outcome_t outcome;
    if (dom->cluster) {
        os64_js_realm_t *realm = os64_js_realm_from_context(dom->runtime, ctx, abi, &outcome);
        for (size_t i = 0; realm && i < dom->cluster->count; ++i)
            if (dom->cluster->members[i]->realm == realm && !dom->cluster->members[i]->closed) return true;
        d_error(ctx, "InvalidStateError", "Context is not a usable cluster member");
        return false;
    }
    if (os64_js_realm_context(dom->realm, abi, &outcome) != ctx) {
        d_error(ctx, "InvalidStateError", "DOM runtime is unavailable");
        return false;
    }
    return true;
}

bool d_delivery_active(const os64_dom_t *dom)
{
    return dom && !dom->closed && !dom->installing && !(dom->cluster && (dom->cluster->poisoned || dom->cluster->drained)) &&
           !os64_js_realm_is_retired(dom->realm);
}

bool d_require_active(os64_dom_t *dom, JSContext *ctx)
{
    if (d_delivery_active(dom)) return true;
    d_error(ctx, "InvalidStateError", "The document's host service is inactive");
    return false;
}

void d_provider_hold(os64_dom_t *dom)
{
    dom->provider_borrows++;
    if (dom->cluster) dom->cluster->provider_borrows++;
}

bool d_provider_enter(os64_dom_t *dom)
{
    if (!d_delivery_active(dom)) return false;
    d_provider_hold(dom);
    return true;
}

bool d_provider_begin(os64_dom_t *dom, JSContext *ctx)
{
    if (d_provider_enter(dom)) return true;
    d_error(ctx, "InvalidStateError", "The document's host service is inactive");
    return false;
}

void d_provider_leave(os64_dom_t *dom)
{
    dom->provider_borrows--;
    if (dom->cluster) dom->cluster->provider_borrows--;
}

bool d_provider_busy(const os64_dom_t *dom)
{
    return dom && (dom->provider_borrows || (dom->cluster && dom->cluster->provider_borrows));
}

uint64_t d_host_now_ms(const os64_dom_t *dom)
{
    if (dom == NULL) return 0;
    os64_dom_t *owner = (os64_dom_t *)dom;
    if (dom->options.now_ms != NULL && d_provider_enter(owner)) {
        owner->clock_seen_ms = dom->options.now_ms(dom->options.host_opaque);
        d_provider_leave(owner);
    }
    return dom->clock_seen_ms;
}

uint64_t d_host_now_us(const os64_dom_t *dom)
{
    if (dom == NULL) return 0;
    if (!d_delivery_active(dom)) return dom->clock_seen_us;
    os64_dom_t *owner = (os64_dom_t *)dom;
    if (dom->options.now_us == NULL) owner->clock_seen_us = d_host_now_ms(dom) * 1000;
    else if (d_provider_enter(owner)) {
        owner->clock_seen_us = dom->options.now_us(dom->options.host_opaque);
        d_provider_leave(owner);
    }
    return dom->clock_seen_us;
}

int d_callback_eligible(os64_dom_t *owner, JSContext *ctx, JSValueConst callback)
{
    /* A bound function or callable proxy can belong to a different realm
     * from the registration. A revoked proxy keeps its normal exception. */
    JSContext *function_ctx = JS_GetFunctionRealm(ctx, callback);
    if (!function_ctx) return -1;
    os64_js_outcome_t outcome;
    os64_js_realm_t *realm = os64_js_realm_from_context(owner->runtime, function_ctx, OS64_JS_ABI_ID, &outcome);
    if (!realm) {
        d_error(ctx, "InvalidStateError", "Callback realm is unavailable");
        return -1;
    }
    return !os64_js_realm_is_retired(realm);
}

int d_delivery_eligible(os64_dom_t *activity, JSContext *ctx, JSValueConst callback)
{
    return d_delivery_active(activity) ? d_callback_eligible(activity, ctx, callback) : 0;
}

bool d_require_callback(os64_dom_t *owner, JSContext *ctx, JSValueConst callback)
{
    int eligible = d_callback_eligible(owner, ctx, callback);
    if (eligible == 0) d_error(ctx, "InvalidStateError", "The host callback is inactive");
    return eligible > 0;
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
    if (status == OS64_JS_BAD_ARGUMENT) return os64_js_realm_context(dom->realm, OS64_JS_ABI_ID, &probe) != NULL;
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
    DValue *entry = d_alloc_kind(dom, sizeof(*entry), OS64_DOM_MEM_RETAINED);
    if (entry == NULL) {
        JS_FreeValue(ctx, value);
        d_error(ctx, "QuotaExceededError", "DOM registry quota exceeded");
        return NULL;
    }
    entry->value = value;
    entry->dom = dom;
    entry->next = dom->values;
    dom->values = entry;
    dom->retained++;
    return entry;
}

const DValueMore d_no_more;

DValueMore *d_more(os64_dom_t *dom, JSContext *ctx, DValue *entry)
{
    if (entry->more != NULL) return entry->more;
    entry->more = d_alloc_kind(entry->dom != NULL ? entry->dom : dom, sizeof(*entry->more), OS64_DOM_MEM_EXTRA);
    if (entry->more == NULL && ctx != NULL) d_error(ctx, "QuotaExceededError", "DOM wrapper quota exceeded");
    return entry->more;
}

void d_unretain(os64_dom_t *dom, JSContext *ctx, DValue *entry)
{
    dom->values = entry->next;
    dom->retained--;
    JS_FreeValue(ctx, entry->value);
    d_free(dom, entry);
}

void d_more_free(os64_dom_t *dom, DValue *entry)
{
    d_free(dom, entry->more);
    entry->more = NULL;
}

bool d_same_object(JSValueConst value, const DValue *held)
{
    return held != NULL && JS_IsObject(value) && JS_VALUE_GET_PTR(value) == JS_VALUE_GET_PTR(held->value);
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
    if (magic == 0 && dom->options.alert != NULL) {
        if (!d_provider_begin(dom, ctx)) { d_string_free(dom, &message); return JS_EXCEPTION; }
        dom->options.alert(dom->options.alert_opaque, message.data != NULL ? message.data : "", message.length);
        d_provider_leave(dom);
    }
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
    os64_js_realm_t *realm = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, outcome);
    return realm != NULL ? d_create_in_realm(runtime, realm, document, state, options, outcome) : NULL;
}

static os64_dom_t *create_binding(os64_js_runtime_t *, os64_js_realm_t *,
    os64_html_document_t *, os64_page_state_t *, const os64_dom_options_t *,
    os64_js_outcome_t *, os64_dom_cluster_t *, bool *);

os64_dom_t *d_create_in_realm(os64_js_runtime_t *runtime, os64_js_realm_t *realm,
    os64_html_document_t *document, os64_page_state_t *state,
    const os64_dom_options_t *options, os64_js_outcome_t *outcome)
{
    return create_binding(runtime, realm, document, state, options, outcome, NULL, NULL);
}

os64_dom_admission_t os64_dom_create_in_realm(os64_dom_cluster_t *cluster, os64_js_realm_t *realm,
    os64_html_document_t *document, os64_page_state_t *state,
    const os64_dom_options_t *options, os64_js_outcome_t *outcome)
{
    os64_dom_admission_t admission = {0};
    if (!outcome) return admission;
    if (!cluster) { outcome->status = OS64_JS_BAD_ARGUMENT; return admission; }
    admission.binding = create_binding(cluster->runtime, realm, document, state,
        options, outcome, cluster, &admission.consumed);
    return admission;
}

os64_dom_cluster_t *os64_dom_cluster_create(os64_js_runtime_t *runtime,
    const os64_dom_cluster_limits_t *limits, os64_js_outcome_t *outcome)
{
    if (!outcome) return NULL;
    os64_js_realm_t *primary = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, outcome);
    JSContext *ctx = os64_js_realm_context(primary, OS64_JS_ABI_ID, outcome);
    if (!ctx) return NULL;
    JSRuntime *engine = JS_GetRuntime(ctx);
    JSClassID id = os64_js_class_id(&cluster_slot);
    if (!limits || limits->dom_bytes < sizeof(os64_dom_cluster_t) ||
        JS_IsRegisteredClass(engine, id) ||
        JS_IsRegisteredClass(engine, os64_js_class_id(&anchor_slot))) {
        outcome->status = OS64_JS_BAD_ARGUMENT;
        return NULL;
    }
    os64_dom_cluster_t *cluster = os64_calloc(1, sizeof(*cluster));
    if (!cluster) { outcome->status = OS64_JS_HOST_FAILURE; return NULL; }
    cluster->runtime = runtime;
    cluster->limits = *limits;
    cluster->bytes = sizeof(*cluster);
    cluster->html = os64_html_group_create(limits->html_bytes);
    cluster->page = os64_page_state_group_create(limits->page_bytes);
    if (!cluster->html || !cluster->page) {
        outcome->status = OS64_JS_HOST_FAILURE;
        goto fail;
    }
    if (os64_js_realm_task_begin(primary, OS64_JS_ABI_ID, "DOM cluster", outcome) != OS64_JS_OK) goto fail;
    /* Registration claims this runtime without using libjs's opaque slots
     * or storing a native pointer in an engine object. */
    const JSClassDef definition = {.class_name = "DOMCluster"};
    int registered = JS_NewClass(engine, id, &definition);
    if (os64_js_task_end(runtime, OS64_JS_ABI_ID, outcome) != OS64_JS_OK) goto fail;
    if (registered < 0) { outcome->status = OS64_JS_HOST_FAILURE; goto fail; }
    return cluster;
fail:
    os64_page_state_group_free(cluster->page);
    os64_html_group_free(cluster->html);
    os64_free(cluster);
    return NULL;
}

os64_dom_cluster_t *os64_dom_cluster_create_with_timer_settings(os64_js_runtime_t *runtime,
    const os64_dom_cluster_limits_t *limits, os64_js_outcome_t *outcome)
{
    if (os64_js_enable_host_settings(runtime, OS64_JS_ABI_ID, outcome) != OS64_JS_OK) return NULL;
    os64_dom_cluster_t *cluster = os64_dom_cluster_create(runtime, limits, outcome);
    if (cluster) cluster->timer_settings = true;
    return cluster;
}

/* Trusted bootstrap: install interfaces and private algorithms, without
 * exposing the candidate or invoking author code. */
static int install_binding(os64_dom_t *dom, JSContext *ctx)
{
    os64_dom_options_t selected = dom->options;
    os64_html_document_t *document = dom->document;
    os64_dom_cluster_t *cluster = dom->cluster;
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
    if ((!JS_IsRegisteredClass(dom->engine, dom->anchor_class) && JS_NewClass(dom->engine, dom->anchor_class, &anchor_definition) < 0) ||
        (!JS_IsRegisteredClass(dom->engine, dom->node_class) && JS_NewClass(dom->engine, dom->node_class, &node_definition) < 0) ||
        (!JS_IsRegisteredClass(dom->engine, dom->collection_class) && JS_NewClass(dom->engine, dom->collection_class, &collection_definition) < 0) ||
        (!JS_IsRegisteredClass(dom->engine, dom->style_class) && JS_NewClass(dom->engine, dom->style_class, &style_definition) < 0) ||
        (!JS_IsRegisteredClass(dom->engine, dom->token_class) && JS_NewClass(dom->engine, dom->token_class, &token_definition) < 0) ||
        (!JS_IsRegisteredClass(dom->engine, dom->map_class) && JS_NewClass(dom->engine, dom->map_class, &map_definition) < 0)) goto fail;
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
    if (d_document_templates(dom, ctx, document->document) < 0 || d_node_install(dom, ctx) < 0) goto fail;
    JSValue collection_prototype = JS_NewObject(ctx);
    if (JS_IsException(collection_prototype)) goto fail;
    if (d_collection_install(dom, ctx, collection_prototype) < 0) {
        JS_FreeValue(ctx, collection_prototype);
        goto fail;
    }
    JS_SetClassProto(ctx, dom->collection_class, collection_prototype);
    if (d_static_list_install(dom, ctx) < 0 || d_mixin_install(dom, ctx) < 0) goto fail;
    if (selected.modern || cluster) d_mutation_start(dom);
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
    return 0;
fail:
    return -1;
}

static os64_dom_t *allocate_binding(os64_js_runtime_t *runtime, os64_js_realm_t *realm,
    JSContext *ctx, os64_html_document_t *document, os64_page_state_t *state,
    os64_dom_options_t selected, os64_dom_cluster_t *cluster, os64_js_outcome_t *outcome)
{
    os64_dom_t *dom = os64_calloc(1, sizeof(*dom));
    if (dom == NULL) { outcome->status = OS64_JS_HOST_FAILURE; return NULL; }
    dom->runtime = runtime;
    dom->realm = realm;
    dom->cluster = cluster;
    dom->wa_borrowed_floor = UINT64_MAX;
    /* A cluster member can adopt targets whose effects predate its admission. */
    dom->wa_index_stale = true;
    dom->observers = cluster ? cluster->observers : NULL;
    dom->engine = JS_GetRuntime(ctx);
    dom->document = document;
    dom->state = state;
    dom->options = selected;
    dom->bytes = sizeof(*dom);
    dom->memory[OS64_DOM_MEM_OTHER] = (os64_dom_mem_t){sizeof(*dom), 1, 1};
    dom->anchor_class = os64_js_class_id(&anchor_slot);
    dom->node_class = os64_js_class_id(&node_slot);
    dom->collection_class = os64_js_class_id(&collection_slot);
    dom->style_class = os64_js_class_id(&style_slot);
    dom->token_class = os64_js_class_id(&token_slot);
    dom->map_class = os64_js_class_id(&map_slot);
    return dom;
}

static os64_dom_t *create_binding(os64_js_runtime_t *runtime, os64_js_realm_t *realm,
                              os64_html_document_t *document, os64_page_state_t *state,
                              const os64_dom_options_t *options, os64_js_outcome_t *outcome,
                              os64_dom_cluster_t *cluster, bool *consumed)
{
    if (outcome == NULL) return NULL;
    JSContext *ctx = os64_js_realm_context(realm, OS64_JS_ABI_ID, outcome);
    if (ctx == NULL) return NULL;
    if (os64_js_realm_from_context(runtime, ctx, OS64_JS_ABI_ID, outcome) != realm) return NULL;
    if (document == NULL || state == NULL || os64_page_state_document(state) != document) {
        outcome->status = OS64_JS_BAD_ARGUMENT;
        return NULL;
    }
    if (cluster) {
        if (cluster->poisoned || cluster->drained || cluster->count == D_REALMS) {
            outcome->status = OS64_JS_BAD_ARGUMENT;
            return NULL;
        }
        for (size_t i = 0; i < cluster->count; ++i)
            if (cluster->members[i]->realm == realm) {
                outcome->status = OS64_JS_BAD_ARGUMENT;
                return NULL;
            }
        if (os64_html_group_check(cluster->html, document) != OS64_HTML_OK ||
            !os64_page_state_group_check(cluster->page, state) ||
            sizeof(os64_dom_t) > cluster->limits.dom_bytes - cluster->bytes) {
            outcome->status = OS64_JS_HOST_FAILURE;
            return NULL;
        }
    } else if (JS_IsRegisteredClass(JS_GetRuntime(ctx), os64_js_class_id(&anchor_slot)) ||
               JS_IsRegisteredClass(JS_GetRuntime(ctx), os64_js_class_id(&cluster_slot))) {
        outcome->status = OS64_JS_BAD_ARGUMENT;
        return NULL;
    }
    os64_dom_options_t selected = options != NULL ? *options : os64_dom_default_options();
    if (selected.max_bytes < sizeof(os64_dom_t)) {
        outcome->status = OS64_JS_HOST_FAILURE;
        return NULL;
    }
    os64_dom_t *dom = allocate_binding(runtime, realm, ctx, document, state, selected, cluster, outcome);
    if (!dom) return NULL;
    if (cluster) {
        /* Both ledgers were preflighted and no script has run since then.
         * Admission itself allocates nothing; ownership changes as one pair. */
        if (os64_js_realm_task_begin(realm, OS64_JS_ABI_ID, "DOM installation", outcome) != OS64_JS_OK) {
            os64_free(dom);
            return NULL;
        }
        if (os64_html_group_add(cluster->html, document, (uint8_t)cluster->count) != OS64_HTML_OK ||
            !os64_page_state_group_add(cluster->page, state))
            os64_exit((int32_t)OS64_PAGE_FATAL_EXIT);
        cluster->members[cluster->count++] = dom;
        cluster->bytes += sizeof(*dom);
        *consumed = true;
    }
    if (install_binding(dom, ctx) < 0) goto fail;
    if (cluster) {
        if (os64_js_task_end(runtime, OS64_JS_ABI_ID, outcome) != OS64_JS_OK) {
            cluster->poisoned = true;
            os64_dom_cluster_drain(cluster);
            return NULL;
        }
    }
    outcome->status = OS64_JS_OK;
    return dom;
fail:
    if (cluster) {
        construction_error(ctx, outcome);
        os64_js_outcome_t ended;
        os64_js_status_t status = os64_js_task_end(runtime, OS64_JS_ABI_ID, &ended);
        if (status != OS64_JS_OK) *outcome = ended;
        cluster->poisoned = true;
        os64_dom_cluster_drain(cluster);
        return NULL;
    }
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

os64_js_status_t os64_dom_depart(os64_dom_t *dom, os64_js_outcome_t *outcome)
{
    return os64_js_realm_retire(dom ? dom->realm : NULL, OS64_JS_ABI_ID, outcome);
}

static bool departure_busy(const os64_dom_t *dom)
{
    if (dom->closed) return false;
    return dom->departure_finishing || dom->dispatching || dom->current_event ||
        dom->animation_running || dom->wa_running || dom->idle_running ||
        dom->pointer.running || dom->pointer.canceling || dom->pointer.draining ||
        dom->clicking || dom->submitting || d_custom_busy(dom) || d_observers_busy(dom) ||
        d_intersections_busy(dom) || d_resizes_busy(dom);
}

static void departure_detach(os64_dom_t *dom)
{
    /* Keep copied configuration and cached values; none of these host
     * callbacks or opaque pointers may outlive a successful finish. */
    dom->options.alert = NULL;
    dom->options.alert_opaque = NULL;
    dom->options.now_ms = NULL;
    dom->options.now_us = NULL;
    dom->options.script_connected = NULL;
    dom->options.activate = NULL;
    dom->options.write = NULL;
    dom->options.timing = NULL;
    dom->options.zoom = NULL;
    dom->options.broken = NULL;
    dom->options.failed = NULL;
    dom->options.entropy = NULL;
    dom->options.focus = NULL;
    dom->options.host_opaque = NULL;
    os64_memset(&dom->image_provider, 0, sizeof(dom->image_provider));
    os64_memset(&dom->canvas_text, 0, sizeof(dom->canvas_text));
    dom->geometry = NULL; dom->geometry_opaque = NULL;
    dom->svg_hit = NULL; dom->svg_hit_opaque = NULL;
    dom->hit = NULL; dom->hit_opaque = NULL;
    dom->text_rects = NULL; dom->text_rects_opaque = NULL;
    dom->user_agent = NULL; dom->user_agent_opaque = NULL;
    os64_memset(&dom->cookies, 0, sizeof(dom->cookies));
    os64_memset(&dom->view, 0, sizeof(dom->view));
    dom->computed = NULL; dom->computed_opaque = NULL;
    os64_memset(&dom->history_host, 0, sizeof(dom->history_host));
    dom->intersections_provider = NULL; dom->intersections_opaque = NULL;
    dom->resizes_provider = NULL; dom->resizes_opaque = NULL;
    os64_dom_set_global_miss(dom, NULL, NULL);
}

static bool child_runtime_failed(os64_js_status_t status)
{
    return status == OS64_JS_FAILED_RUNTIME || status == OS64_JS_LIMIT ||
        status == OS64_JS_CANCELLED || status == OS64_JS_HOST_FAILURE;
}

static os64_js_status_t child_install(os64_js_realm_t *realm, JSContext *ctx, void *opaque)
{
    (void)realm;
    os64_dom_t *dom = opaque;
    if (install_binding(dom, ctx) == 0) return OS64_JS_OK;
    return JS_HasException(ctx) ? OS64_JS_EXCEPTION : OS64_JS_ADMISSION_REFUSED;
}

/* Bootstrap creates private interfaces and wrapper records, not author
 * registrations or native tasks. Keep its shell for engine finalizers, but
 * remove its wrappers and newly created dispatchers from sibling lookup. */
static void child_abort(os64_dom_t *dom, struct DObservers *observers, struct DCustomAgent *custom)
{
    os64_dom_cluster_t *cluster = dom->cluster;
    dom->closed = true;
    d_mutation_stop(dom);
    departure_detach(dom);
    dom->options.url = dom->options.referrer = NULL;
    for (size_t b = 0; b < D_BUCKETS; ++b)
        for (DValue **at = &cluster->buckets[b]; *at;) {
            DValue *entry = *at;
            if (entry->dom != dom) { at = &entry->hash_next; continue; }
            *at = entry->hash_next;
            entry->hash_next = NULL;
        }
    cluster->observers = observers;
    cluster->custom_agent = custom;
    for (size_t i = 0; i < cluster->count; ++i)
        if (!cluster->members[i]->closed) cluster->members[i]->observers = observers;
    for (DValue *entry = dom->values; entry; entry = entry->next) {
        JS_FreeValueRT(dom->engine, entry->value);
        entry->value = JS_UNDEFINED;
    }
    dom->retained = 0;
    dom->installing = false;
}

os64_dom_child_t os64_dom_create_child(os64_dom_cluster_t *cluster,
    os64_html_document_t *document, os64_page_state_t *state,
    const os64_dom_options_t *options, const os64_dom_installation_t *installation,
    os64_js_outcome_t *outcome)
{
    os64_dom_child_t child = {0};
    os64_js_runtime_t *runtime = cluster ? cluster->runtime : NULL;
    /* The engine owns active-outcome alias protection, including invalid
     * native requests made while an enclosing call borrows that outcome. */
    os64_js_status_t status = os64_js_realm_preflight(runtime, OS64_JS_ABI_ID,
        installation ? installation->engine_headroom : 1, outcome);
    if (status != OS64_JS_OK) {
        if (child_runtime_failed(status)) child.state = OS64_DOM_CHILD_RUNTIME_FAILED;
        return child;
    }
    if (!installation || cluster->poisoned || cluster->drained || !document || !state ||
        os64_page_state_document(state) != document) {
        outcome->status = OS64_JS_BAD_ARGUMENT;
        return child;
    }
    if (cluster->provider_borrows) { outcome->status = OS64_JS_BUSY; return child; }
    if (cluster->count == D_REALMS) { outcome->status = OS64_JS_REALM_LIMIT; return child; }
    os64_dom_options_t selected = options ? *options : os64_dom_default_options();
    if (selected.max_bytes < sizeof(os64_dom_t) ||
        sizeof(os64_dom_t) > cluster->limits.dom_bytes - cluster->bytes ||
        os64_html_group_check(cluster->html, document) != OS64_HTML_OK ||
        !os64_page_state_group_check(cluster->page, state)) {
        outcome->status = OS64_JS_ADMISSION_REFUSED;
        return child;
    }
    JSContext *ctx = os64_js_context(runtime, OS64_JS_ABI_ID, outcome);
    if (!ctx) { child.state = OS64_DOM_CHILD_RUNTIME_FAILED; return child; }
    os64_dom_t *dom = allocate_binding(runtime, NULL, ctx, document, state, selected, cluster, outcome);
    if (!dom) { outcome->status = OS64_JS_ADMISSION_REFUSED; return child; }
    status = os64_js_realm_stage(runtime, OS64_JS_ABI_ID, installation->engine_headroom,
                                 &child.realm, outcome);
    if (status != OS64_JS_OK) {
        os64_free(dom);
        if (child_runtime_failed(status)) child.state = OS64_DOM_CHILD_RUNTIME_FAILED;
        return child;
    }
    dom->realm = child.realm;
    dom->installing = true;
    dom->clock_seen_ms = installation->now_ms;
    dom->clock_seen_us = installation->now_us;
    dom->timing_seen = installation->timing;
    dom->time_origin_us = installation->timing.origin_us;
    dom->time_origin_epoch_us = installation->timing.origin_epoch_us;
    if (os64_html_group_add(cluster->html, document, (uint8_t)cluster->count) != OS64_HTML_OK ||
        !os64_page_state_group_add(cluster->page, state))
        os64_exit((int32_t)OS64_PAGE_FATAL_EXIT);
    cluster->members[cluster->count++] = dom;
    cluster->bytes += sizeof(*dom);
    child.binding = dom;
    child.consumed = true;
    struct DObservers *observers = cluster->observers;
    struct DCustomAgent *custom = cluster->custom_agent;
    status = os64_js_realm_setup(child.realm, OS64_JS_ABI_ID, child_install, dom, outcome);
    if (status == OS64_JS_OK)
        status = os64_js_realm_activate(child.realm, OS64_JS_ABI_ID, outcome);
    if (status == OS64_JS_OK) {
        dom->installing = false;
        child.state = OS64_DOM_CHILD_ACTIVE;
    } else {
        os64_js_outcome_t retired;
        (void)os64_js_realm_retire(child.realm, OS64_JS_ABI_ID, &retired);
        child_abort(dom, observers, custom);
        child.state = child_runtime_failed(status) ? OS64_DOM_CHILD_RUNTIME_FAILED : OS64_DOM_CHILD_FAILED;
    }
    return child;
}

os64_js_status_t os64_dom_depart_finish(os64_dom_t *dom, os64_js_outcome_t *outcome)
{
    /* Query first: in particular, do not overwrite an outcome borrowed by
     * an enclosing engine call, even when a DOM precondition also fails. */
    os64_js_status_t status = os64_js_quiescent(dom ? dom->runtime : NULL, OS64_JS_ABI_ID, outcome);
    if (status != OS64_JS_OK) return status;
    if (dom->closed || (dom->cluster && (dom->cluster->poisoned || dom->cluster->drained)))
        return outcome->status = OS64_JS_FAILED_RUNTIME;
    if (!os64_js_realm_is_retired(dom->realm)) return outcome->status = OS64_JS_BAD_ARGUMENT;
    if (d_provider_busy(dom)) return outcome->status = OS64_JS_BUSY;
    if (dom->cluster) {
        if (dom->cluster->dispatching) return outcome->status = OS64_JS_BUSY;
        for (size_t i = 0; i < dom->cluster->count; ++i)
            if (departure_busy(dom->cluster->members[i])) return outcome->status = OS64_JS_BUSY;
    } else if (departure_busy(dom)) return outcome->status = OS64_JS_BUSY;
    if (dom->departure_finished) return OS64_JS_OK;
    dom->departure_finishing = true;
    /* These native callbacks still own their opaque storage. Reentrant
     * finish, including a sibling's, sees the finishing flag or their loan. */
    d_storage_drain(dom);
    d_fetch_drain(dom);
    status = os64_js_quiescent(dom->runtime, OS64_JS_ABI_ID, outcome);
    if (status != OS64_JS_OK) goto done;
    status = os64_js_realm_set_module_loader(dom->realm, OS64_JS_ABI_ID, NULL, outcome);
    if (status != OS64_JS_OK) goto done;
    d_report_drain(dom);
    d_timer_drain(dom);
    d_timer_free(dom);
    d_idle_drain(dom);
    d_idle_free(dom);
    d_animation_drain(dom);
    d_message_cancel(dom);
    d_pointer_drain(dom);
    d_web_animations_cancel(dom);
    d_custom_cancel(dom);
    d_observers_cancel(dom);
    d_intersections_cancel(dom);
    d_performance_cancel(dom);
    if (dom->subtle_queue) {
        JS_FreeValueRT(dom->engine, dom->subtle_queue->value);
        dom->subtle_queue->value = JS_UNDEFINED;
    }
    dom->subtle_head = dom->subtle_tail = 0;
    dom->subtle_drain_queued = false;
    departure_detach(dom);
    dom->departure_finished = true;
done:
    dom->departure_finishing = false;
    return status;
}

static void binding_drain(os64_dom_t *dom)
{
    if (dom == NULL || dom->closed) return;
    dom->closed = true;
    d_mutation_stop(dom);
    /* What the page changed in its storage and has not committed goes to
     * the store before the binding is gone: the last of its commits. */
    d_storage_drain(dom);
    d_report_drain(dom);
    d_event_drain(dom);
    d_timer_drain(dom);
    d_message_drain(dom);
    d_idle_drain(dom);
    d_animation_drain(dom);
    d_web_animations_drain(dom);
    d_pointer_drain(dom);
    d_observers_drain(dom);
    d_intersections_drain(dom);
    d_resizes_drain(dom);
    d_walkers_drain(dom);
    d_performance_drain(dom);
    /* The requests out are cancelled with the host and forgotten. */
    d_fetch_drain(dom);
    for (DValue *entry = dom->values; entry != NULL; entry = entry->next) {
        JS_FreeValueRT(dom->engine, entry->value);
        entry->value = JS_UNDEFINED;
    }
    dom->retained = 0;
}

static void binding_free(os64_dom_t *dom)
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
    d_web_animations_free(dom);
    d_fetch_free(dom);
    d_window_free(dom);
    d_document_free(dom);
    d_view_free(dom);
    d_storage_free(dom);
    d_attr_table_free(dom);
    d_report_free(dom);
    d_custom_free(dom);
    d_observers_free(dom);
    d_intersections_free(dom);
    d_resizes_free(dom);
    d_walkers_free(dom);
    d_ranges_free(dom);
    if (dom->body_context != NULL) os64_html_release(dom->document, dom->body_context);
    d_performance_free(dom);
    d_kept_free_all(dom);
    for (DValue *entry = dom->values; entry != NULL;) {
        DValue *next = entry->next;
        d_event_free(dom, entry);
        d_attr_free(dom, entry);
        d_canvas_free(entry);
        d_adopted_free(dom, entry);
        d_free(dom, d_more_peek(entry)->output_default);
        d_more_free(dom, entry);
        if (entry->node != NULL)
            os64_html_release(dom->document, entry->node);
        if (entry->target != NULL)
            os64_html_release(dom->document, entry->target);
        d_free(dom, entry);
        entry = next;
    }
    if (dom->cluster) dom->cluster->bytes -= sizeof(*dom);
    else os64_free(dom);
}

void os64_dom_drain(os64_dom_t *dom)
{
    if (dom && dom->cluster) return;
    binding_drain(dom);
}

void os64_dom_free(os64_dom_t *dom)
{
    if (dom && dom->cluster) return;
    binding_free(dom);
}

void os64_dom_cluster_drain(os64_dom_cluster_t *cluster)
{
    if (!cluster || cluster->drained) return;
    cluster->drained = true;
    /* Close every entry door before releasing callbacks in any member. */
    for (size_t i = 0; i < cluster->count; ++i)
        d_mutation_stop(cluster->members[i]);
    for (size_t i = 0; i < cluster->count; ++i)
        binding_drain(cluster->members[i]);
}

void os64_dom_cluster_free(os64_dom_cluster_t *cluster)
{
    if (!cluster) return;
    /* A later member may release storage charged to an earlier member.
     * Keep all ledger descriptors until the native sweeps are finished. */
    for (size_t i = 0; i < cluster->count; ++i) binding_free(cluster->members[i]);
    for (size_t i = 0; i < cluster->count; ++i) os64_free(cluster->members[i]);
    os64_page_state_group_free(cluster->page);
    os64_html_group_free(cluster->html);
    os64_free(cluster);
}

os64_dom_cluster_limits_t os64_dom_cluster_bytes(const os64_dom_cluster_t *cluster)
{
    if (!cluster) return (os64_dom_cluster_limits_t){0};
    return (os64_dom_cluster_limits_t){os64_html_group_bytes(cluster->html),
        os64_page_state_group_bytes(cluster->page), cluster->bytes,
        cluster->kept_bytes, cluster->canvas_bytes};
}

size_t os64_dom_bytes(const os64_dom_t *dom) { return dom != NULL ? dom->bytes : 0; }
/* The retained registries: wrappers and other held values, listener callbacks,
 * timer callbacks with their arguments, what the requests out hold, the
 * failures waiting for their events (report.c), the windows that posted
 * the messages waiting (message.c), idle callbacks (idle.c) and frame callbacks (animation.c),
 * registered mutation observers (observer.c), the intersection observers that observe
 * something or have entries waiting (intersection.c), and the resize observers that observe
 * something (resize.c). */
size_t os64_dom_registry_count(const os64_dom_t *dom)
{
    return dom != NULL ? dom->retained + dom->listeners_held + dom->timers_held + dom->requests_held +
        dom->pending_held + dom->messages_held + dom->idle_live + dom->animation_count + dom->observers_held +
        dom->intersections_held + dom->resizes_held : 0;
}
