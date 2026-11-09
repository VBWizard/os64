/* libdom's Web Cryptography core (userland/libdom/subtle_core.c) held to
 * the standards' published vectors, each family's function naming its
 * sources, and the registry matching names as normalization does. Run by
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

/* RFC 6979 A.2.5-A.2.7: each curve's key, then r || s for each hash over "sample" and "test"; every one checked against OpenSSL's verifier. */
static const struct { subtle_curve_t curve; const char *d, *x, *y; struct { subtle_alg_t hash; const char *message, *signature; } sigs[8]; } kRfc6979[] = {
    {SUBTLE_CURVE_P256, "c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721",
     "60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6",
     "7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299",
     {
      {SUBTLE_ALG_SHA_1, "sample", "61340c88c3aaebeb4f6d667f672ca9759a6ccaa9fa8811313039ee4a35471d326d7f147dac089441bb2e2fe8f7a3fa264b9c475098fdcf6e00d7c996e1b8b7eb"},
      {SUBTLE_ALG_SHA_1, "test", "0cbcc86fd6abd1d99e703e1ec50069ee5c0b4ba4b9ac60e409e8ec5910d81a8901b9d7b73dfaa60d5651ec4591a0136f87653e0fd780c3b1bc872ffdeae479b1"},
      {SUBTLE_ALG_SHA_256, "sample", "efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8"},
      {SUBTLE_ALG_SHA_256, "test", "f1abb023518351cd71d881567b1ea663ed3efcf6c5132b354f28d3b0b7d38367019f4113742a2b14bd25926b49c649155f267e60d3814b4c0cc84250e46f0083"},
      {SUBTLE_ALG_SHA_384, "sample", "0eafea039b20e9b42309fb1d89e213057cbf973dc0cfc8f129edddc800ef77194861f0491e6998b9455193e34e7b0d284ddd7149a74b95b9261f13abde940954"},
      {SUBTLE_ALG_SHA_384, "test", "83910e8b48bb0c74244ebdf7f07a1c5413d61472bd941ef3920e623fbccebeb68ddbec54cf8cd5874883841d712142a56a8d0f218f5003cb0296b6b509619f2c"},
      {SUBTLE_ALG_SHA_512, "sample", "8496a60b5e9b47c825488827e0495b0e3fa109ec4568fd3f8d1097678eb97f002362ab1adbe2b8adf9cb9edab740ea6049c028114f2460f96554f61fae3302fe"},
      {SUBTLE_ALG_SHA_512, "test", "461d93f31b6540894788fd206c07cfa0cc35f46fa3c91816fff1040ad1581a0439af9f15de0db8d97e72719c74820d304ce5226e32dedae67519e840d1194e55"},
     }},
    {SUBTLE_CURVE_P384, "6b9d3dad2e1b8c1c05b19875b6659f4de23c3b667bf297ba9aa47740787137d896d5724e4c70a825f872c9ea60d2edf5",
     "ec3a4e415b4e19a4568618029f427fa5da9a8bc4ae92e02e06aae5286b300c64def8f0ea9055866064a254515480bc13",
     "8015d9b72d7d57244ea8ef9ac0c621896708a59367f9dfb9f54ca84b3f1c9db1288b231c3ae0d4fe7344fd2533264720",
     {
      {SUBTLE_ALG_SHA_1, "sample", "ec748d839243d6fbef4fc5c4859a7dffd7f3abddf72014540c16d73309834fa37b9ba002899f6fda3a4a9386790d4eb2a3bcfa947beef4732bf247ac17f71676cb31a847b9ff0cbc9c9ed4c1a5b3facf26f49ca031d4857570ccb5ca4424a443"},
      {SUBTLE_ALG_SHA_1, "test", "4bc35d3a50ef4e30576f58cd96ce6bf638025ee624004a1f7789a8b8e43d0678acd9d29876daf46638645f7f404b11c7d5a6326c494ed3ff614703878961c0fde7b2c278f9a65fd8c4b7186201a2991695ba1c84541327e966fa7b50f7382282"},
      {SUBTLE_ALG_SHA_256, "sample", "21b13d1e013c7fa1392d03c5f99af8b30c570c6f98d4ea8e354b63a21d3daa33bde1e888e63355d92fa2b3c36d8fb2cdf3aa443fb107745bf4bd77cb3891674632068a10ca67e3d45db2266fa7d1feebefdc63eccd1ac42ec0cb8668a4fa0ab0"},
      {SUBTLE_ALG_SHA_256, "test", "6d6defac9ab64dabafe36c6bf510352a4cc27001263638e5b16d9bb51d451559f918eedaf2293be5b475cc8f0188636b2d46f3becbcc523d5f1a1256bf0c9b024d879ba9e838144c8ba6baeb4b53b47d51ab373f9845c0514eefb14024787265"},
      {SUBTLE_ALG_SHA_384, "sample", "94edbb92a5ecb8aad4736e56c691916b3f88140666ce9fa73d64c4ea95ad133c81a648152e44acf96e36dd1e80fabe4699ef4aeb15f178cea1fe40db2603138f130e740a19624526203b6351d0a3a94fa329c145786e679e7b82c71a38628ac8"},
      {SUBTLE_ALG_SHA_384, "test", "8203b63d3c853e8d77227fb377bcf7b7b772e97892a80f36ab775d509d7a5feb0542a7f0812998da8f1dd3ca3cf023dbddd0760448d42d8a43af45af836fce4de8be06b485e9b61b827c2f13173923e06a739f040649a667bf3b828246baa5a5"},
      {SUBTLE_ALG_SHA_512, "sample", "ed0959d5880ab2d869ae7f6c2915c6d60f96507f9cb3e047c0046861da4a799cfe30f35cc900056d7c99cd7882433709512c8cceee3890a84058ce1e22dbc2198f42323ce8aca9135329f03c068e5112dc7cc3ef3446defceb01a45c2667fdd5"},
      {SUBTLE_ALG_SHA_512, "test", "a0d5d090c9980faf3c2ce57b7ae951d31977dd11c775d314af55f76c676447d06fb6495cd21b4b6e340fc236584fb277976984e59b4c77b0e8e4460dca3d9f20e07b9bb1f63beefaf576f6b2e8b224634a2092cd3792e0159ad9cee37659c736"},
     }},
    {SUBTLE_CURVE_P521, "00fad06daa62ba3b25d2fb40133da757205de67f5bb0018fee8c86e1b68c7e75caa896eb32f1f47c70855836a6d16fcc1466f6d8fbec67db89ec0c08b0e996b83538",
     "01894550d0785932e00eaa23b694f213f8c3121f86dc97a04e5a7167db4e5bcd371123d46e45db6b5d5370a7f20fb633155d38ffa16d2bd761dcac474b9a2f5023a4",
     "00493101c962cd4d2fddf782285e64584139c2f91b47f87ff82354d6630f746a28a0db25741b5b34a828008b22acc23f924faafbd4d33f81ea66956dfeaa2bfdfcf5",
     {
      {SUBTLE_ALG_SHA_1, "sample", "00343b6ec45728975ea5cba6659bbb6062a5ff89eea58be3c80b619f322c87910fe092f7d45bb0f8eee01ed3f20babec079d202ae677b243ab40b5431d497c55d75d00e7b0e675a9b24413d448b8cc119d2bf7b2d2df032741c096634d6d65d0dbe3d5694625fb9e8104d3b842c1b0e2d0b98bea19341e8676aef66ae4eba3d5475d5d16"},
      {SUBTLE_ALG_SHA_1, "test", "013bad9f29abe20de37ebeb823c252ca0f63361284015a3bf430a46aaa80b87b0693f0694bd88afe4e661fc33b094cd3b7963bed5a727ed8bd6a3a202abe009d036701e9bb81ff7944ca409ad138dbbee228e1afcc0c890fc78ec8604639cb0dbdc90f717a99ead9d272855d00162ee9527567dd6a92cbd629805c0445282bbc916797ff"},
      {SUBTLE_ALG_SHA_256, "sample", "01511bb4d675114fe266fc4372b87682baecc01d3cc62cf2303c92b3526012659d16876e25c7c1e57648f23b73564d67f61c6f14d527d54972810421e7d87589e1a7004a171143a83163d6df460aaf61522695f207a58b95c0644d87e52aa1a347916e4f7a72930b1bc06dbe22ce3f58264afd23704cbb63b29b931f7de6c9d949a7ecfc"},
      {SUBTLE_ALG_SHA_256, "test", "000e871c4a14f993c6c7369501900c4bc1e9c7b0b4ba44e04868b30b41d8071042eb28c4c250411d0ce08cd197e4188ea4876f279f90b3d8d74a3c76e6f1e4656aa800cd52dbaa33b063c3a6cd8058a1fb0a46a4754b034fcc644766ca14da8ca5ca9fde00e88c1ad60ccba759025299079d7a427ec3cc5b619bfbc828e7769bcd694e86"},
      {SUBTLE_ALG_SHA_384, "sample", "01ea842a0e17d2de4f92c15315c63ddf72685c18195c2bb95e572b9c5136ca4b4b576ad712a52be9730627d16054ba40cc0b8d3ff035b12ae75168397f5d50c6745101f21a3cee066e1961025fb048bd5fe2b7924d0cd797babe0a83b66f1e35eeaf5fde143fa85dc394a7dee766523393784484bdf3e00114a1c857cde1aa203db65d61"},
      {SUBTLE_ALG_SHA_384, "test", "014bee21a18b6d8b3c93fab08d43e739707953244fdbe924fa926d76669e7ac8c89df62ed8975c2d8397a65a49dcc09f6b0ac62272741924d479354d74ff6075578c0133330865c067a0eaf72362a65e2d7bc4e461e8c8995c3b6226a21bd1aa78f0ed94fe536a0dca35534f0cd1510c41525d163fe9d74d134881e35141ed5e8e95b979"},
      {SUBTLE_ALG_SHA_512, "sample", "00c328fafcbd79dd77850370c46325d987cb525569fb63c5d3bc53950e6d4c5f174e25a1ee9017b5d450606add152b534931d7d4e8455cc91f9b15bf05ec36e377fa00617cce7cf5064806c467f678d3b4080d6f1cc50af26ca209417308281b68af282623eaa63e5b5c0723d8b8c37ff0777b1a20f8ccb1dccc43997f1ee0e44da4a67a"},
      {SUBTLE_ALG_SHA_512, "test", "013e99020abf5cee7525d16b69b229652ab6bdf2affcaef38773b4b7d08725f10cdb93482fdcc54edcee91eca4166b2a7c6265ef0ce2bd7051b7cef945babd47ee6d01fbd0013c674aa79cb39849527916ce301c66ea7ce8b80682786ad60f98f7e78a19ca69eff5c57400e3b3a0ad66ce0978214d13baf4e9ac60752f7b155e2de4dce3"},
     }},
};
/* RFC 5903 § 8.1-8.3: the initiator's key i and public (gix, giy), the responder's r and (grx, gry), and the shared girx. */
static const struct { subtle_curve_t curve; const char *i, *gi, *r, *gr, *shared; } kRfc5903[] = {
    {SUBTLE_CURVE_P256,
     "c88f01f510d9ac3f70a292daa2316de544e9aab8afe84049c62a9c57862d1433",
     "04dad0b65394221cf9b051e1feca5787d098dfe637fc90b9ef945d0c37725811805271a0461cdb8252d61f1c456fa3e59ab1f45b33accf5f58389e0577b8990bb3",
     "c6ef9c5d78ae012a011164acb397ce2088685d8f06bf9be0b283ab46476bee53",
     "04d12dfb5289c8d4f81208b70270398c342296970a0bccb74c736fc7554494bf6356fbf3ca366cc23e8157854c13c58d6aac23f046ada30f8353e74f33039872ab",
     "d6840f6b42f6edafd13116e0e12565202fef8e9ece7dce03812464d04b9442de"},
    {SUBTLE_CURVE_P384,
     "099f3c7034d4a2c699884d73a375a67f7624ef7c6b3c0f160647b67414dce655e35b538041e649ee3faef896783ab194",
     "04667842d7d180ac2cde6f74f37551f55755c7645c20ef73e31634fe72b4c55ee6de3ac808acb4bdb4c88732aee95f41aa9482ed1fc0eeb9cafc4984625ccfc23f65032149e0e144ada024181535a0f38eeb9fcff3c2c947dae69b4c634573a81c",
     "41cb0779b4bdb85d47846725fbec3c9430fab46cc8dc5060855cc9bda0aa2942e0308312916b8ed2960e4bd55a7448fc",
     "04e558dbef53eecde3d3fccfc1aea08a89a987475d12fd950d83cfa41732bc509d0d1ac43a0336def96fda41d0774a3571dcfbec7aacf3196472169e838430367f66eebe3c6e70c416dd5f0c68759dd1fff83fa40142209dff5eaad96db9e6386c",
     "11187331c279962d93d604243fd592cb9d0a926f422e47187521287e7156c5c4d603135569b9e9d09cf5d4a270f59746"},
    {SUBTLE_CURVE_P521,
     "0037ade9319a89f4dabdb3ef411aaccca5123c61acab57b5393dce47608172a095aa85a30fe1c2952c6771d937ba9777f5957b2639bab072462f68c27a57382d4a52",
     "040015417e84dbf28c0ad3c278713349dc7df153c897a1891bd98bab4357c9ecbee1e3bf42e00b8e380aeae57c2d107564941885942af5a7f4601723c4195d176ced3e017cae20b6641d2eeb695786d8c946146239d099e18e1d5a514c739d7cb4a10ad8a788015ac405d7799dc75e7b7d5b6cf2261a6a7f1507438bf01beb6ca3926f9582",
     "0145ba99a847af43793fdd0e872e7cdfa16be30fdc780f97bccc3f078380201e9c677d600b343757a3bdbf2a3163e4c2f869cca7458aa4a4effc311f5cb151685eb9",
     "0400d0b3975ac4b799f5bea16d5e13e9af971d5e9b984c9f39728b5e5739735a219b97c356436adc6e95bb0352f6be64a6c2912d4ef2d0433ced2b6171640012d9460f015c68226383956e3bd066e797b623c27ce0eac2f551a10c2c724d9852077b87220b6536c5c408a1d2aebb8e86d678ae49cb57091f4732296579ab44fcd17f0fc56a",
     "01144c7d79ae6956bc8edb8e7c787c4521cb086fa64407f97894e5e6b2d79b04d1427e73ca4baa240a34786859810c06b3c715a3a8cc3151f2bee417996d19f3ddea"},
};
/* WPT's EC key fixtures (WebCryptoAPI/util/ec_key_fixtures.js): spki, pkcs8 as other browsers write it, pkcs8 without the public key, and the JWK's d. */
static const struct { subtle_curve_t curve; const char *spki, *pkcs8, *pkcs8_private_only, *raw, *d; } kWptEc[] = {
    {SUBTLE_CURVE_P256,
     "3059301306072a8648ce3d020106082a8648ce3d03010703420004d210b0a6f9d9f012868058b43fa4f471018543bba00c9250df9257c2acae5dd1ce037552d481450ce39b4d1095701b175bfab34b8e6c099e18f1c198358361e8",
     "308187020100301306072a8648ce3d020106082a8648ce3d030107046d306b020101042013d33a2d5abf9cf9ebb21ff860d4aefe6e56e77790f4dee9b40884ebd33544eaa14403420004d210b0a6f9d9f012868058b43fa4f471018543bba00c9250df9257c2acae5dd1ce037552d481450ce39b4d1095701b175bfab34b8e6c099e18f1c198358361e8",
     "3041020100301306072a8648ce3d020106082a8648ce3d03010704273025020101042013d33a2d5abf9cf9ebb21ff860d4aefe6e56e77790f4dee9b40884ebd33544ea",
     "04d210b0a6f9d9f012868058b43fa4f471018543bba00c9250df9257c2acae5dd1ce037552d481450ce39b4d1095701b175bfab34b8e6c099e18f1c198358361e8",
     "13d33a2d5abf9cf9ebb21ff860d4aefe6e56e77790f4dee9b40884ebd33544ea"},
    {SUBTLE_CURVE_P384,
     "3076301006072a8648ce3d020106052b8104002203620004219c14d66617b36ec6d8856b385b73a74d344fd8ae75ef046435dda54e3b44bd5fbdebd1d08dd69e2d7dc1dc218cb435bd28138cc778337a842f6bd61b240e74249f24667c2a5810a76bfc28e0335f88a6501dec01976da85afb00869cb6ace8",
     "3081b6020100301006072a8648ce3d020106052b8104002204819e30819b02010104304537b5990784d3c2d22e96a8f92fa1aa492ee873e576a41582e144183c9888d10e6b9eb4ced4b2cc4012e4ac5ea84073a16403620004219c14d66617b36ec6d8856b385b73a74d344fd8ae75ef046435dda54e3b44bd5fbdebd1d08dd69e2d7dc1dc218cb435bd28138cc778337a842f6bd61b240e74249f24667c2a5810a76bfc28e0335f88a6501dec01976da85afb00869cb6ace8",
     "304e020100301006072a8648ce3d020106052b810400220437303502010104304537b5990784d3c2d22e96a8f92fa1aa492ee873e576a41582e144183c9888d10e6b9eb4ced4b2cc4012e4ac5ea84073",
     "04219c14d66617b36ec6d8856b385b73a74d344fd8ae75ef046435dda54e3b44bd5fbdebd1d08dd69e2d7dc1dc218cb435bd28138cc778337a842f6bd61b240e74249f24667c2a5810a76bfc28e0335f88a6501dec01976da85afb00869cb6ace8",
     "4537b5990784d3c2d22e96a8f92fa1aa492ee873e576a41582e144183c9888d10e6b9eb4ced4b2cc4012e4ac5ea84073"},
    {SUBTLE_CURVE_P521,
     "30819b301006072a8648ce3d020106052b8104002303818600040156f479f8df1e20a7ffc04ce420c3e154ae251996bee42f034b84d41b743f34e45f311b813a9cdec8cda59bbbbd31d460b3292521e7c1b722e5667c03db2fae753f01501736cfe247394320d8e4afc2fd39b5a9331061b81e2241282b9e17891822b5b79e052f4597b59643fd39379c51bd5125c4f48bc3f025ce3cd36953286ccb38fb",
     "3081ee020100301006072a8648ce3d020106052b810400230481d63081d3020101044200f408758368ba930f30f76ae054fe5cd2ce7fda2c9f76a6d436cf75d66c440bfe6331c7c172a12478193c8251487bc91263fa50217f85ff636f59cd546e3ab483b4a1818903818600040156f479f8df1e20a7ffc04ce420c3e154ae251996bee42f034b84d41b743f34e45f311b813a9cdec8cda59bbbbd31d460b3292521e7c1b722e5667c03db2fae753f01501736cfe247394320d8e4afc2fd39b5a9331061b81e2241282b9e17891822b5b79e052f4597b59643fd39379c51bd5125c4f48bc3f025ce3cd36953286ccb38fb",
     "3060020100301006072a8648ce3d020106052b8104002304493047020101044200f408758368ba930f30f76ae054fe5cd2ce7fda2c9f76a6d436cf75d66c440bfe6331c7c172a12478193c8251487bc91263fa50217f85ff636f59cd546e3ab483b4",
     "040156f479f8df1e20a7ffc04ce420c3e154ae251996bee42f034b84d41b743f34e45f311b813a9cdec8cda59bbbbd31d460b3292521e7c1b722e5667c03db2fae753f01501736cfe247394320d8e4afc2fd39b5a9331061b81e2241282b9e17891822b5b79e052f4597b59643fd39379c51bd5125c4f48bc3f025ce3cd36953286ccb38fb",
     "00f408758368ba930f30f76ae054fe5cd2ce7fda2c9f76a6d436cf75d66c440bfe6331c7c172a12478193c8251487bc91263fa50217f85ff636f59cd546e3ab483b4"},
};

/* The elliptic curves: RFC 6979's deterministic signatures reproduced
 * exactly (BearSSL's nonce is RFC 6979's, so a sign that differs by a bit
 * is a bug) and verified, a flipped bit or a short signature refused;
 * RFC 5903's ECDH from both sides; WPT's spki and pkcs8 decoded and
 * re-encoded byte for byte; and what each must refuse. */
static void ec_cases(void)
{
    uint8_t d[SUBTLE_EC_SIZE_MAX], point[SUBTLE_EC_POINT_MAX], other[SUBTLE_EC_POINT_MAX];
    uint8_t sig[2 * SUBTLE_EC_SIZE_MAX], der[SUBTLE_EC_DER_MAX + 2], out[SUBTLE_EC_DER_MAX];
    char want[2 * SUBTLE_EC_POINT_MAX + 3], what[128];
    for (size_t v = 0; v < sizeof(kRfc6979) / sizeof(kRfc6979[0]); v++) {
        subtle_curve_t c = kRfc6979[v].curve;
        size_t size = subtle_ec_size(c);
        unhex(kRfc6979[v].d, d);
        check(subtle_ec_scalar_ok(c, d), "an RFC 6979 key is a private key");
        snprintf(want, sizeof(want), "04%s%s", kRfc6979[v].x, kRfc6979[v].y);
        subtle_ec_public(c, d, point);
        snprintf(what, sizeof(what), "%s: RFC 6979's public key", subtle_curve_name(c));
        same_hex(point, 1 + 2 * size, want, what);
        check(subtle_ec_point_ok(c, point, 1 + 2 * size), what);
        for (int s = 0; s < 8; s++) {
            const char *message = kRfc6979[v].sigs[s].message;
            subtle_alg_t hash = kRfc6979[v].sigs[s].hash;
            size_t length = strlen(message);
            snprintf(what, sizeof(what), "%s, %s, \"%s\": RFC 6979's signature", subtle_curve_name(c),
                     subtle_alg_name(hash), message);
            check(subtle_ecdsa_sign(c, hash, d, (const uint8_t *)message, length, sig) == 2 * size, what);
            same_hex(sig, 2 * size, kRfc6979[v].sigs[s].signature, what);
            check(subtle_ecdsa_verify(c, hash, point, (const uint8_t *)message, length, sig, 2 * size), what);
            sig[size - 1] ^= 1;
            check(!subtle_ecdsa_verify(c, hash, point, (const uint8_t *)message, length, sig, 2 * size),
                  "a signature with a bit flipped does not verify");
            sig[size - 1] ^= 1;
            check(!subtle_ecdsa_verify(c, hash, point, (const uint8_t *)"Sample", length, sig, 2 * size),
                  "another message does not verify");
            check(!subtle_ecdsa_verify(c, hash, point, (const uint8_t *)message, length, sig, 2 * size - 1),
                  "a signature a byte short does not verify");
            /* Each half with a zero byte before it: the same integers, which
             * BearSSL's raw verify would take, and no WebCrypto signature. */
            uint8_t padded[2 * SUBTLE_EC_SIZE_MAX + 2] = {0};
            memcpy(padded + 1, sig, size);
            memcpy(padded + 2 + size, sig + size, size);
            check(!subtle_ecdsa_verify(c, hash, point, (const uint8_t *)message, length, padded, 2 * size + 2),
                  "halves padded past the curve's size do not verify");
        }
    }
    for (size_t v = 0; v < sizeof(kRfc5903) / sizeof(kRfc5903[0]); v++) {
        subtle_curve_t c = kRfc5903[v].curve;
        size_t size = subtle_ec_size(c);
        uint8_t secret[SUBTLE_EC_SIZE_MAX];
        snprintf(what, sizeof(what), "%s: RFC 5903's shared secret", subtle_curve_name(c));
        unhex(kRfc5903[v].i, d);
        unhex(kRfc5903[v].gr, point);
        subtle_ec_public(c, d, other);
        same_hex(other, 1 + 2 * size, kRfc5903[v].gi, "RFC 5903's g^i");
        check(subtle_ecdh(c, d, point, secret), what);
        same_hex(secret, size, kRfc5903[v].shared, what);
        unhex(kRfc5903[v].r, d);
        check(subtle_ecdh(c, d, other, secret), what);
        same_hex(secret, size, kRfc5903[v].shared, what);
        point[1 + size] ^= 1;
        check(!subtle_ecdh(c, d, point, secret) && !subtle_ec_point_ok(c, point, 1 + 2 * size),
              "a point off the curve: no ECDH, and no public key");
    }
    for (size_t v = 0; v < sizeof(kWptEc) / sizeof(kWptEc[0]); v++) {
        subtle_curve_t c = kWptEc[v].curve, got;
        size_t size = subtle_ec_size(c), length, point_length;
        const uint8_t *key;
        const char *name = subtle_curve_name(c);
        uint8_t raw[SUBTLE_EC_POINT_MAX], scalar[SUBTLE_EC_SIZE_MAX];
        unhex(kWptEc[v].raw, raw);
        unhex(kWptEc[v].d, scalar);
        snprintf(what, sizeof(what), "%s: WPT's spki decodes to its raw point", name);
        length = unhex(kWptEc[v].spki, der);
        check(subtle_ec_spki_decode(der, length, &got, &key, &point_length) && got == c &&
                  point_length == 1 + 2 * size && memcmp(key, raw, point_length) == 0,
              what);
        snprintf(what, sizeof(what), "%s: spki encodes as WPT's", name);
        same_hex(out, subtle_ec_spki_encode(c, raw, out), kWptEc[v].spki, what);
        snprintf(what, sizeof(what), "%s: WPT's pkcs8 decodes to its d and point", name);
        length = unhex(kWptEc[v].pkcs8, der);
        check(subtle_ec_pkcs8_decode(der, length, &got, d, &key, &point_length) && got == c &&
                  memcmp(d, scalar, size) == 0 && key != NULL && point_length == 1 + 2 * size &&
                  memcmp(key, raw, point_length) == 0,
              what);
        snprintf(what, sizeof(what), "%s: pkcs8 encodes as WPT's", name);
        same_hex(out, subtle_ec_pkcs8_encode(c, scalar, raw, out), kWptEc[v].pkcs8, what);
        snprintf(what, sizeof(what), "%s: a pkcs8 without its public key", name);
        length = unhex(kWptEc[v].pkcs8_private_only, der);
        check(subtle_ec_pkcs8_decode(der, length, &got, d, &key, &point_length) && got == c &&
                  memcmp(d, scalar, size) == 0 && key == NULL,
              what);
        subtle_ec_public(c, d, point);
        check(memcmp(point, raw, 1 + 2 * size) == 0, "d's public key is the fixture's");
        /* What both containers refuse: a byte short, a byte over, another
         * algorithm, and (spki) a compressed point left to the caller. */
        length = unhex(kWptEc[v].spki, der);
        check(!subtle_ec_spki_decode(der, length - 1, &got, &key, &point_length), "a truncated spki is refused");
        der[length] = 0;
        check(!subtle_ec_spki_decode(der, length + 1, &got, &key, &point_length), "a byte after an spki is refused");
        der[8] ^= 1; /* id-ecPublicKey's last byte but two */
        check(!subtle_ec_spki_decode(der, length, &got, &key, &point_length), "another algorithm's spki is refused");
        length = unhex(kWptEc[v].pkcs8, der);
        check(!subtle_ec_pkcs8_decode(der, length - 1, &got, d, &key, &point_length), "a truncated pkcs8 is refused");
        der[length] = 0;
        check(!subtle_ec_pkcs8_decode(der, length + 1, &got, d, &key, &point_length),
              "a byte after a pkcs8 is refused");
        raw[0] = 0x02;
        check(!subtle_ec_point_ok(c, raw, 1 + size), "a compressed point is refused");
        check(!subtle_ec_point_ok(c, raw, 1 + 2 * size), "a point that is not 0x04-led is refused");
    }
    /* A pkcs8 whose ECPrivateKey names its curve, the same and another;
     * and one whose d dropped its leading zero byte. Made from P-256's. */
    {
        subtle_curve_t got;
        const uint8_t *key;
        size_t point_length;
        static const char kNamed[] =
            "3081930201003013" "06072a8648ce3d0201" "06082a8648ce3d030107" "0479" "3077" "020101"
            "0420" "13d33a2d5abf9cf9ebb21ff860d4aefe6e56e77790f4dee9b40884ebd33544ea"
            "a00a" "06082a8648ce3d030107"
            "a144034200" "04d210b0a6f9d9f012868058b43fa4f471018543bba00c9250df9257c2acae5dd1"
            "ce037552d481450ce39b4d1095701b175bfab34b8e6c099e18f1c198358361e8";
        size_t length = unhex(kNamed, der);
        check(subtle_ec_pkcs8_decode(der, length, &got, d, &key, &point_length) && key != NULL,
              "an ECPrivateKey naming its own curve");
        der[79] = 0x08; /* the [0] curve's last byte: no longer secp256r1 */
        check(!subtle_ec_pkcs8_decode(der, length, &got, d, &key, &point_length),
              "an ECPrivateKey naming another curve is refused");
        static const char kShort[] = "3040020100301306072a8648ce3d020106082a8648ce3d03010704263024020101"
                                     "041f" "d33a2d5abf9cf9ebb21ff860d4aefe6e56e77790f4dee9b40884ebd33544ea";
        length = unhex(kShort, der);
        check(subtle_ec_pkcs8_decode(der, length, &got, d, &key, &point_length) && d[0] == 0 && d[1] == 0xd3,
              "a d without its leading zero is padded back");
    }
    /* RFC 5958's version 1, whose [1] public key no PrivateKeyInfo has,
     * refused with and without one; and a d with a leading zero more than
     * the curve's size, read as its value. Made from WPT's P-256 key. */
    {
        subtle_curve_t got;
        const uint8_t *key;
        size_t point_length, length;
        uint8_t scalar[SUBTLE_EC_SIZE_MAX];
        static const char kVersion1[] = "308185020101301306072a8648ce3d020106082a8648ce3d03010704273025020101042013d33a2d5abf9cf9ebb21ff860d4aefe6e56e77790f4dee9b40884ebd33544ea81420004d210b0a6f9d9f012868058b43fa4f471018543bba00c9250df9257c2acae5dd1ce037552d481450ce39b4d1095701b175bfab34b8e6c099e18f1c198358361e8";
        static const char kVersion0Outer[] = "308185020100301306072a8648ce3d020106082a8648ce3d03010704273025020101042013d33a2d5abf9cf9ebb21ff860d4aefe6e56e77790f4dee9b40884ebd33544ea81420004d210b0a6f9d9f012868058b43fa4f471018543bba00c9250df9257c2acae5dd1ce037552d481450ce39b4d1095701b175bfab34b8e6c099e18f1c198358361e8";
        static const char kExtraZero[] = "3042020100301306072a8648ce3d020106082a8648ce3d0301070428302602010104210013d33a2d5abf9cf9ebb21ff860d4aefe6e56e77790f4dee9b40884ebd33544ea";
        unhex(kWptEc[0].d, scalar);
        length = unhex(kVersion1, der);
        check(!subtle_ec_pkcs8_decode(der, length, &got, d, &key, &point_length), "a version 1 pkcs8 is refused");
        length = unhex(kWptEc[0].pkcs8_private_only, der);
        der[4] = 1; /* the version's value */
        check(!subtle_ec_pkcs8_decode(der, length, &got, d, &key, &point_length),
              "version 1 without a public key is refused too");
        length = unhex(kVersion0Outer, der);
        check(!subtle_ec_pkcs8_decode(der, length, &got, d, &key, &point_length),
              "a [1] public key after a version 0 key is refused");
        length = unhex(kExtraZero, der);
        check(subtle_ec_pkcs8_decode(der, length, &got, d, &key, &point_length) && memcmp(d, scalar, 32) == 0,
              "a d with a leading zero too many is read as its value");
    }
    /* Scalars: zero, the order, the order minus one. */
    for (int c = 0; c < SUBTLE_CURVE_COUNT; c++) {
        size_t size = subtle_ec_size((subtle_curve_t)c);
        memset(d, 0, sizeof(d));
        check(!subtle_ec_scalar_ok((subtle_curve_t)c, d), "zero is no private key");
        static const char *const kOrders[] = {
            "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
            "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
            "01fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa51868783bf2f966b7fcc0148f709a5d03bb5c9b8899c47aebb6fb71e91386409",
        };
        unhex(kOrders[c], d);
        check(!subtle_ec_scalar_ok((subtle_curve_t)c, d), "the order is no private key");
        d[size - 1] -= 1;
        check(subtle_ec_scalar_ok((subtle_curve_t)c, d), "the order less one is a private key");
    }
    /* Generated keys sign, and verify, and agree. */
    for (int c = 0; c < SUBTLE_CURVE_COUNT; c++) {
        size_t size = subtle_ec_size((subtle_curve_t)c);
        uint8_t seed[48], d2[SUBTLE_EC_SIZE_MAX], s1[SUBTLE_EC_SIZE_MAX], s2[SUBTLE_EC_SIZE_MAX];
        for (int i = 0; i < 48; i++) seed[i] = (uint8_t)(i * 7 + c);
        check(subtle_ec_generate((subtle_curve_t)c, seed, sizeof(seed), d) && subtle_ec_scalar_ok((subtle_curve_t)c, d),
              "a generated key is a private key");
        seed[0] ^= 1;
        check(subtle_ec_generate((subtle_curve_t)c, seed, sizeof(seed), d2) && memcmp(d, d2, size) != 0,
              "another seed, another key");
        subtle_ec_public((subtle_curve_t)c, d, point);
        subtle_ec_public((subtle_curve_t)c, d2, other);
        check(subtle_ecdsa_sign((subtle_curve_t)c, SUBTLE_ALG_SHA_256, d, (const uint8_t *)"abc", 3, sig) == 2 * size &&
                  subtle_ecdsa_verify((subtle_curve_t)c, SUBTLE_ALG_SHA_256, point, (const uint8_t *)"abc", 3, sig,
                                      2 * size),
              "a generated key signs, and its public key verifies");
        check(subtle_ecdh((subtle_curve_t)c, d, other, s1) && subtle_ecdh((subtle_curve_t)c, d2, point, s2) &&
                  memcmp(s1, s2, size) == 0,
              "two generated keys agree");
    }
    check(subtle_curve_named("P-256", 5) == SUBTLE_CURVE_P256 && subtle_curve_named("P-521", 5) == SUBTLE_CURVE_P521 &&
              subtle_curve_named("p-256", 5) < 0 && subtle_curve_named("P-256\0", 6) < 0 &&
              subtle_curve_named("P-512", 5) < 0,
          "a curve is named exactly");
    check(subtle_ecdsa_sign(SUBTLE_CURVE_P256, SUBTLE_ALG_COUNT, d, (const uint8_t *)"a", 1, sig) == 0,
          "ECDSA refuses what is no hash");
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
    check(subtle_registered(SUBTLE_OP_SIGN, "ecdsa", 5, &dict) == SUBTLE_ALG_ECDSA && dict == SUBTLE_DICT_ECDSA &&
              subtle_registered(SUBTLE_OP_VERIFY, "ECDSA", 5, &dict) == SUBTLE_ALG_ECDSA && dict == SUBTLE_DICT_ECDSA &&
              subtle_registered(SUBTLE_OP_GENERATE_KEY, "ECDSA", 5, &dict) == SUBTLE_ALG_ECDSA &&
              dict == SUBTLE_DICT_EC_KEY_GEN &&
              subtle_registered(SUBTLE_OP_IMPORT_KEY, "EcDsA", 5, &dict) == SUBTLE_ALG_ECDSA &&
              dict == SUBTLE_DICT_EC_KEY_IMPORT &&
              subtle_registered(SUBTLE_OP_DERIVE_BITS, "ecdh", 4, &dict) == SUBTLE_ALG_ECDH &&
              dict == SUBTLE_DICT_ECDH_DERIVE &&
              subtle_registered(SUBTLE_OP_GENERATE_KEY, "ECDH", 4, &dict) == SUBTLE_ALG_ECDH &&
              dict == SUBTLE_DICT_EC_KEY_GEN &&
              subtle_registered(SUBTLE_OP_IMPORT_KEY, "ECDH", 4, &dict) == SUBTLE_ALG_ECDH &&
              dict == SUBTLE_DICT_EC_KEY_IMPORT,
          "ECDSA and ECDH, each operation with its dictionary");
    check(subtle_registered(SUBTLE_OP_SIGN, "ECDH", 4, &dict) < 0 &&
              subtle_registered(SUBTLE_OP_DERIVE_BITS, "ECDSA", 5, &dict) < 0 &&
              subtle_registered(SUBTLE_OP_GET_KEY_LENGTH, "ECDH", 4, &dict) < 0 &&
              subtle_registered(SUBTLE_OP_ENCRYPT, "ECDH", 4, &dict) < 0,
          "neither curve algorithm is another's, and no key is derived as ECDH's");
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
    ec_cases();

    printf("subtle core: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
