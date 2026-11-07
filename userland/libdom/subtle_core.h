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
    SUBTLE_ALG_COUNT
} subtle_alg_t;

/* The IDL dictionary an algorithm's parameters convert to for an operation
 * (normalization's "desired type"). */
typedef enum {
    SUBTLE_DICT_ALGORITHM,
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

#endif
