#include "subtle_core.h"

#include "bearssl_hash.h"

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
};

static const SubtleRegistration kRegistry[] = {
    {SUBTLE_OP_DIGEST, SUBTLE_ALG_SHA_1, SUBTLE_DICT_ALGORITHM},
    {SUBTLE_OP_DIGEST, SUBTLE_ALG_SHA_256, SUBTLE_DICT_ALGORITHM},
    {SUBTLE_OP_DIGEST, SUBTLE_ALG_SHA_384, SUBTLE_DICT_ALGORITHM},
    {SUBTLE_OP_DIGEST, SUBTLE_ALG_SHA_512, SUBTLE_DICT_ALGORITHM},
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
        br_sha1_update(&c, data, length);
        br_sha1_out(&c, out);
        return true;
    }
    case SUBTLE_ALG_SHA_256: {
        br_sha256_context c;
        br_sha256_init(&c);
        br_sha256_update(&c, data, length);
        br_sha256_out(&c, out);
        return true;
    }
    case SUBTLE_ALG_SHA_384: {
        br_sha384_context c;
        br_sha384_init(&c);
        br_sha384_update(&c, data, length);
        br_sha384_out(&c, out);
        return true;
    }
    case SUBTLE_ALG_SHA_512: {
        br_sha512_context c;
        br_sha512_init(&c);
        br_sha512_update(&c, data, length);
        br_sha512_out(&c, out);
        return true;
    }
    default: return false;
    }
}
