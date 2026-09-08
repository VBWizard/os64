#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../userland/libtls/port/client_engine.h"
#include "tls_engine_fixture.h"

// Single-threaded fault-injection fixture; this is not the library allocator.
static size_t allocations, frees, allocation_calls, fail_allocation;
typedef union { max_align_t alignment; size_t size; } allocation_header;
void *os64_malloc(size_t n)
{
    if (++allocation_calls == fail_allocation) return NULL;
    allocation_header *h = malloc(sizeof *h + n);
    assert(h); h->size = n;
    memset(h + 1, 0xa5, n);
    allocations++;
    return h + 1;
}
void os64_free(void *p)
{
    if (!p) return;
    allocation_header *h = (allocation_header *)p - 1;
    for (size_t i = 0; i < h->size; i++) assert(((unsigned char *)p)[i] == 0);
    frees++; free(h);
}

typedef struct { bool rsa, reject, factory_fail; tls_status entropy_result; } policy;
typedef struct {
    br_x509_knownkey_context known;
    br_x509_decoder_context decoder;
    br_x509_class rejecting;
} peer;
static size_t factory_calls, factory_destroys, entropy_calls;
static uint32_t observed_days, observed_seconds;
static unsigned reject_chain(const br_x509_class **ctx) { (void)ctx; return BR_ERR_X509_NOT_TRUSTED; }

// Fixture-only pinned-key verifier: cryptographic handshakes are exercised,
// but these sample certificates do not establish production PKI policy.
static tls_status create_peer(void *context, const char *hostname, uint32_t days,
                              uint32_t seconds, const br_x509_class ***out)
{
    policy *p = context;
    assert(!strcmp(hostname, "example.test"));
    observed_days = days; observed_seconds = seconds; factory_calls++;
    peer *v = os64_malloc(sizeof *v);
    if (!v) return TLS_NO_MEMORY;
    memset(v, 0, sizeof *v);
    if (p->rsa) tls_fixture_rsa(NULL, &v->decoder);
    else tls_fixture_ec(NULL, &v->decoder);
    br_x509_pkey *key = br_x509_decoder_get_pkey(&v->decoder);
    assert(key);
    if (p->rsa) br_x509_knownkey_init_rsa(&v->known, &key->key.rsa, BR_KEYTYPE_SIGN);
    else br_x509_knownkey_init_ec(&v->known, &key->key.ec, BR_KEYTYPE_SIGN);
    if (p->reject) {
        v->rejecting = *v->known.vtable;
        v->rejecting.end_chain = reject_chain;
        v->known.vtable = &v->rejecting;
    }
    *out = &v->known.vtable;
    return p->factory_fail ? TLS_CERTIFICATE : TLS_OK;
}
static void destroy_peer(const br_x509_class **v)
{
    factory_destroys++;
    memset((void *)v, 0, sizeof(peer));
    os64_free((void *)v);
}
static tls_status entropy(void *context, unsigned char *out, size_t n)
{
    policy *p = context; entropy_calls++;
    memset(out, 0x59, n);
    return p->entropy_result;
}
static tls_engine_config config(policy *p)
{
    static const tls_name alpn[] = {{"http/1.1", 8}};
    return (tls_engine_config){
        .hostname = {"ExAmPlE.TeSt", 12}, .alpn = alpn, .alpn_count = 1, .epoch = -1,
        .entropy = entropy, .entropy_context = p,
        .validator = {create_peer, destroy_peer, p}
    };
}

typedef struct { unsigned char bytes[BR_SSL_BUFSIZE_BIDI]; size_t at, length; } queue;
typedef struct {
    os64_tls_engine *client;
    br_ssl_server_context server;
    unsigned char records[BR_SSL_BUFSIZE_BIDI];
    queue cs, sc;
    size_t fragment;
    bool corrupt;
    policy policy;
} connection;

static connection *connect_fixture(uint16_t suite, size_t fragment, bool reject)
{
    connection *c = calloc(1, sizeof *c); assert(c);
    c->policy.rsa = suite == 0xcca8 || suite == 0xc02f || suite == 0xc030;
    c->policy.reject = reject; c->fragment = fragment;
    tls_engine_config cfg = config(&c->policy);
    char hostname[] = "ExAmPlE.TeSt", protocol[] = "http/1.1";
    tls_name name = {protocol, 8};
    cfg.hostname.data = hostname; cfg.alpn = &name;
    assert(os64_tls_engine_create(&cfg, &c->client) == TLS_OK);
    // Destroy caller storage before the handshake consumes ALPN and SNI.
    memset(hostname, 'x', sizeof hostname); memset(protocol, 'x', sizeof protocol);
    assert(observed_days == 719527 && observed_seconds == 86399);
    if (c->policy.rsa) tls_fixture_rsa(&c->server, NULL);
    else tls_fixture_ec(&c->server, NULL);
    br_ssl_engine_set_versions(&c->server.eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(&c->server.eng, &suite, 1);
    br_ssl_engine_set_buffer(&c->server.eng, c->records, sizeof c->records, 1);
    static const char *names[] = {"http/1.1"};
    br_ssl_engine_set_protocol_names(&c->server.eng, names, 1);
    unsigned char seed[32] = {0x51};
    br_ssl_engine_inject_entropy(&c->server.eng, seed, sizeof seed);
    br_ssl_server_reset(&c->server);
    return c;
}
static void disconnect_fixture(connection *c)
{
    os64_tls_engine_destroy(c->client); free(c);
}
static size_t smaller(size_t a, size_t b) { return a < b ? a : b; }

static size_t pump(connection *c)
{
    size_t progress = 0, n;
    if (c->cs.at == c->cs.length) {
        tls_transfer r = os64_tls_engine_take(c->client, c->cs.bytes, sizeof c->cs.bytes);
        c->cs.at = 0; c->cs.length = r.transferred; progress += r.transferred;
    }
    if (c->cs.at < c->cs.length) {
        unsigned char *p = br_ssl_engine_recvrec_buf(&c->server.eng, &n);
        if (p && n) {
            n = smaller(n, smaller(c->fragment, c->cs.length - c->cs.at));
            memcpy(p, c->cs.bytes + c->cs.at, n);
            br_ssl_engine_recvrec_ack(&c->server.eng, n);
            c->cs.at += n; progress += n;
        }
    }
    if (c->sc.at == c->sc.length) {
        unsigned char *p = br_ssl_engine_sendrec_buf(&c->server.eng, &n);
        if (p && n) {
            assert(n <= sizeof c->sc.bytes);
            memcpy(c->sc.bytes, p, n);
            if (c->corrupt) { assert(p[0] == 23 && n > 8); c->sc.bytes[8] ^= 1; c->corrupt = false; }
            br_ssl_engine_sendrec_ack(&c->server.eng, n);
            c->sc.at = 0; c->sc.length = n; progress += n;
        }
    }
    if (c->sc.at < c->sc.length) {
        tls_transfer r = os64_tls_engine_feed(c->client, c->sc.bytes + c->sc.at,
            smaller(c->fragment, c->sc.length - c->sc.at));
        assert(r.transferred <= c->sc.length - c->sc.at);
        c->sc.at += r.transferred; progress += r.transferred;
    }
    return progress;
}

static void handshake(connection *c)
{
    unsigned char sentinel = 0xa5;
    tls_transfer r = os64_tls_engine_read(c->client, &sentinel, 1);
    assert(r.status == TLS_NEED_PROGRESS && !r.transferred && sentinel == 0xa5);
    r = os64_tls_engine_write(c->client, &sentinel, 1);
    assert(r.status == TLS_NEED_PROGRESS && !r.transferred);
    for (unsigned i = 0; i < 100000; i++) {
        tls_state state = os64_tls_engine_state(c->client);
        assert(state.status == TLS_OK);
        if ((state.flags & TLS_HANDSHAKE_DONE) &&
            (br_ssl_engine_current_state(&c->server.eng) & BR_SSL_SENDAPP)) {
            assert(state.alpn && !strcmp(state.alpn, "http/1.1")); return;
        }
        assert(pump(c));
    }
    assert(!"handshake did not finish");
}

static unsigned char pattern(size_t i, unsigned salt) { return (unsigned char)(i * 37 + salt); }
static void exchange(connection *c)
{
    const size_t total = 70013;
    unsigned char *request = malloc(total), *response = malloc(total); assert(request && response);
    for (size_t i = 0; i < total; i++) { request[i] = pattern(i, 1); response[i] = pattern(i, 2); }
    size_t cw = 0, sw = 0, cr = 0, sr = 0;
    for (unsigned i = 0; i < 500000; i++) {
        size_t progress = pump(c), n;
        assert(os64_tls_engine_state(c->client).status == TLS_OK);
        assert(br_ssl_engine_last_error(&c->server.eng) == 0);
        if (cw < total) {
            tls_transfer r = os64_tls_engine_write(c->client, request + cw, total - cw);
            cw += r.transferred; progress += r.transferred;
            if (r.transferred) assert(os64_tls_engine_flush(c->client) == TLS_OK);
        }
        unsigned char *p = br_ssl_engine_sendapp_buf(&c->server.eng, &n);
        if (sw < total && p && n) {
            n = smaller(n, total - sw); memcpy(p, response + sw, n);
            br_ssl_engine_sendapp_ack(&c->server.eng, n);
            br_ssl_engine_flush(&c->server.eng, 0); sw += n; progress += n;
        }
        p = br_ssl_engine_recvapp_buf(&c->server.eng, &n);
        if (p && n) {
            n = smaller(n, 23); assert(sr + n <= total);
            for (size_t j = 0; j < n; j++) assert(p[j] == request[sr + j]);
            br_ssl_engine_recvapp_ack(&c->server.eng, n); sr += n; progress += n;
        }
        unsigned char got[19];
        tls_transfer r = os64_tls_engine_read(c->client, got, sizeof got);
        assert(cr + r.transferred <= total);
        for (size_t j = 0; j < r.transferred; j++) assert(got[j] == response[cr + j]);
        cr += r.transferred; progress += r.transferred;
        if (cr == total && sr == total) { assert(cw == total && sw == total); free(request); free(response); return; }
        assert(progress);
    }
    assert(!"data exchange did not finish");
}

static void server_message(connection *c)
{
    size_t n; unsigned char *p = br_ssl_engine_sendapp_buf(&c->server.eng, &n);
    assert(p && n >= 5); memcpy(p, "hello", 5);
    br_ssl_engine_sendapp_ack(&c->server.eng, 5); br_ssl_engine_flush(&c->server.eng, 0);
}
static void await_plain(connection *c)
{
    for (unsigned i = 0; i < 10000; i++) {
        if (os64_tls_engine_state(c->client).flags & TLS_RECV_PLAIN) return;
        assert(pump(c));
    }
    assert(!"plaintext did not arrive");
}
static void close_with_buffered_plaintext(connection *c)
{
    server_message(c); await_plain(c);
    tls_transfer written = os64_tls_engine_write(c->client, "final", 5);
    assert(written.transferred == 5); // Close must flush this accepted prefix.
    assert(os64_tls_engine_close(c->client) == TLS_OK);
    assert(!(os64_tls_engine_state(c->client).flags & TLS_SEND_PLAIN));
    unsigned char got[5];
    tls_transfer r = os64_tls_engine_read(c->client, got, 2);
    assert(r.transferred == 2 && !memcmp(got, "he", 2));
    r = os64_tls_engine_read(c->client, got, sizeof got);
    assert(r.transferred == 3 && !memcmp(got, "llo", 3));
    size_t received = 0;
    for (unsigned i = 0; i < 10000; i++) {
        if (os64_tls_engine_state(c->client).status == TLS_CLEAN_EOF) { assert(received == 5); return; }
        size_t progress = pump(c), n;
        unsigned char *p = br_ssl_engine_recvapp_buf(&c->server.eng, &n);
        if (p && n) {
            assert(received + n <= 5 && !memcmp(p, "final" + received, n));
            br_ssl_engine_recvapp_ack(&c->server.eng, n);
            received += n; progress += n;
        }
        assert(progress);
    }
    assert(!"close exchange did not finish");
}

static void creation_failures(void)
{
    policy p = {0}; tls_engine_config cfg = config(&p);
    os64_tls_engine *c = (void *)(uintptr_t)1;
    size_t old = entropy_calls;
    for (size_t i = 1; i <= 2; i++) {
        fail_allocation = allocation_calls + i;
        assert(os64_tls_engine_create(&cfg, &c) == TLS_NO_MEMORY && !c);
        assert(allocations == frees);
    }
    fail_allocation = 0;
    p.factory_fail = true;
    assert(os64_tls_engine_create(&cfg, &c) == TLS_CERTIFICATE && !c);
    assert(entropy_calls == old && allocations == frees);
    p.factory_fail = false;
    const tls_status entropy_errors[] = {TLS_ENTROPY_UNAVAILABLE, TLS_CANCELLED, TLS_TIMEOUT, TLS_TRANSPORT};
    for (size_t i = 0; i < sizeof entropy_errors / sizeof entropy_errors[0]; i++) {
        p.entropy_result = entropy_errors[i];
        tls_status expected = p.entropy_result == TLS_TRANSPORT ? TLS_ENTROPY_UNAVAILABLE : p.entropy_result;
        assert(os64_tls_engine_create(&cfg, &c) == expected && !c);
        assert(allocations == frees);
    }
    p.entropy_result = TLS_OK;
    const char *bad[] = {"", ".x", "x.", "-x", "x-", "a..b", "bad/name", "a b"};
    old = factory_calls;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        cfg.hostname = (tls_name){bad[i], strlen(bad[i])};
        assert(os64_tls_engine_create(&cfg, &c) == TLS_BAD_ARGUMENT && !c);
    }
    cfg.hostname = (tls_name){"127.0.0.1", 9};
    assert(os64_tls_engine_create(&cfg, &c) == TLS_UNSUPPORTED && !c);
    cfg = config(&p); cfg.epoch = INT64_MAX;
    assert(os64_tls_engine_create(&cfg, &c) == TLS_BAD_TIME && !c);
    cfg = config(&p); cfg.alpn_count = 9;
    assert(os64_tls_engine_create(&cfg, &c) == TLS_LIMIT && !c);
    char name_bytes[256]; memset(name_bytes, 'a', sizeof name_bytes);
    cfg = config(&p); cfg.hostname = (tls_name){name_bytes, 254};
    assert(os64_tls_engine_create(&cfg, &c) == TLS_LIMIT && !c);
    cfg.hostname.length = 64;
    assert(os64_tls_engine_create(&cfg, &c) == TLS_BAD_ARGUMENT && !c);
    cfg.hostname = (tls_name){"a\0b", 3};
    assert(os64_tls_engine_create(&cfg, &c) == TLS_BAD_ARGUMENT && !c);
    tls_name names[8];
    for (size_t i = 0; i < 8; i++) names[i] = (tls_name){name_bytes, 255};
    cfg = config(&p); cfg.alpn = names; cfg.alpn_count = 5;
    assert(os64_tls_engine_create(&cfg, &c) == TLS_LIMIT && !c);
    cfg.alpn_count = 1; names[0].length = 256;
    assert(os64_tls_engine_create(&cfg, &c) == TLS_LIMIT && !c);
    names[0] = (tls_name){"a\0b", 3};
    assert(os64_tls_engine_create(&cfg, &c) == TLS_BAD_ARGUMENT && !c);
    names[0].length = 0;
    assert(os64_tls_engine_create(&cfg, &c) == TLS_BAD_ARGUMENT && !c);
    assert(factory_calls == old && allocations == frees);

    cfg = config(&p); cfg.alpn = names; cfg.alpn_count = 8;
    for (size_t i = 0; i < 8; i++) names[i] = (tls_name){name_bytes, 128};
    assert(os64_tls_engine_create(&cfg, &c) == TLS_OK);
    os64_tls_engine_destroy(c); // 1024 bytes plus eight internal terminators.
    const int64_t epochs[] = {0, 86400, 951782400, -86400};
    const uint32_t days[] = {719528, 719529, 730544, 719527};
    for (size_t i = 0; i < sizeof epochs / sizeof epochs[0]; i++) {
        cfg = config(&p); cfg.epoch = epochs[i];
        assert(os64_tls_engine_create(&cfg, &c) == TLS_OK);
        assert(observed_days == days[i] && observed_seconds == 0);
        os64_tls_engine_destroy(c);
    }
}

static void peer_close_at_eof(void)
{
    connection *c = connect_fixture(0xc02b, 17, false); handshake(c);
    br_ssl_engine_close(&c->server.eng);
    for (unsigned i = 0; i < 10000; i++) {
        if (os64_tls_engine_state(c->client).flags & TLS_SEND_CIPHER) break;
        assert(pump(c));
    }
    assert(os64_tls_engine_state(c->client).flags & TLS_SEND_CIPHER);
    assert(os64_tls_engine_eof(c->client) == TLS_OK);
    unsigned char got[3];
    tls_transfer r;
    do { r = os64_tls_engine_take(c->client, got, sizeof got); assert(r.transferred); }
    while (r.status == TLS_OK);
    assert(r.status == TLS_CLEAN_EOF);
    disconnect_fixture(c);
}

static void partial_record_and_abort(void)
{
    connection *c = connect_fixture(0xc02b, 17, false); handshake(c);
    server_message(c);
    size_t n; unsigned char *record = br_ssl_engine_sendrec_buf(&c->server.eng, &n);
    assert(record && n > 5);
    tls_transfer r = os64_tls_engine_feed(c->client, record, 3);
    assert(r.transferred == 3 && r.status == TLS_OK);
    assert(os64_tls_engine_eof(c->client) == TLS_TRUNCATED);
    disconnect_fixture(c);

    const tls_status errors[] = {TLS_TRANSPORT, TLS_TIMEOUT, TLS_CANCELLED};
    for (size_t i = 0; i < sizeof errors / sizeof errors[0]; i++) {
        c = connect_fixture(0xc02b, 17, false); handshake(c);
        r = os64_tls_engine_write(c->client, "queued", 6); assert(r.transferred == 6);
        assert(os64_tls_engine_flush(c->client) == TLS_OK);
        assert(os64_tls_engine_state(c->client).flags & TLS_SEND_CIPHER);
        assert(os64_tls_engine_abort(c->client, errors[i]) == errors[i]);
        assert(os64_tls_engine_abort(c->client, TLS_CANCELLED) == errors[i]);
        assert(!(os64_tls_engine_state(c->client).flags &
            (TLS_SEND_CIPHER | TLS_RECV_CIPHER | TLS_SEND_PLAIN | TLS_RECV_PLAIN)));
        disconnect_fixture(c);
    }
}

static void independent_connections(void)
{
    connection *a = connect_fixture(0xc02b, 1, false);
    connection *b = connect_fixture(0xcca8, 73, false);
    for (unsigned i = 0; i < 100000; i++) {
        if ((os64_tls_engine_state(a->client).flags & TLS_HANDSHAKE_DONE) &&
            (os64_tls_engine_state(b->client).flags & TLS_HANDSHAKE_DONE)) break;
        size_t progress = pump(a); progress += pump(b); assert(progress);
    }
    assert(os64_tls_engine_state(a->client).flags & TLS_HANDSHAKE_DONE);
    assert(os64_tls_engine_state(b->client).flags & TLS_HANDSHAKE_DONE);
    assert(os64_tls_engine_abort(a->client, TLS_CANCELLED) == TLS_CANCELLED);
    disconnect_fixture(a);
    exchange(b); close_with_buffered_plaintext(b); disconnect_fixture(b);
}

int main(void)
{
    creation_failures(); puts("PASS creation validation, allocation/factory/entropy failures, and wiped cleanup");
    const uint16_t suites[] = {0xcca9, 0xcca8, 0xc02b, 0xc02f, 0xc02c, 0xc030};
    const size_t fragments[] = {1, 7, 17, 4096, 13, 257};
    for (size_t i = 0; i < 6; i++) {
        connection *c = connect_fixture(suites[i], fragments[i], false);
        assert(os64_tls_engine_feed(c->client, NULL, 0).status == TLS_OK);
        assert(os64_tls_engine_take(c->client, NULL, 0).status == TLS_OK);
        assert(os64_tls_engine_read(c->client, NULL, 1).status == TLS_BAD_ARGUMENT);
        handshake(c); exchange(c); close_with_buffered_plaintext(c);
        assert(os64_tls_engine_eof(c->client) == TLS_CLEAN_EOF);
        disconnect_fixture(c); assert(allocations == frees);
        printf("PASS suite %04x: fragmented handshake, bidirectional multi-record data, buffered close\n", suites[i]);
    }
    connection *c = connect_fixture(0xc02b, 11, true);
    for (unsigned i = 0; os64_tls_engine_state(c->client).status == TLS_OK && i < 10000; i++) assert(pump(c));
    assert(os64_tls_engine_state(c->client).status == TLS_CERTIFICATE);
    assert(!(os64_tls_engine_state(c->client).flags & (TLS_SEND_PLAIN | TLS_RECV_PLAIN)));
    assert(os64_tls_engine_abort(c->client, TLS_TIMEOUT) == TLS_CERTIFICATE);
    disconnect_fixture(c);
    c = connect_fixture(0xc02b, 19, false); handshake(c);
    server_message(c); await_plain(c);
    assert(os64_tls_engine_eof(c->client) == TLS_OK);
    unsigned char got[16]; tls_transfer r = os64_tls_engine_read(c->client, got, sizeof got);
    assert(r.transferred == 5 && r.status == TLS_TRUNCATED && !memcmp(got, "hello", 5));
    assert(os64_tls_engine_feed(c->client, got, 1).status == TLS_TRUNCATED);
    assert(os64_tls_engine_close(c->client) == TLS_TRUNCATED);
    disconnect_fixture(c);
    c = connect_fixture(0xc02b, 31, false); handshake(c);
    c->corrupt = true; server_message(c);
    for (unsigned i = 0; os64_tls_engine_state(c->client).status == TLS_OK && i < 10000; i++) {
        assert(pump(c));
        assert(os64_tls_engine_read(c->client, got, sizeof got).transferred == 0);
    }
    assert(os64_tls_engine_state(c->client).status == TLS_PROTOCOL);
    disconnect_fixture(c);
    c = connect_fixture(0xc02b, 31, false);
    assert(os64_tls_engine_close(c->client) == TLS_CANCELLED);
    assert(os64_tls_engine_take(c->client, got, sizeof got).transferred == 0);
    disconnect_fixture(c);
    peer_close_at_eof(); partial_record_and_abort(); independent_connections();
    assert(allocations == frees && factory_destroys > 0);
    puts("PASS certificate refusal, authenticated drain before truncation, damaged records, and cancellation");
    puts("PASS EOF with pending close reply, partial record EOF, sticky abort, and interleaved connection ownership");
    return 0;
}
