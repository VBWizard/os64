#ifndef LIBDOM_SUBTLE_CORE_H
#define LIBDOM_SUBTLE_CORE_H
/* Web Cryptography's algorithms, without the engine (WEB_CRYPTO.md § 4):
 * which algorithm each operation registers under which name, and the
 * operations themselves over BearSSL. Pure: no engine, no allocation of its
 * own, so tools/test_subtle_core_host.sh holds it to the standards' vectors
 * under ASan. The binding (subtle.c) converts a page's arguments, asks here,
 * and makes the page's objects of the answer. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The operations an algorithm registers for (Web Cryptography § 18.4.1,
 * "supportedAlgorithms"). exportKey normalizes nothing, and deriveKey is
 * deriveBits, importKey and "get key length" together. */
typedef enum {
    SUBTLE_OP_ENCRYPT,
    SUBTLE_OP_DECRYPT,
    SUBTLE_OP_SIGN,
    SUBTLE_OP_VERIFY,
    SUBTLE_OP_DIGEST,
    SUBTLE_OP_GENERATE_KEY,
    SUBTLE_OP_DERIVE_BITS,
    SUBTLE_OP_IMPORT_KEY,
    SUBTLE_OP_WRAP_KEY,
    SUBTLE_OP_UNWRAP_KEY,
    SUBTLE_OP_GET_KEY_LENGTH,
    SUBTLE_OP_COUNT
} subtle_op_t;

typedef enum {
    SUBTLE_ALG_SHA_1,
    SUBTLE_ALG_SHA_256,
    SUBTLE_ALG_SHA_384,
    SUBTLE_ALG_SHA_512,
    SUBTLE_ALG_AES_CTR,
    SUBTLE_ALG_AES_CBC,
    SUBTLE_ALG_AES_GCM,
    SUBTLE_ALG_HMAC,
    SUBTLE_ALG_COUNT
} subtle_alg_t;

/* The IDL dictionary an algorithm's parameters convert to for an operation
 * (normalization's "desired type"). */
typedef enum {
    SUBTLE_DICT_ALGORITHM,
    SUBTLE_DICT_AES_KEY_GEN,    /* AesKeyGenParams: length */
    SUBTLE_DICT_AES_CTR,        /* AesCtrParams: counter, length */
    SUBTLE_DICT_AES_CBC,        /* AesCbcParams: iv */
    SUBTLE_DICT_AES_GCM,        /* AesGcmParams: additionalData, iv, tagLength */
    SUBTLE_DICT_HMAC_KEY_GEN,   /* HmacKeyGenParams: hash, length */
    SUBTLE_DICT_HMAC_IMPORT,    /* HmacImportParams: hash, length */
} subtle_dict_t;

/* The algorithm registered for `op` whose name matches `name` (`length`
 * bytes, UTF-8, which may hold a NUL) ASCII case-insensitively, as
 * normalization matches (§ 18.4.4 step 4): `*dict` its dictionary. -1 when
 * none is, which the binding answers with NotSupportedError. */
int subtle_registered(subtle_op_t op, const char *name, size_t length, subtle_dict_t *dict);

/* The algorithm's registered spelling, which a normalized algorithm's name
 * becomes ("sha-256" is "SHA-256"). */
const char *subtle_alg_name(subtle_alg_t alg);

/* The most bytes a digest is (SHA-512's). */
#define SUBTLE_DIGEST_MAX 64

/* A hash's output length in bytes; 0 for an algorithm that is no hash. */
size_t subtle_digest_size(subtle_alg_t alg);

/* `length` bytes of `data` hashed by `alg` into `out`, which holds
 * subtle_digest_size(alg) bytes; false for an algorithm that is no hash. */
bool subtle_digest(subtle_alg_t alg, const uint8_t *data, size_t length, uint8_t *out);

/* ── base64url (RFC 4648 § 5), as JWK writes key bytes ──────────────────
 * Unpadded, as JWK requires. */

/* The encoding of `length` bytes into `out`, which holds
 * subtle_base64url_size(length) + 1 bytes (a NUL ends it); its length. */
size_t subtle_base64url_size(size_t length);
size_t subtle_base64url_encode(const uint8_t *in, size_t length, char *out);
/* `text` (`length` characters) decoded into `out`, which holds length * 3
 * / 4 bytes; false for a character outside the URL alphabet (padding
 * included), a length no encoding has, or bits past the last byte that
 * are not zero. */
bool subtle_base64url_decode(const char *text, size_t length, uint8_t *out, size_t *out_length);

/* ── AES (FIPS 197) under Web Cryptography's three modes ────────────────
 * A key is 16, 24 or 32 bytes. Each answers into `out`, which holds what
 * the comment says, and may not overlap `in`. The key schedules live on
 * the stack and are wiped before return. */
#define SUBTLE_AES_BLOCK 16
bool subtle_aes_key_length_ok(size_t length);

/* AES-CBC with PKCS#7 padding (Web Cryptography § 28): `out` holds
 * length + 16 bytes; the ciphertext's length. */
size_t subtle_aes_cbc_encrypt(const uint8_t *key, size_t key_length, const uint8_t iv[SUBTLE_AES_BLOCK],
                              const uint8_t *in, size_t length, uint8_t *out);
/* The plaintext into `out` (length bytes) and its length; false for
 * ciphertext that is empty or not a whole number of blocks, or whose
 * padding is not PKCS#7's. */
bool subtle_aes_cbc_decrypt(const uint8_t *key, size_t key_length, const uint8_t iv[SUBTLE_AES_BLOCK],
                            const uint8_t *in, size_t length, uint8_t *out, size_t *out_length);

/* AES-CTR (§ 27): the keystream is the cipher of the counter block, whose
 * low `length_bits` bits (1 to 128) count up by one a block and wrap
 * within themselves, the high bits fixed. `out` holds length bytes. False
 * when the data is longer than the counter can count without repeating a
 * block, which would repeat keystream. */
bool subtle_aes_ctr(const uint8_t *key, size_t key_length, const uint8_t counter[SUBTLE_AES_BLOCK],
                    unsigned length_bits, const uint8_t *in, size_t length, uint8_t *out);

/* AES-GCM (§ 29): `tag_bytes` of the tag (4, 8, 12 to 16) appended to the
 * ciphertext, so `out` holds length + tag_bytes. */
void subtle_aes_gcm_encrypt(const uint8_t *key, size_t key_length, const uint8_t *iv, size_t iv_length,
                            const uint8_t *aad, size_t aad_length, size_t tag_bytes, const uint8_t *in,
                            size_t length, uint8_t *out);
/* The plaintext into `out` (length - tag_bytes bytes) and its length;
 * false when the input is shorter than the tag or the tag does not match,
 * in which case `out` holds nothing a caller may use. */
bool subtle_aes_gcm_decrypt(const uint8_t *key, size_t key_length, const uint8_t *iv, size_t iv_length,
                            const uint8_t *aad, size_t aad_length, size_t tag_bytes, const uint8_t *in,
                            size_t length, uint8_t *out, size_t *out_length);

/* ── HMAC (RFC 2104) ──────────────────────────────────────────────────── */

/* A hash's block size in bytes (an HMAC key's default length); 0 for an
 * algorithm that is no hash. */
size_t subtle_hash_block_size(subtle_alg_t hash);

/* The MAC of `data` under `key` with `hash` into `out`, which holds
 * subtle_digest_size(hash) bytes; false for an algorithm that is no hash. */
bool subtle_hmac(subtle_alg_t hash, const uint8_t *key, size_t key_length, const uint8_t *data, size_t length,
                 uint8_t *out);

/* Whether `a` and `b` hold the same `length` bytes, in time that depends on
 * the length alone (a MAC's comparison). */
bool subtle_same_bytes(const uint8_t *a, const uint8_t *b, size_t length);

#endif
