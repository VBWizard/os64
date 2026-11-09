#include "ssh_internal.h"

void ssh_wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}
uint8_t ssh_byte(ssh_reader *r)
{
    if (!r->n) { r->bad = 1; return 0; }
    uint8_t b = *r->p++; r->n--; return b;
}
uint32_t ssh_u32(ssh_reader *r)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | ssh_byte(r);
    return v;
}
ssh_reader ssh_string(ssh_reader *r)
{
    uint32_t n = ssh_u32(r);
    if (r->bad || n > r->n) { r->bad = 1; return (ssh_reader){0, 0, 1}; }
    ssh_reader v = {r->p, n, 0}; r->p += n; r->n -= n; return v;
}
int ssh_equal(ssh_reader r, const char *s)
{ return !r.bad && r.n == strlen(s) && !memcmp(r.p, s, r.n); }
void ssh_put_bytes(ssh_writer *w, const void *p, size_t n)
{
    if (w->bad || n > w->cap - w->n) { w->bad = 1; return; }
    if (n) memcpy(w->p + w->n, p, n);
    w->n += n;
}
void ssh_put_byte(ssh_writer *w, uint8_t n) { ssh_put_bytes(w, &n, 1); }
void ssh_put_u32(ssh_writer *w, uint32_t n)
{
    uint8_t p[4] = {n >> 24, n >> 16, n >> 8, n}; ssh_put_bytes(w, p, 4);
}
void ssh_put_string(ssh_writer *w, const void *p, size_t n)
{ ssh_put_u32(w, (uint32_t)n); ssh_put_bytes(w, p, n); }
void ssh_put_text(ssh_writer *w, const char *s) { ssh_put_string(w, s, strlen(s)); }
void ssh_put_mpint(ssh_writer *w, const uint8_t *p, size_t n)
{
    while (n && !*p) { p++; n--; }
    int pad = n && (*p & 128);
    ssh_put_u32(w, (uint32_t)n + pad);
    if (pad) ssh_put_byte(w, 0);
    ssh_put_bytes(w, p, n);
}
int ssh_host_public(const uint8_t private_key[32], uint8_t public_key[65])
{
    size_t n;
    const uint8_t *order = br_ec_p256_m31.order(BR_EC_secp256r1, &n);
    uint8_t nonzero = 0;
    for (size_t i = 0; i < 32; i++) nonzero |= private_key[i];
    if (!nonzero || n != 32 || memcmp(private_key, order, 32) >= 0) return 0;
    br_ec_private_key sk = {BR_EC_secp256r1, (uint8_t *)private_key, 32};
    br_ec_public_key pk;
    return br_ec_compute_pub(&br_ec_p256_m31, &pk, public_key, &sk) == 65;
}
size_t ssh_public_blob(uint8_t *out, size_t cap, const uint8_t public_key[65])
{
    ssh_writer w = {out, 0, cap, 0};
    ssh_put_text(&w, SSH_KEY_TYPE); ssh_put_text(&w, "nistp256");
    ssh_put_string(&w, public_key, 65); return w.bad ? 0 : w.n;
}
int ssh_parse_public(const uint8_t *blob, size_t len, uint8_t point[65])
{
    ssh_reader r = {blob, len, 0};
    ssh_reader type = ssh_string(&r), curve = ssh_string(&r), q = ssh_string(&r);
    if (r.bad || r.n || !ssh_equal(type, SSH_KEY_TYPE) ||
        !ssh_equal(curve, "nistp256") || q.n != 65 || q.p[0] != 4) return 0;
    memcpy(point, q.p, 65);
    uint8_t check[65], one = 1; memcpy(check, point, 65);
    return br_ec_p256_m31.mul(check, 65, &one, 1, BR_EC_secp256r1) == 1;
}
static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
size_t ssh_base64_encode(const uint8_t *p, size_t n, char *out, size_t cap)
{
    size_t need = ((n + 2) / 3) * 4, o = 0;
    if (cap <= need) return 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t x = (uint32_t)p[i] << 16;
        if (i + 1 < n) x |= (uint32_t)p[i+1] << 8;
        if (i + 2 < n) x |= p[i+2];
        out[o++] = b64[x >> 18]; out[o++] = b64[(x >> 12) & 63];
        out[o++] = i+1 < n ? b64[(x >> 6) & 63] : '=';
        out[o++] = i+2 < n ? b64[x & 63] : '=';
    }
    out[o] = 0; return o;
}
int ssh_base64_decode(const char *s, size_t n, uint8_t *out, size_t cap)
{
    size_t o = 0;
    if (!n || n % 4) return -1;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t x = 0; int pad = 0;
        for (int j = 0; j < 4; j++) {
            const char *v = 0;
            for (size_t k = 0; k < 64; k++)
                if (s[i+j] == b64[k]) { v = b64 + k; break; }
            if (s[i+j] == '=' && i+4 == n && j >= 2) { pad++; x <<= 6; }
            else if (!v || pad) return -1;
            else x = (x << 6) | (uint32_t)(v - b64);
        }
        if (pad > 2 || (pad == 1 && (x & 255)) || (pad == 2 && (x & 65535))) return -1;
        if (cap - o < (size_t)(3 - pad)) return -1;
        out[o++] = x >> 16;
        if (pad < 2) out[o++] = x >> 8;
        if (!pad) out[o++] = x;
    }
    return (int)o;
}
