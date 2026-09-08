#include "client_profile.h"

void os64_bearssl_client_algorithms(br_ssl_client_context *client)
{
    static const uint16_t suites[] = {
        BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384
    };
    static const br_hash_class *const hashes[] = {
        &br_sha256_vtable, &br_sha384_vtable, &br_sha512_vtable
    };

    br_ssl_client_zero(client);
    br_ssl_engine_set_versions(&client->eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(&client->eng, suites, sizeof suites / sizeof suites[0]);
    br_ssl_engine_add_flags(&client->eng,
        BR_OPT_NO_RENEGOTIATION | BR_OPT_FAIL_ON_ALPN_MISMATCH);
    br_ssl_engine_set_prf_sha256(&client->eng, br_tls12_sha256_prf);
    br_ssl_engine_set_prf_sha384(&client->eng, br_tls12_sha384_prf);
    br_ssl_engine_set_ec(&client->eng, &br_ec_all_m31);
    br_ssl_engine_set_rsavrfy(&client->eng, br_rsa_i31_pkcs1_vrfy);
    br_ssl_engine_set_ecdsa(&client->eng, br_ecdsa_i31_vrfy_asn1);

    for (size_t i = 0; i < sizeof hashes / sizeof hashes[0]; i++)
        br_ssl_engine_set_hash(&client->eng, br_sha256_ID + (int)i, hashes[i]);

    br_ssl_engine_set_aes_ctr(&client->eng, &br_aes_ct64_ctr_vtable);
    br_ssl_engine_set_ghash(&client->eng, br_ghash_ctmul64);
    br_ssl_engine_set_gcm(&client->eng, &br_sslrec_in_gcm_vtable,
                         &br_sslrec_out_gcm_vtable);
    br_ssl_engine_set_chacha20(&client->eng, br_chacha20_ct_run);
    br_ssl_engine_set_poly1305(&client->eng, br_poly1305_ctmul_run);
    br_ssl_engine_set_chapol(&client->eng, &br_sslrec_in_chapol_vtable,
                            &br_sslrec_out_chapol_vtable);
}

void os64_bearssl_client_profile(br_ssl_client_context *client,
                                br_x509_minimal_context *x509,
                                const br_x509_trust_anchor *anchors,
                                size_t anchor_count)
{
    os64_bearssl_client_algorithms(client);
    br_x509_minimal_init(x509, &br_sha256_vtable, anchors, anchor_count);
    br_x509_minimal_set_rsa(x509, br_rsa_i31_pkcs1_vrfy);
    br_x509_minimal_set_ecdsa(x509, &br_ec_all_m31, br_ecdsa_i31_vrfy_asn1);
    // This byte limit excludes anchors; actual bit lengths and anchor policy
    // need the additional checks described in TLS.md.
    br_x509_minimal_set_minrsa(x509, 256);
    br_x509_minimal_set_hash(x509, br_sha256_ID, &br_sha256_vtable);
    br_x509_minimal_set_hash(x509, br_sha384_ID, &br_sha384_vtable);
    br_x509_minimal_set_hash(x509, br_sha512_ID, &br_sha512_vtable);
    br_ssl_engine_set_x509(&client->eng, &x509->vtable);
}
