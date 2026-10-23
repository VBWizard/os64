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
 * of a resizable ArrayBuffer is filled like any other, where WebIDL would
 * refuse one with a TypeError (DEBTS). A view whose buffer is detached
 * holds no bytes, and is answered as it is. Nothing here runs page code:
 * the view is told by its class and read through the engine, never
 * through its getters. */
static JSValue get_random_values(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "getRandomValues needs an array");
    JSValueConst array = argv[0];
    int kind = d_view_kind(dom, ctx, array);
    if (kind == -2) return JS_EXCEPTION;
    if (kind < 0) return JS_ThrowTypeError(ctx, "getRandomValues needs an ArrayBufferView");
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

int d_crypto_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef crypto_class = {.class_name = "Crypto"};
    JSClassID id = os64_js_class_id(&crypto_slot);
    JSValue prototype;
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
