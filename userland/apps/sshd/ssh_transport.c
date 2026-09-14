#include "ssh_internal.h"

static void hash(const void *p, size_t n, uint8_t out[32])
{
    br_sha256_context h; br_sha256_init(&h); br_sha256_update(&h, p, n); br_sha256_out(&h, out);
}
static void hash_string(br_sha256_context *h, const void *p, size_t n)
{
    uint8_t b[4]; ssh_writer w = {b, 0, sizeof(b), 0}; ssh_put_u32(&w, (uint32_t)n);
    br_sha256_update(h, b, 4); br_sha256_update(h, p, n);
}
static void queue(ssh_engine *s, const uint8_t *p, size_t n)
{
    if (n > SSH_OUTPUT_CAP - s->out_len) { s->closed = 1; return; }
    for (size_t i = 0; i < n; i++) s->output[(s->out_head + s->out_len + i) % SSH_OUTPUT_CAP] = p[i];
    s->out_len += n;
}
size_t ssh_output(ssh_engine *s, const uint8_t **data)
{
    *data = s->output + s->out_head;
    size_t n = SSH_OUTPUT_CAP - s->out_head;
    return n < s->out_len ? n : s->out_len;
}
void ssh_output_consume(ssh_engine *s, size_t n)
{
    if (n > s->out_len) n = s->out_len;
    s->out_head = (s->out_head + n) % SSH_OUTPUT_CAP; s->out_len -= n;
}
static void mac(ssh_direction *d, const uint8_t *p, size_t n, uint8_t out[32])
{
    uint8_t seq[4]; ssh_writer w = {seq, 0, 4, 0}; ssh_put_u32(&w, d->seq);
    br_hmac_context h; br_hmac_init(&h, &d->mac, 32);
    br_hmac_update(&h, seq, 4); br_hmac_update(&h, p, n); br_hmac_out(&h, out);
}
static size_t padding_for(size_t n, size_t block)
{
    size_t padding = block - ((n + 5) % block);
    return padding < 4 ? padding + block : padding;
}
/* Wire bytes an n-byte payload costs once framed, padded and, under live
 * transmit keys, authenticated. */
static size_t framed(size_t n, int keyed)
{
    return n + 5 + padding_for(n, keyed ? 16 : 8) + (keyed ? 32 : 0);
}
int ssh_packet_send(ssh_engine *s, const uint8_t *p, size_t n)
{
    if (s->closed) return 0;
    if (s->kex && n && p[0] >= 50) {
        if (n + 4 > sizeof(s->deferred) - s->deferred_len) {
            ssh_disconnect(s, 11, "rekey deferred reply limit"); return 0;
        }
        ssh_writer w = {s->deferred, s->deferred_len, sizeof(s->deferred), 0};
        ssh_put_string(&w, p, n); s->deferred_len = w.n;
        /* A reply is replayed after NEWKEYS, so it goes out under the new
         * keys whatever the transmit state is now. ssh_receive reserves
         * this much output room before it takes another packet. */
        s->deferred_wire += framed(n, 1); return 1;
    }
    size_t block = s->tx.active ? 16 : 8;
    size_t padding = padding_for(n, block);
    size_t total = n + padding + 5, wire = framed(n, s->tx.active);
    if (s->closed || total > SSH_PACKET_MAX + 4 || wire > SSH_OUTPUT_CAP - s->out_len) return 0;
    ssh_writer w = {s->scratch, 0, sizeof(s->scratch), 0};
    ssh_put_u32(&w, (uint32_t)total - 4); ssh_put_byte(&w, (uint8_t)padding);
    ssh_put_bytes(&w, p, n);
    br_hmac_drbg_generate(&s->rng, s->scratch + w.n, padding);
    if (s->tx.active) {
        mac(&s->tx, s->scratch, total, s->scratch + total);
        br_aes_ct64_ctrcbc_ctr(&s->tx.aes, s->tx.iv, s->scratch, total);
    }
    queue(s, s->scratch, wire); s->tx.seq++; s->tx.bytes += wire;
    return 1;
}
/* RFC 4253 section 11.4: every unrecognized message is answered with
 * UNIMPLEMENTED naming its sequence number, never with a disconnect. */
void ssh_unimplemented(ssh_engine *s)
{
    uint8_t response[5]; ssh_writer w = {response, 0, sizeof(response), 0};
    ssh_put_byte(&w, 3); ssh_put_u32(&w, s->rx.seq - 1); ssh_packet_send(s, response, w.n);
}
void ssh_disconnect(ssh_engine *s, uint32_t reason, const char *description)
{
    if (s->closed) return;
    size_t n = strlen(description); if (n >= sizeof(s->error)) n = sizeof(s->error) - 1;
    memcpy(s->error, description, n); s->error[n] = 0;
    uint8_t p[256]; ssh_writer w = {p, 0, sizeof(p), 0};
    ssh_put_byte(&w, 1); ssh_put_u32(&w, reason); ssh_put_text(&w, s->error); ssh_put_text(&w, "");
    ssh_packet_send(s, p, w.n); s->closed = 1;
}
static void install(ssh_engine *s, ssh_direction *d, int server)
{
    br_aes_ct64_ctrcbc_init(&d->aes, s->pending_keys[2 + server], 16);
    br_hmac_key_init(&d->mac, &br_sha256_vtable, s->pending_keys[4 + server], 32);
    memcpy(d->iv, s->pending_keys[server], 16);
    d->active = 1; d->bytes = 0; if (s->strict) d->seq = 0;
}
static void kex_send(ssh_engine *s)
{
    ssh_writer w = {s->server_kex, 0, sizeof(s->server_kex), 0};
    uint8_t cookie[16]; br_hmac_drbg_generate(&s->rng, cookie, sizeof(cookie));
    ssh_put_byte(&w, 20); ssh_put_bytes(&w, cookie, 16);
    ssh_put_text(&w, s->established ? "curve25519-sha256" :
                 "curve25519-sha256,kex-strict-s-v00@openssh.com");
    ssh_put_text(&w, SSH_KEY_TYPE);
    ssh_put_text(&w, "aes128-ctr"); ssh_put_text(&w, "aes128-ctr");
    ssh_put_text(&w, "hmac-sha2-256"); ssh_put_text(&w, "hmac-sha2-256");
    ssh_put_text(&w, "none"); ssh_put_text(&w, "none");
    ssh_put_text(&w, ""); ssh_put_text(&w, "");
    ssh_put_byte(&w, 0); ssh_put_u32(&w, 0);
    s->server_kex_len = w.n;
    if (!ssh_packet_send(s, w.p, w.n)) ssh_disconnect(s, 11, "output capacity during key exchange");
    s->kex = 1;
}
void ssh_rekey(ssh_engine *s)
{
    if (s->established && !s->kex && !s->closed && SSH_OUTPUT_CAP - s->out_len >= 2048) kex_send(s);
}
int ssh_init(ssh_engine *s, const uint8_t seed[32], const uint8_t host_key[32])
{
    memset(s, 0, sizeof(*s));
    if (!ssh_host_public(host_key, s->host_public)) return 0;
    memcpy(s->host_private, host_key, 32);
    br_hmac_drbg_init(&s->rng, &br_sha256_vtable, seed, 32);
    s->cols = 80; s->rows = 24;
    const char ident[] = SSH_IDENT "\r\n"; queue(s, (const uint8_t *)ident, sizeof(ident) - 1);
    kex_send(s); return 1;
}
/* Validate the complete name-list, including entries after the match. */
static int listed(ssh_reader list, const char *name, int *first)
{
    if (list.bad || list.n > 4096) return -1;
    size_t off = 0; int found = 0;
    if (first) *first = 0;
    while (off < list.n) {
        size_t end = off;
        while (end < list.n && list.p[end] != ',') {
            if (list.p[end] < 33 || list.p[end] > 126) return -1;
            end++;
        }
        if (end == off || end - off > 64) return -1;
        ssh_reader item = {list.p + off, end - off, 0};
        if (ssh_equal(item, name)) { found = 1; if (first && !off) *first = 1; }
        if (end < list.n && end + 1 == list.n) return -1;
        off = end + 1;
    }
    return found;
}
static void kex_client(ssh_engine *s, const uint8_t *p, size_t n)
{
    if (s->kex > 1 || n < 17 || n > sizeof(s->client_kex)) goto bad;
    ssh_reader r = {p + 17, n - 17, 0}, lists[10];
    for (int i = 0; i < 10; i++) lists[i] = ssh_string(&r);
    uint8_t follows = ssh_byte(&r); uint32_t reserved = ssh_u32(&r);
    if (r.bad || r.n || reserved || follows > 1) goto bad;
    const char *names[10] = {"curve25519-sha256", SSH_KEY_TYPE, "aes128-ctr", "aes128-ctr",
                           "hmac-sha2-256", "hmac-sha2-256", "none", "none", "", ""};
    int first_kex = 0, first_host = 0;
    for (int i = 0; i < 10; i++) {
        int result = listed(lists[i], names[i], i == 0 ? &first_kex : i == 1 ? &first_host : 0);
        if (result < 0) goto bad;
        if (!result && i < 8) { ssh_disconnect(s, 3, "no supported key exchange, host key, cipher or MAC"); return; }
    }
    if (!s->established) {
        s->strict = listed(lists[0], "kex-strict-c-v00@openssh.com", 0) == 1;
        if (s->strict && s->rx.seq != 1) goto bad;
    }
    if (!s->kex) kex_send(s);
    memcpy(s->client_kex, p, n); s->client_kex_len = n;
    s->skip_guess = follows && (!first_kex || !first_host);
    s->kex = 2; return;
bad:
    ssh_disconnect(s, 2, "malformed or out-of-order KEXINIT");
}
static void kex_ecdh(ssh_engine *s, const uint8_t *p, size_t n)
{
    ssh_reader r = {p + 1, n - 1, 0}, qc = ssh_string(&r);
    uint8_t scalar[32], qs[32], shared[32], h[32], signed_hash[32], raw[64];
    uint8_t host[SSH_KEY_BLOB_MAX], mp[37], reply[512], sig[128], inner[80];
    if (s->kex != 2 || r.bad || r.n || qc.n != 32) {
        ssh_disconnect(s, 3, "invalid X25519 exchange"); return;
    }
    br_hmac_drbg_generate(&s->rng, scalar, sizeof(scalar));
    br_ec_c25519_i31.mulgen(qs, scalar, 32, BR_EC_curve25519);
    memcpy(shared, qc.p, 32);
    int valid = br_ec_c25519_i31.mul(shared, 32, scalar, 32, BR_EC_curve25519);
    ssh_wipe(scalar, sizeof(scalar));
    uint8_t nz = 0; for (size_t i = 0; i < 32; i++) nz |= shared[i];
    if (!valid || !nz) { ssh_wipe(shared, 32); ssh_disconnect(s, 3, "invalid X25519 shared secret"); return; }
    size_t hostlen = ssh_public_blob(host, sizeof(host), s->host_public);
    ssh_writer mw = {mp, 0, sizeof(mp), 0}; ssh_put_mpint(&mw, shared, 32);
    br_sha256_context ctx; br_sha256_init(&ctx);
    hash_string(&ctx, s->client_ident, s->ident_len);
    hash_string(&ctx, SSH_IDENT, strlen(SSH_IDENT));
    hash_string(&ctx, s->client_kex, s->client_kex_len);
    hash_string(&ctx, s->server_kex, s->server_kex_len);
    hash_string(&ctx, host, hostlen); hash_string(&ctx, qc.p, qc.n); hash_string(&ctx, qs, 32);
    br_sha256_update(&ctx, mp, mw.n); br_sha256_out(&ctx, h);
    if (!s->established) memcpy(s->session_id, h, 32);
    for (int i = 0; i < 6; i++) {
        uint8_t letter = 'A' + i;
        br_sha256_init(&ctx); br_sha256_update(&ctx, mp, mw.n);
        br_sha256_update(&ctx, h, 32); br_sha256_update(&ctx, &letter, 1);
        br_sha256_update(&ctx, s->session_id, 32); br_sha256_out(&ctx, s->pending_keys[i]);
    }
    hash(h, 32, signed_hash);
    br_ec_private_key sk = {BR_EC_secp256r1, s->host_private, 32};
    size_t siglen = br_ecdsa_i31_sign_raw(&br_ec_p256_m31, &br_sha256_vtable, signed_hash, &sk, raw);
    ssh_wipe(shared, sizeof(shared)); ssh_wipe(mp, sizeof(mp)); ssh_wipe(&ctx, sizeof(ctx));
    if (siglen != 64) { ssh_disconnect(s, 3, "host signature failed"); return; }
    ssh_writer iw = {inner, 0, sizeof(inner), 0};
    ssh_put_mpint(&iw, raw, 32); ssh_put_mpint(&iw, raw + 32, 32);
    ssh_writer sw = {sig, 0, sizeof(sig), 0};
    ssh_put_text(&sw, SSH_KEY_TYPE); ssh_put_string(&sw, inner, iw.n);
    ssh_writer w = {reply, 0, sizeof(reply), 0};
    ssh_put_byte(&w, 31); ssh_put_string(&w, host, hostlen);
    ssh_put_string(&w, qs, 32); ssh_put_string(&w, sig, sw.n);
    if (w.bad || sw.bad || iw.bad || !ssh_packet_send(s, reply, w.n)) { ssh_disconnect(s, 3, "key exchange reply failed"); return; }
    uint8_t newkeys = 21; ssh_packet_send(s, &newkeys, 1);
    install(s, &s->tx, 1); s->kex = 3;
}
static void packet(ssh_engine *s, const uint8_t *p, size_t n)
{
    if (!n) { ssh_disconnect(s, 2, "empty packet"); return; }
    uint8_t type = p[0];
    if (type == 1) {
        ssh_reader r = {p + 1, n - 1, 0};
        (void)ssh_u32(&r); (void)ssh_string(&r); (void)ssh_string(&r);
        if (r.bad || r.n) ssh_disconnect(s, 2, "malformed disconnect");
        else s->closed = 1;
        return;
    }
    if (s->skip_guess) { s->skip_guess = 0; return; }
    if (type == 20) { kex_client(s, p, n); return; }
    if (type == 2 || type == 4 || type == 3) {
        /* IGNORE, DEBUG and UNIMPLEMENTED are legal at any time after
         * identification (RFC 4253 section 11). Strict KEX refuses them
         * during the initial exchange alone, where nothing is authenticated
         * yet, which is OpenSSH's own rule; a rekey runs under live keys
         * and keeps accepting them, as does a peer without strict KEX. */
        if (s->strict && !s->established) {
            ssh_disconnect(s, 2, "transport message during strict key exchange"); return;
        }
        ssh_reader r = {p + 1, n - 1, 0};
        if (type == 3) (void)ssh_u32(&r);
        else {
            if (type == 4 && ssh_byte(&r) > 1) r.bad = 1;
            (void)ssh_string(&r);
            if (type == 4) (void)ssh_string(&r);
        }
        if (r.bad || r.n) ssh_disconnect(s, 2, "malformed transport message");
        return;
    }
    if (s->kex) {
        if (type == 30 && s->kex == 2) { kex_ecdh(s, p, n); return; }
        if (type == 21 && s->kex == 3 && n == 1) {
            install(s, &s->rx, 0); ssh_wipe(s->pending_keys, sizeof(s->pending_keys));
            s->kex = 0; s->established = 1;
            /* The output room for every deferred reply was reserved before
             * this packet was taken, so a refusal here is a tripwire. */
            ssh_reader replies = {s->deferred, s->deferred_len, 0};
            while (replies.n && !s->closed) {
                ssh_reader reply = ssh_string(&replies);
                if (replies.bad || !ssh_packet_send(s, reply.p, reply.n)) {
                    ssh_disconnect(s, 11, "rekey reply output limit"); break;
                }
            }
            s->deferred_len = s->deferred_wire = 0; return;
        }
        /* A rekey's KEXINIT can cross connection messages the peer had
         * already sent; they are honoured until the peer's own KEXINIT
         * arrives. After that, the transport messages above aside, only
         * the exchange's own messages are legal. */
        if (s->established && s->kex == 1 && type >= 50) {
            ssh_connection_packet(s, p, n); return;
        }
        /* Strict KEX ends the initial exchange on any unexpected packet.
         * Otherwise a message this server knows (service request and
         * accept, ext-info, NEWKEYS, the kex-specific range, a userauth
         * request, a connection message), arriving out of order, is a
         * protocol error, while one it does not know earns UNIMPLEMENTED
         * (RFC 4253 section 11.4), as OpenSSH's kex_protocol_error does. */
        int known = type == 5 || type == 6 || type == 7 || type == 21 ||
                    (type >= 30 && type <= 50) || (type >= 80 && type <= 100);
        if ((s->strict && !s->established) || known) ssh_disconnect(s, 2, "unexpected packet during key exchange");
        else ssh_unimplemented(s);
        return;
    }
    if (type == 5) {
        ssh_reader r = {p + 1, n - 1, 0}, service = ssh_string(&r);
        if (s->service || r.bad || r.n || !ssh_equal(service, "ssh-userauth")) {
            ssh_disconnect(s, 7, "unsupported service"); return;
        }
        uint8_t response[32]; ssh_writer w = {response, 0, sizeof(response), 0};
        ssh_put_byte(&w, 6); ssh_put_text(&w, "ssh-userauth");
        ssh_packet_send(s, response, w.n); s->service = 1; return;
    }
    ssh_connection_packet(s, p, n);
}
size_t ssh_receive(ssh_engine *s, const uint8_t *data, size_t len)
{
    size_t used = 0; s->event = SSH_EVENT_NONE;
    /* Room for the largest reply one packet can draw, plus every deferred
     * reply NEWKEYS will replay. The budget's worst case is one-byte
     * replies, 5 deferred bytes and 48 wire bytes each; with one packet it
     * fits the queue, so draining always clears this. */
    _Static_assert(SSH_OUTPUT_CAP >= SSH_PACKET_MAX + 1024 + sizeof(((ssh_engine *)0)->deferred) / 5 * 48,
                   "deferred replay must fit the output queue");
    if (s->closed || SSH_OUTPUT_CAP - s->out_len < SSH_PACKET_MAX + 1024 + s->deferred_wire) return 0;
    while (used < len && !s->closed) {
        if (!s->identified) {
            uint8_t c = data[used++];
            if (c == '\n') {
                if (s->ident_len && s->client_ident[s->ident_len - 1] == '\r') s->ident_len--;
                s->client_ident[s->ident_len] = 0;
                if (s->ident_len < 9 || memcmp(s->client_ident, "SSH-2.0-", 8)) {
                    ssh_disconnect(s, 8, "SSH-2.0 identification required"); break;
                }
                s->identified = 1; continue;
            }
            if ((!c || c < 32 || c > 126) && c != '\r') {
                ssh_disconnect(s, 2, "invalid identification"); break;
            }
            /* RFC 4253 section 4.2 allows 255 bytes including CRLF: 253 of
             * content, then the CR may stand at index 253. */
            if (s->ident_len >= (c == '\r' ? 254u : 253u) || (s->ident_len && s->client_ident[s->ident_len - 1] == '\r')) {
                ssh_disconnect(s, 2, "invalid identification length or CR"); break;
            }
            s->client_ident[s->ident_len++] = (char)c; continue;
        }
        size_t block = s->rx.active ? 16 : 8;
        size_t want = s->packet_need ? s->packet_need : block;
        size_t take = want - s->packet_have; if (take > len - used) take = len - used;
        memcpy(s->packet + s->packet_have, data + used, take);
        s->packet_have += take; used += take;
        if (!s->packet_need && s->packet_have == block) {
            if (s->rx.active) br_aes_ct64_ctrcbc_ctr(&s->rx.aes, s->rx.iv, s->packet, block);
            ssh_reader r = {s->packet, 4, 0}; uint32_t length = ssh_u32(&r);
            if (length < 12 || length > SSH_PACKET_MAX || (length + 4) % block) {
                ssh_disconnect(s, 2, "invalid packet length"); break;
            }
            s->packet_need = length + 4 + (s->rx.active ? 32 : 0);
        }
        if (!s->packet_need || s->packet_have < s->packet_need) continue;
        size_t total = s->packet_need - (s->rx.active ? 32 : 0);
        if (s->rx.active) {
            br_aes_ct64_ctrcbc_ctr(&s->rx.aes, s->rx.iv, s->packet + block, total - block);
            uint8_t tag[32]; mac(&s->rx, s->packet, total, tag);
            uint8_t diff = 0; for (size_t i = 0; i < 32; i++) diff |= tag[i] ^ s->packet[total+i];
            if (diff) { ssh_disconnect(s, 5, "packet authentication failed"); break; }
        }
        uint8_t pad = s->packet[4];
        if (pad < 4 || (size_t)pad + 6 > total) { ssh_disconnect(s, 2, "invalid padding"); break; }
        s->rx.seq++; s->rx.bytes += s->packet_need;
        s->packet_have = s->packet_need = 0;
        packet(s, s->packet + 5, total - pad - 5);
        /* One packet per call gives the host an opportunity to service I/O
         * and keeps events and replies bounded under a busy peer. */
        break;
    }
    return used;
}
