#include "client_engine.h"
#include "client_profile.h"
#include "os64/mem.h"
#include "os64/str.h"

struct os64_tls_engine {
    br_ssl_client_context client;
    const br_x509_class **validator;
    void (*destroy_validator)(const br_x509_class **);
    tls_policy_reason (*validator_reason)(const br_x509_class *const *);
    size_t handshake_received;
    unsigned char records[BR_SSL_BUFSIZE_BIDI];
    char hostname[254];
    char alpn_bytes[1032];
    const char *alpn[8];
    tls_status terminal;
    int upstream_error;
    bool handshake, closing, close_started, eof, aborted;
};

static void wipe(void *data, size_t length)
{
    volatile unsigned char *p = data;
    while (length--) *p++ = 0;
}

static tls_status hostname_copy(char *out, tls_name name)
{
    if (!name.data || !name.length) return TLS_BAD_ARGUMENT;
    if (name.length > 253) return TLS_LIMIT;
    size_t label = 0, dots = 0;
    bool numeric = true;
    for (size_t i = 0; i < name.length; i++) {
        unsigned char ch = (unsigned char)name.data[i];
        if (ch == ':' || ch == '[' || ch == ']') return TLS_UNSUPPORTED;
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (ch == '.') {
            if (!label || out[i - 1] == '-') return TLS_BAD_ARGUMENT;
            label = 0; dots++;
        } else {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-'))
                return TLS_BAD_ARGUMENT;
            if ((!label && ch == '-') || ++label > 63) return TLS_BAD_ARGUMENT;
            if (ch < '0' || ch > '9') numeric = false;
        }
        out[i] = (char)ch;
    }
    if (!label || out[name.length - 1] == '-') return TLS_BAD_ARGUMENT;
    // Reserve dotted numeric quads for the future IP-literal policy, including
    // invalid address spellings; do not reinterpret them as trusted DNS names.
    if (numeric && dots == 3) return TLS_UNSUPPORTED;
    out[name.length] = 0;
    return TLS_OK;
}

static tls_status classify(int error)
{
    if (error >= BR_ERR_X509_INVALID_VALUE && error <= BR_ERR_X509_NOT_TRUSTED)
        return TLS_CERTIFICATE;
    if (error == BR_ERR_NO_RANDOM) return TLS_ENTROPY_UNAVAILABLE;
    return TLS_PROTOCOL;
}

static void fail(os64_tls_engine *c, tls_status reason)
{
    if (c->terminal == TLS_OK) c->terminal = reason;
}

// Advancing shutdown retains no borrowed buffer view. Drain authenticated input
// and flush accepted output before upstream's discarding close or EOF truncation.
static unsigned advance(os64_tls_engine *c)
{
    unsigned state = br_ssl_engine_current_state(&c->client.eng);
    int error = br_ssl_engine_last_error(&c->client.eng);
    if (error) {
        if (!c->upstream_error) c->upstream_error = error;
        fail(c, classify(error));
    }
    if (state & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) c->handshake = true;
    if ((state & BR_SSL_CLOSED) && c->terminal == TLS_OK)
        fail(c, c->handshake ? TLS_CLEAN_EOF : TLS_PROTOCOL);
    if (c->terminal != TLS_OK || c->aborted) return state;
    if (((c->closing && !c->close_started) || c->eof) && !(state & (BR_SSL_RECVAPP | BR_SSL_SENDREC))) {
        br_ssl_engine_flush(&c->client.eng, 0);
        state = br_ssl_engine_current_state(&c->client.eng);
        if (c->closing && !c->close_started && !(state & BR_SSL_SENDREC)) {
            c->close_started = true;
            br_ssl_engine_close(&c->client.eng);
            return advance(c);
        }
    }
    if (c->eof && !(state & (BR_SSL_RECVAPP | BR_SSL_SENDREC))) fail(c, TLS_TRUNCATED);
    return state;
}

tls_status os64_tls_engine_create(const tls_engine_config *cfg, os64_tls_engine **out)
{
    if (!out) return TLS_BAD_ARGUMENT;
    *out = NULL;
    if (!cfg || !cfg->entropy || !cfg->validator.create || !cfg->validator.destroy ||
        (cfg->alpn_count && !cfg->alpn)) return TLS_BAD_ARGUMENT;
    if (cfg->alpn_count > 8) return TLS_LIMIT;
    int64_t days = cfg->epoch / 86400, seconds = cfg->epoch % 86400;
    if (seconds < 0) { days--; seconds += 86400; }
    days += 719528;
    if (days < 1 || days > UINT32_MAX) return TLS_BAD_TIME;
    os64_tls_engine *c = os64_malloc(sizeof *c);
    if (!c) return TLS_NO_MEMORY;
    os64_memset(c, 0, sizeof *c);
    tls_status result = hostname_copy(c->hostname, cfg->hostname);
    if (result != TLS_OK) goto cleanup;
    size_t used = 0;
    for (size_t i = 0; i < cfg->alpn_count; i++) {
        tls_name name = cfg->alpn[i];
        if (!name.data || !name.length) { result = TLS_BAD_ARGUMENT; goto cleanup; }
        if (name.length > 255 || used - i + name.length > 1024) { result = TLS_LIMIT; goto cleanup; }
        for (size_t j = 0; j < name.length; j++)
            if (!name.data[j]) { result = TLS_BAD_ARGUMENT; goto cleanup; }
        c->alpn[i] = c->alpn_bytes + used;
        os64_memcpy(c->alpn_bytes + used, name.data, name.length);
        used += name.length;
        c->alpn_bytes[used++] = 0;
    }
    os64_bearssl_client_algorithms(&c->client);
    c->destroy_validator = cfg->validator.destroy;
    c->validator_reason = cfg->validator.policy_reason;
    result = cfg->validator.create(cfg->validator.context, c->hostname,
        (uint32_t)days, (uint32_t)seconds, &c->validator);
    if (result != TLS_OK) goto cleanup;
    if (!c->validator || !*c->validator) { result = TLS_BAD_ARGUMENT; goto cleanup; }
    br_ssl_engine_set_x509(&c->client.eng, c->validator);
    br_ssl_engine_set_buffer(&c->client.eng, c->records, sizeof c->records, 1);
    br_ssl_engine_set_protocol_names(&c->client.eng, c->alpn, cfg->alpn_count);
    unsigned char seed[32] = {0};
    result = cfg->entropy(cfg->entropy_context, seed, sizeof seed);
    if (result == TLS_OK) br_ssl_engine_inject_entropy(&c->client.eng, seed, sizeof seed);
    wipe(seed, sizeof seed);
    if (result != TLS_OK) {
        if (result != TLS_CANCELLED && result != TLS_TIMEOUT) result = TLS_ENTROPY_UNAVAILABLE;
        goto cleanup;
    }
    if (!br_ssl_client_reset(&c->client, c->hostname, 0)) {
        result = classify(br_ssl_engine_last_error(&c->client.eng));
        goto cleanup;
    }
    *out = c;
    return TLS_OK;
cleanup:
    os64_tls_engine_destroy(c);
    return result;
}

void os64_tls_engine_destroy(os64_tls_engine *c)
{
    if (!c) return;
    if (c->validator) c->destroy_validator(c->validator);
    wipe(c, sizeof *c);
    os64_free(c);
}

tls_state os64_tls_engine_state(os64_tls_engine *c)
{
    tls_state result = { .status = TLS_BAD_ARGUMENT };
    if (!c) return result;
    unsigned state = advance(c);
    result.status = c->terminal;
    result.upstream_error = c->upstream_error;
    if (c->validator_reason) result.policy_reason = c->validator_reason(c->validator);
    if (c->handshake) {
        result.flags |= TLS_HANDSHAKE_DONE;
        result.alpn = br_ssl_engine_get_selected_protocol(&c->client.eng);
    }
    if (c->closing) result.flags |= TLS_CLOSING;
    if (c->aborted) return result;
    if (state & BR_SSL_SENDREC) result.flags |= TLS_SEND_CIPHER;
    if (c->terminal != TLS_OK) return result;
    if (!c->eof && (state & BR_SSL_RECVREC)) result.flags |= TLS_RECV_CIPHER;
    if (c->handshake && (state & BR_SSL_RECVAPP)) result.flags |= TLS_RECV_PLAIN;
    if (c->handshake && !c->closing && !c->eof && (state & BR_SSL_SENDAPP)) result.flags |= TLS_SEND_PLAIN;
    return result;
}

typedef enum { FEED, TAKE, WRITE, READ } transfer_kind;
static tls_transfer transfer(os64_tls_engine *c, void *buffer, size_t length, transfer_kind kind)
{
    tls_transfer result = {TLS_BAD_ARGUMENT, 0};
    if (!c || (length && !buffer)) return result;
    tls_state state = os64_tls_engine_state(c);
    result.status = state.status;
    if (!length || c->aborted) return result;
    const unsigned flag[] = {TLS_RECV_CIPHER, TLS_SEND_CIPHER, TLS_SEND_PLAIN, TLS_RECV_PLAIN};
    if (!(state.flags & flag[kind])) {
        if (result.status == TLS_OK) result.status = TLS_NEED_PROGRESS;
        return result;
    }
    size_t available;
    unsigned char *view;
    switch (kind) {
    case FEED: view = br_ssl_engine_recvrec_buf(&c->client.eng, &available); break;
    case TAKE: view = br_ssl_engine_sendrec_buf(&c->client.eng, &available); break;
    case WRITE: view = br_ssl_engine_sendapp_buf(&c->client.eng, &available); break;
    default: view = br_ssl_engine_recvapp_buf(&c->client.eng, &available); break;
    }
    if (!view || !available) { result.status = TLS_NEED_PROGRESS; return result; }
    size_t count = length < available ? length : available;
    if (kind == FEED && !c->handshake) {
        size_t remaining = TLS_HANDSHAKE_CIPHER_MAX - c->handshake_received;
        if (!remaining) {
            fail(c, TLS_LIMIT); result.status = c->terminal; return result;
        }
        if (count > remaining) count = remaining;
        c->handshake_received += count;
    }
    if (kind == FEED || kind == WRITE) os64_memcpy(view, buffer, count);
    else os64_memcpy(buffer, view, count);
    switch (kind) {
    case FEED: br_ssl_engine_recvrec_ack(&c->client.eng, count); break;
    case TAKE: br_ssl_engine_sendrec_ack(&c->client.eng, count); break;
    case WRITE: br_ssl_engine_sendapp_ack(&c->client.eng, count); break;
    case READ: br_ssl_engine_recvapp_ack(&c->client.eng, count); break;
    }
    result.transferred = count;
    result.status = os64_tls_engine_state(c).status;
    return result;
}

tls_transfer os64_tls_engine_feed(os64_tls_engine *c, const void *p, size_t n) { return transfer(c, (void *)p, n, FEED); }
tls_transfer os64_tls_engine_take(os64_tls_engine *c, void *p, size_t n) { return transfer(c, p, n, TAKE); }
tls_transfer os64_tls_engine_write(os64_tls_engine *c, const void *p, size_t n) { return transfer(c, (void *)p, n, WRITE); }
tls_transfer os64_tls_engine_read(os64_tls_engine *c, void *p, size_t n) { return transfer(c, p, n, READ); }

tls_status os64_tls_engine_flush(os64_tls_engine *c)
{
    if (!c) return TLS_BAD_ARGUMENT;
    advance(c);
    if (c->terminal == TLS_OK) br_ssl_engine_flush(&c->client.eng, 0);
    advance(c);
    return c->terminal;
}

tls_status os64_tls_engine_close(os64_tls_engine *c)
{
    if (!c) return TLS_BAD_ARGUMENT;
    advance(c);
    if (c->terminal != TLS_OK) return c->terminal;
    if (!c->handshake) return os64_tls_engine_abort(c, TLS_CANCELLED);
    c->closing = true;
    advance(c);
    return c->terminal;
}

tls_status os64_tls_engine_eof(os64_tls_engine *c)
{
    if (!c) return TLS_BAD_ARGUMENT;
    c->eof = true;
    advance(c);
    return c->terminal;
}

tls_status os64_tls_engine_abort(os64_tls_engine *c, tls_status reason)
{
    if (!c || (reason != TLS_TRANSPORT && reason != TLS_TIMEOUT && reason != TLS_CANCELLED))
        return TLS_BAD_ARGUMENT;
    advance(c);
    fail(c, reason);
    c->aborted = true;
    return c->terminal;
}
