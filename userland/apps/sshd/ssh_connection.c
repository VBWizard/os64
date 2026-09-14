#include "ssh_internal.h"

static void failure(ssh_engine *s)
{
    uint8_t p[32]; ssh_writer w = {p, 0, sizeof(p), 0};
    ssh_put_byte(&w, 51); ssh_put_text(&w, "publickey"); ssh_put_byte(&w, 0);
    ssh_packet_send(s, p, w.n);
    if (++s->failures >= 5) ssh_disconnect(s, 14, "publickey authentication failed five times");
}
static int signature_integer(ssh_reader r, uint8_t out[32])
{
    if (r.bad || !r.n || r.n > 33 || (r.p[0] & 128)) return 0;
    if (r.p[0] == 0) {
        if (r.n == 1 || !(r.p[1] & 128)) return 0;
        r.p++; r.n--;
    }
    if (r.n > 32) return 0;
    memset(out, 0, 32); memcpy(out + 32 - r.n, r.p, r.n); return 1;
}
static void auth(ssh_engine *s, const uint8_t *p, size_t n)
{
    if (!s->service || n > 8192) { ssh_disconnect(s, 2, "userauth outside service or over limit"); return; }
    ssh_reader r = {p + 1, n - 1, 0};
    ssh_reader user = ssh_string(&r), service = ssh_string(&r), method = ssh_string(&r);
    if (r.bad || !user.n || user.n >= sizeof(s->username)) goto malformed;
    for (size_t i = 0; i < user.n; i++) if (user.p[i] < 32 || user.p[i] > 126) goto malformed;
    if (!ssh_equal(service, "ssh-connection")) { failure(s); return; }
    if (!ssh_equal(method, "publickey")) { failure(s); return; }
    uint8_t signed_request = ssh_byte(&r);
    ssh_reader algorithm = ssh_string(&r), key = ssh_string(&r);
    if (r.bad || signed_request > 1) goto malformed;
    size_t signed_length = n - r.n;
    ssh_reader signature = {0, 0, 0}; if (signed_request) signature = ssh_string(&r);
    if (r.bad || r.n) goto malformed;
    if (!ssh_equal(algorithm, SSH_KEY_TYPE)) { failure(s); return; }
    int enrolled = 0;
    for (size_t i = 0; i < s->key_count; i++)
        if (s->keys[i].len == key.n && !memcmp(s->keys[i].blob, key.p, key.n)) enrolled = 1;
    uint8_t point[65];
    if (!enrolled || !ssh_parse_public(key.p, key.n, point)) { failure(s); return; }
    if (!signed_request) {
        uint8_t response[160]; ssh_writer w = {response, 0, sizeof(response), 0};
        ssh_put_byte(&w, 60); ssh_put_text(&w, SSH_KEY_TYPE); ssh_put_string(&w, key.p, key.n);
        ssh_packet_send(s, response, w.n); return;
    }
    ssh_reader sigtype = ssh_string(&signature), pair = ssh_string(&signature);
    if (signature.bad || signature.n || !ssh_equal(sigtype, SSH_KEY_TYPE)) { failure(s); return; }
    ssh_reader rr = ssh_string(&pair), ss = ssh_string(&pair);
    uint8_t raw[64], digest[32], id[36];
    if (pair.bad || pair.n || !signature_integer(rr, raw) || !signature_integer(ss, raw+32)) { failure(s); return; }
    ssh_writer w = {id, 0, sizeof(id), 0}; ssh_put_string(&w, s->session_id, 32);
    br_sha256_context h; br_sha256_init(&h); br_sha256_update(&h, id, w.n);
    br_sha256_update(&h, p, signed_length); br_sha256_out(&h, digest);
    br_ec_public_key pk = {BR_EC_secp256r1, point, 65};
    if (!br_ecdsa_i31_vrfy_raw(&br_ec_p256_m31, digest, 32, &pk, raw, 64)) { failure(s); return; }
    memcpy(s->username, user.p, user.n); s->username[user.n] = 0;
    s->authenticated = 1; s->event = SSH_EVENT_AUTH;
    uint8_t success = 52; ssh_packet_send(s, &success, 1); return;
malformed:
    ssh_disconnect(s, 2, "malformed publickey userauth");
}
static void reply(ssh_engine *s, int success)
{
    if (!s->request_reply) return;
    uint8_t p[5]; ssh_writer w = {p, 0, sizeof(p), 0};
    ssh_put_byte(&w, success ? 99 : 100); ssh_put_u32(&w, s->peer_channel);
    ssh_packet_send(s, p, w.n);
}
void ssh_start_result(ssh_engine *s, int success)
{
    s->started = success; reply(s, success);
}
void ssh_resize_result(ssh_engine *s, int success)
{
    if (s->event != SSH_EVENT_RESIZE) return;
    if (success) { s->cols = s->resize_cols; s->rows = s->resize_rows; }
    s->event = SSH_EVENT_NONE;
    reply(s, success);
}
static int dimensions(ssh_reader *r, uint32_t *cols, uint32_t *rows)
{
    *cols = ssh_u32(r); *rows = ssh_u32(r);
    (void)ssh_u32(r); (void)ssh_u32(r);
    /* Match pty_create_slave/tty_resize; SSH zero means unspecified. */
    return !r->bad && (!*cols || (*cols >= 2 && *cols <= 512)) &&
           (!*rows || (*rows >= 2 && *rows <= 256));
}
static int modes_ok(ssh_reader r)
{
    /* STREAM PTYs own their line discipline. Parse the RFC's opcode/value
     * framing, but do not claim support for a client-requested raw mode. */
    while (r.n) {
        uint8_t op = ssh_byte(&r);
        if (!op) return !r.bad && !r.n;
        if (op >= 160) return 1;
        (void)ssh_u32(&r); if (r.bad) return 0;
    }
    return 0;
}
static void channel_request(ssh_engine *s, ssh_reader r)
{
    ssh_reader name = ssh_string(&r); s->request_reply = ssh_byte(&r);
    if (r.bad || s->request_reply > 1 || r.n > 8192) goto malformed;
    if (ssh_equal(name, "pty-req")) {
        if (s->started || s->pty) { reply(s, 0); return; }
        ssh_reader term = ssh_string(&r);
        uint32_t cols, rows;
        int good = dimensions(&r, &cols, &rows); ssh_reader modes = ssh_string(&r);
        if (r.bad || r.n) goto malformed;
        if (!good || term.bad || term.n >= sizeof(s->term) || !modes_ok(modes)) { reply(s, 0); return; }
        /* Zero keeps the last accepted size; a refused request changes none. */
        if (cols) s->cols = cols;
        if (rows) s->rows = rows;
        memcpy(s->term, term.p, term.n); s->term[term.n] = 0; s->pty = 1; reply(s, 1); return;
    }
    if (ssh_equal(name, "window-change")) {
        uint32_t cols, rows;
        int good = dimensions(&r, &cols, &rows);
        if (r.bad || r.n) goto malformed;
        if (s->pty && good) {
            s->resize_cols = cols ? cols : s->cols;
            s->resize_rows = rows ? rows : s->rows;
            s->event = SSH_EVENT_RESIZE;
        }
        else reply(s, 0);
        return;
    }
    if (ssh_equal(name, "shell")) {
        if (r.n) goto malformed;
        if (s->started || !s->pty) { reply(s, 0); return; }
        s->event = SSH_EVENT_SHELL; return;
    }
    if (ssh_equal(name, "exec")) {
        ssh_reader command = ssh_string(&r);
        if (r.bad || r.n) goto malformed;
        if (s->started || s->pty || !command.n || command.n > SSH_COMMAND_MAX) { reply(s, 0); return; }
        for (size_t i = 0; i < command.n; i++) if (!command.p[i]) { reply(s, 0); return; }
        memcpy(s->command, command.p, command.n); s->command[command.n] = 0;
        s->event = SSH_EVENT_EXEC; return;
    }
    reply(s, 0); return;
malformed:
    ssh_disconnect(s, 2, "malformed channel request");
}
void ssh_connection_packet(ssh_engine *s, const uint8_t *p, size_t n)
{
    uint8_t type = p[0];
    if (!s->authenticated) {
        if (type == 50) auth(s, p, n);
        else ssh_disconnect(s, 2, "authentication required");
        return;
    }
    /* RFC 4252: authentication requests after success are ignored. */
    if (type == 50) return;
    ssh_reader r = {p+1, n-1, 0};
    if (type == 80) {
        (void)ssh_string(&r); uint8_t want = ssh_byte(&r);
        if (r.bad || want > 1) goto malformed;
        if (want) { uint8_t no = 82; ssh_packet_send(s, &no, 1); }
        return;
    }
    if (type == 90) {
        ssh_reader kind = ssh_string(&r);
        uint32_t id = ssh_u32(&r), window = ssh_u32(&r), max = ssh_u32(&r);
        if (r.bad) goto malformed;
        uint8_t response[128]; ssh_writer w = {response, 0, sizeof(response), 0};
        if (!ssh_equal(kind, "session") || s->channel || max < 1) {
            ssh_put_byte(&w, 92); ssh_put_u32(&w, id); ssh_put_u32(&w, 1);
            ssh_put_text(&w, "one session channel; forwarding disabled"); ssh_put_text(&w, "");
        } else {
            if (r.n) goto malformed;
            s->channel = 1; s->peer_channel = id; s->peer_window = window;
            s->peer_packet = max; s->receive_window = SSH_WINDOW;
            ssh_put_byte(&w, 91); ssh_put_u32(&w, id); ssh_put_u32(&w, 0);
            ssh_put_u32(&w, SSH_WINDOW); ssh_put_u32(&w, SSH_DATA_MAX);
        }
        ssh_packet_send(s, response, w.n); return;
    }
    if (type >= 93 && type <= 100) {
        uint32_t id = ssh_u32(&r);
        if (r.bad || !s->channel || id != 0) goto malformed;
        if (type == 97) {
            if (r.n) goto malformed;
            if (!s->sent_close) {
                uint8_t response[5]; ssh_writer w = {response, 0, sizeof(response), 0};
                ssh_put_byte(&w, 97); ssh_put_u32(&w, s->peer_channel); ssh_packet_send(s, response, w.n);
                s->sent_close = 1;
            }
            s->event = SSH_EVENT_CLOSE; return;
        }
        if (s->sent_close) return;
        if (type == 93) {
            uint32_t add = ssh_u32(&r);
            if (r.bad || r.n || add > UINT32_MAX - s->peer_window) goto malformed;
            s->peer_window += add; return;
        }
        if (type == 94 || type == 95) {
            if (type == 95) (void)ssh_u32(&r);
            ssh_reader data = ssh_string(&r);
            if (r.bad || r.n || s->input_eof || !s->started || data.n > SSH_DATA_MAX || data.n > s->receive_window) goto malformed;
            s->receive_window -= (uint32_t)data.n;
            if (type == 95) { ssh_input_consumed(s, (uint32_t)data.n); return; }
            s->event = SSH_EVENT_INPUT; s->event_data = data.p; s->event_len = data.n; return;
        }
        if (type == 96) {
            if (r.n) goto malformed;
            s->input_eof = 1; s->event = SSH_EVENT_EOF; return;
        }
        if (type == 98) { if (n > 8192) goto malformed; channel_request(s, r); return; }
        if (type == 99 || type == 100) { if (r.n) goto malformed; return; }
    }
    { uint8_t response[5]; ssh_writer w = {response, 0, sizeof(response), 0};
      ssh_put_byte(&w, 3); ssh_put_u32(&w, s->rx.seq - 1); ssh_packet_send(s, response, w.n); }
    return;
malformed:
    ssh_disconnect(s, 2, "invalid channel message, state or window");
}
size_t ssh_send_data(ssh_engine *s, const uint8_t *p, size_t n, int stderr_stream)
{
    if (!s->started || s->sent_close || s->closed || s->kex) return 0;
    if (n > s->peer_window) n = s->peer_window;
    if (n > s->peer_packet) n = s->peer_packet;
    if (n > SSH_DATA_MAX) n = SSH_DATA_MAX;
    if (!n || SSH_OUTPUT_CAP - s->out_len < n + 64) return 0;
    uint8_t payload[SSH_DATA_MAX + 13]; ssh_writer w = {payload, 0, sizeof(payload), 0};
    ssh_put_byte(&w, stderr_stream ? 95 : 94); ssh_put_u32(&w, s->peer_channel);
    if (stderr_stream) ssh_put_u32(&w, 1);
    ssh_put_string(&w, p, n);
    if (!ssh_packet_send(s, payload, w.n)) return 0;
    s->peer_window -= (uint32_t)n; return n;
}
void ssh_input_consumed(ssh_engine *s, uint32_t n)
{
    if (!n || s->closed || s->sent_close) return;
    if (n > SSH_WINDOW - s->receive_window) { ssh_disconnect(s, 2, "invalid local window credit"); return; }
    uint8_t p[9]; ssh_writer w = {p, 0, sizeof(p), 0};
    ssh_put_byte(&w, 93); ssh_put_u32(&w, s->peer_channel); ssh_put_u32(&w, n);
    if (ssh_packet_send(s, p, w.n)) s->receive_window += n;
}
void ssh_send_exit(ssh_engine *s, uint32_t status)
{
    if (s->sent_close || s->kex || SSH_OUTPUT_CAP - s->out_len < 256) return;
    uint8_t p[64]; ssh_writer w = {p, 0, sizeof(p), 0};
    ssh_put_byte(&w, 98); ssh_put_u32(&w, s->peer_channel);
    ssh_put_text(&w, "exit-status"); ssh_put_byte(&w, 0); ssh_put_u32(&w, status);
    ssh_packet_send(s, p, w.n);
    w.n = 0; ssh_put_byte(&w, 96); ssh_put_u32(&w, s->peer_channel); ssh_packet_send(s, p, w.n);
    p[0] = 97; ssh_packet_send(s, p, w.n); s->sent_close = 1;
}
