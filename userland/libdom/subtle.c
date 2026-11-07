#include "internal.h"
#include "subtle_core.h"

/* `crypto.subtle` (Web Cryptography's SubtleCrypto and CryptoKey;
 * WEB_CRYPTO.md). [SecureContext]: crypto.c installs it only on a secure
 * page. Every one of the twelve methods is here whole; what an algorithm
 * can do is subtle_core.c's registry, and an algorithm it does not hold is
 * NotSupportedError, which is a browser's answer for one it does not know.
 *
 * EVERY METHOD REJECTS; NONE THROWS. WebIDL makes an exception raised
 * while converting a promise-returning operation's arguments a rejected
 * promise, and the spec's own steps reject, so each method does its work
 * and settles once at the end (d_promise_settled), whatever failed.
 *
 * THE WORK IS DONE ON THE CALL (WEB_CRYPTO.md § 5): the promise returned is
 * already settled, as Blob.text()'s is, where the spec works "in parallel"
 * and settles from a task. Its dividend is that a BufferSource is read in
 * place, never copied: no page code runs between the read and the answer.
 * The read comes after normalization, whose dictionary reads can run a
 * page's getters, and a getter may detach or refill the buffer. */

static JSClassID subtle_slot, key_slot;
static int subtle_mark;

enum {
    S_ENCRYPT,
    S_DECRYPT,
    S_SIGN,
    S_VERIFY,
    S_DIGEST,
    S_GENERATE_KEY,
    S_DERIVE_KEY,
    S_DERIVE_BITS,
    S_IMPORT_KEY,
    S_EXPORT_KEY,
    S_WRAP_KEY,
    S_UNWRAP_KEY,
    S_METHODS,
    K_TYPE = S_METHODS,
    K_EXTRACTABLE,
    K_ALGORITHM,
    K_USAGES,
    W_SUBTLE
};

/* An argument's IDL type, which says how it is converted. */
typedef enum {
    A_ALGORITHM, /* AlgorithmIdentifier: (object or DOMString) */
    A_KEY,       /* CryptoKey */
    A_BUFFER,    /* BufferSource */
    A_BOOLEAN,
    A_USAGES,    /* sequence<KeyUsage> */
    A_FORMAT,    /* KeyFormat */
    A_KEY_DATA,  /* (BufferSource or JsonWebKey) */
    A_LENGTH,    /* optional [EnforceRange] unsigned long? length = null */
} SubtleArg;

#define SUBTLE_ARGS_MAX 7

/* Each method as the IDL declares it: its name, its required arguments
 * (its `length`), its arguments' types in order, and the operation its
 * first algorithm is normalized for. */
static const struct {
    const char *name;
    int required, count;
    SubtleArg args[SUBTLE_ARGS_MAX];
    subtle_op_t op;
} kMethods[S_METHODS] = {
    [S_ENCRYPT] = {"encrypt", 3, 3, {A_ALGORITHM, A_KEY, A_BUFFER}, SUBTLE_OP_ENCRYPT},
    [S_DECRYPT] = {"decrypt", 3, 3, {A_ALGORITHM, A_KEY, A_BUFFER}, SUBTLE_OP_DECRYPT},
    [S_SIGN] = {"sign", 3, 3, {A_ALGORITHM, A_KEY, A_BUFFER}, SUBTLE_OP_SIGN},
    [S_VERIFY] = {"verify", 4, 4, {A_ALGORITHM, A_KEY, A_BUFFER, A_BUFFER}, SUBTLE_OP_VERIFY},
    [S_DIGEST] = {"digest", 2, 2, {A_ALGORITHM, A_BUFFER}, SUBTLE_OP_DIGEST},
    [S_GENERATE_KEY] = {"generateKey", 3, 3, {A_ALGORITHM, A_BOOLEAN, A_USAGES}, SUBTLE_OP_GENERATE_KEY},
    [S_DERIVE_KEY] = {"deriveKey", 5, 5, {A_ALGORITHM, A_KEY, A_ALGORITHM, A_BOOLEAN, A_USAGES},
                      SUBTLE_OP_DERIVE_BITS},
    [S_DERIVE_BITS] = {"deriveBits", 2, 3, {A_ALGORITHM, A_KEY, A_LENGTH}, SUBTLE_OP_DERIVE_BITS},
    [S_IMPORT_KEY] = {"importKey", 5, 5, {A_FORMAT, A_KEY_DATA, A_ALGORITHM, A_BOOLEAN, A_USAGES},
                      SUBTLE_OP_IMPORT_KEY},
    [S_EXPORT_KEY] = {"exportKey", 2, 2, {A_FORMAT, A_KEY}, SUBTLE_OP_COUNT},
    [S_WRAP_KEY] = {"wrapKey", 4, 4, {A_FORMAT, A_KEY, A_KEY, A_ALGORITHM}, SUBTLE_OP_WRAP_KEY},
    [S_UNWRAP_KEY] = {"unwrapKey", 7, 7,
                      {A_FORMAT, A_BUFFER, A_KEY, A_ALGORITHM, A_ALGORITHM, A_BOOLEAN, A_USAGES},
                      SUBTLE_OP_UNWRAP_KEY},
};

/* KeyFormat and KeyUsage as the IDL enumerates them (the draft's raw-*
 * formats and encapsulation usages included: what the IDL admits, an
 * algorithm may still refuse). */
static const char *const kFormats[] = {"raw-public", "raw-private", "raw-seed", "raw-secret", "raw", "spki",
                                       "pkcs8", "jwk"};
static const char *const kUsages[] = {"encrypt", "decrypt", "sign", "verify", "deriveKey", "deriveBits",
                                      "wrapKey", "unwrapKey", "encapsulateKey", "encapsulateBits",
                                      "decapsulateKey", "decapsulateBits"};

/* A CryptoKey's slots. A key is made by generateKey, importKey, deriveKey
 * or unwrapKey under an algorithm the registry holds for that operation,
 * and while it holds none (DEBTS.md, "crypto.subtle does only digest") no
 * object has these slots. The class is here so the interface is whole: a
 * page sees CryptoKey and its members, and an argument declared a
 * CryptoKey is told from anything else. */
typedef struct {
    const char *type;
    bool extractable;
    DValue *algorithm, *usages;
} DKey;

/* An argument converted, which the method's steps read. */
typedef struct {
    JSValue algorithm[2]; /* an AlgorithmIdentifier: the object, or the string; JS_UNDEFINED unused */
    int algorithms;
    JSValueConst buffers[2]; /* BufferSources, borrowed from argv, read after normalization */
    int buffer_count;
    DKey *keys[2];
    int key_count;
    bool extractable;
    uint32_t usages; /* a bit per kUsages entry */
    int format;      /* an index of kFormats, -1 none */
    bool length_given;
    uint32_t length;
} SubtleCall;

static void call_free(JSContext *ctx, SubtleCall *c)
{
    for (int i = 0; i < c->algorithms; i++) JS_FreeValue(ctx, c->algorithm[i]);
}

/* The position `index` (from 0) as an argument's ordinal in a message. */
static JSValue arg_error(JSContext *ctx, int method, int index, const char *type)
{
    return JS_ThrowTypeError(ctx, "SubtleCrypto.%s: argument %d is not a %s", kMethods[method].name, index + 1, type);
}

/* `value`'s string, which matches one of `names` exactly: its index, or -1
 * with a TypeError pending (as WebIDL converts an enumeration). */
static int enumeration(JSContext *ctx, JSValueConst value, const char *const *names, size_t count, const char *type)
{
    size_t length;
    const char *text = JS_ToCStringLen(ctx, &length, value);
    if (text == NULL) return -1;
    int found = -1;
    for (size_t i = 0; i < count && found < 0; i++)
        if (os64_strlen(names[i]) == length && os64_memcmp(names[i], text, length) == 0) found = (int)i;
    if (found < 0) JS_ThrowTypeError(ctx, "'%s' is not a valid %s", length > 64 ? "(a long string)" : text, type);
    JS_FreeCString(ctx, text);
    return found;
}

/* WebIDL's [EnforceRange] unsigned long: NaN and the infinities refused,
 * the rest truncated and refused outside 0..2^32-1. */
static int enforce_unsigned_long(JSContext *ctx, JSValueConst value, uint32_t *out)
{
    double x;
    if (JS_ToFloat64(ctx, &x, value) < 0) return -1;
    if (!(x > -1.0 && x < 4294967296.0)) {
        JS_ThrowTypeError(ctx, "A length must be a whole number from 0 to 4294967295");
        return -1;
    }
    *out = x <= 0 ? 0 : (uint32_t)x;
    return 0;
}

static int buffer_source(os64_dom_t *dom, JSContext *ctx, JSValueConst value, int method, int index)
{
    const uint8_t *bytes;
    size_t length;
    int kind = d_buffer_source(dom, ctx, value, &bytes, &length);
    if (kind < 0) return -1;
    if (kind == 0) {
        arg_error(ctx, method, index, "BufferSource");
        return -1;
    }
    int resizable = d_resizable_buffer_source(dom, ctx, value);
    if (resizable < 0) return -1;
    if (resizable) {
        JS_ThrowTypeError(ctx, "SubtleCrypto.%s: argument %d's buffer is resizable", kMethods[method].name,
                          index + 1);
        return -1;
    }
    return 0;
}

/* The method's arguments converted in order, as WebIDL converts them; -1
 * with the first failure pending. */
static int convert(os64_dom_t *dom, JSContext *ctx, int method, int argc, JSValueConst *argv, SubtleCall *c)
{
    if (argc < kMethods[method].required) {
        JS_ThrowTypeError(ctx, "SubtleCrypto.%s needs %d arguments, not %d", kMethods[method].name,
                          kMethods[method].required, argc);
        return -1;
    }
    for (int i = 0; i < kMethods[method].count; i++) {
        JSValueConst v = i < argc ? argv[i] : JS_UNDEFINED;
        switch (kMethods[method].args[i]) {
        case A_ALGORITHM: {
            JSValue held = JS_IsObject(v) ? JS_DupValue(ctx, v) : JS_ToString(ctx, v);
            if (JS_IsException(held)) return -1;
            c->algorithm[c->algorithms++] = held;
            break;
        }
        case A_KEY: {
            DKey *key = JS_GetOpaque(v, os64_js_class_id(&key_slot));
            if (key == NULL) {
                arg_error(ctx, method, i, "CryptoKey");
                return -1;
            }
            c->keys[c->key_count++] = key;
            break;
        }
        case A_BUFFER:
            if (buffer_source(dom, ctx, v, method, i) < 0) return -1;
            c->buffers[c->buffer_count++] = v;
            break;
        case A_BOOLEAN:
            c->extractable = JS_ToBool(ctx, v);
            break;
        case A_USAGES: {
            JSValue *items;
            size_t count;
            if (d_sequence(dom, ctx, v, D_SEQUENCE_MAX, &items, &count) < 0) return -1;
            int bad = 0;
            for (size_t k = 0; k < count && !bad; k++) {
                int usage = enumeration(ctx, items[k], kUsages, sizeof(kUsages) / sizeof(kUsages[0]), "KeyUsage");
                if (usage < 0) bad = 1;
                else c->usages |= 1u << usage;
            }
            d_sequence_free(ctx, items, count);
            if (bad) return -1;
            break;
        }
        case A_FORMAT:
            c->format = enumeration(ctx, v, kFormats, sizeof(kFormats) / sizeof(kFormats[0]), "KeyFormat");
            if (c->format < 0) return -1;
            break;
        case A_KEY_DATA: {
            /* (BufferSource or JsonWebKey): a buffer source is one; any
             * other object, undefined or null is the dictionary, whose
             * members are read where a format reads them (the JWK format
             * does, and is not built: DEBTS.md); anything else neither. */
            const uint8_t *bytes;
            size_t length;
            int kind = JS_IsObject(v) ? d_buffer_source(dom, ctx, v, &bytes, &length) : 0;
            if (kind < 0) return -1;
            if (kind == 1) {
                if (buffer_source(dom, ctx, v, method, i) < 0) return -1;
                c->buffers[c->buffer_count++] = v;
            } else if (!JS_IsObject(v) && !JS_IsUndefined(v) && !JS_IsNull(v)) {
                arg_error(ctx, method, i, "BufferSource or JsonWebKey");
                return -1;
            }
            break;
        }
        case A_LENGTH:
            if (JS_IsUndefined(v) || JS_IsNull(v)) break;
            if (enforce_unsigned_long(ctx, v, &c->length) < 0) return -1;
            c->length_given = true;
            break;
        }
    }
    return 0;
}

/* NORMALIZE AN ALGORITHM (Web Cryptography § 18.4.4) for `op`: the
 * algorithm the registry holds under the name `algorithm` gives, or -1
 * with the failure pending. A string is the name itself. An object is the
 * Algorithm dictionary first, its required `name` read (a TypeError when it
 * is missing), the name matched ASCII case-insensitively (NotSupportedError
 * when nothing matches), and then the dictionary the algorithm's
 * parameters convert to, whose reads run in its members' order and so
 * read `name` again: a getter there runs twice, as it does in a browser. */
static int name_of(JSContext *ctx, JSValueConst algorithm, char **text, size_t *length)
{
    JSValue name = JS_GetPropertyStr(ctx, algorithm, "name");
    if (JS_IsException(name)) return -1;
    if (JS_IsUndefined(name)) {
        JS_ThrowTypeError(ctx, "An algorithm needs a name");
        return -1;
    }
    *text = (char *)JS_ToCStringLen(ctx, length, name);
    JS_FreeValue(ctx, name);
    return *text != NULL ? 0 : -1;
}

static int normalize(JSContext *ctx, JSValueConst algorithm, subtle_op_t op, subtle_alg_t *out)
{
    char *name = NULL;
    size_t length = 0;
    bool object = JS_IsObject(algorithm);
    if (object) {
        if (name_of(ctx, algorithm, &name, &length) < 0) return -1;
    } else {
        name = (char *)JS_ToCStringLen(ctx, &length, algorithm);
        if (name == NULL) return -1;
    }
    subtle_dict_t dict;
    int alg = op < SUBTLE_OP_COUNT ? subtle_registered(op, name, length, &dict) : -1;
    JS_FreeCString(ctx, name);
    if (alg < 0) {
        d_error(ctx, "NotSupportedError", "This browser does not support that algorithm for this operation");
        return -1;
    }
    /* The desired type's conversion, member by member in its order. */
    switch (dict) {
    case SUBTLE_DICT_ALGORITHM:
        /* Its one member, the name, read again. */
        if (object) {
            if (name_of(ctx, algorithm, &name, &length) < 0) return -1;
            JS_FreeCString(ctx, name);
        }
        break;
    }
    *out = (subtle_alg_t)alg;
    return 0;
}

/* digest(algorithm, data) (§ 14.3.5): the bytes `data` holds once the
 * algorithm is normalized, hashed. */
static JSValue digest(os64_dom_t *dom, JSContext *ctx, SubtleCall *c)
{
    subtle_alg_t alg;
    if (normalize(ctx, c->algorithm[0], SUBTLE_OP_DIGEST, &alg) < 0) return JS_EXCEPTION;
    const uint8_t *bytes;
    size_t length;
    if (d_buffer_source(dom, ctx, c->buffers[0], &bytes, &length) < 0) return JS_EXCEPTION;
    uint8_t out[SUBTLE_DIGEST_MAX];
    if (!subtle_digest(alg, bytes, length, out)) {
        d_error(ctx, "OperationError", "The digest could not be made");
        return JS_EXCEPTION;
    }
    return JS_NewArrayBufferCopy(ctx, out, subtle_digest_size(alg));
}

static JSValue subtle_method(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                             JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue result = JS_EXCEPTION;
    SubtleCall c = {.format = -1};
    if (JS_GetOpaque(self, os64_js_class_id(&subtle_slot)) == NULL) {
        JS_ThrowTypeError(ctx, "SubtleCrypto.%s called on something that is not a SubtleCrypto",
                          kMethods[magic].name);
    } else if (convert(dom, ctx, magic, argc, argv, &c) == 0) {
        if (magic == S_DIGEST) {
            result = digest(dom, ctx, &c);
        } else if (magic == S_EXPORT_KEY) {
            /* No key exists to reach here (DKey's comment); a key's
             * algorithm that cannot export is NotSupportedError. */
            d_error(ctx, "NotSupportedError", "This key cannot be exported");
        } else {
            /* The other operations: the algorithm normalized for the
             * method's operation, which refuses any name the registry does
             * not hold. The registry holds an algorithm only for an
             * operation whose steps are here (subtle_core.c's rule), so
             * this NotSupportedError answers only a registration that
             * broke it. */
            subtle_alg_t alg;
            if (normalize(ctx, c.algorithm[0], kMethods[magic].op, &alg) == 0)
                d_error(ctx, "NotSupportedError", "This browser cannot do that operation with that algorithm");
        }
    }
    call_free(ctx, &c);
    return d_promise_settled(ctx, result);
}

static JSValue key_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                          JSValue *data)
{
    (void)argc;
    (void)argv;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (magic == W_SUBTLE) {
        if (!d_same_object(self, dom->crypto)) return JS_ThrowTypeError(ctx, "Expected a Crypto");
        return JS_DupValue(ctx, dom->subtle->value);
    }
    DKey *key = JS_GetOpaque(self, os64_js_class_id(&key_slot));
    if (key == NULL) return JS_ThrowTypeError(ctx, "Expected a CryptoKey");
    switch (magic) {
    case K_TYPE: return JS_NewString(ctx, key->type);
    case K_EXTRACTABLE: return JS_NewBool(ctx, key->extractable);
    case K_ALGORITHM: return JS_DupValue(ctx, key->algorithm->value);
    default: return JS_DupValue(ctx, key->usages->value);
    }
}

int d_subtle_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global, JSValueConst crypto_prototype)
{
    static const JSClassDef subtle_class = {.class_name = "SubtleCrypto"};
    static const JSClassDef key_class = {.class_name = "CryptoKey"};
    JSValue prototype;
    JSClassID id = os64_js_class_id(&subtle_slot);
    if (d_interface_class(dom, ctx, global, id, &subtle_class, &prototype) < 0) return -1;
    for (int i = 0; i < S_METHODS; i++)
        if (d_method_named(dom, ctx, prototype, kMethods[i].name, subtle_method, kMethods[i].required, i) < 0)
            return -1;
    JSValue key_prototype;
    if (d_interface_class(dom, ctx, global, os64_js_class_id(&key_slot), &key_class, &key_prototype) < 0 ||
        d_accessor(dom, ctx, key_prototype, "type", key_member, K_TYPE, false) < 0 ||
        d_accessor(dom, ctx, key_prototype, "extractable", key_member, K_EXTRACTABLE, false) < 0 ||
        d_accessor(dom, ctx, key_prototype, "algorithm", key_member, K_ALGORITHM, false) < 0 ||
        d_accessor(dom, ctx, key_prototype, "usages", key_member, K_USAGES, false) < 0 ||
        d_idl_members(ctx, prototype) < 0 || d_idl_members(ctx, key_prototype) < 0)
        return -1;
    JSValue subtle = JS_NewObjectClass(ctx, (int)id);
    dom->subtle = d_retain(dom, ctx, subtle);
    if (dom->subtle == NULL) return -1;
    JS_SetOpaque(subtle, &subtle_mark);
    /* crypto.c shapes Crypto's members, this one among them (d_idl_members). */
    return d_accessor(dom, ctx, crypto_prototype, "subtle", key_member, W_SUBTLE, false);
}
