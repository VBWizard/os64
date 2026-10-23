#include "internal.h"
#include "way/origin.h"

/* `crypto` (Web Cryptography's Crypto interface): getRandomValues and
 * randomUUID, over the host's entropy (options.entropy; yonder's is
 * /dev/random, through libtls's door). `subtle` is absent: SubtleCrypto is
 * a design of its own, and pages detect it apart from getRandomValues
 * (WINDOW_DOCUMENT.md, ruling 1), so a secure page's crypto lacks the one
 * member a browser's has, and what it has is whole. randomUUID is
 * [SecureContext]: it exists only on a page whose address libway judges
 * potentially trustworthy, the judgement isSecureContext reads
 * (browsing.c).
 *
 * THE BYTES ARE THE HOST'S OR NONE. A hook that fails throws an
 * OperationError, and nothing it wrote reaches the page: it writes into
 * the binding's scratch, copied into the array only once it has said every
 * byte is there. A page never gets zeros, or a short fill, as randomness. */

static JSClassID crypto_slot;
static int crypto_mark;

enum { C_GET_RANDOM_VALUES, C_RANDOM_UUID, W_CRYPTO };

/* The most getRandomValues fills at once (its step 3: more is a
 * QuotaExceededError). */
#define CRYPTO_QUOTA 65536

/* `length` bytes of the host's entropy into `out`; false with the exception
 * pending. */
static bool entropy(os64_dom_t *dom, JSContext *ctx, uint8_t *out, size_t length)
{
    if (length == 0) return true;
    uint8_t *scratch = d_alloc(dom, length);
    if (scratch == NULL) {
        d_error(ctx, "QuotaExceededError", "crypto quota exceeded");
        return false;
    }
    bool got = dom->options.entropy != NULL && dom->options.entropy(dom->options.host_opaque, scratch, length);
    if (got) os64_memcpy(out, scratch, length);
    d_free(dom, scratch);
    if (!got) d_error(ctx, "OperationError", "This browser could not read the system's randomness");
    return got;
}

/* getRandomValues(array): the integer typed arrays only, 65,536 bytes at
 * most, filled whole and answered. WebIDL's ArrayBufferView comes first,
 * so what is no view at all is a TypeError, and a DataView or a float
 * array is a view of the wrong type, TypeMismatchError. There is no
 * SharedArrayBuffer to view (libjs removes it from every runtime). A view
 * of a resizable ArrayBuffer is a TypeError, as WebIDL makes one for an
 * argument without [AllowResizable]: the engine reports such a view's
 * length as it was made and never as the buffer grows or shrinks, so
 * filling one would fill short and call it whole. A view whose fixed
 * buffer is detached holds no bytes, and is answered as it is. Nothing here
 * runs page code: the view is told by its class, and its buffer and that
 * buffer's resizability are read through the engine's own getters, taken
 * before any page ran (d_crypto_install), never through a page's. */

/* Whether the buffer of `array`, a view of `kind` (d_view_kind), is
 * resizable: 1, 0, or -1 with an exception pending. */
static int resizable_view(os64_dom_t *dom, JSContext *ctx, JSValueConst array, int kind)
{
    JSValueConst getter = kind == D_VIEW_DATA_VIEW ? dom->data_view[1]->value : dom->crypto_getters[0]->value;
    JSValue buffer = JS_Call(ctx, getter, array, 0, NULL);
    if (JS_IsException(buffer)) return -1;
    JSValue resizable = JS_Call(ctx, dom->crypto_getters[1]->value, buffer, 0, NULL);
    JS_FreeValue(ctx, buffer);
    if (JS_IsException(resizable)) return -1;
    int answer = JS_ToBool(ctx, resizable);
    JS_FreeValue(ctx, resizable);
    return answer;
}

static JSValue get_random_values(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "getRandomValues needs an array");
    JSValueConst array = argv[0];
    int kind = d_view_kind(dom, ctx, array);
    if (kind == -2) return JS_EXCEPTION;
    if (kind < 0) return JS_ThrowTypeError(ctx, "getRandomValues needs an ArrayBufferView");
    int resizable = resizable_view(dom, ctx, array, kind);
    if (resizable < 0) return JS_EXCEPTION;
    if (resizable) return JS_ThrowTypeError(ctx, "getRandomValues does not fill a view of a resizable buffer");
    if (kind == D_VIEW_DATA_VIEW || kind == JS_TYPED_ARRAY_FLOAT16 || kind == JS_TYPED_ARRAY_FLOAT32 ||
        kind == JS_TYPED_ARRAY_FLOAT64)
        return d_error(ctx, "TypeMismatchError", "getRandomValues fills an integer typed array");
    size_t offset = 0, span = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, array, &offset, &span, NULL);
    if (JS_IsException(buffer)) {
        /* Detached, or past its buffer's end: no bytes to fill. */
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_DupValue(ctx, array);
    }
    size_t whole = 0;
    uint8_t *bytes = JS_GetArrayBuffer(ctx, &whole, buffer);
    JS_FreeValue(ctx, buffer);
    if (bytes == NULL) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_DupValue(ctx, array);
    }
    if (span > CRYPTO_QUOTA)
        return d_error(ctx, "QuotaExceededError", "getRandomValues fills at most 65,536 bytes");
    if (offset > whole || span > whole - offset) return JS_DupValue(ctx, array);
    if (!entropy(dom, ctx, bytes + offset, span)) return JS_EXCEPTION;
    return JS_DupValue(ctx, array);
}

/* randomUUID(): a version 4 UUID (RFC 9562 § 5.4) from 16 bytes of
 * entropy, in lowercase hex with its four dashes, as Web Cryptography
 * writes one. */
static JSValue random_uuid(os64_dom_t *dom, JSContext *ctx)
{
    uint8_t b[16];
    if (!entropy(dom, ctx, b, sizeof(b))) return JS_EXCEPTION;
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    static const char kHex[] = "0123456789abcdef";
    char text[37];
    size_t at = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) text[at++] = '-';
        text[at++] = kHex[b[i] >> 4];
        text[at++] = kHex[b[i] & 15];
    }
    text[at] = '\0';
    return JS_NewStringLen(ctx, text, at);
}

static JSValue crypto_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                             JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (magic == W_CRYPTO) return JS_DupValue(ctx, dom->crypto->value);
    if (JS_GetOpaque(self, os64_js_class_id(&crypto_slot)) == NULL)
        return JS_ThrowTypeError(ctx, "Expected a Crypto");
    return magic == C_GET_RANDOM_VALUES ? get_random_values(dom, ctx, argc, argv) : random_uuid(dom, ctx);
}

/* `object`'s own accessor `name`'s getter, held in *out; -1 with an
 * exception pending. */
static int take_getter(os64_dom_t *dom, JSContext *ctx, JSValueConst object, const char *name, DValue **out)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL) return -1;
    JSPropertyDescriptor descriptor;
    int found = JS_GetOwnProperty(ctx, &descriptor, object, atom);
    JS_FreeAtom(ctx, atom);
    if (found <= 0) {
        if (found == 0) JS_ThrowTypeError(ctx, "The engine has no %s getter", name);
        return -1;
    }
    JS_FreeValue(ctx, descriptor.value);
    JS_FreeValue(ctx, descriptor.setter);
    if (!JS_IsFunction(ctx, descriptor.getter)) {
        JS_FreeValue(ctx, descriptor.getter);
        JS_ThrowTypeError(ctx, "The engine has no %s getter", name);
        return -1;
    }
    *out = d_retain(dom, ctx, descriptor.getter);
    return *out != NULL ? 0 : -1;
}

/* The typed arrays' `buffer` getter (%TypedArray%.prototype's) and
 * ArrayBuffer.prototype's `resizable`, as the engine made them, before any
 * page could replace either: what resizable_view reads a view through. */
static int take_buffer_getters(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    JSValue uint8 = JS_GetPropertyStr(ctx, global, "Uint8Array");
    JSValue uint8_prototype = JS_IsException(uint8) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, uint8, "prototype");
    JS_FreeValue(ctx, uint8);
    JSValue typed_prototype = JS_IsException(uint8_prototype) ? JS_EXCEPTION : JS_GetPrototype(ctx, uint8_prototype);
    JS_FreeValue(ctx, uint8_prototype);
    if (JS_IsException(typed_prototype)) return -1;
    int result = take_getter(dom, ctx, typed_prototype, "buffer", &dom->crypto_getters[0]);
    JS_FreeValue(ctx, typed_prototype);
    if (result < 0) return -1;
    JSValue buffer = JS_GetPropertyStr(ctx, global, "ArrayBuffer");
    JSValue buffer_prototype = JS_IsException(buffer) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, buffer, "prototype");
    JS_FreeValue(ctx, buffer);
    if (JS_IsException(buffer_prototype)) return -1;
    result = take_getter(dom, ctx, buffer_prototype, "resizable", &dom->crypto_getters[1]);
    JS_FreeValue(ctx, buffer_prototype);
    return result;
}

int d_crypto_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef crypto_class = {.class_name = "Crypto"};
    JSClassID id = os64_js_class_id(&crypto_slot);
    JSValue prototype;
    if (take_buffer_getters(dom, ctx, global) < 0) return -1;
    if (d_interface_class(dom, ctx, global, id, &crypto_class, &prototype) < 0 ||
        d_method_named(dom, ctx, prototype, "getRandomValues", crypto_member, 1, C_GET_RANDOM_VALUES) < 0)
        return -1;
    /* [SecureContext]: on a page that is not one, the member is not there. */
    if (way_url_trustworthy(dom->url != NULL ? dom->url : "about:blank") &&
        d_method_named(dom, ctx, prototype, "randomUUID", crypto_member, 0, C_RANDOM_UUID) < 0)
        return -1;
    JSValue crypto = JS_NewObjectClass(ctx, (int)id);
    dom->crypto = d_retain(dom, ctx, crypto);
    if (dom->crypto == NULL) return -1;
    JS_SetOpaque(crypto, &crypto_mark);
    return d_accessor(dom, ctx, global, "crypto", crypto_member, W_CRYPTO, false);
}
