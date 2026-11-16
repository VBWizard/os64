#include "subtle_core.h"

#include "bearssl_aead.h"
#include "bearssl_block.h"
#include "bearssl_ec.h"
#include "bearssl_hash.h"
#include "bearssl_hmac.h"
#include "bearssl_kdf.h"
#include "bearssl_rand.h"

/* What a NULL buffer of no bytes is handed to BearSSL as: its code copies
 * with memcpy, which C forbids a null pointer even for a length of zero
 * (an absent additionalData, a detached buffer). */
static const uint8_t kNothing[1];
#define SOME(p) ((p) != NULL ? (p) : kNothing)

/* THE REGISTRY (Web Cryptography § 18.4.1): each operation's algorithms by
 * name and the dictionary each one's parameters convert to. A name absent
 * here is NotSupportedError for that operation, which is what a browser
 * answers for an algorithm it does not know. THE RULE: an algorithm is
 * registered for an operation only when that operation's steps for it are
 * built, in the binding and here; subtle.c answers a registration without
 * steps NotSupportedError too, but nothing should reach that answer. */
typedef struct {
    subtle_op_t op;
    subtle_alg_t alg;
    subtle_dict_t dict;
} SubtleRegistration;

static const char *const kNames[SUBTLE_ALG_COUNT] = {
    [SUBTLE_ALG_SHA_1] = "SHA-1",
    [SUBTLE_ALG_SHA_256] = "SHA-256",
    [SUBTLE_ALG_SHA_384] = "SHA-384",
    [SUBTLE_ALG_SHA_512] = "SHA-512",
    [SUBTLE_ALG_AES_CTR] = "AES-CTR",
    [SUBTLE_ALG_AES_CBC] = "AES-CBC",
    [SUBTLE_ALG_AES_GCM] = "AES-GCM",
    [SUBTLE_ALG_HMAC] = "HMAC",
    [SUBTLE_ALG_AES_KW] = "AES-KW",
    [SUBTLE_ALG_HKDF] = "HKDF",
    [SUBTLE_ALG_PBKDF2] = "PBKDF2",
    [SUBTLE_ALG_ECDSA] = "ECDSA",
    [SUBTLE_ALG_ECDH] = "ECDH",
};

static const SubtleRegistration kRegistry[] = {
    {SUBTLE_OP_DIGEST, SUBTLE_ALG_SHA_1, SUBTLE_DICT_ALGORITHM},
    {SUBTLE_OP_DIGEST, SUBTLE_ALG_SHA_256, SUBTLE_DICT_ALGORITHM},
    {SUBTLE_OP_DIGEST, SUBTLE_ALG_SHA_384, SUBTLE_DICT_ALGORITHM},
    {SUBTLE_OP_DIGEST, SUBTLE_ALG_SHA_512, SUBTLE_DICT_ALGORITHM},
    /* The AES modes (§ 27-29): wrapKey and unwrapKey are not theirs, so
     * those fall back to encrypt and decrypt (§ 14.3.11). */
    {SUBTLE_OP_ENCRYPT, SUBTLE_ALG_AES_CTR, SUBTLE_DICT_AES_CTR},
    {SUBTLE_OP_DECRYPT, SUBTLE_ALG_AES_CTR, SUBTLE_DICT_AES_CTR},
    {SUBTLE_OP_GENERATE_KEY, SUBTLE_ALG_AES_CTR, SUBTLE_DICT_AES_KEY_GEN},
    {SUBTLE_OP_IMPORT_KEY, SUBTLE_ALG_AES_CTR, SUBTLE_DICT_ALGORITHM},
    {SUBTLE_OP_ENCRYPT, SUBTLE_ALG_AES_CBC, SUBTLE_DICT_AES_CBC},
    {SUBTLE_OP_DECRYPT, SUBTLE_ALG_AES_CBC, SUBTLE_DICT_AES_CBC},
    {SUBTLE_OP_GENERATE_KEY, SUBTLE_ALG_AES_CBC, SUBTLE_DICT_AES_KEY_GEN},
    {SUBTLE_OP_IMPORT_KEY, SUBTLE_ALG_AES_CBC, SUBTLE_DICT_ALGORITHM},
    {SUBTLE_OP_ENCRYPT, SUBTLE_ALG_AES_GCM, SUBTLE_DICT_AES_GCM},
    {SUBTLE_OP_DECRYPT, SUBTLE_ALG_AES_GCM, SUBTLE_DICT_AES_GCM},
    {SUBTLE_OP_GENERATE_KEY, SUBTLE_ALG_AES_GCM, SUBTLE_DICT_AES_KEY_GEN},
    {SUBTLE_OP_IMPORT_KEY, SUBTLE_ALG_AES_GCM, SUBTLE_DICT_ALGORITHM},
    /* HMAC (§ 31): sign and verify read no parameters, the key's hash is
     * the one used. */
    {SUBTLE_OP_SIGN, SUBTLE_ALG_HMAC, SUBTLE_DICT_ALGORITHM},
    {SUBTLE_OP_VERIFY, SUBTLE_ALG_HMAC, SUBTLE_DICT_ALGORITHM},
    {SUBTLE_OP_GENERATE_KEY, SUBTLE_ALG_HMAC, SUBTLE_DICT_HMAC_KEY_GEN},
    {SUBTLE_OP_IMPORT_KEY, SUBTLE_ALG_HMAC, SUBTLE_DICT_HMAC_IMPORT},
    /* "Get key length" is no method: deriveKey asks it of the key type it
     * derives (§ 14.3.7 step 6), and an algorithm without the row is
     * NotSupportedError there. */
    {SUBTLE_OP_GET_KEY_LENGTH, SUBTLE_ALG_AES_CTR, SUBTLE_DICT_AES_DERIVED},
    {SUBTLE_OP_GET_KEY_LENGTH, SUBTLE_ALG_AES_CBC, SUBTLE_DICT_AES_DERIVED},
    {SUBTLE_OP_GET_KEY_LENGTH, SUBTLE_ALG_AES_GCM, SUBTLE_DICT_AES_DERIVED},
    {SUBTLE_OP_GET_KEY_LENGTH, SUBTLE_ALG_HMAC, SUBTLE_DICT_HMAC_IMPORT},
    /* AES-KW (§ 30): wrapKey and unwrapKey are its own; it has no encrypt
     * or decrypt. */
    {SUBTLE_OP_WRAP_KEY, SUBTLE_ALG_AES_KW, SUBTLE_DICT_ALGORITHM},
    {SUBTLE_OP_UNWRAP_KEY, SUBTLE_ALG_AES_KW, SUBTLE_DICT_ALGORITHM},
    {SUBTLE_OP_GENERATE_KEY, SUBTLE_ALG_AES_KW, SUBTLE_DICT_AES_KEY_GEN},
    {SUBTLE_OP_IMPORT_KEY, SUBTLE_ALG_AES_KW, SUBTLE_DICT_ALGORITHM},
    {SUBTLE_OP_GET_KEY_LENGTH, SUBTLE_ALG_AES_KW, SUBTLE_DICT_AES_DERIVED},
    /* HKDF and PBKDF2 (§ 33, § 34): keys only imported, raw, never
     * extractable. Their key length is null: ECDH's derive bits takes it
     * as the whole secret, and theirs refuse it (OperationError), so one
     * is derived from an ECDH key and never from another derivation's. */
    {SUBTLE_OP_DERIVE_BITS, SUBTLE_ALG_HKDF, SUBTLE_DICT_HKDF},
    {SUBTLE_OP_IMPORT_KEY, SUBTLE_ALG_HKDF, SUBTLE_DICT_ALGORITHM},
    {SUBTLE_OP_GET_KEY_LENGTH, SUBTLE_ALG_HKDF, SUBTLE_DICT_ALGORITHM},
    {SUBTLE_OP_DERIVE_BITS, SUBTLE_ALG_PBKDF2, SUBTLE_DICT_PBKDF2},
    {SUBTLE_OP_IMPORT_KEY, SUBTLE_ALG_PBKDF2, SUBTLE_DICT_ALGORITHM},
    {SUBTLE_OP_GET_KEY_LENGTH, SUBTLE_ALG_PBKDF2, SUBTLE_DICT_ALGORITHM},
    /* ECDSA and ECDH (§ 23, § 24): key pairs on a named curve. ECDH's
     * operation is derive bits; no key is derived as one of its own, so
     * it has no key length. */
    {SUBTLE_OP_SIGN, SUBTLE_ALG_ECDSA, SUBTLE_DICT_ECDSA},
    {SUBTLE_OP_VERIFY, SUBTLE_ALG_ECDSA, SUBTLE_DICT_ECDSA},
    {SUBTLE_OP_GENERATE_KEY, SUBTLE_ALG_ECDSA, SUBTLE_DICT_EC_KEY_GEN},
    {SUBTLE_OP_IMPORT_KEY, SUBTLE_ALG_ECDSA, SUBTLE_DICT_EC_KEY_IMPORT},
    {SUBTLE_OP_DERIVE_BITS, SUBTLE_ALG_ECDH, SUBTLE_DICT_ECDH_DERIVE},
    {SUBTLE_OP_GENERATE_KEY, SUBTLE_ALG_ECDH, SUBTLE_DICT_EC_KEY_GEN},
    {SUBTLE_OP_IMPORT_KEY, SUBTLE_ALG_ECDH, SUBTLE_DICT_EC_KEY_IMPORT},
};

/* ASCII case-insensitive: only A-Z fold, so a name spelled with a
 * character that folds to a Latin letter elsewhere (U+212A, the Kelvin
 * sign, for K) matches nothing. */
static bool ascii_same(const char *a, size_t length, const char *b)
{
    size_t i = 0;
    for (; i < length; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (y == '\0') return false;
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y + 32);
        if (x != y) return false;
    }
    return b[i] == '\0';
}

int subtle_registered(subtle_op_t op, const char *name, size_t length, subtle_dict_t *dict)
{
    for (size_t i = 0; i < sizeof(kRegistry) / sizeof(kRegistry[0]); i++) {
        if (kRegistry[i].op != op || !ascii_same(name, length, kNames[kRegistry[i].alg])) continue;
        *dict = kRegistry[i].dict;
        return (int)kRegistry[i].alg;
    }
    return -1;
}

const char *subtle_alg_name(subtle_alg_t alg)
{
    return (unsigned)alg < SUBTLE_ALG_COUNT ? kNames[alg] : "";
}

size_t subtle_digest_size(subtle_alg_t alg)
{
    switch (alg) {
    case SUBTLE_ALG_SHA_1: return br_sha1_SIZE;
    case SUBTLE_ALG_SHA_256: return br_sha256_SIZE;
    case SUBTLE_ALG_SHA_384: return br_sha384_SIZE;
    case SUBTLE_ALG_SHA_512: return br_sha512_SIZE;
    default: return 0;
    }
}

bool subtle_digest(subtle_alg_t alg, const uint8_t *data, size_t length, uint8_t *out)
{
    switch (alg) {
    case SUBTLE_ALG_SHA_1: {
        br_sha1_context c;
        br_sha1_init(&c);
        br_sha1_update(&c, SOME(data), length);
        br_sha1_out(&c, out);
        return true;
    }
    case SUBTLE_ALG_SHA_256: {
        br_sha256_context c;
        br_sha256_init(&c);
        br_sha256_update(&c, SOME(data), length);
        br_sha256_out(&c, out);
        return true;
    }
    case SUBTLE_ALG_SHA_384: {
        br_sha384_context c;
        br_sha384_init(&c);
        br_sha384_update(&c, SOME(data), length);
        br_sha384_out(&c, out);
        return true;
    }
    case SUBTLE_ALG_SHA_512: {
        br_sha512_context c;
        br_sha512_init(&c);
        br_sha512_update(&c, SOME(data), length);
        br_sha512_out(&c, out);
        return true;
    }
    default: return false;
    }
}

/* A secret's bytes overwritten, through a pointer the compiler may not
 * assume dead. */
static void wipe(void *p, size_t length)
{
    volatile uint8_t *v = p;
    while (length-- > 0) *v++ = 0;
}

bool subtle_aes_key_length_ok(size_t length)
{
    return length == 16 || length == 24 || length == 32;
}

size_t subtle_aes_cbc_encrypt(const uint8_t *key, size_t key_length, const uint8_t iv[SUBTLE_AES_BLOCK],
                              const uint8_t *in, size_t length, uint8_t *out)
{
    size_t pad = SUBTLE_AES_BLOCK - length % SUBTLE_AES_BLOCK, total = length + pad;
    for (size_t i = 0; i < length; i++) out[i] = in[i];
    for (size_t i = length; i < total; i++) out[i] = (uint8_t)pad;
    uint8_t chain[SUBTLE_AES_BLOCK];
    for (int i = 0; i < SUBTLE_AES_BLOCK; i++) chain[i] = iv[i];
    br_aes_ct64_cbcenc_keys k;
    br_aes_ct64_cbcenc_init(&k, key, key_length);
    br_aes_ct64_cbcenc_run(&k, chain, out, total);
    wipe(&k, sizeof(k));
    return total;
}

bool subtle_aes_cbc_decrypt(const uint8_t *key, size_t key_length, const uint8_t iv[SUBTLE_AES_BLOCK],
                            const uint8_t *in, size_t length, uint8_t *out, size_t *out_length)
{
    if (length == 0 || length % SUBTLE_AES_BLOCK != 0) return false;
    for (size_t i = 0; i < length; i++) out[i] = in[i];
    uint8_t chain[SUBTLE_AES_BLOCK];
    for (int i = 0; i < SUBTLE_AES_BLOCK; i++) chain[i] = iv[i];
    br_aes_ct64_cbcdec_keys k;
    br_aes_ct64_cbcdec_init(&k, key, key_length);
    br_aes_ct64_cbcdec_run(&k, chain, out, length);
    wipe(&k, sizeof(k));
    unsigned pad = out[length - 1];
    bool good = pad >= 1 && pad <= SUBTLE_AES_BLOCK;
    for (unsigned i = 1; good && i <= pad; i++) good = out[length - i] == pad;
    if (!good) {
        wipe(out, length);
        return false;
    }
    *out_length = length - pad;
    return true;
}

/* One block enciphered: CBC over a single block from a zero IV, which is
 * the block cipher itself (the IV is XORed in first, and zero changes
 * nothing), so no door into BearSSL's internals is needed. */
static void aes_block(const br_aes_ct64_cbcenc_keys *k, const uint8_t in[SUBTLE_AES_BLOCK],
                      uint8_t out[SUBTLE_AES_BLOCK])
{
    uint8_t zero[SUBTLE_AES_BLOCK] = {0};
    for (int i = 0; i < SUBTLE_AES_BLOCK; i++) out[i] = in[i];
    br_aes_ct64_cbcenc_run(k, zero, out, SUBTLE_AES_BLOCK);
    /* CBC leaves its last block in the IV, and that block is `out`. */
    wipe(zero, sizeof(zero));
}

/* The counter block's low `bits` bits plus one, wrapped within them. */
static void counter_next(uint8_t c[SUBTLE_AES_BLOCK], unsigned bits)
{
    for (int i = SUBTLE_AES_BLOCK - 1; i >= 0 && bits > 0; i--) {
        unsigned span = bits >= 8 ? 8 : bits;
        unsigned mask = (1u << span) - 1;
        unsigned low = (c[i] & mask) + 1;
        c[i] = (uint8_t)((c[i] & ~mask) | (low & mask));
        if ((low & ~mask) == 0) return;
        bits -= span;
    }
}

bool subtle_aes_ctr(const uint8_t *key, size_t key_length, const uint8_t counter[SUBTLE_AES_BLOCK],
                    unsigned length_bits, const uint8_t *in, size_t length, uint8_t *out)
{
    size_t blocks = (length + SUBTLE_AES_BLOCK - 1) / SUBTLE_AES_BLOCK;
    if (length_bits < 64 && blocks > ((uint64_t)1 << length_bits)) return false;
    uint8_t c[SUBTLE_AES_BLOCK], stream[SUBTLE_AES_BLOCK];
    for (int i = 0; i < SUBTLE_AES_BLOCK; i++) c[i] = counter[i];
    br_aes_ct64_cbcenc_keys k;
    br_aes_ct64_cbcenc_init(&k, key, key_length);
    for (size_t at = 0; at < length; at += SUBTLE_AES_BLOCK) {
        aes_block(&k, c, stream);
        for (size_t i = 0; i < SUBTLE_AES_BLOCK && at + i < length; i++) out[at + i] = in[at + i] ^ stream[i];
        counter_next(c, length_bits);
    }
    wipe(&k, sizeof(k));
    wipe(stream, sizeof(stream));
    return true;
}

/* One block deciphered: CBC decryption of a single block from a zero IV,
 * which is the block decipher itself, as aes_block is the encipher. */
static void aes_unblock(const br_aes_ct64_cbcdec_keys *k, uint8_t block[SUBTLE_AES_BLOCK])
{
    uint8_t zero[SUBTLE_AES_BLOCK] = {0};
    br_aes_ct64_cbcdec_run(k, zero, block, SUBTLE_AES_BLOCK);
    /* CBC leaves the input block in the IV, and mid-unwrap that block is
     * the scheme's secret state. */
    wipe(zero, sizeof(zero));
}

/* RFC 3394's step counter t = n*j + i, XORed into A's low bytes. */
static void kw_xor_step(uint8_t a[8], uint64_t t)
{
    for (int i = 7; i >= 0 && t != 0; i--, t >>= 8) a[i] ^= (uint8_t)t;
}

static const uint8_t kKwIv[8] = {0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6};

size_t subtle_aes_kw_wrap(const uint8_t *key, size_t key_length, const uint8_t *in, size_t length, uint8_t *out)
{
    if (length < 16 || length % 8 != 0) return 0;
    size_t n = length / 8;
    uint8_t *a = out, *r = out + 8, b[SUBTLE_AES_BLOCK];
    for (int i = 0; i < 8; i++) a[i] = kKwIv[i];
    for (size_t i = 0; i < length; i++) r[i] = in[i];
    br_aes_ct64_cbcenc_keys k;
    br_aes_ct64_cbcenc_init(&k, key, key_length);
    for (uint64_t j = 0; j < 6; j++) {
        for (size_t i = 0; i < n; i++) {
            for (int x = 0; x < 8; x++) {
                b[x] = a[x];
                b[8 + x] = r[8 * i + x];
            }
            aes_block(&k, b, b);
            for (int x = 0; x < 8; x++) {
                a[x] = b[x];
                r[8 * i + x] = b[8 + x];
            }
            kw_xor_step(a, n * j + i + 1);
        }
    }
    wipe(&k, sizeof(k));
    wipe(b, sizeof(b));
    return length + 8;
}

bool subtle_aes_kw_unwrap(const uint8_t *key, size_t key_length, const uint8_t *in, size_t length, uint8_t *out,
                          size_t *out_length)
{
    if (length < 24 || length % 8 != 0) return false;
    size_t n = length / 8 - 1;
    uint8_t a[8], b[SUBTLE_AES_BLOCK];
    for (int i = 0; i < 8; i++) a[i] = in[i];
    for (size_t i = 0; i < 8 * n; i++) out[i] = in[8 + i];
    br_aes_ct64_cbcdec_keys k;
    br_aes_ct64_cbcdec_init(&k, key, key_length);
    for (uint64_t j = 6; j-- > 0;) {
        for (size_t i = n; i-- > 0;) {
            kw_xor_step(a, n * j + i + 1);
            for (int x = 0; x < 8; x++) {
                b[x] = a[x];
                b[8 + x] = out[8 * i + x];
            }
            aes_unblock(&k, b);
            for (int x = 0; x < 8; x++) {
                a[x] = b[x];
                out[8 * i + x] = b[8 + x];
            }
        }
    }
    wipe(&k, sizeof(k));
    wipe(b, sizeof(b));
    bool good = subtle_same_bytes(a, kKwIv, 8);
    if (!good) wipe(out, 8 * n);
    else *out_length = 8 * n;
    return good;
}

static void gcm_start(br_gcm_context *g, br_aes_ct64_ctr_keys *k, const uint8_t *key, size_t key_length,
                      const uint8_t *iv, size_t iv_length, const uint8_t *aad, size_t aad_length)
{
    br_aes_ct64_ctr_init(k, key, key_length);
    br_gcm_init(g, &k->vtable, br_ghash_ctmul64);
    br_gcm_reset(g, SOME(iv), iv_length);
    br_gcm_aad_inject(g, SOME(aad), aad_length);
    br_gcm_flip(g);
}

void subtle_aes_gcm_encrypt(const uint8_t *key, size_t key_length, const uint8_t *iv, size_t iv_length,
                            const uint8_t *aad, size_t aad_length, size_t tag_bytes, const uint8_t *in,
                            size_t length, uint8_t *out)
{
    br_aes_ct64_ctr_keys k;
    br_gcm_context g;
    gcm_start(&g, &k, key, key_length, iv, iv_length, aad, aad_length);
    for (size_t i = 0; i < length; i++) out[i] = in[i];
    br_gcm_run(&g, 1, out, length);
    br_gcm_get_tag_trunc(&g, out + length, tag_bytes);
    wipe(&k, sizeof(k));
    wipe(&g, sizeof(g));
}

bool subtle_aes_gcm_decrypt(const uint8_t *key, size_t key_length, const uint8_t *iv, size_t iv_length,
                            const uint8_t *aad, size_t aad_length, size_t tag_bytes, const uint8_t *in,
                            size_t length, uint8_t *out, size_t *out_length)
{
    if (length < tag_bytes) return false;
    size_t body = length - tag_bytes;
    br_aes_ct64_ctr_keys k;
    br_gcm_context g;
    gcm_start(&g, &k, key, key_length, iv, iv_length, aad, aad_length);
    for (size_t i = 0; i < body; i++) out[i] = in[i];
    br_gcm_run(&g, 0, out, body);
    bool good = br_gcm_check_tag_trunc(&g, in + body, tag_bytes) == 1;
    wipe(&k, sizeof(k));
    wipe(&g, sizeof(g));
    if (!good) {
        wipe(out, body);
        return false;
    }
    *out_length = body;
    return true;
}

static const br_hash_class *hash_class(subtle_alg_t hash)
{
    switch (hash) {
    case SUBTLE_ALG_SHA_1: return &br_sha1_vtable;
    case SUBTLE_ALG_SHA_256: return &br_sha256_vtable;
    case SUBTLE_ALG_SHA_384: return &br_sha384_vtable;
    case SUBTLE_ALG_SHA_512: return &br_sha512_vtable;
    default: return NULL;
    }
}

size_t subtle_hash_block_size(subtle_alg_t hash)
{
    switch (hash) {
    case SUBTLE_ALG_SHA_1:
    case SUBTLE_ALG_SHA_256: return 64;
    case SUBTLE_ALG_SHA_384:
    case SUBTLE_ALG_SHA_512: return 128;
    default: return 0;
    }
}

bool subtle_hmac(subtle_alg_t hash, const uint8_t *key, size_t key_length, const uint8_t *data, size_t length,
                 uint8_t *out)
{
    const br_hash_class *h = hash_class(hash);
    if (h == NULL) return false;
    br_hmac_key_context kc;
    br_hmac_context c;
    br_hmac_key_init(&kc, h, SOME(key), key_length);
    br_hmac_init(&c, &kc, 0);
    br_hmac_update(&c, SOME(data), length);
    br_hmac_out(&c, out);
    wipe(&kc, sizeof(kc));
    wipe(&c, sizeof(c));
    return true;
}

bool subtle_pbkdf2(subtle_alg_t hash, const uint8_t *password, size_t password_length, const uint8_t *salt,
                   size_t salt_length, uint32_t iterations, uint8_t *out, size_t length)
{
    const br_hash_class *h = hash_class(hash);
    if (h == NULL || iterations == 0) return false;
    size_t size = subtle_digest_size(hash);
    br_hmac_key_context kc;
    br_hmac_context c;
    uint8_t u[SUBTLE_DIGEST_MAX], t[SUBTLE_DIGEST_MAX];
    br_hmac_key_init(&kc, h, SOME(password), password_length);
    for (uint32_t block = 1; length > 0; block++) {
        uint8_t index[4] = {(uint8_t)(block >> 24), (uint8_t)(block >> 16), (uint8_t)(block >> 8), (uint8_t)block};
        br_hmac_init(&c, &kc, 0);
        br_hmac_update(&c, SOME(salt), salt_length);
        br_hmac_update(&c, index, sizeof(index));
        br_hmac_out(&c, u);
        for (size_t i = 0; i < size; i++) t[i] = u[i];
        for (uint32_t round = 1; round < iterations; round++) {
            br_hmac_init(&c, &kc, 0);
            br_hmac_update(&c, u, size);
            br_hmac_out(&c, u);
            for (size_t i = 0; i < size; i++) t[i] ^= u[i];
        }
        size_t take = length < size ? length : size;
        for (size_t i = 0; i < take; i++) *out++ = t[i];
        length -= take;
    }
    wipe(&kc, sizeof(kc));
    wipe(&c, sizeof(c));
    wipe(u, sizeof(u));
    wipe(t, sizeof(t));
    return true;
}

bool subtle_hkdf(subtle_alg_t hash, const uint8_t *key, size_t key_length, const uint8_t *salt, size_t salt_length,
                 const uint8_t *info, size_t info_length, uint8_t *out, size_t length)
{
    const br_hash_class *h = hash_class(hash);
    if (h == NULL || length > 255 * subtle_digest_size(hash)) return false;
    br_hkdf_context c;
    br_hkdf_init(&c, h, SOME(salt), salt_length);
    br_hkdf_inject(&c, SOME(key), key_length);
    br_hkdf_flip(&c);
    size_t made = br_hkdf_produce(&c, SOME(info), info_length, out, length);
    wipe(&c, sizeof(c));
    return made == length;
}

bool subtle_same_bytes(const uint8_t *a, const uint8_t *b, size_t length)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < length; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static const char kBase64url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t subtle_base64url_size(size_t length)
{
    return length / 3 * 4 + (length % 3 == 0 ? 0 : length % 3 + 1);
}

size_t subtle_base64url_encode(const uint8_t *in, size_t length, char *out)
{
    size_t n = 0, i = 0;
    for (; i + 3 <= length; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        out[n++] = kBase64url[v >> 18];
        out[n++] = kBase64url[v >> 12 & 63];
        out[n++] = kBase64url[v >> 6 & 63];
        out[n++] = kBase64url[v & 63];
    }
    if (length - i == 1) {
        out[n++] = kBase64url[in[i] >> 2];
        out[n++] = kBase64url[(in[i] & 3) << 4];
    } else if (length - i == 2) {
        uint32_t v = (uint32_t)in[i] << 8 | in[i + 1];
        out[n++] = kBase64url[v >> 10];
        out[n++] = kBase64url[v >> 4 & 63];
        out[n++] = kBase64url[(v & 15) << 2];
    }
    out[n] = '\0';
    return n;
}

static int base64url_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

bool subtle_base64url_decode(const char *text, size_t length, uint8_t *out, size_t *out_length)
{
    if (length % 4 == 1) return false;
    size_t n = 0;
    uint32_t bits = 0;
    unsigned count = 0;
    for (size_t i = 0; i < length; i++) {
        int v = base64url_value(text[i]);
        if (v < 0) return false;
        bits = bits << 6 | (uint32_t)v;
        count += 6;
        if (count >= 8) {
            count -= 8;
            out[n++] = (uint8_t)(bits >> count);
            bits &= (1u << count) - 1;
        }
    }
    if (bits != 0) return false;
    *out_length = n;
    return true;
}

/* ── The elliptic curves ─────────────────────────────────────────────── */

/* Each curve's BearSSL identifier, its name, its size, and the DER of its
 * named-curve object identifier (RFC 5480 § 2.1.1.1). */
static const struct {
    int id;
    const char *name;
    size_t size;
    uint8_t oid[8];
    size_t oid_length;
} kCurves[SUBTLE_CURVE_COUNT] = {
    [SUBTLE_CURVE_P256] = {BR_EC_secp256r1, "P-256", 32, {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07}, 8},
    [SUBTLE_CURVE_P384] = {BR_EC_secp384r1, "P-384", 48, {0x2b, 0x81, 0x04, 0x00, 0x22}, 5},
    [SUBTLE_CURVE_P521] = {BR_EC_secp521r1, "P-521", 66, {0x2b, 0x81, 0x04, 0x00, 0x23}, 5},
};

/* id-ecPublicKey (RFC 5480 § 2.1.1). */
static const uint8_t kEcPublicKey[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01};

/* P-256 has BearSSL's dedicated code; the other two its generic prime
 * curves, the pair libtls uses too. */
static const br_ec_impl *ec_impl(subtle_curve_t curve)
{
    return curve == SUBTLE_CURVE_P256 ? &br_ec_p256_m31 : &br_ec_prime_i31;
}

int subtle_curve_named(const char *name, size_t length)
{
    for (int c = 0; c < SUBTLE_CURVE_COUNT; c++) {
        size_t n = 0;
        while (kCurves[c].name[n] != '\0') n++;
        bool same = n == length;
        for (size_t i = 0; same && i < n; i++) same = name[i] == kCurves[c].name[i];
        if (same) return c;
    }
    return -1;
}

const char *subtle_curve_name(subtle_curve_t curve)
{
    return (unsigned)curve < SUBTLE_CURVE_COUNT ? kCurves[curve].name : "";
}

size_t subtle_ec_size(subtle_curve_t curve)
{
    return (unsigned)curve < SUBTLE_CURVE_COUNT ? kCurves[curve].size : 0;
}

bool subtle_ec_point_ok(subtle_curve_t curve, const uint8_t *point, size_t length)
{
    size_t size = kCurves[curve].size;
    if (length != 1 + 2 * size || point[0] != 0x04) return false;
    /* The point times one: BearSSL's multiply decodes its point and checks
     * it against the curve's equation first, and answers 0 for one off it. */
    uint8_t copy[SUBTLE_EC_POINT_MAX];
    for (size_t i = 0; i < length; i++) copy[i] = point[i];
    static const uint8_t kOne[1] = {1};
    return ec_impl(curve)->mul(copy, length, kOne, 1, kCurves[curve].id) == 1;
}

bool subtle_ec_scalar_ok(subtle_curve_t curve, const uint8_t *d)
{
    size_t size = kCurves[curve].size, order_length;
    const uint8_t *order = ec_impl(curve)->order(kCurves[curve].id, &order_length);
    if (order_length != size) return false;
    /* d < order, and d is not zero, as br_ec_keygen asks of its own draws,
     * in time that does not depend on d. */
    unsigned borrow = 0, any = 0;
    for (size_t i = size; i-- > 0;) {
        borrow = ((unsigned)(d[i] - order[i] - borrow) >> 8) & 1;
        any |= d[i];
    }
    return borrow != 0 && any != 0;
}

void subtle_ec_public(subtle_curve_t curve, const uint8_t *d, uint8_t *point)
{
    ec_impl(curve)->mulgen(point, d, kCurves[curve].size, kCurves[curve].id);
}

bool subtle_ec_generate(subtle_curve_t curve, const uint8_t *seed, size_t seed_length, uint8_t *d)
{
    br_hmac_drbg_context rng;
    br_hmac_drbg_init(&rng, &br_sha256_vtable, seed, seed_length);
    size_t made = br_ec_keygen(&rng.vtable, ec_impl(curve), NULL, d, kCurves[curve].id);
    wipe(&rng, sizeof(rng));
    return made == kCurves[curve].size;
}

size_t subtle_ecdsa_sign(subtle_curve_t curve, subtle_alg_t hash, const uint8_t *d, const uint8_t *data,
                         size_t length, uint8_t *out)
{
    uint8_t digest[SUBTLE_DIGEST_MAX];
    const br_hash_class *hc = hash_class(hash);
    if (hc == NULL || !subtle_digest(hash, data, length, digest)) return 0;
    br_ec_private_key sk = {kCurves[curve].id, (unsigned char *)d, kCurves[curve].size};
    size_t made = br_ecdsa_i31_sign_raw(ec_impl(curve), hc, digest, &sk, out);
    wipe(digest, sizeof(digest));
    return made == 2 * kCurves[curve].size ? made : 0;
}

bool subtle_ecdsa_verify(subtle_curve_t curve, subtle_alg_t hash, const uint8_t *point, const uint8_t *data,
                         size_t length, const uint8_t *signature, size_t signature_length)
{
    size_t size = kCurves[curve].size;
    uint8_t digest[SUBTLE_DIGEST_MAX];
    /* BearSSL reads a shorter signature as one whose halves lost leading
     * zeros; Web Cryptography's is exactly two halves of the curve's size. */
    if (signature_length != 2 * size || !subtle_digest(hash, data, length, digest)) return false;
    br_ec_public_key pk = {kCurves[curve].id, (unsigned char *)point, 1 + 2 * size};
    return br_ecdsa_i31_vrfy_raw(ec_impl(curve), digest, subtle_digest_size(hash), &pk, signature,
                                 signature_length) == 1;
}

bool subtle_ecdh(subtle_curve_t curve, const uint8_t *d, const uint8_t *point, uint8_t *out)
{
    size_t size = kCurves[curve].size, length = 1 + 2 * size;
    uint8_t product[SUBTLE_EC_POINT_MAX];
    for (size_t i = 0; i < length; i++) product[i] = point[i];
    bool made = ec_impl(curve)->mul(product, length, d, size, kCurves[curve].id) == 1;
    for (size_t i = 0; made && i < size; i++) out[i] = product[1 + i];
    wipe(product, sizeof(product));
    return made;
}

/* ── DER, as much of it as a key's two containers need ──────────────────
 * Strict: definite lengths in their shortest form, and nothing after the
 * outermost value, so one key has one encoding. */
typedef struct {
    const uint8_t *p;
    size_t n;
} Der;

/* The next value of `in`, which must have tag `tag`: its contents in *out,
 * `in` moved past it. */
static bool der_take(Der *in, uint8_t tag, Der *out)
{
    if (in->n < 2 || in->p[0] != tag) return false;
    size_t at = 2, length = in->p[1];
    if (length & 0x80) {
        size_t bytes = length & 0x7f;
        /* No indefinite length, nothing past four bytes of one, no leading
         * zero, and no long form for what fits the short one. */
        if (bytes == 0 || bytes > 4 || in->n < 2 + bytes || in->p[2] == 0) return false;
        length = 0;
        for (size_t i = 0; i < bytes; i++) length = length << 8 | in->p[2 + i];
        if (length < 0x80) return false;
        at += bytes;
    }
    if (length > in->n - at) return false;
    out->p = in->p + at;
    out->n = length;
    in->p += at + length;
    in->n -= at + length;
    return true;
}

static bool der_is(Der d, const uint8_t *bytes, size_t length)
{
    if (d.n != length) return false;
    for (size_t i = 0; i < length; i++)
        if (d.p[i] != bytes[i]) return false;
    return true;
}

/* An AlgorithmIdentifier that is id-ecPublicKey with a named curve here. */
static bool der_ec_algorithm(Der *in, subtle_curve_t *curve)
{
    Der alg, oid, named;
    if (!der_take(in, 0x30, &alg) || !der_take(&alg, 0x06, &oid) ||
        !der_is(oid, kEcPublicKey, sizeof(kEcPublicKey)) || !der_take(&alg, 0x06, &named) || alg.n != 0)
        return false;
    for (int c = 0; c < SUBTLE_CURVE_COUNT; c++) {
        if (der_is(named, kCurves[c].oid, kCurves[c].oid_length)) {
            *curve = (subtle_curve_t)c;
            return true;
        }
    }
    return false;
}

/* A BIT STRING of whole bytes: its bytes after the unused-bits count. */
static bool der_bytes_of_bits(Der *in, uint8_t tag, Der *out)
{
    if (!der_take(in, tag, out) || out->n < 1 || out->p[0] != 0) return false;
    out->p++;
    out->n--;
    return true;
}

/* A value's header (tag and length) into `out`; its length. */
static size_t der_header(uint8_t *out, uint8_t tag, size_t length)
{
    out[0] = tag;
    if (length < 0x80) {
        out[1] = (uint8_t)length;
        return 2;
    }
    out[1] = 0x81;
    out[2] = (uint8_t)length;
    return 3;
}

static size_t der_header_size(size_t length)
{
    return length < 0x80 ? 2 : 3;
}

static size_t der_put(uint8_t *out, const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; i++) out[i] = bytes[i];
    return length;
}

/* The AlgorithmIdentifier of `curve`'s keys into `out`; its length. */
static size_t der_put_algorithm(uint8_t *out, subtle_curve_t curve)
{
    size_t body = 2 + sizeof(kEcPublicKey) + 2 + kCurves[curve].oid_length, at = der_header(out, 0x30, body);
    at += der_header(out + at, 0x06, sizeof(kEcPublicKey));
    at += der_put(out + at, kEcPublicKey, sizeof(kEcPublicKey));
    at += der_header(out + at, 0x06, kCurves[curve].oid_length);
    at += der_put(out + at, kCurves[curve].oid, kCurves[curve].oid_length);
    return at;
}

size_t subtle_ec_spki_encode(subtle_curve_t curve, const uint8_t *point, uint8_t *out)
{
    size_t point_length = 1 + 2 * kCurves[curve].size, algorithm = 2 + 2 + sizeof(kEcPublicKey) + 2 +
                                                                    kCurves[curve].oid_length,
           bits = 1 + point_length, body = algorithm + der_header_size(bits) + bits;
    size_t at = der_header(out, 0x30, body);
    at += der_put_algorithm(out + at, curve);
    at += der_header(out + at, 0x03, bits);
    out[at++] = 0;
    at += der_put(out + at, point, point_length);
    return at;
}

bool subtle_ec_spki_decode(const uint8_t *der, size_t length, subtle_curve_t *curve, const uint8_t **point,
                           size_t *point_length)
{
    Der in = {der, length}, spki, key;
    if (!der_take(&in, 0x30, &spki) || in.n != 0 || !der_ec_algorithm(&spki, curve) ||
        !der_bytes_of_bits(&spki, 0x03, &key) || spki.n != 0)
        return false;
    *point = key.p;
    *point_length = key.n;
    return true;
}

size_t subtle_ec_pkcs8_encode(subtle_curve_t curve, const uint8_t *d, const uint8_t *point, uint8_t *out)
{
    size_t size = kCurves[curve].size, point_length = 1 + 2 * size;
    size_t bits = 1 + point_length, tagged = der_header_size(bits) + bits;
    size_t ec_body = 3 + 2 + size + der_header_size(tagged) + tagged;
    size_t ec = der_header_size(ec_body) + ec_body;
    size_t algorithm = 2 + 2 + sizeof(kEcPublicKey) + 2 + kCurves[curve].oid_length;
    size_t body = 3 + algorithm + der_header_size(ec) + ec;
    size_t at = der_header(out, 0x30, body);
    static const uint8_t kVersion0[] = {0x02, 0x01, 0x00}, kVersion1[] = {0x02, 0x01, 0x01};
    at += der_put(out + at, kVersion0, sizeof(kVersion0));
    at += der_put_algorithm(out + at, curve);
    at += der_header(out + at, 0x04, ec);
    at += der_header(out + at, 0x30, ec_body);
    at += der_put(out + at, kVersion1, sizeof(kVersion1));
    at += der_header(out + at, 0x04, size);
    at += der_put(out + at, d, size);
    at += der_header(out + at, 0xa1, tagged);
    at += der_header(out + at, 0x03, bits);
    out[at++] = 0;
    at += der_put(out + at, point, point_length);
    return at;
}

/* An INTEGER whose value is `value` (0 to 127), in its one DER spelling. */
static bool der_small_integer(Der *in, uint8_t value)
{
    Der v;
    return der_take(in, 0x02, &v) && v.n == 1 && v.p[0] == value;
}

bool subtle_ec_pkcs8_decode(const uint8_t *der, size_t length, subtle_curve_t *curve, uint8_t *d,
                            const uint8_t **point, size_t *point_length)
{
    Der in = {der, length}, info, wrapped, ec, scalar, holder;
    if (!der_take(&in, 0x30, &info) || in.n != 0) return false;
    /* RFC 5208's PrivateKeyInfo, the spec's structure: version 0 (RFC
     * 5958's version 1, with its own public key field, is another one), and
     * after the key its optional [0] attributes, which say nothing of it. */
    Der skipped;
    if (!der_small_integer(&info, 0) || !der_ec_algorithm(&info, curve) || !der_take(&info, 0x04, &wrapped))
        return false;
    if (info.n != 0 && info.p[0] == 0xa0 && !der_take(&info, 0xa0, &skipped)) return false;
    if (info.n != 0) return false;
    /* The ECPrivateKey (RFC 5915 § 3), the whole of the octet string. */
    if (!der_take(&wrapped, 0x30, &ec) || wrapped.n != 0 || !der_small_integer(&ec, 1) ||
        !der_take(&ec, 0x04, &scalar))
        return false;
    size_t size = kCurves[*curve].size;
    /* RFC 5915 fixes d's length at the curve's size; an encoder that wrote
     * more leading zeros wrote the same number, and BoringSSL reads it so. */
    while (scalar.n > size && scalar.p[0] == 0) {
        scalar.p++;
        scalar.n--;
    }
    if (scalar.n == 0 || scalar.n > size) return false;
    for (size_t i = 0; i < size - scalar.n; i++) d[i] = 0;
    for (size_t i = 0; i < scalar.n; i++) d[size - scalar.n + i] = scalar.p[i];
    *point = NULL;
    *point_length = 0;
    if (ec.n != 0 && ec.p[0] == 0xa0) {
        Der named;
        if (!der_take(&ec, 0xa0, &holder) || !der_take(&holder, 0x06, &named) || holder.n != 0 ||
            !der_is(named, kCurves[*curve].oid, kCurves[*curve].oid_length))
            goto refuse;
    }
    if (ec.n != 0 && ec.p[0] == 0xa1) {
        Der key;
        if (!der_take(&ec, 0xa1, &holder) || !der_bytes_of_bits(&holder, 0x03, &key) || holder.n != 0) goto refuse;
        *point = key.p;
        *point_length = key.n;
    }
    if (ec.n == 0) return true;
refuse:
    wipe(d, size);
    return false;
}
