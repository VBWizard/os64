#include "internal.h"

/* DOMException (WebIDL § 2.8.1): the exception the Fetch, XMLHttpRequest
 * and DOM standards name their refusals with (AbortError, InvalidStateError,
 * TimeoutError, NetworkError...), with its legacy `code`. An instance's name
 * and message are its own, in engine memory; its prototype inherits
 * Error.prototype, as WebIDL puts it. The binding's older refusals
 * (d_error) are DOMExceptions too, made through this file, once it is
 * installed (core.c's d_error says how). */

static JSClassID exception_slot;

typedef struct {
    char *name, *message;       /* js_malloc'd, UTF-8 */
} DException;

/* WebIDL's names table: the names with a legacy code, in code order
 * (1..25). Codes 2, 6 and 16 keep their constants (DOMSTRING_SIZE_ERR,
 * NO_DATA_ALLOWED_ERR, VALIDATION_ERR) but no name has them, so no
 * instance's code is one of them. */
static const char *const kCodes[] = {
    NULL, "IndexSizeError", NULL, "HierarchyRequestError", "WrongDocumentError",
    "InvalidCharacterError", NULL, "NoModificationAllowedError", "NotFoundError",
    "NotSupportedError", "InUseAttributeError", "InvalidStateError", "SyntaxError",
    "InvalidModificationError", "NamespaceError", "InvalidAccessError", NULL,
    "TypeMismatchError", "SecurityError", "NetworkError", "AbortError", "URLMismatchError",
    "QuotaExceededError", "TimeoutError", "InvalidNodeTypeError", "DataCloneError",
};
static const char *const kConstants[] = {
    NULL, "INDEX_SIZE_ERR", "DOMSTRING_SIZE_ERR", "HIERARCHY_REQUEST_ERR", "WRONG_DOCUMENT_ERR",
    "INVALID_CHARACTER_ERR", "NO_DATA_ALLOWED_ERR", "NO_MODIFICATION_ALLOWED_ERR", "NOT_FOUND_ERR",
    "NOT_SUPPORTED_ERR", "INUSE_ATTRIBUTE_ERR", "INVALID_STATE_ERR", "SYNTAX_ERR",
    "INVALID_MODIFICATION_ERR", "NAMESPACE_ERR", "INVALID_ACCESS_ERR", "VALIDATION_ERR",
    "TYPE_MISMATCH_ERR", "SECURITY_ERR", "NETWORK_ERR", "ABORT_ERR", "URL_MISMATCH_ERR",
    "QUOTA_EXCEEDED_ERR", "TIMEOUT_ERR", "INVALID_NODE_TYPE_ERR", "DATA_CLONE_ERR",
};

static int32_t code_of(const char *name)
{
    for (int32_t i = 1; i < (int32_t)(sizeof(kCodes) / sizeof(kCodes[0])); i++)
        if (kCodes[i] != NULL && os64_streq(kCodes[i], name)) return i;
    return 0;
}

static void exception_finalizer(JSRuntime *rt, JSValue value)
{
    DException *e = JS_GetOpaque(value, os64_js_class_id(&exception_slot));
    if (e == NULL) return;
    js_free_rt(rt, e->name);
    js_free_rt(rt, e->message);
    js_free_rt(rt, e);
}

static JSValue exception_make(JSContext *ctx, JSValueConst prototype, const char *message, size_t message_length,
                              const char *name, size_t name_length)
{
    JSClassID id = os64_js_class_id(&exception_slot);
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id)
                                            : JS_NewObjectClass(ctx, (int)id);
    if (JS_IsException(object)) return object;
    DException *e = js_mallocz(ctx, sizeof(*e));
    char *n = e != NULL ? js_malloc(ctx, name_length + 1) : NULL;
    char *m = n != NULL ? js_malloc(ctx, message_length + 1) : NULL;
    if (m == NULL) {
        js_free(ctx, n);
        js_free(ctx, e);
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    os64_memcpy(n, name, name_length);
    n[name_length] = '\0';
    os64_memcpy(m, message, message_length);
    m[message_length] = '\0';
    e->name = n;
    e->message = m;
    JS_SetOpaque(object, e);
    return object;
}

JSValue d_dom_exception_new(os64_dom_t *dom, JSContext *ctx, const char *name, const char *message)
{
    JSValueConst prototype = dom->exception_prototype != NULL ? dom->exception_prototype->value : JS_UNDEFINED;
    return exception_make(ctx, prototype, message, os64_strlen(message), name, os64_strlen(name));
}

JSValue d_dom_exception(os64_dom_t *dom, JSContext *ctx, const char *name, const char *message)
{
    JSValue error = d_dom_exception_new(dom, ctx, name, message);
    if (JS_IsException(error)) return error;
    return JS_Throw(ctx, error);
}

/* new DOMException(message = "", name = "Error"). */
static JSValue exception_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    DString message = {0}, name = {0};
    if (argc > 0 && !JS_IsUndefined(argv[0]) && !d_string_text(dom, ctx, argv[0], &message, false))
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !d_string_text(dom, ctx, argv[1], &name, false)) {
        d_string_free(dom, &message);
        return JS_EXCEPTION;
    }
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    JSValue made = JS_IsException(prototype) ? JS_EXCEPTION
        : exception_make(ctx, JS_IsObject(prototype) ? (JSValueConst)prototype : dom->exception_prototype->value,
                         message.data != NULL ? message.data : "", message.length,
                         name.data != NULL ? name.data : "Error", name.data != NULL ? name.length : 5);
    JS_FreeValue(ctx, prototype);
    d_string_free(dom, &message);
    d_string_free(dom, &name);
    return made;
}

enum { X_NAME, X_MESSAGE, X_CODE };

static JSValue exception_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                                JSValue *data)
{
    (void)argc; (void)argv;
    if (d_callback(ctx, data, OS64_JS_ABI_ID) == NULL) return JS_EXCEPTION;
    DException *e = JS_GetOpaque(self, os64_js_class_id(&exception_slot));
    if (e == NULL) return JS_ThrowTypeError(ctx, "Expected a DOMException");
    switch (magic) {
    case X_NAME: return JS_NewString(ctx, e->name);
    case X_MESSAGE: return JS_NewString(ctx, e->message);
    default: return JS_NewInt32(ctx, code_of(e->name));
    }
}

static int constants(JSContext *ctx, JSValueConst target)
{
    for (int32_t i = 1; i < (int32_t)(sizeof(kConstants) / sizeof(kConstants[0])); i++)
        if (JS_DefinePropertyValueStr(ctx, target, kConstants[i], JS_NewInt32(ctx, i), JS_PROP_ENUMERABLE) < 0)
            return -1;
    return 0;
}

int d_exception_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef definition = {.class_name = "DOMException", .finalizer = exception_finalizer};
    JSClassID id = os64_js_class_id(&exception_slot);
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, &definition) < 0) return -1;
    /* DOMException.prototype inherits Error.prototype (WebIDL § 3.7.3's
     * exception rule), read before any page can replace Error. */
    JSValue error = JS_GetPropertyStr(ctx, global, "Error");
    JSValue error_prototype = JS_IsException(error) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, error, "prototype");
    JS_FreeValue(ctx, error);
    if (JS_IsException(error_prototype)) return -1;
    JSValue prototype = JS_NewObjectProto(ctx, error_prototype);
    JS_FreeValue(ctx, error_prototype);
    dom->exception_prototype = d_retain(dom, ctx, prototype);
    if (dom->exception_prototype == NULL) return -1;
    JSValueConst p = dom->exception_prototype->value;
    JS_SetClassProto(ctx, id, JS_DupValue(ctx, p));
    if (d_accessor(dom, ctx, p, "name", exception_member, X_NAME, false) < 0 ||
        d_accessor(dom, ctx, p, "message", exception_member, X_MESSAGE, false) < 0 ||
        d_accessor(dom, ctx, p, "code", exception_member, X_CODE, false) < 0 || constants(ctx, p) < 0)
        return -1;
    JSValue interface = d_interface_new(ctx, "DOMException", 0, exception_constructor, p, JS_UNDEFINED);
    if (JS_IsException(interface)) return -1;
    if (constants(ctx, interface) < 0) {
        JS_FreeValue(ctx, interface);
        return -1;
    }
    return JS_DefinePropertyValueStr(ctx, global, "DOMException", interface, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}
