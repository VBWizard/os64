#include "internal.h"
#include "os64/fmt.h"
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
 * and settles once at the end (settle_later), whatever failed.
 *
 * THE WORK IS DONE ON THE CALL, THE PROMISE SETTLED FROM A TASK
 * (WEB_CRYPTO.md § 5). The spec works "in parallel" and queues a task to
 * settle; here the work is done before the method returns, and the
 * answer waits for the task that settles it (THE SETTLE QUEUE, below), so
 * a page's reactions to a result run in a task of their own, as in other
 * browsers: a chain of thousands of calls is thousands of short tasks, not
 * one long one the script deadline would end. Doing the work on the call
 * is why a BufferSource ARGUMENT (the data, a signature, raw key bytes, a
 * wrapped key) is read in place, never copied: no page code runs between
 * the read and the answer. Every read comes after the last normalization,
 * whose dictionary reads can run a page's getters, and a getter may detach
 * or refill the buffer. A dictionary MEMBER (an iv, a salt) is copied as
 * § 18.4.4 copies it, because a later normalization's getters run after it
 * (normalize).
 *
 * THE CHECKS RUN IN THE SPEC'S ORDER, because the error a page sees names
 * the first one that failed (WPT holds each one): the arguments' WebIDL
 * conversions, then normalization, then the operation's own steps, each
 * method's below as § 14.3 writes them. */

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
 * (its `length`), and its arguments' types in order. */
static const struct {
    const char *name;
    int required, count;
    SubtleArg args[SUBTLE_ARGS_MAX];
} kMethods[S_METHODS] = {
    [S_ENCRYPT] = {"encrypt", 3, 3, {A_ALGORITHM, A_KEY, A_BUFFER}},
    [S_DECRYPT] = {"decrypt", 3, 3, {A_ALGORITHM, A_KEY, A_BUFFER}},
    [S_SIGN] = {"sign", 3, 3, {A_ALGORITHM, A_KEY, A_BUFFER}},
    [S_VERIFY] = {"verify", 4, 4, {A_ALGORITHM, A_KEY, A_BUFFER, A_BUFFER}},
    [S_DIGEST] = {"digest", 2, 2, {A_ALGORITHM, A_BUFFER}},
    [S_GENERATE_KEY] = {"generateKey", 3, 3, {A_ALGORITHM, A_BOOLEAN, A_USAGES}},
    [S_DERIVE_KEY] = {"deriveKey", 5, 5, {A_ALGORITHM, A_KEY, A_ALGORITHM, A_BOOLEAN, A_USAGES}},
    [S_DERIVE_BITS] = {"deriveBits", 2, 3, {A_ALGORITHM, A_KEY, A_LENGTH}},
    [S_IMPORT_KEY] = {"importKey", 5, 5, {A_FORMAT, A_KEY_DATA, A_ALGORITHM, A_BOOLEAN, A_USAGES}},
    [S_EXPORT_KEY] = {"exportKey", 2, 2, {A_FORMAT, A_KEY}},
    [S_WRAP_KEY] = {"wrapKey", 4, 4, {A_FORMAT, A_KEY, A_KEY, A_ALGORITHM}},
    [S_UNWRAP_KEY] = {"unwrapKey", 7, 7, {A_FORMAT, A_BUFFER, A_KEY, A_ALGORITHM, A_ALGORITHM, A_BOOLEAN, A_USAGES}},
};

/* KeyFormat and KeyUsage as the IDL WPT holds this binding to enumerates
 * them. Its newer formats (raw-secret and the rest) and its encapsulation
 * usages belong to the draft's newer algorithms, which are not here
 * (DEBTS): the IDL admits them, and each algorithm here refuses them where
 * it refuses any format or usage it does not have (NotSupportedError,
 * SyntaxError), which is what WPT's failure tests ask. */
static const char *const kFormats[] = {"raw-public", "raw-private", "raw-seed", "raw-secret", "raw", "spki",
                                       "pkcs8", "jwk"};
enum { F_RAW = 4, F_SPKI, F_PKCS8, F_JWK };
static const char *const kUsages[] = {"encrypt", "decrypt", "sign", "verify", "deriveKey", "deriveBits",
                                      "wrapKey", "unwrapKey", "encapsulateKey", "encapsulateBits",
                                      "decapsulateKey", "decapsulateBits"};
#define USAGE_COUNT ((int)(sizeof(kUsages) / sizeof(kUsages[0])))
/* A usage set is a bit per kUsages entry. Web Cryptography's "normalized
 * value" of a usage list is its recognized usages in this order, without
 * repeats, which the bits give for nothing. */
enum {
    U_ENCRYPT = 1u << 0,
    U_DECRYPT = 1u << 1,
    U_SIGN = 1u << 2,
    U_VERIFY = 1u << 3,
    U_DERIVE_KEY = 1u << 4,
    U_DERIVE_BITS = 1u << 5,
    U_WRAP = 1u << 6,
    U_UNWRAP = 1u << 7,
};
/* The longest HMAC key generateKey makes: getRandomValues' 65,536 bytes. */
#define SUBTLE_HMAC_GENERATE_MAX_BITS (65536u * 8)
/* The most PBKDF2 derives: the same 65,536 bytes. The spec bounds it only
 * by `length`, 2^32 - 1 bits, past the script heap; each output block
 * costs the whole iteration count again. */
#define SUBTLE_PBKDF2_MAX_BYTES 65536u

/* A CryptoKey's slots (§ 13). The key material is the engine's memory,
 * overwritten before it is given back when the key is collected; the
 * algorithm and usages objects are made with the key and answered on every
 * read, as `[[algorithm_cached]]` and `[[usages_cached]]` are. */
typedef struct {
    const char *type;       /* "secret": a key pair's halves are tier 3's (WEB_CRYPTO.md § 2) */
    subtle_alg_t alg, hash; /* hash: HMAC's */
    uint32_t bits;          /* the key's length in bits */
    uint8_t *material;      /* (bits + 7) / 8 bytes, the bits past `bits` zero */
    size_t bytes;
    bool extractable;
    uint32_t usages;
    JSValue algorithm, usage_list;
} DKey;

static void wipe(uint8_t *p, size_t length)
{
    volatile uint8_t *v = p;
    while (length-- > 0) *v++ = 0;
}

static void key_finalizer(JSRuntime *rt, JSValue value)
{
    DKey *k = JS_GetOpaque(value, os64_js_class_id(&key_slot));
    if (k == NULL) return;
    if (k->material != NULL) {
        wipe(k->material, k->bytes);
        js_free_rt(rt, k->material);
    }
    JS_FreeValueRT(rt, k->algorithm);
    JS_FreeValueRT(rt, k->usage_list);
    js_free_rt(rt, k);
}

static void key_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DKey *k = JS_GetOpaque(value, os64_js_class_id(&key_slot));
    if (k == NULL) return;
    JS_MarkValue(rt, k->algorithm, mark);
    JS_MarkValue(rt, k->usage_list, mark);
}

/* A string a dictionary member held, as the engine gave it (freed with
 * JS_FreeCString); text NULL for a member that was absent. */
typedef struct {
    const char *text;
    size_t length;
} DText;

static bool text_is(DText t, const char *literal)
{
    return t.text != NULL && t.length == os64_strlen(literal) && os64_memcmp(t.text, literal, t.length) == 0;
}

/* A JsonWebKey dictionary converted (§ 17's JsonWebKey; WebIDL's dictionary
 * conversion): each string member, `ext`, and `key_ops`. `oth` is
 * converted and not kept, since no algorithm here reads it. */
enum { J_ALG, J_CRV, J_D, J_DP, J_DQ, J_E, J_K, J_KTY, J_N, J_P, J_Q, J_QI, J_USE, J_X, J_Y, J_STRINGS };
typedef struct {
    bool present;
    DText s[J_STRINGS];
    bool has_ext, ext;
    bool has_ops;
    DText *ops;
    size_t nops, cap;
} DJwk;

static void jwk_free(JSContext *ctx, DJwk *j)
{
    for (int i = 0; i < J_STRINGS; i++)
        if (j->s[i].text != NULL) JS_FreeCString(ctx, j->s[i].text);
    for (size_t i = 0; i < j->nops; i++) JS_FreeCString(ctx, j->ops[i].text);
    js_free(ctx, j->ops);
    os64_memset(j, 0, sizeof(*j));
}

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
    bool key_data_buffer; /* importKey: keyData was a BufferSource (the last of buffers) */
    DJwk jwk;             /* importKey: keyData was the dictionary */
} SubtleCall;

static void call_free(JSContext *ctx, SubtleCall *c)
{
    for (int i = 0; i < c->algorithms; i++) JS_FreeValue(ctx, c->algorithm[i]);
    jwk_free(ctx, &c->jwk);
}

static int fail(JSContext *ctx, const char *name, const char *message)
{
    d_error(ctx, name, message);
    return -1;
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

/* WebIDL's [EnforceRange] for an unsigned integer type whose largest value
 * is `max`: NaN and the infinities refused, the rest truncated and refused
 * outside 0..max. */
static int enforce_range(JSContext *ctx, JSValueConst value, double max, uint32_t *out)
{
    double x;
    if (JS_ToFloat64(ctx, &x, value) < 0) return -1;
    if (!(x > -1.0 && x < max + 1.0)) {
        JS_ThrowTypeError(ctx, "A number is outside its type's range, or not a number");
        return -1;
    }
    *out = x <= 0 ? 0 : (uint32_t)x;
    return 0;
}

/* One KeyUsage of a sequence, converted as the iterator yields it, before
 * the next is asked for (WebIDL's sequence conversion): a string that is
 * no usage ends the iteration there. */
static int usage_item(void *opaque, JSContext *ctx, JSValueConst item)
{
    int usage = enumeration(ctx, item, kUsages, USAGE_COUNT, "KeyUsage");
    if (usage < 0) return -1;
    *(uint32_t *)opaque |= 1u << usage;
    return 0;
}

/* THE JSONWEBKEY DICTIONARY, converted as WebIDL converts a dictionary
 * argument: each member read and converted in lexicographic order, while
 * the arguments are, so its getters run before the algorithm is
 * normalized and a member of the wrong type is a TypeError there.
 *
 * OR A PARSED ONE (unwrapKey's "parse a JWK", § 9), which the spec parses
 * and converts in a new global object, where no page has touched
 * Object.prototype or Array.prototype. Here the JSON is parsed in the
 * page's realm, so `parsed` converts it without consulting either: a
 * member is an own property or absent, an array is walked by index, and
 * nothing is converted that would call a page's toString. JSON makes only
 * own data properties and arrays without holes, so for every JSON value
 * this reads what the new global would, save one: a string member that
 * is an object or an array (which the new global would stringify) is a
 * DataError, a key no one makes. */
typedef struct {
    DJwk *jwk;
    bool parsed;
} DJwkRead;

static int text_of(JSContext *ctx, JSValueConst value, DText *out, bool parsed)
{
    if (parsed && JS_IsObject(value)) {
        fail(ctx, "DataError", "The unwrapped JWK's members are strings");
        return -1;
    }
    out->text = JS_ToCStringLen(ctx, &out->length, value);
    return out->text != NULL ? 0 : -1;
}

/* A dictionary member: [[Get]], or for a parsed JWK the own property. */
static JSValue jwk_member(JSContext *ctx, JSValueConst object, const char *name, bool parsed)
{
    if (!parsed) return JS_GetPropertyStr(ctx, object, name);
    JSAtom atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL) return JS_EXCEPTION;
    JSPropertyDescriptor desc;
    int own = JS_GetOwnProperty(ctx, &desc, object, atom);
    JS_FreeAtom(ctx, atom);
    if (own <= 0) return own < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    JS_FreeValue(ctx, desc.getter);
    JS_FreeValue(ctx, desc.setter);
    return desc.value;
}

/* A sequence member: WebIDL's iteration, or for a parsed JWK the array's
 * elements by index (anything else is what no new global iterates). */
static int jwk_sequence(os64_dom_t *dom, JSContext *ctx, JSValueConst value, bool parsed,
                        int (*item)(void *, JSContext *, JSValueConst), void *opaque)
{
    if (!parsed) return d_sequence_each(dom, ctx, value, JS_UNDEFINED, item, opaque);
    int array = JS_IsArray(ctx, value);
    if (array <= 0) {
        if (array == 0) JS_ThrowTypeError(ctx, "A JsonWebKey's sequence is an array");
        return -1;
    }
    uint32_t length;
    JSValue held = JS_GetPropertyStr(ctx, value, "length");
    int status = JS_ToUint32(ctx, &length, held);
    JS_FreeValue(ctx, held);
    for (uint32_t i = 0; status == 0 && i < length; i++) {
        JSValue element = JS_GetPropertyUint32(ctx, value, i);
        status = JS_IsException(element) ? -1 : item(opaque, ctx, element);
        JS_FreeValue(ctx, element);
    }
    return status;
}

static int key_op_item(void *opaque, JSContext *ctx, JSValueConst item)
{
    DJwkRead *r = opaque;
    DJwk *j = r->jwk;
    if (j->nops == j->cap) {
        size_t cap = j->cap ? j->cap * 2 : 8;
        DText *grown = js_realloc(ctx, j->ops, cap * sizeof(*grown));
        if (grown == NULL) return -1;
        j->ops = grown;
        j->cap = cap;
    }
    if (text_of(ctx, item, &j->ops[j->nops], r->parsed) < 0) return -1;
    j->nops++;
    return 0;
}

/* One RsaOtherPrimesInfo of `oth`: a dictionary whose members, d, r and t,
 * are strings. */
static int other_prime_item(void *opaque, JSContext *ctx, JSValueConst item)
{
    bool parsed = ((DJwkRead *)opaque)->parsed;
    static const char *const kMembers[] = {"d", "r", "t"};
    if (JS_IsUndefined(item) || JS_IsNull(item)) return 0;
    if (!JS_IsObject(item)) {
        JS_ThrowTypeError(ctx, "A JsonWebKey's oth holds dictionaries");
        return -1;
    }
    for (size_t i = 0; i < sizeof(kMembers) / sizeof(kMembers[0]); i++) {
        JSValue member = jwk_member(ctx, item, kMembers[i], parsed);
        if (JS_IsException(member)) return -1;
        DText t = {0};
        int status = JS_IsUndefined(member) ? 0 : text_of(ctx, member, &t, parsed);
        if (t.text != NULL) JS_FreeCString(ctx, t.text);
        JS_FreeValue(ctx, member);
        if (status < 0) return -1;
    }
    return 0;
}

static int json_web_key(os64_dom_t *dom, JSContext *ctx, JSValueConst value, DJwk *j, bool parsed)
{
    DJwkRead read = {j, parsed};
    j->present = true;
    if (JS_IsUndefined(value) || JS_IsNull(value)) return 0;
    if (!JS_IsObject(value)) {
        JS_ThrowTypeError(ctx, "A JsonWebKey is a dictionary");
        return -1;
    }
    static const struct {
        const char *name;
        int slot; /* a J_ index, or -1 ext, -2 key_ops, -3 oth */
    } kMembers[] = {
        {"alg", J_ALG}, {"crv", J_CRV}, {"d", J_D}, {"dp", J_DP}, {"dq", J_DQ}, {"e", J_E},
        {"ext", -1}, {"k", J_K}, {"key_ops", -2}, {"kty", J_KTY}, {"n", J_N}, {"oth", -3},
        {"p", J_P}, {"q", J_Q}, {"qi", J_QI}, {"use", J_USE}, {"x", J_X}, {"y", J_Y},
    };
    for (size_t i = 0; i < sizeof(kMembers) / sizeof(kMembers[0]); i++) {
        JSValue member = jwk_member(ctx, value, kMembers[i].name, parsed);
        if (JS_IsException(member)) return -1;
        int status = 0;
        if (!JS_IsUndefined(member)) {
            switch (kMembers[i].slot) {
            case -1:
                j->has_ext = true;
                j->ext = JS_ToBool(ctx, member);
                break;
            case -2:
                j->has_ops = true;
                status = jwk_sequence(dom, ctx, member, parsed, key_op_item, &read);
                break;
            case -3: status = jwk_sequence(dom, ctx, member, parsed, other_prime_item, &read); break;
            default: status = text_of(ctx, member, &j->s[kMembers[i].slot], parsed); break;
            }
        }
        JS_FreeValue(ctx, member);
        if (status < 0) return -1;
    }
    return 0;
}

/* Whether `value` is a BufferSource WebIDL takes here: -1 with a TypeError
 * (or the failure) pending when it is none, or its buffer is resizable. */
static int buffer_source(os64_dom_t *dom, JSContext *ctx, JSValueConst value, const char *what)
{
    const uint8_t *bytes;
    size_t length;
    int kind = d_buffer_source(dom, ctx, value, &bytes, &length);
    if (kind < 0) return -1;
    if (kind == 0) {
        JS_ThrowTypeError(ctx, "%s is not a BufferSource", what);
        return -1;
    }
    int resizable = d_resizable_buffer_source(dom, ctx, value);
    if (resizable < 0) return -1;
    if (resizable) {
        JS_ThrowTypeError(ctx, "%s's buffer is resizable", what);
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
            if (buffer_source(dom, ctx, v, "An argument") < 0) return -1;
            c->buffers[c->buffer_count++] = v;
            break;
        case A_BOOLEAN:
            c->extractable = JS_ToBool(ctx, v);
            break;
        case A_USAGES:
            if (d_sequence_each(dom, ctx, v, JS_UNDEFINED, usage_item, &c->usages) < 0) return -1;
            break;
        case A_FORMAT:
            c->format = enumeration(ctx, v, kFormats, sizeof(kFormats) / sizeof(kFormats[0]), "KeyFormat");
            if (c->format < 0) return -1;
            break;
        case A_KEY_DATA: {
            /* (BufferSource or JsonWebKey): a buffer source is one; any
             * other object, undefined or null is the dictionary, converted
             * here; anything else neither. */
            const uint8_t *bytes;
            size_t length;
            int kind = JS_IsObject(v) ? d_buffer_source(dom, ctx, v, &bytes, &length) : 0;
            if (kind < 0) return -1;
            if (kind == 1) {
                if (buffer_source(dom, ctx, v, "The key data") < 0) return -1;
                c->buffers[c->buffer_count++] = v;
                c->key_data_buffer = true;
            } else if (!JS_IsObject(v) && !JS_IsUndefined(v) && !JS_IsNull(v)) {
                arg_error(ctx, method, i, "BufferSource or JsonWebKey");
                return -1;
            } else if (json_web_key(dom, ctx, v, &c->jwk, false) < 0) {
                return -1;
            }
            break;
        }
        case A_LENGTH:
            if (JS_IsUndefined(v) || JS_IsNull(v)) break;
            if (enforce_range(ctx, v, 4294967295.0, &c->length) < 0) return -1;
            c->length_given = true;
            break;
        }
    }
    return 0;
}

/* ── Normalization (§ 18.4.4) ────────────────────────────────────────── */

/* A normalized algorithm: the registry's algorithm and the members of its
 * parameter dictionary. A BufferSource member is a private copy of its
 * bytes, made in § 18.4.4 step 10 after the hash member is normalized (so
 * a getter of the hash's runs first, as the spec orders it), so a getter
 * that runs later (deriveKey and unwrapKey normalize a second algorithm
 * after this one) cannot change what the operation reads. */
typedef struct {
    subtle_alg_t alg;
    subtle_dict_t dict;
    bool has_length, has_tag;
    uint32_t length, tag_length;
    bool has_iterations;
    uint32_t iterations;
    subtle_alg_t hash;
    /* BufferSource members: held while the dictionary converts, then
     * replaced by private copies of their bytes (normalize). */
    JSValue iv, counter, additional, salt, info;
} SubtleParams;

static void params_init(SubtleParams *p)
{
    os64_memset(p, 0, sizeof(*p));
    p->hash = SUBTLE_ALG_COUNT;
    p->iv = p->counter = p->additional = p->salt = p->info = JS_UNDEFINED;
}

static void params_free(JSContext *ctx, SubtleParams *p)
{
    JS_FreeValue(ctx, p->iv);
    JS_FreeValue(ctx, p->counter);
    JS_FreeValue(ctx, p->additional);
    JS_FreeValue(ctx, p->salt);
    JS_FreeValue(ctx, p->info);
    p->iv = p->counter = p->additional = p->salt = p->info = JS_UNDEFINED;
}

/* A dictionary's member, read with [[Get]] so an inherited one counts;
 * JS_UNDEFINED when `object` is no object (an algorithm given as a string
 * is a dictionary with only its name). */
static JSValue member(JSContext *ctx, JSValueConst object, const char *name)
{
    return JS_IsObject(object) ? JS_GetPropertyStr(ctx, object, name) : JS_UNDEFINED;
}

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

/* A required BufferSource member into `*out` (held). */
static int buffer_member(os64_dom_t *dom, JSContext *ctx, JSValueConst object, const char *name, bool required,
                         JSValue *out)
{
    JSValue v = member(ctx, object, name);
    if (JS_IsException(v)) return -1;
    if (JS_IsUndefined(v)) {
        if (required) JS_ThrowTypeError(ctx, "The algorithm needs %s", name);
        return required ? -1 : 0;
    }
    if (buffer_source(dom, ctx, v, name) < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    *out = v;
    return 0;
}

/* An [EnforceRange] integer member, its type's largest value `max`. */
static int number_member(JSContext *ctx, JSValueConst object, const char *name, double max, bool required,
                         bool *has, uint32_t *out)
{
    JSValue v = member(ctx, object, name);
    if (JS_IsException(v)) return -1;
    if (JS_IsUndefined(v)) {
        if (required) JS_ThrowTypeError(ctx, "The algorithm needs %s", name);
        return required ? -1 : 0;
    }
    int status = enforce_range(ctx, v, max, out);
    JS_FreeValue(ctx, v);
    if (status == 0 && has != NULL) *has = true;
    return status;
}

static int normalize(os64_dom_t *dom, JSContext *ctx, JSValueConst algorithm, subtle_op_t op, SubtleParams *p);

/* A required HashAlgorithmIdentifier member, (object or DOMString), kept
 * as given into `*out` and normalized for digest once the whole
 * dictionary is read. */
static int hash_member(JSContext *ctx, JSValueConst algorithm, JSValue *out)
{
    JSValue hash = member(ctx, algorithm, "hash");
    if (JS_IsException(hash)) return -1;
    if (JS_IsUndefined(hash)) return fail(ctx, "TypeError", "The algorithm needs a hash");
    if (!JS_IsObject(hash)) {
        JSValue text = JS_ToString(ctx, hash);
        JS_FreeValue(ctx, hash);
        if (JS_IsException(text)) return -1;
        hash = text;
    }
    *out = hash;
    return 0;
}

/* A held BufferSource member replaced by a copy of the bytes it holds
 * now, in an ArrayBuffer no page can reach (a detached one holds none). */
static int copy_member(os64_dom_t *dom, JSContext *ctx, JSValue *held)
{
    if (JS_IsUndefined(*held)) return 0;
    const uint8_t *bytes;
    size_t length;
    int status = d_buffer_source(dom, ctx, *held, &bytes, &length);
    JSValue copy = status < 0 ? JS_EXCEPTION : JS_NewArrayBufferCopy(ctx, bytes, length);
    JS_FreeValue(ctx, *held);
    *held = JS_IsException(copy) ? JS_UNDEFINED : copy;
    return JS_IsException(copy) ? -1 : 0;
}

/* NORMALIZE AN ALGORITHM (§ 18.4.4) for `op` into `p` (params_init'd): -1
 * with the failure pending. A string is the name itself. An object is the
 * Algorithm dictionary first, its required `name` read (a TypeError when it
 * is missing), the name matched ASCII case-insensitively (NotSupportedError
 * when nothing matches), and then the dictionary the algorithm's
 * parameters convert to, read in its members' order, `name` first: § 18.4.4
 * converts the object twice, so a getter there runs twice. Then step 10:
 * a hash member is normalized for digest, and each BufferSource member
 * replaced by a copy of its bytes. */
static int normalize(os64_dom_t *dom, JSContext *ctx, JSValueConst algorithm, subtle_op_t op, SubtleParams *p)
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
    if (alg < 0) return fail(ctx, "NotSupportedError", "This browser does not support that algorithm for this operation");
    p->alg = (subtle_alg_t)alg;
    p->dict = dict;
    if (object) {
        if (name_of(ctx, algorithm, &name, &length) < 0) return -1;
        JS_FreeCString(ctx, name);
    }
    JSValue hash = JS_UNDEFINED;
    int status = 0;
    switch (dict) {
    case SUBTLE_DICT_ALGORITHM:
        break;
    case SUBTLE_DICT_AES_KEY_GEN:
        status = number_member(ctx, algorithm, "length", 65535.0, true, &p->has_length, &p->length);
        break;
    case SUBTLE_DICT_AES_CTR:
        status = buffer_member(dom, ctx, algorithm, "counter", true, &p->counter);
        if (status == 0) status = number_member(ctx, algorithm, "length", 255.0, true, &p->has_length, &p->length);
        break;
    case SUBTLE_DICT_AES_CBC:
        status = buffer_member(dom, ctx, algorithm, "iv", true, &p->iv);
        break;
    case SUBTLE_DICT_AES_GCM:
        status = buffer_member(dom, ctx, algorithm, "additionalData", false, &p->additional);
        if (status == 0) status = buffer_member(dom, ctx, algorithm, "iv", true, &p->iv);
        if (status == 0) status = number_member(ctx, algorithm, "tagLength", 255.0, false, &p->has_tag, &p->tag_length);
        break;
    case SUBTLE_DICT_HMAC_KEY_GEN:
    case SUBTLE_DICT_HMAC_IMPORT:
        if (hash_member(ctx, algorithm, &hash) < 0) return -1;
        status = number_member(ctx, algorithm, "length", 4294967295.0, false, &p->has_length, &p->length);
        break;
    case SUBTLE_DICT_AES_DERIVED:
        status = number_member(ctx, algorithm, "length", 65535.0, true, &p->has_length, &p->length);
        break;
    case SUBTLE_DICT_HKDF:
        if (hash_member(ctx, algorithm, &hash) < 0) return -1;
        status = buffer_member(dom, ctx, algorithm, "info", true, &p->info);
        if (status == 0) status = buffer_member(dom, ctx, algorithm, "salt", true, &p->salt);
        break;
    case SUBTLE_DICT_PBKDF2:
        if (hash_member(ctx, algorithm, &hash) < 0) return -1;
        status = number_member(ctx, algorithm, "iterations", 4294967295.0, true, &p->has_iterations, &p->iterations);
        if (status == 0) status = buffer_member(dom, ctx, algorithm, "salt", true, &p->salt);
        break;
    }
    if (status == 0 && !JS_IsUndefined(hash)) {
        SubtleParams h;
        params_init(&h);
        status = normalize(dom, ctx, hash, SUBTLE_OP_DIGEST, &h);
        p->hash = h.alg;
        params_free(ctx, &h);
    }
    JS_FreeValue(ctx, hash);
    if (status == 0) status = copy_member(dom, ctx, &p->iv);
    if (status == 0) status = copy_member(dom, ctx, &p->counter);
    if (status == 0) status = copy_member(dom, ctx, &p->additional);
    if (status == 0) status = copy_member(dom, ctx, &p->salt);
    if (status == 0) status = copy_member(dom, ctx, &p->info);
    return status;
}

/* The bytes a BufferSource holds now, borrowed; none for one absent. */
static int bytes_of(os64_dom_t *dom, JSContext *ctx, JSValueConst value, const uint8_t **bytes, size_t *length)
{
    *bytes = NULL;
    *length = 0;
    if (JS_IsUndefined(value)) return 0;
    return d_buffer_source(dom, ctx, value, bytes, length) < 0 ? -1 : 0;
}

/* ── Keys ────────────────────────────────────────────────────────────── */

/* The AES family: its keys are 128, 192 or 256 bits, imported and
 * exported raw and as JWK the same way, whatever the mode. */
static bool is_aes(subtle_alg_t alg)
{
    return alg == SUBTLE_ALG_AES_CTR || alg == SUBTLE_ALG_AES_CBC || alg == SUBTLE_ALG_AES_GCM ||
           alg == SUBTLE_ALG_AES_KW;
}

/* The derivations: a key that is only a secret to derive from, imported
 * raw and never exported. */
static bool is_kdf(subtle_alg_t alg)
{
    return alg == SUBTLE_ALG_HKDF || alg == SUBTLE_ALG_PBKDF2;
}

/* The usages each algorithm's keys may carry (its generate and import
 * steps refuse any other with SyntaxError). */
static uint32_t usages_allowed(subtle_alg_t alg)
{
    switch (alg) {
    case SUBTLE_ALG_AES_KW: return U_WRAP | U_UNWRAP;
    case SUBTLE_ALG_HMAC: return U_SIGN | U_VERIFY;
    case SUBTLE_ALG_HKDF:
    case SUBTLE_ALG_PBKDF2: return U_DERIVE_KEY | U_DERIVE_BITS;
    default: return is_aes(alg) ? U_ENCRYPT | U_DECRYPT | U_WRAP | U_UNWRAP : 0;
    }
}

static int define(JSContext *ctx, JSValueConst object, const char *name, JSValue value)
{
    if (JS_IsException(value)) return -1;
    return JS_DefinePropertyValueStr(ctx, object, name, value, JS_PROP_C_W_E) < 0 ? -1 : 0;
}

/* The usage set's names as an array, in the recognized order. */
static JSValue usage_array(JSContext *ctx, uint32_t usages)
{
    JSValue array = JS_NewArray(ctx);
    uint32_t at = 0;
    for (int i = 0; i < USAGE_COUNT && !JS_IsException(array); i++) {
        if (!(usages & (1u << i))) continue;
        if (JS_DefinePropertyValueUint32(ctx, array, at++, JS_NewString(ctx, kUsages[i]), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, array);
            array = JS_EXCEPTION;
        }
    }
    return array;
}

/* A new secret CryptoKey holding `material` (taken, whatever happens):
 * its algorithm object is the key algorithm dictionary as an object, its
 * name the registry's spelling (AesKeyAlgorithm: name, length;
 * HmacKeyAlgorithm: name, hash {name}, length; a derivation's KeyAlgorithm:
 * name alone). */
static JSValue key_new(JSContext *ctx, subtle_alg_t alg, subtle_alg_t hash, uint32_t bits, uint8_t *material,
                       size_t bytes, bool extractable, uint32_t usages)
{
    DKey *k = js_mallocz(ctx, sizeof(*k));
    if (k == NULL) {
        wipe(material, bytes);
        js_free(ctx, material);
        return JS_EXCEPTION;
    }
    *k = (DKey){.type = "secret", .alg = alg, .hash = hash, .bits = bits, .material = material, .bytes = bytes,
                .extractable = extractable, .usages = usages, .algorithm = JS_UNDEFINED,
                .usage_list = JS_UNDEFINED};
    JSValue key = JS_NewObjectClass(ctx, (int)os64_js_class_id(&key_slot));
    if (JS_IsException(key)) {
        wipe(material, bytes);
        js_free(ctx, material);
        js_free(ctx, k);
        return key;
    }
    JS_SetOpaque(key, k);
    JSValue algorithm = JS_NewObject(ctx);
    int status = JS_IsException(algorithm) ? -1 : define(ctx, algorithm, "name", JS_NewString(ctx, subtle_alg_name(alg)));
    if (status == 0 && alg == SUBTLE_ALG_HMAC) {
        JSValue h = JS_NewObject(ctx);
        status = JS_IsException(h) ? -1 : define(ctx, h, "name", JS_NewString(ctx, subtle_alg_name(hash)));
        status = status == 0 ? define(ctx, algorithm, "hash", h) : (JS_FreeValue(ctx, h), -1);
    }
    if (status == 0 && !is_kdf(alg)) status = define(ctx, algorithm, "length", JS_NewUint32(ctx, bits));
    k->algorithm = algorithm;
    if (status == 0) {
        k->usage_list = usage_array(ctx, usages);
        if (JS_IsException(k->usage_list)) {
            k->usage_list = JS_UNDEFINED;
            status = -1;
        }
    }
    if (status < 0) {
        JS_FreeValue(ctx, key);
        return JS_EXCEPTION;
    }
    return key;
}

/* `bits` bits of `data` as a key's material: a copy of the bytes that
 * hold them, the bits past the last zeroed. NULL with the failure pending. */
static uint8_t *material_of(JSContext *ctx, const uint8_t *data, uint32_t bits, size_t *bytes)
{
    *bytes = ((size_t)bits + 7) / 8;
    uint8_t *m = js_malloc(ctx, *bytes ? *bytes : 1);
    if (m == NULL) return NULL;
    if (*bytes) os64_memcpy(m, data, *bytes);
    if (bits % 8) m[*bytes - 1] &= (uint8_t)(0xff << (8 - bits % 8));
    return m;
}

/* JWK's alg for a key (§ 27-31's exportKey): A<bits>CTR/CBC/GCM/KW, HS<n>. */
static const char *jwk_alg(const DKey *k, char *out, size_t cap)
{
    if (k->alg == SUBTLE_ALG_HMAC) {
        switch (k->hash) {
        case SUBTLE_ALG_SHA_1: return "HS1";
        case SUBTLE_ALG_SHA_256: return "HS256";
        case SUBTLE_ALG_SHA_384: return "HS384";
        default: return "HS512";
        }
    }
    const char *mode = k->alg == SUBTLE_ALG_AES_CTR   ? "CTR"
                       : k->alg == SUBTLE_ALG_AES_CBC ? "CBC"
                       : k->alg == SUBTLE_ALG_AES_KW  ? "KW"
                                                      : "GCM";
    os64_snprintf(out, cap, "A%u%s", (unsigned)k->bits, mode);
    return out;
}

/* generateKey's operation (§ 27-31's "generate key"): the usage check, the
 * length, the random key, then the method's empty-usages check. */
static JSValue generate_key(os64_dom_t *dom, JSContext *ctx, const SubtleParams *p, bool extractable, uint32_t usages)
{
    bool aes = is_aes(p->alg);
    if (usages & ~usages_allowed(p->alg)) {
        fail(ctx, "SyntaxError", "That usage is not this algorithm's");
        return JS_EXCEPTION;
    }
    uint32_t bits;
    if (aes) {
        if (p->length != 128 && p->length != 192 && p->length != 256) {
            fail(ctx, "OperationError", "An AES key is 128, 192 or 256 bits");
            return JS_EXCEPTION;
        }
        bits = p->length;
    } else if (p->has_length) {
        if (p->length == 0) {
            fail(ctx, "OperationError", "An HMAC key's length cannot be zero");
            return JS_EXCEPTION;
        }
        /* Its random bytes come as getRandomValues' do, at most 65,536 of
         * them; a longer key is "the key generation step fails". */
        if (p->length > SUBTLE_HMAC_GENERATE_MAX_BITS) {
            fail(ctx, "OperationError", "An HMAC key that long is more than this browser generates");
            return JS_EXCEPTION;
        }
        bits = p->length;
    } else {
        bits = (uint32_t)subtle_hash_block_size(p->hash) * 8;
    }
    size_t bytes = ((size_t)bits + 7) / 8;
    uint8_t *random = js_malloc(ctx, bytes);
    if (random == NULL) return JS_EXCEPTION;
    if (!d_crypto_entropy(dom, ctx, random, bytes)) {
        js_free(ctx, random);
        return JS_EXCEPTION;
    }
    size_t kept;
    uint8_t *material = material_of(ctx, random, bits, &kept);
    wipe(random, bytes);
    js_free(ctx, random);
    if (material == NULL) return JS_EXCEPTION;
    if (usages == 0) {
        wipe(material, kept);
        js_free(ctx, material);
        fail(ctx, "SyntaxError", "A secret key needs at least one usage");
        return JS_EXCEPTION;
    }
    return key_new(ctx, p->alg, p->hash, bits, material, kept, extractable, usages);
}

/* Whether a JWK's key_ops (RFC 7517 § 4.3) admit `usages`: none repeats,
 * and every requested usage is there. */
static bool key_ops_admit(const DJwk *j, uint32_t usages)
{
    for (size_t a = 0; a < j->nops; a++)
        for (size_t b = a + 1; b < j->nops; b++)
            if (j->ops[a].length == j->ops[b].length &&
                os64_memcmp(j->ops[a].text, j->ops[b].text, j->ops[a].length) == 0)
                return false;
    for (int i = 0; i < USAGE_COUNT; i++) {
        if (!(usages & (1u << i))) continue;
        bool found = false;
        for (size_t a = 0; a < j->nops && !found; a++) found = text_is(j->ops[a], kUsages[i]);
        if (!found) return false;
    }
    return true;
}

/* A derivation's key (§ 33.4.2, § 34.4.2): raw only, checked before the
 * usages; never extractable; any bytes, none included. */
static JSValue import_kdf_key(JSContext *ctx, const SubtleParams *p, int format, const uint8_t *raw,
                              size_t raw_length, bool extractable, uint32_t usages)
{
    if (format != F_RAW) {
        fail(ctx, "NotSupportedError", "A derivation's key is imported raw");
        return JS_EXCEPTION;
    }
    if (usages & ~usages_allowed(p->alg)) {
        fail(ctx, "SyntaxError", "That usage is not this algorithm's");
        return JS_EXCEPTION;
    }
    if (extractable) {
        fail(ctx, "SyntaxError", "A derivation's key cannot be extractable");
        return JS_EXCEPTION;
    }
    if (usages == 0) {
        fail(ctx, "SyntaxError", "A secret key needs at least one usage");
        return JS_EXCEPTION;
    }
    if ((uint64_t)raw_length * 8 > 4294967295u) {
        fail(ctx, "DataError", "That key is too long");
        return JS_EXCEPTION;
    }
    size_t kept;
    uint8_t *material = material_of(ctx, raw, (uint32_t)raw_length * 8, &kept);
    if (material == NULL) return JS_EXCEPTION;
    return key_new(ctx, p->alg, SUBTLE_ALG_COUNT, (uint32_t)raw_length * 8, material, kept, false, usages);
}

/* importKey's operation, the method's part after normalization included
 * (§ 14.3.9 steps 8-12; § 27-34's "import key"), for importKey, unwrapKey
 * and deriveKey: `raw` (raw_length bytes) for a byte format, `j` for JWK. */
static JSValue import_key(JSContext *ctx, const SubtleParams *p, int format, const uint8_t *raw, size_t raw_length,
                          const DJwk *j, bool extractable, uint32_t usages)
{
    if (is_kdf(p->alg)) return import_kdf_key(ctx, p, format, raw, raw_length, extractable, usages);
    bool aes = is_aes(p->alg);
    if (p->alg == SUBTLE_ALG_HMAC && p->has_length && p->length == 0) {
        fail(ctx, "DataError", "An HMAC key's length cannot be zero");
        return JS_EXCEPTION;
    }
    if (usages & ~usages_allowed(p->alg)) {
        fail(ctx, "SyntaxError", "That usage is not this algorithm's");
        return JS_EXCEPTION;
    }
    const uint8_t *data = raw;
    size_t length = raw_length;
    uint8_t *decoded = NULL;
    JSValue result = JS_EXCEPTION;
    if (format == F_JWK) {
        char want[16];
        if (!text_is(j->s[J_KTY], "oct")) {
            fail(ctx, "DataError", "A secret key's JWK has kty \"oct\"");
            goto out;
        }
        if (j->s[J_K].text == NULL) {
            fail(ctx, "DataError", "A secret key's JWK has its key in k");
            goto out;
        }
        decoded = js_malloc(ctx, j->s[J_K].length * 3 / 4 + 1);
        if (decoded == NULL) goto out;
        if (!subtle_base64url_decode(j->s[J_K].text, j->s[J_K].length, decoded, &length)) {
            fail(ctx, "DataError", "A JWK's k is unpadded base64url");
            goto out;
        }
        data = decoded;
        DKey shape = {.alg = p->alg, .hash = p->hash, .bits = (uint32_t)length * 8};
        if (aes && !subtle_aes_key_length_ok(length)) {
            fail(ctx, "DataError", "An AES key is 128, 192 or 256 bits");
            goto out;
        }
        if (j->s[J_ALG].text != NULL && !text_is(j->s[J_ALG], jwk_alg(&shape, want, sizeof(want)))) {
            fail(ctx, "DataError", "The JWK's alg is not this key's");
            goto out;
        }
        if (usages != 0 && j->s[J_USE].text != NULL && !text_is(j->s[J_USE], aes ? "enc" : "sig")) {
            fail(ctx, "DataError", "The JWK's use does not allow these usages");
            goto out;
        }
        if (j->has_ops && !key_ops_admit(j, usages)) {
            fail(ctx, "DataError", "The JWK's key_ops do not allow these usages");
            goto out;
        }
        if (j->has_ext && !j->ext && extractable) {
            fail(ctx, "DataError", "The JWK says the key is not extractable");
            goto out;
        }
    } else if (format != F_RAW) {
        fail(ctx, "NotSupportedError", "This algorithm's keys are not imported in that format");
        goto out;
    }
    uint32_t bits;
    if (aes) {
        if (!subtle_aes_key_length_ok(length)) {
            fail(ctx, "DataError", "An AES key is 128, 192 or 256 bits");
            goto out;
        }
        bits = (uint32_t)length * 8;
    } else {
        uint64_t whole = (uint64_t)length * 8;
        if (whole == 0) {
            fail(ctx, "DataError", "An HMAC key cannot be empty");
            goto out;
        }
        if (whole > 4294967295u) {
            fail(ctx, "DataError", "That HMAC key is too long");
            goto out;
        }
        bits = (uint32_t)whole;
        if (p->has_length) {
            if (p->length > bits || p->length + 8 <= bits) {
                fail(ctx, "DataError", "An HMAC key's length is within its last byte");
                goto out;
            }
            bits = p->length;
        }
    }
    if (usages == 0) {
        fail(ctx, "SyntaxError", "A secret key needs at least one usage");
        goto out;
    }
    size_t kept;
    uint8_t *material = material_of(ctx, data, bits, &kept);
    if (material != NULL) result = key_new(ctx, p->alg, p->hash, bits, material, kept, extractable, usages);
out:
    if (decoded != NULL) {
        wipe(decoded, j->s[J_K].length * 3 / 4 + 1);
        js_free(ctx, decoded);
    }
    return result;
}

/* Whether the key's algorithm has an export operation (§ 14.3.10 step 6,
 * § 14.3.11 step 11): a derivation's key has none, whatever its flag. */
static int key_exports(JSContext *ctx, const DKey *k)
{
    return is_kdf(k->alg) ? fail(ctx, "NotSupportedError", "This algorithm's keys are never exported") : 0;
}

/* exportKey's checks and operation (§ 14.3.10 steps 6-7; § 27-31's
 * "export key"): an ArrayBuffer or the JWK object. */
static JSValue export_key(JSContext *ctx, const DKey *k, int format)
{
    if (key_exports(ctx, k) < 0) return JS_EXCEPTION;
    if (!k->extractable) {
        fail(ctx, "InvalidAccessError", "This key is not extractable");
        return JS_EXCEPTION;
    }
    if (format == F_RAW) return JS_NewArrayBufferCopy(ctx, k->material, k->bytes);
    if (format != F_JWK) {
        fail(ctx, "NotSupportedError", "This algorithm's keys are not exported in that format");
        return JS_EXCEPTION;
    }
    /* The JsonWebKey dictionary as an object: its members in
     * lexicographic order, as WebIDL makes one. */
    char alg[16];
    size_t size = subtle_base64url_size(k->bytes);
    char *text = js_malloc(ctx, size + 1);
    if (text == NULL) return JS_EXCEPTION;
    subtle_base64url_encode(k->material, k->bytes, text);
    JSValue jwk = JS_NewObject(ctx);
    int status = JS_IsException(jwk) ? -1 : define(ctx, jwk, "alg", JS_NewString(ctx, jwk_alg(k, alg, sizeof(alg))));
    if (status == 0) status = define(ctx, jwk, "ext", JS_NewBool(ctx, k->extractable));
    if (status == 0) status = define(ctx, jwk, "k", JS_NewStringLen(ctx, text, size));
    if (status == 0) status = define(ctx, jwk, "key_ops", usage_array(ctx, k->usages));
    if (status == 0) status = define(ctx, jwk, "kty", JS_NewString(ctx, "oct"));
    wipe((uint8_t *)text, size);
    js_free(ctx, text);
    if (status < 0) {
        JS_FreeValue(ctx, jwk);
        return JS_EXCEPTION;
    }
    return jwk;
}

/* A JWK as wrapKey's bytes: the UTF-8 of its JSON (§ 14.3.11 step 14),
 * written here rather than by JSON.stringify, so no page's toJSON runs;
 * every value is a string of letters, digits, - and _, or a boolean, so
 * nothing needs escaping. With `pad`, spaces after it make it a multiple
 * of 8 bytes, which AES-KW wraps (the step's Note allows adapting the
 * serialization to the wrapping algorithm; whitespace leaves the JSON
 * what it was). js_malloc'd, *length bytes. */
static char *jwk_json(JSContext *ctx, const DKey *k, bool pad, size_t *length)
{
    char alg[16];
    size_t key = subtle_base64url_size(k->bytes), cap = key + 168;
    char *json = js_malloc(ctx, cap);
    if (json == NULL) return NULL;
    size_t at = (size_t)os64_snprintf(json, cap, "{\"alg\":\"%s\",\"ext\":%s,\"k\":\"", jwk_alg(k, alg, sizeof(alg)),
                                      k->extractable ? "true" : "false");
    subtle_base64url_encode(k->material, k->bytes, json + at);
    at += key;
    at += (size_t)os64_snprintf(json + at, cap - at, "\",\"key_ops\":[");
    bool first = true;
    for (int i = 0; i < USAGE_COUNT; i++) {
        if (!(k->usages & (1u << i))) continue;
        at += (size_t)os64_snprintf(json + at, cap - at, "%s\"%s\"", first ? "" : ",", kUsages[i]);
        first = false;
    }
    at += (size_t)os64_snprintf(json + at, cap - at, "],\"kty\":\"oct\"}");
    while (pad && at % 8 != 0) json[at++] = ' ';
    *length = at;
    return json;
}

/* ── The operations ──────────────────────────────────────────────────── */

/* The algorithm's encrypt or decrypt (§ 27-29) of `in`: a js_malloc'd
 * result in *out (*out_length bytes), or -1 with the failure pending. The
 * parameters' bytes are normalization's private copies. */
static int cipher(os64_dom_t *dom, JSContext *ctx, const SubtleParams *p, const DKey *k, bool encrypt,
                  const uint8_t *in, size_t length, uint8_t **out, size_t *out_length)
{
    const uint8_t *iv, *aad;
    size_t iv_length, aad_length;
    *out = NULL;
    switch (p->alg) {
    case SUBTLE_ALG_AES_CTR:
        if (bytes_of(dom, ctx, p->counter, &iv, &iv_length) < 0) return -1;
        if (iv_length != SUBTLE_AES_BLOCK) return fail(ctx, "OperationError", "AES-CTR's counter is 16 bytes");
        if (p->length == 0 || p->length > 128) return fail(ctx, "OperationError", "AES-CTR's length is 1 to 128");
        *out = js_malloc(ctx, length ? length : 1);
        if (*out == NULL) return -1;
        if (!subtle_aes_ctr(k->material, k->bytes, iv, p->length, in, length, *out))
            return fail(ctx, "OperationError", "That much data would repeat the counter");
        *out_length = length;
        return 0;
    case SUBTLE_ALG_AES_CBC:
        if (bytes_of(dom, ctx, p->iv, &iv, &iv_length) < 0) return -1;
        if (iv_length != SUBTLE_AES_BLOCK) return fail(ctx, "OperationError", "AES-CBC's iv is 16 bytes");
        *out = js_malloc(ctx, length + SUBTLE_AES_BLOCK);
        if (*out == NULL) return -1;
        if (encrypt) {
            *out_length = subtle_aes_cbc_encrypt(k->material, k->bytes, iv, in, length, *out);
            return 0;
        }
        if (!subtle_aes_cbc_decrypt(k->material, k->bytes, iv, in, length, *out, out_length))
            return fail(ctx, "OperationError", "The ciphertext or its padding is not AES-CBC's");
        return 0;
    case SUBTLE_ALG_AES_GCM: {
        uint32_t tag = p->has_tag ? p->tag_length : 128;
        bool tag_ok = tag == 32 || tag == 64 || tag == 96 || tag == 104 || tag == 112 || tag == 120 || tag == 128;
        /* Decrypt asks the tag first, encrypt the iv first (§ 29). */
        if (!encrypt && !tag_ok) return fail(ctx, "OperationError", "AES-GCM's tagLength is not one it has");
        if (bytes_of(dom, ctx, p->iv, &iv, &iv_length) < 0) return -1;
        /* SP 800-38D asks an IV of at least one bit; § 29 names no
         * bound below, and an empty one is refused (WEB_CRYPTO.md § 3). */
        if (iv_length == 0) return fail(ctx, "OperationError", "AES-GCM's iv cannot be empty");
        if (bytes_of(dom, ctx, p->additional, &aad, &aad_length) < 0) return -1;
        if (encrypt && !tag_ok) return fail(ctx, "OperationError", "AES-GCM's tagLength is not one it has");
        size_t tag_bytes = tag / 8;
        if (encrypt) {
            *out = js_malloc(ctx, length + tag_bytes);
            if (*out == NULL) return -1;
            subtle_aes_gcm_encrypt(k->material, k->bytes, iv, iv_length, aad, aad_length, tag_bytes, in, length,
                                   *out);
            *out_length = length + tag_bytes;
            return 0;
        }
        if (length < tag_bytes) return fail(ctx, "OperationError", "The ciphertext is shorter than its tag");
        *out = js_malloc(ctx, length - tag_bytes + 1);
        if (*out == NULL) return -1;
        if (!subtle_aes_gcm_decrypt(k->material, k->bytes, iv, iv_length, aad, aad_length, tag_bytes, in, length,
                                    *out, out_length))
            return fail(ctx, "OperationError", "The ciphertext did not authenticate");
        return 0;
    }
    default:
        return fail(ctx, "NotSupportedError", "This algorithm does not encrypt");
    }
}

/* The key's name and usage, as every keyed operation checks them first
 * (§ 14.3.1 steps 9-10): InvalidAccessError for either. */
static int key_fits(JSContext *ctx, const SubtleParams *p, const DKey *k, uint32_t usage)
{
    if (k->alg != p->alg) return fail(ctx, "InvalidAccessError", "The key is not this algorithm's");
    if (!(k->usages & usage)) return fail(ctx, "InvalidAccessError", "The key's usages do not include this one");
    return 0;
}

/* A js_malloc'd result as an ArrayBuffer, the result freed (wiped). */
static JSValue buffer_answer(JSContext *ctx, uint8_t *bytes, size_t length)
{
    JSValue buffer = JS_NewArrayBufferCopy(ctx, bytes, length);
    wipe(bytes, length);
    js_free(ctx, bytes);
    return buffer;
}

/* digest(algorithm, data) (§ 14.3.5): the bytes `data` holds once the
 * algorithm is normalized, hashed. */
static JSValue digest(os64_dom_t *dom, JSContext *ctx, SubtleCall *c, SubtleParams *p)
{
    if (normalize(dom, ctx, c->algorithm[0], SUBTLE_OP_DIGEST, p) < 0) return JS_EXCEPTION;
    const uint8_t *bytes;
    size_t length;
    if (bytes_of(dom, ctx, c->buffers[0], &bytes, &length) < 0) return JS_EXCEPTION;
    uint8_t out[SUBTLE_DIGEST_MAX];
    if (!subtle_digest(p->alg, bytes, length, out)) {
        fail(ctx, "OperationError", "The digest could not be made");
        return JS_EXCEPTION;
    }
    return JS_NewArrayBufferCopy(ctx, out, subtle_digest_size(p->alg));
}

/* encrypt and decrypt (§ 14.3.1-2). */
static JSValue crypt(os64_dom_t *dom, JSContext *ctx, SubtleCall *c, SubtleParams *p, bool encrypt)
{
    if (normalize(dom, ctx, c->algorithm[0], encrypt ? SUBTLE_OP_ENCRYPT : SUBTLE_OP_DECRYPT, p) < 0)
        return JS_EXCEPTION;
    const uint8_t *in;
    size_t length;
    if (bytes_of(dom, ctx, c->buffers[0], &in, &length) < 0) return JS_EXCEPTION;
    if (key_fits(ctx, p, c->keys[0], encrypt ? U_ENCRYPT : U_DECRYPT) < 0) return JS_EXCEPTION;
    uint8_t *out;
    size_t out_length;
    if (cipher(dom, ctx, p, c->keys[0], encrypt, in, length, &out, &out_length) < 0) {
        js_free(ctx, out);
        return JS_EXCEPTION;
    }
    return buffer_answer(ctx, out, out_length);
}

/* sign and verify (§ 14.3.3-4): HMAC under the key's own hash; a
 * signature that does not match is false, never an error. */
static JSValue sign(os64_dom_t *dom, JSContext *ctx, SubtleCall *c, SubtleParams *p, bool signing)
{
    if (normalize(dom, ctx, c->algorithm[0], signing ? SUBTLE_OP_SIGN : SUBTLE_OP_VERIFY, p) < 0)
        return JS_EXCEPTION;
    const uint8_t *data, *signature = NULL;
    size_t length, signature_length = 0;
    if (!signing && bytes_of(dom, ctx, c->buffers[0], &signature, &signature_length) < 0) return JS_EXCEPTION;
    if (bytes_of(dom, ctx, c->buffers[signing ? 0 : 1], &data, &length) < 0) return JS_EXCEPTION;
    const DKey *k = c->keys[0];
    if (key_fits(ctx, p, k, signing ? U_SIGN : U_VERIFY) < 0) return JS_EXCEPTION;
    uint8_t mac[SUBTLE_DIGEST_MAX];
    size_t size = subtle_digest_size(k->hash);
    if (!subtle_hmac(k->hash, k->material, k->bytes, data, length, mac)) {
        fail(ctx, "OperationError", "The MAC could not be made");
        return JS_EXCEPTION;
    }
    JSValue answer = signing ? JS_NewArrayBufferCopy(ctx, mac, size)
                             : JS_NewBool(ctx, signature_length == size && subtle_same_bytes(mac, signature, size));
    wipe(mac, sizeof(mac));
    return answer;
}

/* Normalization for wrapKey or unwrapKey: the operation's own, else the
 * encrypt or decrypt its algorithm has (§ 14.3.11 steps 2-4). */
static int normalize_wrapping(os64_dom_t *dom, JSContext *ctx, JSValueConst algorithm, bool wrap, SubtleParams *p)
{
    if (normalize(dom, ctx, algorithm, wrap ? SUBTLE_OP_WRAP_KEY : SUBTLE_OP_UNWRAP_KEY, p) == 0) return 0;
    if (!d_exception_catchable(ctx)) return -1;
    JS_FreeValue(ctx, JS_GetException(ctx));
    params_free(ctx, p);
    params_init(p);
    return normalize(dom, ctx, algorithm, wrap ? SUBTLE_OP_ENCRYPT : SUBTLE_OP_DECRYPT, p);
}

/* AES-KW's own wrap and unwrap (§ 30.3.1-2): RFC 3394 over a multiple of
 * 8 bytes, at least two blocks of it (the RFC's floor, which the spec
 * leaves unsaid), its integrity check refusing anything else; every
 * failure is OperationError. A js_malloc'd result as cipher's is. */
static int key_wrap(JSContext *ctx, const DKey *k, bool wrap, const uint8_t *in, size_t length, uint8_t **out,
                    size_t *out_length)
{
    *out = js_malloc(ctx, length + 8);
    if (*out == NULL) return -1;
    if (wrap) {
        *out_length = subtle_aes_kw_wrap(k->material, k->bytes, in, length, *out);
        if (*out_length == 0)
            return fail(ctx, "OperationError", "AES-KW wraps a multiple of 8 bytes, at least 16 of them");
        return 0;
    }
    if (!subtle_aes_kw_unwrap(k->material, k->bytes, in, length, *out, out_length))
        return fail(ctx, "OperationError", "The wrapped key did not unwrap");
    return 0;
}

/* wrapKey(format, key, wrappingKey, wrapAlgorithm) (§ 14.3.11): the key
 * exported, then wrapped by the algorithm's own wrap operation when it has
 * one, else encrypted. */
static JSValue wrap_key(os64_dom_t *dom, JSContext *ctx, SubtleCall *c, SubtleParams *p)
{
    if (normalize_wrapping(dom, ctx, c->algorithm[0], true, p) < 0) return JS_EXCEPTION;
    const DKey *key = c->keys[0], *wrapping = c->keys[1];
    if (key_fits(ctx, p, wrapping, U_WRAP) < 0) return JS_EXCEPTION;
    if (key_exports(ctx, key) < 0) return JS_EXCEPTION;
    if (!key->extractable) {
        fail(ctx, "InvalidAccessError", "This key is not extractable");
        return JS_EXCEPTION;
    }
    uint8_t *plain;
    size_t plain_length;
    if (c->format == F_RAW) {
        plain = js_malloc(ctx, key->bytes ? key->bytes : 1);
        if (plain == NULL) return JS_EXCEPTION;
        if (key->bytes) os64_memcpy(plain, key->material, key->bytes);
        plain_length = key->bytes;
    } else if (c->format == F_JWK) {
        plain = (uint8_t *)jwk_json(ctx, key, p->alg == SUBTLE_ALG_AES_KW, &plain_length);
        if (plain == NULL) return JS_EXCEPTION;
    } else {
        fail(ctx, "NotSupportedError", "This algorithm's keys are not exported in that format");
        return JS_EXCEPTION;
    }
    uint8_t *out;
    size_t out_length;
    int status = p->alg == SUBTLE_ALG_AES_KW ? key_wrap(ctx, wrapping, true, plain, plain_length, &out, &out_length)
                                             : cipher(dom, ctx, p, wrapping, true, plain, plain_length, &out, &out_length);
    wipe(plain, plain_length);
    js_free(ctx, plain);
    if (status < 0) {
        js_free(ctx, out);
        return JS_EXCEPTION;
    }
    return buffer_answer(ctx, out, out_length);
}

/* unwrapKey (§ 14.3.12): unwrapped by the algorithm's own unwrap operation
 * when it has one, else decrypted, under the unwrapping key; the JWK's
 * JSON parsed when the format is jwk; then imported. */
static JSValue unwrap_key(os64_dom_t *dom, JSContext *ctx, SubtleCall *c, SubtleParams *p)
{
    SubtleParams imported;
    params_init(&imported);
    JSValue result = JS_EXCEPTION;
    uint8_t *plain = NULL;
    size_t plain_length = 0;
    DJwk j = {0};
    if (normalize_wrapping(dom, ctx, c->algorithm[0], false, p) < 0) goto out;
    if (normalize(dom, ctx, c->algorithm[1], SUBTLE_OP_IMPORT_KEY, &imported) < 0) goto out;
    const uint8_t *wrapped;
    size_t length;
    if (bytes_of(dom, ctx, c->buffers[0], &wrapped, &length) < 0) goto out;
    if (key_fits(ctx, p, c->keys[0], U_UNWRAP) < 0) goto out;
    if ((p->alg == SUBTLE_ALG_AES_KW ? key_wrap(ctx, c->keys[0], false, wrapped, length, &plain, &plain_length)
                                     : cipher(dom, ctx, p, c->keys[0], false, wrapped, length, &plain, &plain_length)) < 0)
        goto out;
    if (c->format == F_JWK) {
        /* § 9's "parse a JWK": the bytes as UTF-8 JSON, the value the
         * JsonWebKey dictionary converted as a new global would (THE
         * JSONWEBKEY DICTIONARY), which needs a kty. JSON.parse's own
         * SyntaxError answers what is no JSON. */
        uint8_t *text = js_malloc(ctx, plain_length + 1);
        if (text == NULL) goto out;
        os64_memcpy(text, plain, plain_length);
        text[plain_length] = '\0';
        JSValue parsed = JS_ParseJSON(ctx, (const char *)text, plain_length, "<unwrapped key>");
        wipe(text, plain_length);
        js_free(ctx, text);
        if (JS_IsException(parsed)) goto out;
        int status = json_web_key(dom, ctx, parsed, &j, true);
        JS_FreeValue(ctx, parsed);
        if (status < 0) goto out;
        if (j.s[J_KTY].text == NULL) {
            fail(ctx, "DataError", "The unwrapped JWK has no kty");
            goto out;
        }
    }
    result = import_key(ctx, &imported, c->format, plain, plain_length, &j, c->extractable, c->usages);
out:
    if (plain != NULL) {
        wipe(plain, plain_length);
        js_free(ctx, plain);
    }
    jwk_free(ctx, &j);
    params_free(ctx, &imported);
    return result;
}

/* The derive bits operation (§ 33.4.1, § 34.4.1) of `k` for `bits`
 * (`has_bits` false for a null length): a js_malloc'd result in *out
 * (*out_length bytes), or -1 with the failure pending. Neither derivation
 * takes a null length or one within a byte; HKDF makes at most 255 hashes'
 * worth, PBKDF2 at most SUBTLE_PBKDF2_MAX_BYTES, and PBKDF2 refuses no
 * iterations before it answers a length of none. */
static int derive(JSContext *ctx, os64_dom_t *dom, const SubtleParams *p, const DKey *k, bool has_bits, uint32_t bits,
                  uint8_t **out, size_t *out_length)
{
    *out = NULL;
    if (!has_bits || bits % 8 != 0) return fail(ctx, "OperationError", "A derivation's length is a multiple of 8");
    size_t length = bits / 8;
    const uint8_t *salt, *info;
    size_t salt_length, info_length;
    if (bytes_of(dom, ctx, p->salt, &salt, &salt_length) < 0) return -1;
    if (p->alg == SUBTLE_ALG_HKDF) {
        if (length > 255 * subtle_digest_size(p->hash))
            return fail(ctx, "OperationError", "HKDF makes at most 255 times its hash's length");
        if (bytes_of(dom, ctx, p->info, &info, &info_length) < 0) return -1;
    } else if (p->iterations == 0) {
        return fail(ctx, "OperationError", "PBKDF2 needs at least one iteration");
    } else if (length > SUBTLE_PBKDF2_MAX_BYTES) {
        return fail(ctx, "OperationError", "PBKDF2 derives at most 65,536 bytes here");
    }
    *out = js_malloc(ctx, length ? length : 1);
    if (*out == NULL) return -1;
    *out_length = length;
    if (length == 0) return 0;
    bool made = p->alg == SUBTLE_ALG_HKDF
                    ? subtle_hkdf(p->hash, k->material, k->bytes, salt, salt_length, info, info_length, *out, length)
                    : subtle_pbkdf2(p->hash, k->material, k->bytes, salt, salt_length, p->iterations, *out, length);
    return made ? 0 : fail(ctx, "OperationError", "The derivation failed");
}

/* deriveBits(algorithm, baseKey, length) (§ 14.3.8). */
static JSValue derive_bits(os64_dom_t *dom, JSContext *ctx, SubtleCall *c, SubtleParams *p)
{
    if (normalize(dom, ctx, c->algorithm[0], SUBTLE_OP_DERIVE_BITS, p) < 0) return JS_EXCEPTION;
    if (key_fits(ctx, p, c->keys[0], U_DERIVE_BITS) < 0) return JS_EXCEPTION;
    uint8_t *bits;
    size_t length;
    if (derive(ctx, dom, p, c->keys[0], c->length_given, c->length, &bits, &length) < 0) {
        js_free(ctx, bits);
        return JS_EXCEPTION;
    }
    return buffer_answer(ctx, bits, length);
}

/* The get key length operation (§ 27-34) of a normalized derived key type:
 * 0 with the length in *bits, *has_bits false for null, or -1 with the
 * failure pending. Null is HKDF's and PBKDF2's answer, and both refuse a
 * null length to derive, so a derivation key is never deriveKey's result:
 * OperationError, as the spec's steps imply. */
static int key_length(JSContext *ctx, const SubtleParams *p, bool *has_bits, uint32_t *bits)
{
    *has_bits = true;
    if (is_aes(p->alg)) {
        if (p->length != 128 && p->length != 192 && p->length != 256)
            return fail(ctx, "OperationError", "An AES key is 128, 192 or 256 bits");
        *bits = p->length;
    } else if (p->alg == SUBTLE_ALG_HMAC) {
        if (p->has_length && p->length == 0) return fail(ctx, "TypeError", "An HMAC key's length cannot be zero");
        *bits = p->has_length ? p->length : (uint32_t)subtle_hash_block_size(p->hash) * 8;
    } else {
        *has_bits = false;
    }
    return 0;
}

/* deriveKey(algorithm, baseKey, derivedKeyType, extractable, keyUsages)
 * (§ 14.3.7): the derived key type normalized twice, for importKey and for
 * get key length, before anything is checked; then the base key, the
 * length, the bits, and the bits imported raw as the derived key. */
static JSValue derive_key(os64_dom_t *dom, JSContext *ctx, SubtleCall *c, SubtleParams *p)
{
    SubtleParams imported, sized;
    params_init(&imported);
    params_init(&sized);
    JSValue result = JS_EXCEPTION;
    uint8_t *secret = NULL;
    size_t length = 0;
    bool has_bits = false;
    uint32_t bits = 0;
    if (normalize(dom, ctx, c->algorithm[0], SUBTLE_OP_DERIVE_BITS, p) == 0 &&
        normalize(dom, ctx, c->algorithm[1], SUBTLE_OP_IMPORT_KEY, &imported) == 0 &&
        normalize(dom, ctx, c->algorithm[1], SUBTLE_OP_GET_KEY_LENGTH, &sized) == 0 &&
        key_fits(ctx, p, c->keys[0], U_DERIVE_KEY) == 0 && key_length(ctx, &sized, &has_bits, &bits) == 0 &&
        derive(ctx, dom, p, c->keys[0], has_bits, bits, &secret, &length) == 0) {
        DJwk none = {0};
        result = import_key(ctx, &imported, F_RAW, secret, length, &none, c->extractable, c->usages);
    }
    if (secret != NULL) {
        wipe(secret, length);
        js_free(ctx, secret);
    }
    params_free(ctx, &imported);
    params_free(ctx, &sized);
    return result;
}

/* THE SETTLE QUEUE. A method's answer is made on the call and its promise
 * settled from a task, as the spec's "in parallel" steps settle from a
 * queued task, so a page's reaction runs in a task of its own and a chain
 * of thousands of calls is thousands of short tasks, not one run of
 * microtasks the script deadline would end. The answers wait in one
 * queue, in call order, and ONE binding timer of no delay is the task
 * that settles every answer waiting when it runs: a page with a thousand
 * calls in flight holds one of its timers, not a thousand, so its own
 * setTimeout never meets a quota crypto spent, and the promises settle
 * in the order they were made. An answer queued while the task runs (a
 * `then` getter the settling ran) waits for the next task. */
static JSValue settle_drain(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                            JSValue *data)
{
    (void)self;
    (void)argc;
    (void)argv;
    (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    dom->subtle_drain_queued = false;
    JSValueConst queue = dom->subtle_queue->value;
    uint32_t end = dom->subtle_tail;
    while (dom->subtle_head < end) {
        JSValue entry = JS_GetPropertyUint32(ctx, queue, dom->subtle_head++);
        JSValue settle = JS_GetPropertyUint32(ctx, entry, 0);
        JSValue value = JS_GetPropertyUint32(ctx, entry, 1);
        JS_FreeValue(ctx, entry);
        /* A resolving function throws nothing a page can catch: a `then`
         * getter's exception rejects the promise instead. What comes back
         * is the runtime stopping, and the rest wait with it. */
        JSValue done = JS_Call(ctx, settle, JS_UNDEFINED, 1, (JSValueConst *)&value);
        JS_FreeValue(ctx, settle);
        JS_FreeValue(ctx, value);
        if (JS_IsException(done)) return done;
        JS_FreeValue(ctx, done);
    }
    if (dom->subtle_head == dom->subtle_tail) {
        /* Empty: every entry let go at once, and the indices start over. */
        dom->subtle_head = dom->subtle_tail = 0;
        if (JS_SetPropertyStr(ctx, queue, "length", JS_NewInt32(ctx, 0)) < 0) return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

/* A promise for `result` (taken; JS_EXCEPTION for the failure pending),
 * its answer queued for the settle queue's task, which is queued too when
 * none is. When no task can be had (the page's own timers hold the whole
 * quota, or memory), the promise is settled on the call instead: the
 * answer all the same, only sooner. */
static JSValue settle_later(os64_dom_t *dom, JSContext *ctx, JSValue result)
{
    bool rejected = JS_IsException(result);
    if (rejected && !d_exception_catchable(ctx)) return JS_EXCEPTION;
    JSValue value = rejected ? JS_GetException(ctx) : result;
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, value);
        return promise;
    }
    JSValue settle = resolving[rejected ? 1 : 0];
    JS_FreeValue(ctx, resolving[rejected ? 0 : 1]);
    if (!dom->subtle_drain_queued) {
        JSValue fn_data[1] = {dom->anchor->value};
        JSValue drain = JS_NewCFunctionData(ctx, settle_drain, 0, 0, 1, fn_data);
        dom->subtle_drain_queued = !JS_IsException(drain) && d_timer_add(dom, ctx, drain, 0) != 0;
    }
    if (!dom->subtle_drain_queued) {
        JSValue done = JS_EXCEPTION;
        if (d_exception_catchable(ctx)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            done = JS_Call(ctx, settle, JS_UNDEFINED, 1, (JSValueConst *)&value);
        }
        JS_FreeValue(ctx, settle);
        JS_FreeValue(ctx, value);
        if (JS_IsException(done)) {
            JS_FreeValue(ctx, promise);
            return done;
        }
        JS_FreeValue(ctx, done);
        return promise;
    }
    /* The entry, [settle, value], defined (never set), so no setter a page
     * put on Array.prototype runs. A define takes its value either way. */
    JSValue entry = JS_NewArray(ctx);
    if (JS_IsException(entry)) {
        JS_FreeValue(ctx, settle);
        JS_FreeValue(ctx, value);
    } else if (JS_DefinePropertyValueUint32(ctx, entry, 0, settle, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, value);
        JS_FreeValue(ctx, entry);
    } else if (JS_DefinePropertyValueUint32(ctx, entry, 1, value, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, entry);
    } else if (dom->subtle_tail == UINT32_MAX - 1) {
        /* An array's last index; four billion calls between two tasks. */
        JS_FreeValue(ctx, entry);
        JS_ThrowRangeError(ctx, "Too many crypto.subtle calls are waiting");
    } else if (JS_DefinePropertyValueUint32(ctx, dom->subtle_queue->value, dom->subtle_tail, entry,
                                            JS_PROP_C_W_E) >= 0) {
        dom->subtle_tail++;
        return promise;
    }
    JS_FreeValue(ctx, promise);
    return JS_EXCEPTION;
}

static JSValue subtle_method(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                             JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue result = JS_EXCEPTION;
    SubtleCall c = {.format = -1};
    SubtleParams p;
    params_init(&p);
    if (JS_GetOpaque(self, os64_js_class_id(&subtle_slot)) == NULL) {
        JS_ThrowTypeError(ctx, "SubtleCrypto.%s called on something that is not a SubtleCrypto",
                          kMethods[magic].name);
    } else if (convert(dom, ctx, magic, argc, argv, &c) == 0) {
        switch (magic) {
        case S_DIGEST: result = digest(dom, ctx, &c, &p); break;
        case S_ENCRYPT: result = crypt(dom, ctx, &c, &p, true); break;
        case S_DECRYPT: result = crypt(dom, ctx, &c, &p, false); break;
        case S_SIGN: result = sign(dom, ctx, &c, &p, true); break;
        case S_VERIFY: result = sign(dom, ctx, &c, &p, false); break;
        case S_GENERATE_KEY:
            if (normalize(dom, ctx, c.algorithm[0], SUBTLE_OP_GENERATE_KEY, &p) == 0)
                result = generate_key(dom, ctx, &p, c.extractable, c.usages);
            break;
        case S_IMPORT_KEY: {
            if (normalize(dom, ctx, c.algorithm[0], SUBTLE_OP_IMPORT_KEY, &p) < 0) break;
            /* § 14.3.9 step 8: the key data's type against the format. */
            if ((c.format == F_JWK) == c.key_data_buffer) {
                JS_ThrowTypeError(ctx, c.format == F_JWK ? "A jwk key is a JsonWebKey" : "That format's key is a BufferSource");
                break;
            }
            const uint8_t *raw = NULL;
            size_t raw_length = 0;
            if (c.key_data_buffer && bytes_of(dom, ctx, c.buffers[c.buffer_count - 1], &raw, &raw_length) < 0) break;
            result = import_key(ctx, &p, c.format, raw, raw_length, &c.jwk, c.extractable, c.usages);
            break;
        }
        case S_EXPORT_KEY: result = export_key(ctx, c.keys[0], c.format); break;
        case S_WRAP_KEY: result = wrap_key(dom, ctx, &c, &p); break;
        case S_UNWRAP_KEY: result = unwrap_key(dom, ctx, &c, &p); break;
        case S_DERIVE_BITS: result = derive_bits(dom, ctx, &c, &p); break;
        case S_DERIVE_KEY: result = derive_key(dom, ctx, &c, &p); break;
        }
    }
    params_free(ctx, &p);
    call_free(ctx, &c);
    return settle_later(dom, ctx, result);
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
    case K_ALGORITHM: return JS_DupValue(ctx, key->algorithm);
    default: return JS_DupValue(ctx, key->usage_list);
    }
}

int d_subtle_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global, JSValueConst crypto_prototype)
{
    static const JSClassDef subtle_class = {.class_name = "SubtleCrypto"};
    static const JSClassDef key_class = {.class_name = "CryptoKey", .finalizer = key_finalizer, .gc_mark = key_mark};
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
    JSValue queue = JS_NewArray(ctx);
    if (JS_IsException(queue)) return -1;
    dom->subtle_queue = d_retain(dom, ctx, queue);
    if (dom->subtle_queue == NULL) return -1;
    JS_SetOpaque(subtle, &subtle_mark);
    /* crypto.c shapes Crypto's members, this one among them (d_idl_members). */
    return d_accessor(dom, ctx, crypto_prototype, "subtle", key_member, W_SUBTLE, false);
}
