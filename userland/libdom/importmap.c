#include "internal.h"
#include "os64/str.h"

/* IMPORT MAPS (HTML § 8.1.5.3; MODERN_WEB.md § Import maps, as built): the
 * binding's half, which is JSON and exceptions. os64_dom_import_map is HTML's "create
 * an import map parse result" up to the normalization, which is the host's,
 * and the report "register an import map" makes of a parse that threw:
 * the text parsed as JSON.parse parses it, every TypeError "parse an import
 * map string" throws thrown before anything is walked (normalizing throws
 * nothing, so asking first is the same answer), and then each entry of
 * "imports" and of every scope handed to the host's walk. And
 * HTMLScriptElement.supports, which is how a page learns import maps are
 * here. */

struct DImportParse {
    const char *text;
    size_t length;
    const os64_dom_import_map_walk_t *walk;
};

/* An ordered map as JSON makes one: an object that is no array. */
static bool is_map(JSContext *ctx, JSValueConst value)
{
    return JS_IsObject(value) && JS_IsArray(ctx, value) == 0;
}

/* `object`'s own property `name`: 1 with its value in *out, 0 when it has
 * none, -1 with an exception pending. Own, because a page may have given
 * Object.prototype an `imports` the JSON never had. */
static int own(JSContext *ctx, JSValueConst object, const char *name, JSValue *out)
{
    *out = JS_UNDEFINED;
    JSAtom atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL) return -1;
    JSPropertyDescriptor desc;
    int found = JS_GetOwnProperty(ctx, &desc, object, atom);
    JS_FreeAtom(ctx, atom);
    if (found <= 0) return found;
    JS_FreeValue(ctx, desc.getter);
    JS_FreeValue(ctx, desc.setter);
    *out = desc.value;
    return 1;
}

typedef int (*DEach)(JSContext *ctx, void *opaque, const char *key, JSValueConst value);

/* Each of `object`'s own enumerable string-keyed properties, in its order,
 * handed to `each` with its key as UTF-8. A key holding a NUL names
 * nothing the host can keep, and is passed over. 0, or -1 with an
 * exception pending. */
static int each_entry(JSContext *ctx, JSValueConst object, DEach each, void *opaque)
{
    JSPropertyEnum *names = NULL;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &names, &count, object, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) return -1;
    int result = 0;
    for (uint32_t i = 0; result == 0 && i < count; i++) {
        JSValue name = JS_AtomToString(ctx, names[i].atom);
        size_t length = 0;
        const char *key = JS_IsException(name) ? NULL : JS_ToCStringLen(ctx, &length, name);
        JS_FreeValue(ctx, name);
        JSValue value = key != NULL ? JS_GetProperty(ctx, object, names[i].atom) : JS_EXCEPTION;
        if (JS_IsException(value)) result = -1;
        else if (length == os64_strlen(key)) result = each(ctx, opaque, key, value);
        if (key != NULL) JS_FreeCString(ctx, key);
        JS_FreeValue(ctx, value);
    }
    JS_FreePropertyEnum(ctx, names, count);
    return result;
}

/* A scope's value must be a map ("sort and normalize scopes"). */
static int scope_is_map(JSContext *ctx, void *opaque, const char *key, JSValueConst value)
{
    (void)opaque;
    if (is_map(ctx, value)) return 0;
    JS_ThrowTypeError(ctx, "The import map's scope \"%s\" needs to be a JSON object", key);
    return -1;
}

typedef struct {
    struct DImportParse *parse;
    const char *scope;          /* NULL: "imports" */
} DEntries;

/* One entry of a specifier map, handed to the host: its value when it is a
 * string, NULL when it is anything else (a null rule, the host's to make).
 * A host that cannot keep it ran out of room, and the parse fails with a
 * RangeError: the page's map is too large for it, which is the page's
 * failure and not the runtime's, as an engine's out of memory would be. */
static int specifier_entry(JSContext *ctx, void *opaque, const char *key, JSValueConst value)
{
    DEntries *entries = opaque;
    const char *address = NULL;
    size_t length = 0;
    if (JS_IsString(value)) {
        address = JS_ToCStringLen(ctx, &length, value);
        if (address == NULL) return -1;
        if (length != os64_strlen(address)) {
            JS_FreeCString(ctx, address);
            address = NULL;
        }
    }
    const os64_dom_import_map_walk_t *walk = entries->parse->walk;
    bool kept = walk->entry(walk->opaque, entries->scope, key, address);
    if (address != NULL) JS_FreeCString(ctx, address);
    if (kept) return 0;
    JS_ThrowRangeError(ctx, "The import map is larger than yonder keeps");
    return -1;
}

static int scope_entries(JSContext *ctx, void *opaque, const char *key, JSValueConst value)
{
    DEntries entries = {opaque, key};
    return each_entry(ctx, value, specifier_entry, &entries);
}

/* The parse, as a function the task calls, so what it throws is reported
 * as any task's exception is. */
static JSValue import_map_parse(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                                JSValue *data)
{
    (void)self; (void)argc; (void)argv; (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    struct DImportParse *parse = dom->import_parse;
    if (parse == NULL) return JS_ThrowTypeError(ctx, "No import map is being parsed");
    JSValue parsed = JS_ParseJSON(ctx, parse->text, parse->length, "importmap");
    if (JS_IsException(parsed)) return parsed;
    JSValue imports = JS_UNDEFINED, scopes = JS_UNDEFINED, integrity = JS_UNDEFINED;
    int has_imports = 0, has_scopes = 0, has_integrity = 0;
    int result = 0;
    if (!is_map(ctx, parsed)) {
        JS_ThrowTypeError(ctx, "An import map's top-level value needs to be a JSON object");
        result = -1;
    }
    if (result == 0 && (has_imports = own(ctx, parsed, "imports", &imports)) < 0) result = -1;
    if (result == 0 && has_imports && !is_map(ctx, imports)) {
        JS_ThrowTypeError(ctx, "The import map's \"imports\" needs to be a JSON object");
        result = -1;
    }
    if (result == 0 && (has_scopes = own(ctx, parsed, "scopes", &scopes)) < 0) result = -1;
    if (result == 0 && has_scopes && !is_map(ctx, scopes)) {
        JS_ThrowTypeError(ctx, "The import map's \"scopes\" needs to be a JSON object");
        result = -1;
    }
    if (result == 0 && has_scopes) result = each_entry(ctx, scopes, scope_is_map, NULL);
    /* Integrity is checked for its shape and read no further: yonder
     * checks no Subresource Integrity. */
    if (result == 0 && (has_integrity = own(ctx, parsed, "integrity", &integrity)) < 0) result = -1;
    if (result == 0 && has_integrity && !is_map(ctx, integrity)) {
        JS_ThrowTypeError(ctx, "The import map's \"integrity\" needs to be a JSON object");
        result = -1;
    }
    if (result == 0 && has_imports) {
        DEntries entries = {parse, NULL};
        result = each_entry(ctx, imports, specifier_entry, &entries);
    }
    if (result == 0 && has_scopes) result = each_entry(ctx, scopes, scope_entries, parse);
    JS_FreeValue(ctx, imports);
    JS_FreeValue(ctx, scopes);
    JS_FreeValue(ctx, integrity);
    JS_FreeValue(ctx, parsed);
    return result < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

os64_js_status_t os64_dom_import_map(os64_dom_t *dom, const char *name, const char *text, size_t length,
                                     const os64_dom_import_map_walk_t *walk, os64_js_outcome_t *outcome)
{
    if (outcome == NULL) return OS64_JS_BAD_ARGUMENT;
    os64_memset(outcome, 0, sizeof(*outcome));
    if (dom == NULL || dom->closed || name == NULL || text == NULL || text[length] != '\0' || walk == NULL ||
        walk->entry == NULL || dom->import_parse != NULL) {
        outcome->status = OS64_JS_BAD_ARGUMENT;
        return outcome->status;
    }
    struct DImportParse parse = {text, length, walk};
    if (os64_js_task_begin(dom->runtime, OS64_JS_ABI_ID, name, outcome) == OS64_JS_OK) {
        os64_js_outcome_t access, call;
        JSContext *ctx = os64_js_context(dom->runtime, OS64_JS_ABI_ID, &access);
        JSValue function = ctx != NULL ? JS_NewCFunctionData(ctx, import_map_parse, 0, 0, 1, &dom->anchor->value)
                                       : JS_EXCEPTION;
        if (!JS_IsException(function)) {
            dom->import_parse = &parse;
            os64_js_call(dom->runtime, OS64_JS_ABI_ID, function, JS_UNDEFINED, 0, NULL, NULL, &call);
            dom->import_parse = NULL;
            JS_FreeValue(ctx, function);
            /* What it threw is the window's error event at the task's
             * checkpoint (d_task_finish): a JSON parse queues nothing for
             * the event to wait behind. */
            d_outcome_fold(outcome, &call);
        }
        d_task_finish(dom, outcome);
    }
    d_task_close(dom, name, outcome);
    return outcome->status;
}

/* HTMLScriptElement.supports(type) (HTML § 4.12.1): the script types this
 * window runs. A shim that loads modules itself (es-module-shims) asks it
 * before taking over, and that takeover fetches through blob: URLs, which
 * yonder's module map does not. */
static JSValue script_supports(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                               JSValue *data)
{
    (void)self; (void)magic;
    if (d_callback(ctx, data, OS64_JS_ABI_ID) == NULL) return JS_EXCEPTION;
    size_t length = 0;
    const char *type = JS_ToCStringLen(ctx, &length, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (type == NULL) return JS_EXCEPTION;
    bool yes = length == os64_strlen(type) &&
               (os64_streq(type, "classic") || os64_streq(type, "module") || os64_streq(type, "importmap"));
    JS_FreeCString(ctx, type);
    return JS_NewBool(ctx, yes);
}

int d_import_map_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSValue script = JS_GetPropertyStr(ctx, global, "HTMLScriptElement");
    if (JS_IsException(script)) return -1;
    int made = JS_IsObject(script) ? d_method_named(dom, ctx, script, "supports", script_supports, 1, 0) : -1;
    if (!JS_IsObject(script)) JS_ThrowInternalError(ctx, "The import maps row runs before HTMLScriptElement");
    JS_FreeValue(ctx, script);
    return made;
}
