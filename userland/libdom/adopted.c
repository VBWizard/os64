#include "internal.h"

/* CONSTRUCTED STYLE SHEETS AND ADOPTION (CSSOM § 6.1.2 and
 * DocumentOrShadowRoot's adoptedStyleSheets; SHADOW_DOM.md § 12, D4b).
 *
 * A constructed sheet is a CSSStyleSheet with no owner node and a record
 * of its own (DSheetText): the text replace or replaceSync last gave it, a
 * generation that moves with each, and a key the host knows it by. libdom
 * keeps the text and nothing parsed: the host's cascade reads it, as it
 * reads a `style` element's.
 *
 * A root's adoptedStyleSheets — the document node's or a shadow root's — is
 * one array object for the root's life, a Proxy over a plain array that an
 * embedded factory makes, with WebIDL observable array's shape (no holes,
 * no accessors at an index, never made non-extensible). Its traps hand
 * every value about to go in to `check` (a constructed sheet of the root's
 * document, or a TypeError or NotAllowedError, as CSSOM says) and every
 * change to `changed`, which keeps the root's list here
 * (DAdopted) for the host and touches the root (os64_html_touch), since the
 * host restyles when the document's version moves and an adoption moves no
 * node. Assigning to adoptedStyleSheets replaces the array's contents.
 *
 * Installed with the shadow surface (shadow.c): a page decides from these
 * names and ShadowRoot together. */

struct DSheetText {
    uint64_t key, generation;
    char *text;
    size_t length;
};

struct DAdopted {
    const os64_html_node_t *root;
    DValue *array, *assign;     /* the Proxy, and the function assignment calls */
    DValue **sheets;            /* the constructed sheets' entries, in order */
    size_t n, cap;
    struct DAdopted *next;
};

bool d_sheet_constructed(const DValue *entry)
{
    return entry != NULL && entry->constructed != NULL;
}

void d_adopted_free(os64_dom_t *dom, DValue *entry)
{
    if (entry->constructed != NULL) {
        d_free(dom, entry->constructed->text);
        d_free(dom, entry->constructed);
        entry->constructed = NULL;
    }
    if (entry->adopted != NULL) {
        d_free(dom, entry->adopted->sheets);
        d_free(dom, entry->adopted);
        entry->adopted = NULL;
    }
}

/* The constructed sheet `value` is, or NULL with an exception pending:
 * a TypeError for anything not a CSSStyleSheet, a NotAllowedError for one
 * a node owns. */
static DValue *constructed_of(JSContext *ctx, JSValueConst value)
{
    DValue *entry = JS_IsObject(value) ? JS_GetOpaque(value, d_sheet_class()) : NULL;
    if (entry == NULL) {
        JS_ThrowTypeError(ctx, "Expected a CSSStyleSheet");
        return NULL;
    }
    if (entry->constructed == NULL) {
        d_error(ctx, "NotAllowedError", "Only a constructed CSSStyleSheet can be adopted or replaced");
        return NULL;
    }
    return entry;
}

JSValue d_sheet_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSClassID id = d_sheet_class();
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id) : JS_NewObjectClass(ctx, (int)id);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object)) return object;
    struct DSheetText *text = d_alloc(dom, sizeof(*text));
    DValue *entry = text != NULL ? d_retain(dom, ctx, JS_DupValue(ctx, object)) : NULL;
    if (entry == NULL) {
        d_free(dom, text);
        JS_FreeValue(ctx, object);
        return text == NULL ? d_error(ctx, "QuotaExceededError", "DOM sheet quota exceeded") : JS_EXCEPTION;
    }
    os64_memset(text, 0, sizeof(*text));
    text->key = ++dom->sheet_keys;
    entry->dom = dom;
    entry->constructed = text;
    JS_SetOpaque(object, entry);
    return object;
}

/* Whether `list` holds `sheet`. */
static bool holds(const struct DAdopted *list, const DValue *sheet)
{
    for (size_t i = 0; i < list->n; i++)
        if (list->sheets[i] == sheet) return true;
    return false;
}

enum { A_REPLACE_SYNC, A_REPLACE };

/* The text of the sheet `self` is becomes argument 0, and every root
 * adopting it is touched. JS_UNDEFINED, or JS_EXCEPTION with it pending. */
static JSValue replace_text(os64_dom_t *dom, JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    DValue *entry = constructed_of(ctx, self);
    if (entry == NULL) return JS_EXCEPTION;
    size_t length = 0;
    const char *given = JS_ToCStringLen(ctx, &length, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (given == NULL) return JS_EXCEPTION;
    char *copy = d_alloc(dom, length + 1);
    if (copy == NULL) {
        JS_FreeCString(ctx, given);
        return d_error(ctx, "QuotaExceededError", "DOM sheet quota exceeded");
    }
    os64_memcpy(copy, given, length);
    copy[length] = '\0';
    JS_FreeCString(ctx, given);
    struct DSheetText *text = entry->constructed;
    d_free(dom, text->text);
    text->text = copy;
    text->length = length;
    text->generation++;
    for (struct DAdopted *a = dom->adoptions; a != NULL; a = a->next)
        if (holds(a, entry)) (void)os64_html_touch(dom->document, (os64_html_node_t *)a->root);
    return JS_UNDEFINED;
}

/* replaceSync(text) and replace(text). replace answers a Promise of the
 * sheet, already settled since nothing here is fetched: resolved, or
 * rejected with what replaceSync would have thrown (WebIDL: a
 * Promise-returning operation does not throw). */
static JSValue sheet_replace(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                             JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue done = replace_text(dom, ctx, self, argc, argv);
    if (magic == A_REPLACE_SYNC) return done;
    /* A runtime that must stop keeps its exception, uncatchable. */
    if (JS_IsException(done) && !d_exception_catchable(ctx)) return JS_EXCEPTION;
    JSValue settle = JS_IsException(done) ? JS_GetException(ctx) : JS_DupValue(ctx, self);
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, settle);
        return promise;
    }
    JSValue called = JS_Call(ctx, resolving[JS_IsException(done) ? 1 : 0], JS_UNDEFINED, 1, (JSValueConst *)&settle);
    JS_FreeValue(ctx, settle);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    if (JS_IsException(called)) {
        JS_FreeValue(ctx, promise);
        return called;
    }
    JS_FreeValue(ctx, called);
    return promise;
}

/* check(root, value, size): the factory's traps ask before a value goes
 * into a root's array that will then hold `size` values: a constructed
 * sheet, made in the root's document — the window's, the only one a
 * constructor makes sheets for (CSSOM: NotAllowedError otherwise) — and
 * room in the root's list for `size`. */
static JSValue adopt_check(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL || argc < 3) return JS_EXCEPTION;
    if (constructed_of(ctx, argv[1]) == NULL) return JS_EXCEPTION;
    const os64_html_node_t *root = d_node(dom, ctx, argv[0]);
    if (root == NULL) return JS_EXCEPTION;
    if (d_node_document(dom, root) != dom->document->document)
        return d_error(ctx, "NotAllowedError", "A sheet is adopted only in the document it was made for");
    /* The list's room for the length the array will have, taken now, while
     * a refusal still leaves the array as it was: `changed` then never has
     * to grow it, and JS and the host cannot come to see two lists. */
    DValue *owner = d_find(dom, root);
    struct DAdopted *list = owner != NULL ? owner->adopted : NULL;
    uint32_t size = 0;
    if (list == NULL || JS_ToUint32(ctx, &size, argv[2]) < 0) return JS_EXCEPTION;
    if (size > list->cap) {
        DValue **more = d_alloc(dom, (size_t)size * sizeof(*more));
        if (more == NULL) return d_error(ctx, "QuotaExceededError", "DOM sheet quota exceeded");
        if (list->n != 0) os64_memcpy(more, list->sheets, list->n * sizeof(*more));
        d_free(dom, list->sheets);
        list->sheets = more;
        list->cap = size;
    }
    return JS_UNDEFINED;
}

/* changed(root, array): the array behind a root's Proxy changed; its list
 * here is read again and the root touched. */
static JSValue adopt_changed(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL || argc < 2) return JS_EXCEPTION;
    const os64_html_node_t *root = d_node(dom, ctx, argv[0]);
    if (root == NULL) return JS_EXCEPTION;
    DValue *owner = d_find(dom, root);
    struct DAdopted *list = owner != NULL ? owner->adopted : NULL;
    if (list == NULL) return JS_ThrowTypeError(ctx, "Missing adoptedStyleSheets");
    int64_t length = 0;
    JSValue count = JS_GetPropertyStr(ctx, argv[1], "length");
    int status = JS_IsException(count) ? -1 : JS_ToInt64(ctx, &length, count);
    JS_FreeValue(ctx, count);
    if (status < 0) return JS_EXCEPTION;
    if (length > UINT32_MAX) length = UINT32_MAX;
    list->n = 0;
    for (int64_t i = 0; i < length; i++) {
        JSValue one = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
        if (JS_IsException(one)) return one;
        DValue *sheet = JS_IsObject(one) ? JS_GetOpaque(one, d_sheet_class()) : NULL;
        JS_FreeValue(ctx, one);
        /* Every value went in through `check`, which reserved its room;
         * nothing else is kept. */
        if (sheet == NULL || sheet->constructed == NULL || list->n == list->cap) continue;
        list->sheets[list->n++] = sheet;
    }
    (void)os64_html_touch(dom->document, (os64_html_node_t *)root);
    return JS_UNDEFINED;
}

/* The factory: given check and changed, a function of a root wrapper that
 * answers [the Proxy, assign]. It is run at install, before any page
 * script, and takes the intrinsics it uses then, so a page that replaces
 * Proxy, Reflect or Array.from later changes nothing here. The traps keep
 * WebIDL's observable array's shape: no holes — an index may be set or
 * defined (as a data property) only up to the length, `length` may only
 * shrink, only the last index may be deleted — the array cannot be made
 * non-extensible, and a value about to go in is checked first; each change
 * is reported after. assign(list) takes an iterable, checks every value,
 * then replaces the contents in one report. */
static const char kFactory[] =
    "(function(check, changed){ 'use strict';"
    "var P = Proxy, set = Reflect.set, define = Reflect.defineProperty, remove = Reflect.deleteProperty,"
    " from = Array.from, iter = Symbol.iterator, S = String;"
    "function index(k){ return typeof k === 'string' && S(k >>> 0) === k && (k >>> 0) !== 4294967295; }"
    "function length(t, v){ var n = v >>> 0; if (n !== +v) throw new RangeError('Invalid array length');"
    " if (n > t.length) return false; set(t, 'length', n); return true; }"
    "return function(root){"
    "var a = [];"
    "function put(t, k, v){ if ((k >>> 0) > t.length) return false; var i = k >>> 0; check(root, v, i < t.length ? t.length : i + 1); set(t, k, v); return true; }"
    "var p = new P(a, {"
    " set: function(t, k, v){ var ok = k === 'length' ? length(t, v) : index(k) ? put(t, k, v) : set(t, k, v);"
    "  changed(root, t); return ok; },"
    " defineProperty: function(t, k, d){ var ok;"
    "  if (k === 'length' || index(k)) {"
    "   if ('get' in d || 'set' in d) return false;"
    "   ok = !('value' in d) || (k === 'length' ? length(t, d.value) : put(t, k, d.value));"
    "  } else ok = define(t, k, d);"
    "  changed(root, t); return ok; },"
    " deleteProperty: function(t, k){ var ok;"
    "  if (index(k)) { var i = k >>> 0; ok = i >= t.length || (i === t.length - 1 && set(t, 'length', i)); }"
    "  else ok = remove(t, k);"
    "  changed(root, t); return ok; },"
    " preventExtensions: function(){ return false; }"
    "});"
    "return [p, function(list){"
    " if (list == null || typeof list[iter] !== 'function')"
    "  throw new TypeError('adoptedStyleSheets takes a sequence of CSSStyleSheet');"
    " var v = from(list); for (var i = 0; i < v.length; i++) check(root, v[i], v.length);"
    " set(a, 'length', 0); for (var j = 0; j < v.length; j++) set(a, j, v[j]); changed(root, a); }];"
    "}; })";

/* The root's record, made when first asked for. */
static struct DAdopted *adopted_of(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *root)
{
    JSValue wrapper = d_wrap(dom, ctx, root);
    if (JS_IsException(wrapper)) return NULL;
    DValue *owner = d_find(dom, root);
    if (owner == NULL) {
        JS_FreeValue(ctx, wrapper);
        JS_ThrowTypeError(ctx, "Missing node wrapper");
        return NULL;
    }
    if (owner->adopted != NULL) {
        JS_FreeValue(ctx, wrapper);
        return owner->adopted;
    }
    struct DAdopted *list = d_alloc(dom, sizeof(*list));
    if (list == NULL) {
        JS_FreeValue(ctx, wrapper);
        d_error(ctx, "QuotaExceededError", "DOM sheet quota exceeded");
        return NULL;
    }
    os64_memset(list, 0, sizeof(*list));
    list->root = root;
    JSValue made = JS_Call(ctx, dom->adopt_factory->value, JS_UNDEFINED, 1, (JSValueConst *)&wrapper);
    JS_FreeValue(ctx, wrapper);
    JSValue array = JS_IsException(made) ? JS_EXCEPTION : JS_GetPropertyUint32(ctx, made, 0);
    JSValue assign = JS_IsException(made) ? JS_EXCEPTION : JS_GetPropertyUint32(ctx, made, 1);
    JS_FreeValue(ctx, made);
    list->array = !JS_IsException(array) ? d_retain(dom, ctx, array) : NULL;
    list->assign = !JS_IsException(assign) ? d_retain(dom, ctx, assign) : NULL;
    if (list->array == NULL || list->assign == NULL) {
        d_free(dom, list);
        return NULL;
    }
    owner->adopted = list;
    list->next = dom->adoptions;
    dom->adoptions = list;
    return list;
}

/* adoptedStyleSheets on a document or a shadow root: the root's array, and
 * assigning replaces its contents. */
static JSValue adopted_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                              JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *root = d_node(dom, ctx, self);
    if (root == NULL) return JS_EXCEPTION;
    if (root->kind != OS64_HTML_DOCUMENT && os64_html_shadow_host(root) == NULL)
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    struct DAdopted *list = adopted_of(dom, ctx, root);
    if (list == NULL) return JS_EXCEPTION;
    if ((magic & D_SET) == 0) return JS_DupValue(ctx, list->array->value);
    JSValue done = JS_Call(ctx, list->assign->value, JS_UNDEFINED, 1, argc > 0 ? argv : (JSValueConst[]){JS_UNDEFINED});
    if (JS_IsException(done)) return done;
    JS_FreeValue(ctx, done);
    return JS_UNDEFINED;
}

int d_adopted_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global, JSValueConst sheet_prototype,
                      JSValueConst root_prototype)
{
    (void)global;
    JSValue factory = JS_Eval(ctx, kFactory, sizeof(kFactory) - 1, "adoptedStyleSheets", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(factory)) return -1;
    JSValue hooks[2] = {JS_NewCFunction(ctx, adopt_check, "check", 3),
                        JS_NewCFunction(ctx, adopt_changed, "changed", 2)};
    JSValue made = JS_IsException(hooks[0]) || JS_IsException(hooks[1])
        ? JS_EXCEPTION : JS_Call(ctx, factory, JS_UNDEFINED, 2, (JSValueConst *)hooks);
    JS_FreeValue(ctx, factory);
    JS_FreeValue(ctx, hooks[0]);
    JS_FreeValue(ctx, hooks[1]);
    if (JS_IsException(made)) return -1;
    dom->adopt_factory = d_retain(dom, ctx, made);
    if (dom->adopt_factory == NULL ||
        d_method_named(dom, ctx, sheet_prototype, "replaceSync", sheet_replace, 1, A_REPLACE_SYNC) < 0 ||
        d_method_named(dom, ctx, sheet_prototype, "replace", sheet_replace, 1, A_REPLACE) < 0 ||
        d_accessor(dom, ctx, dom->prototypes[D_PROTO_DOCUMENT]->value, "adoptedStyleSheets", adopted_member, 0,
                   true) < 0 ||
        d_accessor(dom, ctx, root_prototype, "adoptedStyleSheets", adopted_member, 0, true) < 0)
        return -1;
    return 0;
}

size_t os64_dom_adopted(const os64_dom_t *dom, const os64_html_node_t *root, os64_dom_adopted_sheet_t *out,
                        size_t cap)
{
    const DValue *owner = dom != NULL && root != NULL ? d_find((os64_dom_t *)dom, root) : NULL;
    const struct DAdopted *list = owner != NULL ? owner->adopted : NULL;
    if (list == NULL) return 0;
    for (size_t i = 0; i < list->n && i < cap; i++) {
        const struct DSheetText *text = list->sheets[i]->constructed;
        out[i] = (os64_dom_adopted_sheet_t){text->key, text->generation, text->text != NULL ? text->text : "",
                                            text->length};
    }
    return list->n;
}
