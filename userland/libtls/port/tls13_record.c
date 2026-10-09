#include "tls13_internal.h"
#include "os64/str.h"

_Static_assert(TLS13_SEND_MAX >= 5 + 16384 + 1 + 16, "TLS 1.3 send record capacity");

int tls13_record_init(tls13_record *r, const tls13_suite *s, const void *key, const void *iv)
{
    if (!r || !s || !key || !iv || tls13_suite_find(s->id) != s) return BR_ERR_BAD_PARAM;
    if (r->error) return r->error;
    unsigned char k[TLS13_KEY_MAX] = {0}, v[TLS13_IV_SIZE];
    os64_memcpy(k, key, s->key_len); os64_memcpy(v, iv, sizeof v);
    tls13_wipe(r, sizeof *r);
    r->suite = s;
    os64_memcpy(r->key, k, sizeof k); os64_memcpy(r->iv, v, sizeof v);
    if (!s->chacha) br_aes_ct64_ctr_init(&r->aes, k, s->key_len);
    tls13_wipe(k, sizeof k); tls13_wipe(v, sizeof v);
    return 0;
}
bool tls13_record_needs_update(const tls13_record *r)
{
    return r && r->suite && !r->error && (r->exhausted || r->sequence >= TLS13_UPDATE_RECORDS);
}
static int fail(tls13_record *r, unsigned char *buffer, size_t length, int error)
{
    if (buffer) tls13_wipe(buffer, length);
    if (!r->error) {
        tls13_wipe(r, sizeof *r);
        r->error = error;
    }
    return r->error;
}
static int ready(tls13_record *r)
{
    if (r->error) return r->error;
    if (!r->suite) return BR_ERR_BAD_STATE;
    if (r->exhausted) return fail(r, NULL, 0, BR_ERR_LIMIT_EXCEEDED);
    return 0;
}
static void advance(tls13_record *r)
{
    // The final sequence number is usable, but it must not wrap to a reused nonce.
    if (r->sequence == UINT64_MAX) r->exhausted = true;
    else r->sequence++;
}
static bool crypt(tls13_record *r, unsigned char *buffer, size_t length, bool encrypt)
{
    unsigned char nonce[TLS13_IV_SIZE], tag[TLS13_TAG_SIZE];
    os64_memcpy(nonce, r->iv, sizeof nonce);
    for (unsigned i = 0; i < 8; i++) nonce[11 - i] ^= (unsigned char)(r->sequence >> (8 * i));
    bool valid;
    if (r->suite->chacha) {
        br_poly1305_ctmul_run(r->key, nonce, buffer + 5, length, buffer, 5,
                             tag, br_chacha20_ct_run, encrypt);
        valid = encrypt || tls13_equal(tag, buffer + 5 + length, sizeof tag);
        if (encrypt) os64_memcpy(buffer + 5 + length, tag, sizeof tag);
    } else {
        br_gcm_context gcm;
        br_gcm_init(&gcm, &r->aes.vtable, br_ghash_ctmul64);
        br_gcm_reset(&gcm, nonce, sizeof nonce);
        br_gcm_aad_inject(&gcm, buffer, 5);
        br_gcm_flip(&gcm);
        br_gcm_run(&gcm, encrypt, buffer + 5, length);
        if (encrypt) { br_gcm_get_tag(&gcm, buffer + 5 + length); valid = true; }
        else valid = br_gcm_check_tag(&gcm, buffer + 5 + length) != 0;
        tls13_wipe(&gcm, sizeof gcm);
    }
    tls13_wipe(nonce, sizeof nonce); tls13_wipe(tag, sizeof tag);
    return valid;
}
static int content_valid(unsigned type, size_t length, bool application_allowed)
{
    if (type == TLS13_HANDSHAKE) return length ? 0 : BR_ERR_UNEXPECTED;
    if (type == TLS13_ALERT) return length == 2 ? 0 : BR_ERR_BAD_ALERT;
    if (type == TLS13_APPLICATION) return application_allowed ? 0 : BR_ERR_UNEXPECTED;
    return type == TLS13_CCS ? BR_ERR_BAD_CCS : BR_ERR_UNEXPECTED;
}
int tls13_record_seal(tls13_record *r, unsigned type, unsigned char *buffer,
                      size_t content_len, size_t capacity, size_t *wire_len)
{
    if (wire_len) *wire_len = 0;
    if (!r || !buffer || !wire_len) return BR_ERR_BAD_PARAM;
    int error = ready(r);
    if (error) return error;
    if (content_len > TLS13_CONTENT_MAX) return BR_ERR_TOO_LARGE;
    if (capacity < 5 + content_len + 1 + TLS13_TAG_SIZE) return BR_ERR_BAD_PARAM;
    error = content_valid(type, content_len, true);
    if (error) return error;
    size_t payload = content_len + 1 + TLS13_TAG_SIZE;
    buffer[0] = TLS13_APPLICATION; buffer[1] = 3; buffer[2] = 3;
    buffer[3] = (unsigned char)(payload >> 8); buffer[4] = (unsigned char)payload;
    buffer[5 + content_len] = (unsigned char)type;
    crypt(r, buffer, content_len + 1, true);
    advance(r);
    *wire_len = 5 + payload;
    return 0;
}
int tls13_record_length(const unsigned char header[5], bool encrypted, size_t *payload_len)
{
    if (payload_len) *payload_len = 0;
    if (!header || !payload_len) return BR_ERR_BAD_PARAM;
    size_t n = ((size_t)header[3] << 8) | header[4];
    if (n > (encrypted ? TLS13_CIPHER_MAX : TLS13_CONTENT_MAX)) return BR_ERR_TOO_LARGE;
    if (encrypted) {
        if (header[0] != TLS13_APPLICATION) return BR_ERR_UNEXPECTED;
        if (n < TLS13_TAG_SIZE + 1) return BR_ERR_BAD_MAC;
    } else if (header[0] != TLS13_HANDSHAKE && header[0] != TLS13_ALERT && header[0] != TLS13_CCS) {
        return BR_ERR_UNEXPECTED;
    }
    *payload_len = n;
    return 0;
}
int tls13_record_open(tls13_record *r, unsigned char *buffer, size_t wire_len,
                      bool application_allowed, unsigned *type, size_t *content_len)
{
    if (type) *type = 0;
    if (content_len) *content_len = 0;
    if (!r || !buffer || !type || !content_len) return BR_ERR_BAD_PARAM;
    int error = ready(r);
    if (error) return fail(r, buffer, wire_len, error);
    if (wire_len < 5) return fail(r, buffer, wire_len, BR_ERR_BAD_LENGTH);
    size_t n;
    error = tls13_record_length(buffer, true, &n);
    if (error) return fail(r, buffer, wire_len, error);
    if (wire_len != 5 + n) return fail(r, buffer, wire_len, BR_ERR_BAD_LENGTH);
    n -= TLS13_TAG_SIZE;
    if (!crypt(r, buffer, n, false)) return fail(r, buffer, wire_len, BR_ERR_BAD_MAC);
    if (n > TLS13_INNER_MAX) return fail(r, buffer, wire_len, BR_ERR_TOO_LARGE);
    size_t at = n;
    while (at && !buffer[5 + at - 1]) at--;
    if (!at) return fail(r, buffer, wire_len, BR_ERR_UNEXPECTED);
    unsigned inner_type = buffer[5 + at - 1];
    error = content_valid(inner_type, at - 1, application_allowed);
    if (error) return fail(r, buffer, wire_len, error);
    advance(r);
    *type = inner_type; *content_len = at - 1;
    // Expose content alone and erase decrypted padding and tag storage.
    tls13_wipe(buffer + 5 + *content_len, wire_len - 5 - *content_len);
    return 0;
}
int tls13_plaintext_open(const unsigned char *buffer, size_t wire_len,
                         bool ccs_allowed, unsigned *type, size_t *content_len)
{
    if (type) *type = 0;
    if (content_len) *content_len = 0;
    if (!buffer || !type || !content_len) return BR_ERR_BAD_PARAM;
    if (wire_len < 5) return BR_ERR_BAD_LENGTH;
    size_t n;
    int error = tls13_record_length(buffer, false, &n);
    if (error) return error;
    if (wire_len != 5 + n) return BR_ERR_BAD_LENGTH;
    if (buffer[0] == TLS13_CCS) {
        if (!ccs_allowed || n != 1 || buffer[5] != 1) return BR_ERR_BAD_CCS;
        // Success with type CCS tells the engine to discard it, without hashing.
    } else {
        error = content_valid(buffer[0], n, false);
        if (error) return error;
    }
    *type = buffer[0]; *content_len = n;
    return 0;
}
