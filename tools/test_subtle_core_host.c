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

static size_t unhex(const char *text, uint8_t *out)
{
    size_t n = 0;
    for (; text[0] && text[1]; text += 2) {
        unsigned v;
        sscanf(text, "%2x", &v);
        out[n++] = (uint8_t)v;
    }
    return n;
}

static void same_hex(const uint8_t *got, size_t length, const char *want, const char *what)
{
    char text[512];
    hex(got, length, text);
    check(strcmp(text, want) == 0, what);
    if (strcmp(text, want) != 0) printf("  got  %s\n  want %s\n", text, want);
}

/* NIST SP 800-38A's CBC (F.2.1) and CTR (F.5.1) examples, the original GCM
 * paper's test case 4 (and the same with an 8-byte IV), and the failures
 * each mode must refuse. CBC's ciphertext is the NIST blocks and one block
 * of padding; the counters of 8, 4 and 16 bits and the 8-byte GCM IV are
 * values node's crypto computed (no standard gives them). */
static void aes_cases(void)
{
    uint8_t key[32], iv[16], pt[64], ct[96], back[96], counter[16];
    size_t n = 0;
    unhex("2b7e151628aed2a6abf7158809cf4f3c", key);
    unhex("000102030405060708090a0b0c0d0e0f", iv);
    unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52ef"
          "f69f2445df4f9b17ad2b417be66c3710", pt);
    check(subtle_aes_key_length_ok(16) && subtle_aes_key_length_ok(24) && subtle_aes_key_length_ok(32) &&
              !subtle_aes_key_length_ok(0) && !subtle_aes_key_length_ok(20), "AES keys are 128, 192 or 256 bits");
    size_t length = subtle_aes_cbc_encrypt(key, 16, iv, pt, 64, ct);
    check(length == 80, "CBC pads a whole block onto whole blocks");
    same_hex(ct, length, "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b273bed6b8e3c1743b7116e69e22229516"
             "3ff1caa1681fac09120eca307586e1a78cb82807230e1321d3fae00d18cc2012", "AES-128-CBC, SP 800-38A F.2.1");
    check(subtle_aes_cbc_decrypt(key, 16, iv, ct, length, back, &n) && n == 64 && memcmp(back, pt, 64) == 0,
          "CBC decrypts and unpads");
    uint8_t k256[32];
    unhex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", k256);
    length = subtle_aes_cbc_encrypt(k256, 32, iv, (const uint8_t *)"abc", 3, ct);
    same_hex(ct, length, "83286afe49594ce405a251bdb203750f", "AES-256-CBC pads a short message");
    uint8_t bad[96];
    memcpy(bad, ct, 16);
    bad[15] ^= 1;
    check(!subtle_aes_cbc_decrypt(k256, 32, iv, bad, 16, back, &n), "CBC refuses a bad pad");
    check(!subtle_aes_cbc_decrypt(k256, 32, iv, ct, 15, back, &n) && !subtle_aes_cbc_decrypt(k256, 32, iv, ct, 0, back, &n),
          "CBC refuses a ragged or empty ciphertext");

    unhex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", counter);
    check(subtle_aes_ctr(key, 16, counter, 128, pt, 64, ct), "CTR runs");
    same_hex(ct, 64, "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff5ae4df3edbd5d35e5b4f09020db03eab"
             "1e031dda2fbe03d1792170a0f3009cee", "AES-128-CTR, SP 800-38A F.5.1");
    check(subtle_aes_ctr(key, 16, counter, 128, ct, 64, back) && memcmp(back, pt, 64) == 0, "CTR is its own inverse");
    unhex("000102030405060708090a0b0c0d0eff", counter);
    check(subtle_aes_ctr(key, 16, counter, 8, pt, 32, ct), "CTR at an 8-bit counter");
    same_hex(ct, 32, "ab930eb6a1f9b307c8633228ae164c15cc73f9de397d6cabbe04204b6d68a45e",
             "the counter wraps within its 8 bits, the high bits fixed");
    check(subtle_aes_ctr(key, 16, counter, 1, pt, 32, ct) && !subtle_aes_ctr(key, 16, counter, 1, pt, 33, ct),
          "a 1-bit counter counts two blocks and no more");
    unhex("000102030405060708090a0b0c0d0e3f", counter);
    check(subtle_aes_ctr(key, 16, counter, 4, pt, 32, ct), "CTR at a 4-bit counter");
    same_hex(ct, 32, "efcd954843393238905a690ffc030276a04cdfcc096446de24dff16ad2c606e3",
             "a counter that ends inside a byte wraps there, the byte's high bits fixed");
    unhex("000102030405060708090a0b0c0d00ff", counter);
    check(subtle_aes_ctr(key, 16, counter, 16, pt, 32, ct), "CTR at a 16-bit counter");
    same_hex(ct, 32, "60d38862c92d3a0349f105cfbd1c6ded4a610b72fb66ed2a506d913da72d1966",
             "a counter carries from byte to byte within its bits");

    uint8_t gk[16], giv[12], aad[20], gpt[60];
    unhex("feffe9928665731c6d6a8f9467308308", gk);
    unhex("cafebabefacedbaddecaf888", giv);
    unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad);
    unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525"
          "b16aedf5aa0de657ba637b39", gpt);
    subtle_aes_gcm_encrypt(gk, 16, giv, 12, aad, 20, 16, gpt, 60, ct);
    same_hex(ct, 76, "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa05"
             "1ba30b396a0aac973d58e0915bc94fbc3221a5db94fae95ae7121a47", "AES-128-GCM, the GCM paper's test case 4");
    check(subtle_aes_gcm_decrypt(gk, 16, giv, 12, aad, 20, 16, ct, 76, back, &n) && n == 60 && memcmp(back, gpt, 60) == 0,
          "GCM decrypts");
    subtle_aes_gcm_encrypt(gk, 16, giv, 12, aad, 20, 12, gpt, 60, bad);
    check(memcmp(bad + 60, ct + 60, 12) == 0, "a 96-bit tag is the tag's first 12 bytes");
    check(subtle_aes_gcm_decrypt(gk, 16, giv, 12, aad, 20, 12, bad, 72, back, &n) && n == 60, "a truncated tag checks");
    ct[3] ^= 1;
    check(!subtle_aes_gcm_decrypt(gk, 16, giv, 12, aad, 20, 16, ct, 76, back, &n), "GCM refuses altered ciphertext");
    ct[3] ^= 1;
    ct[75] ^= 1;
    check(!subtle_aes_gcm_decrypt(gk, 16, giv, 12, aad, 20, 16, ct, 76, back, &n), "GCM refuses an altered tag");
    check(!subtle_aes_gcm_decrypt(gk, 16, giv, 12, aad, 20, 16, ct, 15, back, &n), "GCM refuses input shorter than its tag");
    subtle_aes_gcm_encrypt(gk, 16, giv, 8, aad, 20, 16, gpt, 60, ct);
    same_hex(ct + 60, 16, "3612d2e79e3b0785561be14aaca2fccb", "GCM with an 8-byte IV (GHASH-derived counter)");
}

/* RFC 4231's test case 1 for SHA-256, -384 and -512 and RFC 2202's for
 * SHA-1: a 20-byte key of 0x0b, "Hi There". */
static void hmac_cases(void)
{
    static const struct { subtle_alg_t alg; const char *mac; } kMacs[] = {
        {SUBTLE_ALG_SHA_1, "b617318655057264e28bc0b6fb378c8ef146be00"},
        {SUBTLE_ALG_SHA_256, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"},
        {SUBTLE_ALG_SHA_384, "afd03944d84895626b0825f4ab46907f15f9dadbe4101ec682aa034c7cebc59cfaea9ea9076ede7f4af152e8b2fa9cb6"},
        {SUBTLE_ALG_SHA_512, "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cdedaa833b7d6b8a702038b274eaea3f4e4"
                             "be9d914eeb61f1702e696c203a126854"},
    };
    uint8_t key[20], out[SUBTLE_DIGEST_MAX];
    memset(key, 0x0b, sizeof(key));
    for (size_t i = 0; i < sizeof(kMacs) / sizeof(kMacs[0]); i++) {
        char what[64];
        snprintf(what, sizeof(what), "HMAC-%s, RFC 4231 / 2202 case 1", subtle_alg_name(kMacs[i].alg));
        check(subtle_hmac(kMacs[i].alg, key, 20, (const uint8_t *)"Hi There", 8, out), what);
        same_hex(out, subtle_digest_size(kMacs[i].alg), kMacs[i].mac, what);
    }
    check(subtle_hash_block_size(SUBTLE_ALG_SHA_1) == 64 && subtle_hash_block_size(SUBTLE_ALG_SHA_256) == 64 &&
              subtle_hash_block_size(SUBTLE_ALG_SHA_384) == 128 && subtle_hash_block_size(SUBTLE_ALG_SHA_512) == 128,
          "the hashes' block sizes");
    check(!subtle_hmac(SUBTLE_ALG_COUNT, key, 20, key, 1, out), "what is no hash makes no MAC");
    check(subtle_same_bytes(key, key, 20) && !subtle_same_bytes((const uint8_t *)"ab", (const uint8_t *)"ac", 2),
          "the constant-time comparison");
}

/* RFC 4648 § 10's vectors, unpadded, and the URL alphabet's two letters. */
static void base64url_cases(void)
{
    static const char *const kText[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    static const char *const kCode[] = {"", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy"};
    char code[16];
    uint8_t back[16];
    size_t n;
    for (int i = 0; i < 7; i++) {
        size_t length = strlen(kText[i]);
        check(subtle_base64url_size(length) == strlen(kCode[i]) &&
                  subtle_base64url_encode((const uint8_t *)kText[i], length, code) == strlen(kCode[i]) &&
                  strcmp(code, kCode[i]) == 0,
              "base64url encodes RFC 4648's vectors, unpadded");
        check(subtle_base64url_decode(kCode[i], strlen(kCode[i]), back, &n) && n == length &&
                  memcmp(back, kText[i], length) == 0,
              "and decodes them");
    }
    static const uint8_t kHigh[] = {0xfb, 0xff};
    subtle_base64url_encode(kHigh, 2, code);
    check(strcmp(code, "-_8") == 0, "62 and 63 are - and _");
    check(!subtle_base64url_decode("Zg==", 4, back, &n) && !subtle_base64url_decode("+/8", 3, back, &n) &&
              !subtle_base64url_decode("Z", 1, back, &n) && !subtle_base64url_decode("Zh", 2, back, &n) &&
              !subtle_base64url_decode("Zm 9", 4, back, &n),
          "padding, the standard alphabet, a lone character, stray bits and spaces are refused");
}

/* RFC 3394 § 4.1-4.6: every KEK size wrapping every key size; then what the
 * scheme refuses. */
static void aes_kw_cases(void)
{
    static const struct { const char *kek, *data, *wrapped, *what; } kVectors[] = {
        {"000102030405060708090A0B0C0D0E0F", "00112233445566778899AABBCCDDEEFF",
         "1FA68B0A8112B447AEF34BD8FB5A7B829D3E862371D2CFE5", "RFC 3394 4.1, 128 bits under a 128-bit KEK"},
        {"000102030405060708090A0B0C0D0E0F1011121314151617", "00112233445566778899AABBCCDDEEFF",
         "96778B25AE6CA435F92B5B97C050AED2468AB8A17AD84E5D", "RFC 3394 4.2, 128 bits under a 192-bit KEK"},
        {"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", "00112233445566778899AABBCCDDEEFF",
         "64E8C3F9CE0F5BA263E9777905818A2A93C8191E7D6E8AE7", "RFC 3394 4.3, 128 bits under a 256-bit KEK"},
        {"000102030405060708090A0B0C0D0E0F1011121314151617", "00112233445566778899AABBCCDDEEFF0001020304050607",
         "031D33264E15D33268F24EC260743EDCE1C6C7DDEE725A936BA814915C6762D2", "RFC 3394 4.4, 192 bits under a 192-bit KEK"},
        {"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F",
         "00112233445566778899AABBCCDDEEFF0001020304050607",
         "A8F9BC1612C68B3FF6E6F4FBE30E71E4769C8B80A32CB8958CD5D17D6B254DA1", "RFC 3394 4.5, 192 bits under a 256-bit KEK"},
        {"000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F",
         "00112233445566778899AABBCCDDEEFF000102030405060708090A0B0C0D0E0F",
         "28C9F404C4B810F4CBCCB35CFB87F8263F5786E2D80ED326CBC7F0E71A99F43BFB988B9B7A02DD21",
         "RFC 3394 4.6, 256 bits under a 256-bit KEK"},
    };
    for (size_t v = 0; v < sizeof(kVectors) / sizeof(kVectors[0]); v++) {
        uint8_t kek[32], data[32], out[40], back[40], want[40];
        size_t kek_length = unhex(kVectors[v].kek, kek), length = unhex(kVectors[v].data, data), n = 0;
        unhex(kVectors[v].wrapped, want);
        size_t wrapped = subtle_aes_kw_wrap(kek, kek_length, data, length, out);
        check(wrapped == length + 8, kVectors[v].what);
        char hexed[96], expected[96];
        hex(out, wrapped, hexed);
        hex(want, length + 8, expected);
        check(strcmp(hexed, expected) == 0, kVectors[v].what);
        check(subtle_aes_kw_unwrap(kek, kek_length, out, wrapped, back, &n) && n == length && memcmp(back, data, n) == 0,
              "and unwraps to the key data");
        out[wrapped - 1] ^= 1;
        check(!subtle_aes_kw_unwrap(kek, kek_length, out, wrapped, back, &n), "an altered last byte fails the check");
    }
    /* 512 bytes, so the step counter t = n*j + i reaches past one byte (it
     * does from n = 43): wrap and unwrap share kw_xor_step, so a slip in its
     * high bytes would round-trip through itself and pass every vector
     * above. node's WebCrypto wrapped the same bytes; its SHA-256 is here. */
    uint8_t big_kek[16], big[512], big_out[520], big_back[520], sum[32];
    unhex("000102030405060708090a0b0c0d0e0f", big_kek);
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)i;
    size_t big_n = 0;
    check(subtle_aes_kw_wrap(big_kek, 16, big, sizeof(big), big_out) == sizeof(big_out), "AES-KW wraps 512 bytes");
    check(subtle_digest(SUBTLE_ALG_SHA_256, big_out, sizeof(big_out), sum), "and its SHA-256");
    same_hex(sum, 32, "fcfa100fa59ec6859a9f6fcfe314c286c84417d87d112bd3497cfd6318ad2988",
             "AES-KW past a one-byte step counter, as node wraps it");
    check(subtle_aes_kw_unwrap(big_kek, 16, big_out, sizeof(big_out), big_back, &big_n) && big_n == sizeof(big) &&
              memcmp(big_back, big, sizeof(big)) == 0,
          "and unwraps back");
    uint8_t kek[16] = {0}, data[24] = {0}, out[40], back[40];
    size_t n = 0;
    check(subtle_aes_kw_wrap(kek, 16, data, 8, out) == 0 && subtle_aes_kw_wrap(kek, 16, data, 0, out) == 0 &&
              subtle_aes_kw_wrap(kek, 16, data, 20, out) == 0,
          "wrap refuses one block, none, and a ragged length (RFC 3394's floor of two blocks)");
    check(!subtle_aes_kw_unwrap(kek, 16, data, 16, back, &n) && !subtle_aes_kw_unwrap(kek, 16, data, 23, back, &n) &&
              !subtle_aes_kw_unwrap(kek, 16, data, 0, back, &n),
          "unwrap refuses two blocks, a ragged length, and none");
}

/* RFC 5869 A.1-A.3 (HKDF-SHA-256), RFC 6070's PBKDF2-HMAC-SHA1 cases with
 * at most 4096 iterations, and each derivation's limits. */
static void kdf_cases(void)
{
    uint8_t ikm[80], salt[80], info[80], out[130];
    size_t ikm_length = unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", ikm);
    size_t salt_length = unhex("000102030405060708090a0b0c", salt);
    size_t info_length = unhex("f0f1f2f3f4f5f6f7f8f9", info);
    check(subtle_hkdf(SUBTLE_ALG_SHA_256, ikm, ikm_length, salt, salt_length, info, info_length, out, 42), "HKDF runs");
    same_hex(out, 42, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
             "HKDF, RFC 5869 A.1");
    ikm_length = unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
                       "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
                       "404142434445464748494a4b4c4d4e4f", ikm);
    salt_length = unhex("606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f"
                        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
                        "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf", salt);
    info_length = unhex("b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
                        "d0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeef"
                        "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", info);
    check(subtle_hkdf(SUBTLE_ALG_SHA_256, ikm, ikm_length, salt, salt_length, info, info_length, out, 82), "HKDF runs");
    same_hex(out, 82,
             "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09"
             "da3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87",
             "HKDF, RFC 5869 A.2 (longer inputs and output)");
    ikm_length = unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", ikm);
    check(subtle_hkdf(SUBTLE_ALG_SHA_256, ikm, ikm_length, NULL, 0, NULL, 0, out, 42), "HKDF runs");
    same_hex(out, 42, "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8",
             "HKDF, RFC 5869 A.3 (no salt, no info)");
    uint8_t *big = malloc(255 * 64 + 1);
    check(big != NULL && subtle_hkdf(SUBTLE_ALG_SHA_512, ikm, ikm_length, NULL, 0, NULL, 0, big, 255 * 64) &&
              !subtle_hkdf(SUBTLE_ALG_SHA_512, ikm, ikm_length, NULL, 0, NULL, 0, big, 255 * 64 + 1),
          "HKDF makes 255 hashes' worth and no more");
    free(big);
    check(!subtle_hkdf(SUBTLE_ALG_HMAC, ikm, 1, NULL, 0, NULL, 0, out, 1), "what is no hash makes no HKDF");

    static const struct { const char *password; size_t password_length; const char *salt; size_t salt_length;
                          uint32_t c; size_t length; const char *dk, *what; } kPbkdf2[] = {
        {"password", 8, "salt", 4, 1, 20, "0c60c80f961f0e71f3a9b524af6012062fe037a6", "PBKDF2, RFC 6070 c=1"},
        {"password", 8, "salt", 4, 2, 20, "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957", "PBKDF2, RFC 6070 c=2"},
        {"password", 8, "salt", 4, 4096, 20, "4b007901b765489abead49d926f721d065a429c1", "PBKDF2, RFC 6070 c=4096"},
        {"passwordPASSWORDpassword", 24, "saltSALTsaltSALTsaltSALTsaltSALTsalt", 36, 4096, 25,
         "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038", "PBKDF2, RFC 6070 two output blocks"},
        {"pass\0word", 9, "sa\0lt", 5, 4096, 16, "56fa6aa75548099dcc37d7f03425e0c3", "PBKDF2, RFC 6070 NULs"},
    };
    for (size_t v = 0; v < sizeof(kPbkdf2) / sizeof(kPbkdf2[0]); v++) {
        check(subtle_pbkdf2(SUBTLE_ALG_SHA_1, (const uint8_t *)kPbkdf2[v].password, kPbkdf2[v].password_length,
                            (const uint8_t *)kPbkdf2[v].salt, kPbkdf2[v].salt_length, kPbkdf2[v].c, out,
                            kPbkdf2[v].length),
              kPbkdf2[v].what);
        same_hex(out, kPbkdf2[v].length, kPbkdf2[v].dk, kPbkdf2[v].what);
    }
    check(!subtle_pbkdf2(SUBTLE_ALG_SHA_1, (const uint8_t *)"p", 1, NULL, 0, 0, out, 20) &&
              !subtle_pbkdf2(SUBTLE_ALG_COUNT, (const uint8_t *)"p", 1, NULL, 0, 1, out, 20),
          "PBKDF2 refuses no iterations, and what is no hash");
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
    check(subtle_registered(SUBTLE_OP_ENCRYPT, "aes-gcm", 7, &dict) == SUBTLE_ALG_AES_GCM && dict == SUBTLE_DICT_AES_GCM &&
              subtle_registered(SUBTLE_OP_DECRYPT, "AES-CTR", 7, &dict) == SUBTLE_ALG_AES_CTR && dict == SUBTLE_DICT_AES_CTR &&
              subtle_registered(SUBTLE_OP_GENERATE_KEY, "AES-CBC", 7, &dict) == SUBTLE_ALG_AES_CBC &&
              dict == SUBTLE_DICT_AES_KEY_GEN &&
              subtle_registered(SUBTLE_OP_IMPORT_KEY, "AES-CBC", 7, &dict) == SUBTLE_ALG_AES_CBC && dict == SUBTLE_DICT_ALGORITHM,
          "the AES modes, each operation with its dictionary");
    check(subtle_registered(SUBTLE_OP_SIGN, "hmac", 4, &dict) == SUBTLE_ALG_HMAC && dict == SUBTLE_DICT_ALGORITHM &&
              subtle_registered(SUBTLE_OP_IMPORT_KEY, "HMAC", 4, &dict) == SUBTLE_ALG_HMAC && dict == SUBTLE_DICT_HMAC_IMPORT &&
              subtle_registered(SUBTLE_OP_GENERATE_KEY, "HMAC", 4, &dict) == SUBTLE_ALG_HMAC &&
              dict == SUBTLE_DICT_HMAC_KEY_GEN,
          "HMAC, each operation with its dictionary");
    check(subtle_registered(SUBTLE_OP_WRAP_KEY, "AES-GCM", 7, &dict) < 0 &&
              subtle_registered(SUBTLE_OP_SIGN, "AES-GCM", 7, &dict) < 0 &&
              subtle_registered(SUBTLE_OP_ENCRYPT, "HMAC", 4, &dict) < 0 &&
              subtle_registered(SUBTLE_OP_DIGEST, "HMAC", 4, &dict) < 0 &&
              subtle_registered(SUBTLE_OP_DERIVE_BITS, "AES-GCM", 7, &dict) < 0,
          "an algorithm is no operation's but its own (wrapKey falls back to encrypt in the binding)");
    check(subtle_registered(SUBTLE_OP_WRAP_KEY, "aes-kw", 6, &dict) == SUBTLE_ALG_AES_KW && dict == SUBTLE_DICT_ALGORITHM &&
              subtle_registered(SUBTLE_OP_UNWRAP_KEY, "AES-KW", 6, &dict) == SUBTLE_ALG_AES_KW &&
              subtle_registered(SUBTLE_OP_ENCRYPT, "AES-KW", 6, &dict) < 0 &&
              subtle_registered(SUBTLE_OP_DIGEST, "AES-KW", 6, &dict) < 0,
          "AES-KW wraps and unwraps, and neither encrypts nor digests");
    check(subtle_registered(SUBTLE_OP_DERIVE_BITS, "hkdf", 4, &dict) == SUBTLE_ALG_HKDF && dict == SUBTLE_DICT_HKDF &&
              subtle_registered(SUBTLE_OP_DERIVE_BITS, "PBKDF2", 6, &dict) == SUBTLE_ALG_PBKDF2 && dict == SUBTLE_DICT_PBKDF2 &&
              subtle_registered(SUBTLE_OP_GENERATE_KEY, "HKDF", 4, &dict) < 0 &&
              subtle_registered(SUBTLE_OP_DIGEST, "PBKDF2", 6, &dict) < 0,
          "the derivations derive bits, with their dictionaries, and generate nothing");
    check(subtle_registered(SUBTLE_OP_GET_KEY_LENGTH, "AES-GCM", 7, &dict) == SUBTLE_ALG_AES_GCM &&
              dict == SUBTLE_DICT_AES_DERIVED &&
              subtle_registered(SUBTLE_OP_GET_KEY_LENGTH, "HMAC", 4, &dict) == SUBTLE_ALG_HMAC &&
              dict == SUBTLE_DICT_HMAC_IMPORT &&
              subtle_registered(SUBTLE_OP_GET_KEY_LENGTH, "HKDF", 4, &dict) == SUBTLE_ALG_HKDF &&
              dict == SUBTLE_DICT_ALGORITHM &&
              subtle_registered(SUBTLE_OP_GET_KEY_LENGTH, "SHA-256", 7, &dict) < 0,
          "get key length, for each key type that can be derived and nothing else");
    check(subtle_digest_size(SUBTLE_ALG_COUNT) == 0 && !subtle_digest(SUBTLE_ALG_COUNT, NULL, 0, NULL),
          "what is no hash digests nothing");
    check(strcmp(subtle_alg_name(SUBTLE_ALG_SHA_384), "SHA-384") == 0, "the registered spelling");

    aes_cases();
    hmac_cases();
    base64url_cases();
    aes_kw_cases();
    kdf_cases();

    printf("subtle core: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
