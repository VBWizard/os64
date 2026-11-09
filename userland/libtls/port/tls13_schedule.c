#include "tls13_internal.h"
#include "os64/str.h"

static const tls13_suite suites[] = {
    {0x1301, &br_sha256_vtable, 32, 16, false},
    {0x1302, &br_sha384_vtable, 48, 32, false},
    {0x1303, &br_sha256_vtable, 32, 32, true}
};
const tls13_group tls13_groups[] = {
    {29, BR_EC_curve25519, 32, 32},
    {23, BR_EC_secp256r1, 65, 32},
    {24, BR_EC_secp384r1, 97, 48}
};
const size_t tls13_group_count = sizeof tls13_groups / sizeof *tls13_groups;
const tls13_suite *tls13_suite_find(uint16_t id)
{
    for (size_t i = 0; i < sizeof suites / sizeof *suites; i++)
        if (suites[i].id == id) return &suites[i];
    return NULL;
}
static bool valid_suite(const tls13_suite *s)
{
    for (size_t i = 0; i < sizeof suites / sizeof *suites; i++)
        if (s == &suites[i]) return true;
    return false;
}
const tls13_group *tls13_group_find(uint16_t id)
{
    for (size_t i = 0; i < tls13_group_count; i++)
        if (tls13_groups[i].id == id) return &tls13_groups[i];
    return NULL;
}
void tls13_wipe(void *data, size_t length)
{
    volatile unsigned char *p = data;
    while (length--) *p++ = 0;
}
bool tls13_equal(const void *a, const void *b, size_t length)
{
    const unsigned char *x = a, *y = b;
    unsigned diff = 0;
    for (size_t i = 0; i < length; i++) diff |= x[i] ^ y[i];
    return diff == 0;
}
static void hmac(const tls13_suite *s, const void *key, const void *data, size_t len, void *out)
{
    br_hmac_key_context k;
    br_hmac_context h;
    br_hmac_key_init(&k, s->hash, key, s->hash_len);
    br_hmac_init(&h, &k, 0);
    br_hmac_update(&h, data, len);
    br_hmac_out(&h, out);
    tls13_wipe(&h, sizeof h);
    tls13_wipe(&k, sizeof k);
}
int tls13_extract(const tls13_suite *s, const void *salt,
                  const void *ikm, size_t ikm_len, void *out)
{
    if (!valid_suite(s) || !salt || (!ikm && ikm_len) || !out) return BR_ERR_BAD_PARAM;
    hmac(s, salt, ikm, ikm_len, out);
    return 0;
}
int tls13_expand_label(const tls13_suite *s, const void *secret, const char *label,
                       const void *context, size_t context_len, void *out, size_t length)
{
    if (!valid_suite(s) || !secret || !label || !out || !length || length > s->hash_len ||
        context_len > 255 || (!context && context_len)) return BR_ERR_BAD_PARAM;
    size_t label_len = 0;
    while (label_len <= 249 && label[label_len]) label_len++;
    if (!label_len || label_len > 249) return BR_ERR_BAD_PARAM;
    // RFC 8446 section 7.1: these suites' key/IV/secret outputs fit one
    // HKDF block. The label includes its vector length and the "tls13 " prefix.
    unsigned char info[2 + 1 + 255 + 1 + 255 + 1], result[TLS13_HASH_MAX];
    size_t n = 0;
    info[n++] = 0; info[n++] = (unsigned char)length;
    info[n++] = (unsigned char)(6 + label_len);
    os64_memcpy(info + n, "tls13 ", 6); n += 6;
    os64_memcpy(info + n, label, label_len); n += label_len;
    info[n++] = (unsigned char)context_len;
    if (context_len) os64_memcpy(info + n, context, context_len);
    n += context_len;
    info[n++] = 1;
    hmac(s, secret, info, n, result);
    os64_memcpy(out, result, length);
    tls13_wipe(result, sizeof result);
    tls13_wipe(info, sizeof info);
    return 0;
}
void tls13_transcript_init(br_multihash_context *t)
{
    br_multihash_zero(t);
    br_multihash_setimpl(t, br_sha256_ID, &br_sha256_vtable);
    br_multihash_setimpl(t, br_sha384_ID, &br_sha384_vtable);
    br_multihash_init(t);
}
static int digest(const tls13_suite *s, const br_multihash_context *t, void *out)
{
    int id = s->hash_len == 32 ? br_sha256_ID : br_sha384_ID;
    return br_multihash_out(t, id, out) == s->hash_len ? 0 : BR_ERR_BAD_STATE;
}
int tls13_derive_secret(const tls13_suite *s, const void *secret, const char *label,
                        const br_multihash_context *t, void *out)
{
    if (!valid_suite(s) || !t) return BR_ERR_BAD_PARAM;
    unsigned char hash[TLS13_HASH_MAX];
    int error = digest(s, t, hash);
    if (!error) error = tls13_expand_label(s, secret, label, hash, s->hash_len, out, s->hash_len);
    tls13_wipe(hash, sizeof hash);
    return error;
}
int tls13_transcript_retry(br_multihash_context *t, const tls13_suite *s)
{
    if (!valid_suite(s) || !t) return BR_ERR_BAD_PARAM;
    unsigned char synthetic[4 + TLS13_HASH_MAX] = {254, 0, 0, 0};
    synthetic[3] = (unsigned char)s->hash_len;
    int error = digest(s, t, synthetic + 4);
    if (!error) {
        // Drop the unselected hash: HRR fixes the suite before replacing CH1.
        br_multihash_zero(t);
        int id = s->hash_len == 32 ? br_sha256_ID : br_sha384_ID;
        br_multihash_setimpl(t, id, s->hash);
        br_multihash_init(t);
        br_multihash_update(t, synthetic, 4 + s->hash_len);
    }
    tls13_wipe(synthetic, sizeof synthetic);
    return error;
}
int tls13_traffic_keys(const tls13_suite *s, const void *secret, void *key, void *iv)
{
    if (!valid_suite(s) || !secret || !key || !iv) return BR_ERR_BAD_PARAM;
    unsigned char k[TLS13_KEY_MAX], v[TLS13_IV_SIZE];
    int error = tls13_expand_label(s, secret, "key", NULL, 0, k, s->key_len);
    if (!error) error = tls13_expand_label(s, secret, "iv", NULL, 0, v, sizeof v);
    if (!error) { os64_memcpy(key, k, s->key_len); os64_memcpy(iv, v, sizeof v); }
    tls13_wipe(k, sizeof k); tls13_wipe(v, sizeof v);
    return error;
}
int tls13_finished(const tls13_suite *s, const void *secret,
                   const br_multihash_context *t, void *out)
{
    if (!valid_suite(s) || !secret || !t || !out) return BR_ERR_BAD_PARAM;
    unsigned char key[TLS13_HASH_MAX], hash[TLS13_HASH_MAX];
    int error = digest(s, t, hash);
    if (!error) error = tls13_expand_label(s, secret, "finished", NULL, 0, key, s->hash_len);
    if (!error) hmac(s, key, hash, s->hash_len, out);
    tls13_wipe(key, sizeof key); tls13_wipe(hash, sizeof hash);
    return error;
}
int tls13_update_secret(const tls13_suite *s, void *secret)
{
    if (!valid_suite(s) || !secret) return BR_ERR_BAD_PARAM;
    // Expand stages its result, so in-place replacement does not erase its input.
    return tls13_expand_label(s, secret, "traffic upd", NULL, 0, secret, s->hash_len);
}
int tls13_shared_secret(const tls13_group *g, const void *scalar, size_t scalar_len,
                         const void *peer, size_t peer_len, void *out)
{
    bool valid = false;
    for (size_t i = 0; i < tls13_group_count; i++)
        if (g == &tls13_groups[i]) valid = true;
    if (!valid || !scalar || !peer || !out || scalar_len != g->secret_len || peer_len != g->point_len)
        return BR_ERR_BAD_PARAM;
    unsigned char point[97], zero[48] = {0};
    os64_memcpy(point, peer, peer_len);
    // Bear validates NIST points during multiplication; X25519 also needs the
    // all-zero result check. Keep the padded X coordinate, including leading 0s.
    int error = 0;
    if (!br_ec_all_m31.mul(point, peer_len, scalar, scalar_len, g->curve)) error = BR_ERR_INVALID_ALGORITHM;
    size_t length;
    size_t offset = br_ec_all_m31.xoff(g->curve, &length);
    if (length != g->secret_len || offset > peer_len || length > peer_len - offset)
        error = BR_ERR_INVALID_ALGORITHM;
    if (!error && g->curve == BR_EC_curve25519 && tls13_equal(point + offset, zero, length))
        error = BR_ERR_INVALID_ALGORITHM;
    if (!error) os64_memcpy(out, point + offset, length);
    else tls13_wipe(out, g->secret_len);
    tls13_wipe(point, sizeof point);
    return error;
}
