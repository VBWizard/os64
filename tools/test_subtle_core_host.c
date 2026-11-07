/* libdom's Web Cryptography core (userland/libdom/subtle_core.c) held to
 * the standards: FIPS 180-2's example messages for every digest (the empty
 * message, "abc", the 448-bit message and a million "a"), and the registry
 * matching names as normalization does. Run by
 * tools/test_subtle_core_host.sh, under ASan and UBSan. */
#include "../userland/libdom/subtle_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static void hex(const uint8_t *bytes, size_t length, char *out)
{
    for (size_t i = 0; i < length; i++) sprintf(out + 2 * i, "%02x", bytes[i]);
    out[2 * length] = '\0';
}

static void digest_case(subtle_alg_t alg, const uint8_t *data, size_t length, const char *want, const char *what)
{
    uint8_t out[SUBTLE_DIGEST_MAX];
    char got[2 * SUBTLE_DIGEST_MAX + 1];
    size_t size = subtle_digest_size(alg);
    check(size * 2 == strlen(want), what);
    check(subtle_digest(alg, data, length, out), what);
    hex(out, size, got);
    check(strcmp(got, want) == 0, what);
    if (strcmp(got, want) != 0) printf("  got  %s\n  want %s\n", got, want);
}

int main(void)
{
    static const struct {
        subtle_alg_t alg;
        const char *empty, *abc, *m448, *million;
    } kVectors[] = {
        {SUBTLE_ALG_SHA_1, "da39a3ee5e6b4b0d3255bfef95601890afd80709", "a9993e364706816aba3e25717850c26c9cd0d89d",
         "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "34aa973cd4c4daa4f61eeb2bdbad27316534016f"},
        {SUBTLE_ALG_SHA_256, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
         "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
        {SUBTLE_ALG_SHA_384,
         "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b",
         "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7",
         "3391fdddfc8dc7393707a65b1b4709397cf8b1d162af05abfe8f450de5f36bc6b0455a8520bc4e6f5fe95b1fe3c8452b",
         "9d0e1809716474cb086e834e310a4a1ced149e9c00f248527972cec5704c2a5b07b8b3dc38ecc4ebae97ddd87f3d8985"},
        {SUBTLE_ALG_SHA_512,
         "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
         "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
         "204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c33596fd15c13b1b07f9aa1d3bea57789ca031ad85c7a71dd70354ec631238ca3445",
         "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b"},
    };
    static const char m448[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uint8_t *million = malloc(1000000);
    if (million == NULL) return 2;
    memset(million, 'a', 1000000);
    for (size_t i = 0; i < sizeof(kVectors) / sizeof(kVectors[0]); i++) {
        char what[96];
        const char *name = subtle_alg_name(kVectors[i].alg);
        snprintf(what, sizeof(what), "%s of the empty message", name);
        digest_case(kVectors[i].alg, (const uint8_t *)"", 0, kVectors[i].empty, what);
        snprintf(what, sizeof(what), "%s of \"abc\"", name);
        digest_case(kVectors[i].alg, (const uint8_t *)"abc", 3, kVectors[i].abc, what);
        snprintf(what, sizeof(what), "%s of the 448-bit message", name);
        digest_case(kVectors[i].alg, (const uint8_t *)m448, sizeof(m448) - 1, kVectors[i].m448, what);
        snprintf(what, sizeof(what), "%s of a million \"a\"", name);
        digest_case(kVectors[i].alg, million, 1000000, kVectors[i].million, what);
    }
    free(million);

    /* The registry: names match ASCII case-insensitively and exactly. */
    subtle_dict_t dict;
    check(subtle_registered(SUBTLE_OP_DIGEST, "SHA-256", 7, &dict) == SUBTLE_ALG_SHA_256 &&
              dict == SUBTLE_DICT_ALGORITHM, "SHA-256 is digest's");
    check(subtle_registered(SUBTLE_OP_DIGEST, "sha-1", 5, &dict) == SUBTLE_ALG_SHA_1 &&
              subtle_registered(SUBTLE_OP_DIGEST, "Sha-384", 7, &dict) == SUBTLE_ALG_SHA_384 &&
              subtle_registered(SUBTLE_OP_DIGEST, "sHa-512", 7, &dict) == SUBTLE_ALG_SHA_512,
          "any ASCII case matches");
    check(subtle_registered(SUBTLE_OP_DIGEST, "SHA-25", 6, &dict) < 0 &&
              subtle_registered(SUBTLE_OP_DIGEST, "SHA-2560", 8, &dict) < 0 &&
              subtle_registered(SUBTLE_OP_DIGEST, "SHA256", 6, &dict) < 0 &&
              subtle_registered(SUBTLE_OP_DIGEST, "", 0, &dict) < 0,
          "a name matches whole or not at all");
    check(subtle_registered(SUBTLE_OP_DIGEST, "SHA-256\0", 8, &dict) < 0, "a NUL is part of the name");
    check(subtle_registered(SUBTLE_OP_DIGEST, "SHA-\xc5\xbf" "56", 8, &dict) < 0,
          "only ASCII folds (U+017F is not s, nor anything here)");
    check(subtle_registered(SUBTLE_OP_ENCRYPT, "SHA-256", 7, &dict) < 0 &&
              subtle_registered(SUBTLE_OP_IMPORT_KEY, "SHA-256", 7, &dict) < 0,
          "a hash is no other operation's");
    for (int op = 0; op < SUBTLE_OP_COUNT; op++) {
        if (op == SUBTLE_OP_DIGEST) continue;
        check(subtle_registered((subtle_op_t)op, "AES-GCM", 7, &dict) < 0, "nothing beyond digest is registered");
    }
    check(subtle_digest_size(SUBTLE_ALG_COUNT) == 0 && !subtle_digest(SUBTLE_ALG_COUNT, NULL, 0, NULL),
          "what is no hash digests nothing");
    check(strcmp(subtle_alg_name(SUBTLE_ALG_SHA_384), "SHA-384") == 0, "the registered spelling");

    printf("subtle core: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
