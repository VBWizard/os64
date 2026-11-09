// Known-answer data is extracted from the pinned upstream test suite.
// Host differential runs and the guest execute this same fixture code.
#include "foundation.h"
#include "os64/str.h"
#include "vectors.h"

#define REQUIRE(test, message) do { if (!(test)) { report(message); return 1; } } while (0)

static unsigned nibble(char c)
{
    return c <= '9' ? (unsigned)(c - '0') :
           c <= 'F' ? (unsigned)(c - 'A' + 10) : (unsigned)(c - 'a' + 10);
}

// Inputs are checked-in vector strings. A capacity check still prevents a
// larger replacement vector from overwriting the fixture's scratch buffers.
static size_t unhex(unsigned char *out, size_t cap, const char *hex)
{
    size_t length = os64_strlen(hex);
    if ((length & 1) || length / 2 > cap)
        return SIZE_MAX;
    for (size_t i = 0; i < length / 2; i++)
        out[i] = (unsigned char)((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
    return length / 2;
}

static int equal(const void *a, const void *b, size_t size)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < size; i++)
        if (x[i] != y[i]) return 0;
    return 1;
}

static int gcm_vectors(bearssl_test_report report)
{
    for (size_t i = 0; KAT_GCM[i]; i += 6) {
        unsigned char key[32], plain[256], aad[256], iv[256], cipher[256], tag[16];
        size_t kn = unhex(key, sizeof key, KAT_GCM[i]);
        size_t pn = unhex(plain, sizeof plain, KAT_GCM[i + 1]);
        size_t an = unhex(aad, sizeof aad, KAT_GCM[i + 2]);
        size_t vn = unhex(iv, sizeof iv, KAT_GCM[i + 3]);
        size_t cn = unhex(cipher, sizeof cipher, KAT_GCM[i + 4]);
        size_t tn = unhex(tag, sizeof tag, KAT_GCM[i + 5]);
        REQUIRE(kn <= sizeof key && pn <= sizeof plain && an <= sizeof aad &&
                vn <= sizeof iv && cn == pn && tn == 16, "FAIL GCM vector bounds");
        br_aes_ct64_ctr_keys aes;
        br_gcm_context gcm;
        br_aes_ct64_ctr_init(&aes, key, kn);
        br_gcm_init(&gcm, &aes.vtable, br_ghash_ctmul64);
        br_gcm_reset(&gcm, iv, vn);
        for (size_t j = 0; j < an; j++) br_gcm_aad_inject(&gcm, aad + j, 1);
        br_gcm_flip(&gcm);
        for (size_t j = 0; j < pn; j++) br_gcm_run(&gcm, 1, plain + j, 1);
        REQUIRE(equal(plain, cipher, pn), "FAIL GCM ciphertext");
        unsigned char actual[16];
        br_gcm_get_tag(&gcm, actual);
        REQUIRE(equal(actual, tag, 16), "FAIL GCM tag");
        // Decrypt and authenticate separately; mutation must fail authentication.
        for (unsigned bad = 0; bad < 2; bad++) {
            os64_memcpy(plain, cipher, pn);
            br_gcm_reset(&gcm, iv, vn);
            br_gcm_aad_inject(&gcm, aad, an);
            br_gcm_flip(&gcm);
            br_gcm_run(&gcm, 0, plain, pn);
            os64_memcpy(actual, tag, 16);
            actual[0] ^= (unsigned char)bad;
            REQUIRE(br_gcm_check_tag(&gcm, actual) == !bad, "FAIL GCM authentication");
            REQUIRE(unhex(cipher, sizeof cipher, KAT_GCM[i + 1]) == pn &&
                    equal(plain, cipher, pn), "FAIL GCM plaintext");
            REQUIRE(unhex(cipher, sizeof cipher, KAT_GCM[i + 4]) == pn,
                    "FAIL GCM restore");
        }
    }
    report("PASS upstream GCM vectors, bytewise input, damaged tags");
    return 0;
}

static int ecdsa_vectors(bearssl_test_report report)
{
    for (size_t i = 0; ECDSA_KAT[i].pub; i++) {
        const ecdsa_kat_vector *v = &ECDSA_KAT[i];
        if (v->hf == &br_sha1_vtable || v->hf == &br_sha224_vtable) continue;
        br_hash_compat_context hash;
        unsigned char digest[64], signature[150], generated[150];
        size_t hn = (v->hf->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK;
        v->hf->init(&hash.vtable);
        v->hf->update(&hash.vtable, v->msg, os64_strlen(v->msg));
        v->hf->out(&hash.vtable, digest);
        size_t sn = unhex(signature, sizeof signature, v->sraw);
        REQUIRE(sn <= sizeof signature, "FAIL ECDSA vector bounds");
        REQUIRE(br_ecdsa_i31_vrfy_raw(br_ec_get_default(), digest, hn,
                                    v->pub, signature, sn), "FAIL ECDSA signature");
        digest[0] ^= 1;
        REQUIRE(!br_ecdsa_i31_vrfy_raw(br_ec_get_default(), digest, hn,
                                     v->pub, signature, sn), "FAIL ECDSA damaged hash");
        digest[0] ^= 1;
        size_t gn = br_ecdsa_i31_sign_raw(br_ec_get_default(), v->hf, digest,
                                        v->priv, generated);
        REQUIRE(gn == sn && equal(generated, signature, sn), "FAIL ECDSA deterministic signing");
    }
    report("PASS upstream ECDSA P-256/P-384/P-521 SHA-2 vectors and rejection");
    return 0;
}

static int decoder_failures(bearssl_test_report report)
{
    br_x509_decoder_context certificate, before_certificate;
    br_skey_decoder_context key, before_key;
    const unsigned char invalid[] = {0x01, 0x01, 0};
    br_x509_decoder_init(&certificate, NULL, NULL);
    br_x509_decoder_push(&certificate, invalid, sizeof invalid);
    REQUIRE(br_x509_decoder_last_error(&certificate) != 0, "FAIL certificate error trigger");
    os64_memcpy(&before_certificate, &certificate, sizeof certificate);
    br_x509_decoder_push(&certificate, invalid, sizeof invalid);
    REQUIRE(equal(&before_certificate, &certificate, sizeof certificate), "FAIL certificate sticky error");
    br_skey_decoder_init(&key);
    br_skey_decoder_push(&key, invalid, sizeof invalid);
    REQUIRE(br_skey_decoder_last_error(&key) != 0, "FAIL key error trigger");
    os64_memcpy(&before_key, &key, sizeof key);
    br_skey_decoder_push(&key, invalid, sizeof invalid);
    REQUIRE(equal(&before_key, &key, sizeof key), "FAIL key sticky error");
    report("PASS decoder errors remain terminal across chunks");
    return 0;
}

static int record_lengths(bearssl_test_report report)
{
    // Regression for upstream 7bea48e: reject non-block-multiple records
    // before a legacy CBC implementation can receive an invalid length.
    const br_block_cbcdec_class *implementations[] = {
        &br_des_ct_cbcdec_vtable, &br_aes_small_cbcdec_vtable,
        &br_aes_big_cbcdec_vtable, &br_aes_ct64_cbcdec_vtable
    };
    unsigned char key[24] = {0}, iv[16] = {0};
    for (size_t i = 0; i < sizeof implementations / sizeof implementations[0]; i++) {
        for (unsigned explicit_iv = 0; explicit_iv < 2; explicit_iv++) {
            br_sslrec_in_cbc_context context;
            br_sslrec_in_cbc_vtable.init(&context.vtable, implementations[i],
                key, i ? 16 : 24, &br_sha256_vtable, key, sizeof key, 32,
                explicit_iv ? NULL : iv);
            const br_sslrec_in_class *const *generic =
                (const br_sslrec_in_class *const *)(const void *)&context.vtable;
            REQUIRE(context.vtable->inner.check_length(generic, 64), "FAIL CBC valid length");
            REQUIRE(!context.vtable->inner.check_length(generic, 0) &&
                    !context.vtable->inner.check_length(generic, 20000), "FAIL CBC bounds");
            for (size_t n = 65; n < 64 + implementations[i]->block_size; n++)
                REQUIRE(!context.vtable->inner.check_length(generic, n), "FAIL CBC block alignment");
        }
    }
    report("PASS malformed CBC record lengths rejected");
    return 0;
}

static unsigned validate_time(br_x509_minimal_context *x, int explicit_time)
{
    br_x509_minimal_init_full(x, NULL, 0);
    if (explicit_time) br_x509_minimal_set_time(x, 1, 0);
    x->vtable->start_chain(&x->vtable, NULL);
    x->vtable->start_cert(&x->vtable, sizeof time_test_certificate);
    x->vtable->append(&x->vtable, time_test_certificate, sizeof time_test_certificate);
    x->vtable->end_cert(&x->vtable);
    return x->vtable->end_chain(&x->vtable);
}

static int transcript(bearssl_test_workspace *w, bearssl_test_report report)
{
    br_sha256_context accumulator;
    br_sha256_init(&accumulator);
    for (size_t n = 0; n <= sizeof w->data; n += n < 128 ? 1 : 127) {
        unsigned char key[32], iv[12], tag[16], hash[32];
        for (size_t j = 0; j < sizeof key; j++) key[j] = (unsigned char)(n + j);
        for (size_t j = 0; j < sizeof iv; j++) iv[j] = (unsigned char)(n ^ j);
        for (size_t j = 0; j < n; j++) w->data[j] = (unsigned char)(j * 37 + n);
        br_sha256_context h;
        br_sha256_init(&h);
        br_sha256_update(&h, w->data, n);
        br_sha256_out(&h, hash);
        br_sha256_update(&accumulator, hash, sizeof hash);
        br_aes_ct64_ctr_keys aes;
        br_gcm_context gcm;
        br_aes_ct64_ctr_init(&aes, key, sizeof key);
        br_gcm_init(&gcm, &aes.vtable, br_ghash_ctmul64);
        br_gcm_reset(&gcm, iv, sizeof iv);
        br_gcm_aad_inject(&gcm, hash, sizeof hash);
        br_gcm_flip(&gcm);
        for (size_t j = 0; j < n;) {
            size_t chunk = n - j < 17 ? n - j : 17;
            br_gcm_run(&gcm, 1, w->data + j, chunk);
            j += chunk;
        }
        br_gcm_get_tag(&gcm, tag);
        br_sha256_update(&accumulator, w->data, n);
        br_sha256_update(&accumulator, tag, sizeof tag);
        br_poly1305_ctmul_run(key, iv, w->data, n, hash, sizeof hash,
                             tag, br_chacha20_ct_run, 1);
        br_sha256_update(&accumulator, w->data, n);
        br_sha256_update(&accumulator, tag, sizeof tag);
    }
    unsigned char digest[32];
    char line[8 + 64 + 1] = "DIGEST ";
    const char *hex = "0123456789abcdef";
    br_sha256_out(&accumulator, digest);
    for (size_t i = 0; i < sizeof digest; i++) {
        line[7 + 2 * i] = hex[digest[i] >> 4];
        line[8 + 2 * i] = hex[digest[i] & 15];
    }
    line[71] = 0;
    report(line);
    return 0;
}

int bearssl_foundation_test(bearssl_test_workspace *w, int port_checks,
                           bearssl_test_report report)
{
    if (gcm_vectors(report) || ecdsa_vectors(report) || decoder_failures(report) ||
        record_lengths(report)) return 1;
    if (port_checks) {
        REQUIRE(validate_time(&w->x509, 0) == BR_ERR_X509_TIME_UNKNOWN,
                "FAIL implicit time discovery enabled");
        REQUIRE(validate_time(&w->x509, 1) == BR_ERR_X509_EXPIRED,
                "FAIL explicit time ignored");
        const char *name = NULL;
        REQUIRE(br_prng_seeder_system(&name) == NULL && name && os64_streq(name, "none"),
                "FAIL implicit entropy provider enabled");
        REQUIRE(br_ec_get_default() == &br_ec_all_m31 &&
                br_ec_p256_m64_get() == NULL && br_ec_c25519_m64_get() == NULL &&
                br_aes_x86ni_ctr_get_vtable() == NULL && br_chacha20_sse2_get() == NULL,
                "FAIL scalar configuration");
        br_ssl_client_init_full(&w->client, &w->x509, NULL, 0);
        br_ssl_engine_set_buffer(&w->client.eng, w->records, sizeof w->records, 1);
        REQUIRE(!br_ssl_client_reset(&w->client, "example.test", 0) &&
                br_ssl_engine_last_error(&w->client.eng) == BR_ERR_NO_RANDOM,
                "FAIL unseeded client did not refuse handshake");
        // This fixed seed exists only in the fixture, never a production provider.
        const unsigned char seed[32] = {1, 2, 3, 4};
        br_ssl_client_init_full(&w->client, &w->x509, NULL, 0);
        br_ssl_engine_set_buffer(&w->client.eng, w->records, sizeof w->records, 1);
        br_ssl_engine_inject_entropy(&w->client.eng, seed, sizeof seed);
        REQUIRE(br_ssl_client_reset(&w->client, "example.test", 0), "FAIL seeded fixture reset");
        REQUIRE(br_ssl_engine_current_state(&w->client.eng) & BR_SSL_SENDREC,
                "FAIL fixture ClientHello unavailable");
    }
    return transcript(w, report);
}

const char *bearssl_test_license(void)
{
    return bearssl_license;
}
