#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../userland/libtls/port/platform_inputs.h"
#include "../userland/libtls/port/trust_store.h"
#include "../userland/libtls/test/trust_vectors.h"
#include "os64/date.h"
#include "os64/io.h"

// Exercise the same failure matrix through the public entry points as well.
#ifdef TLS_TEST_PUBLIC
#define tls_os_config os64_tls_config_t
#define os64_tls_engine os64_tls_client
#define os64_tls_engine_create_os os64_tls_client_create
#define os64_tls_engine_destroy os64_tls_free
#define os64_tls_engine_state os64_tls_state
#define os64_tls_engine_take os64_tls_take_ciphertext
#define os64_tls_engine_feed os64_tls_feed_ciphertext
#define os64_tls_engine_write os64_tls_write_plaintext
#define os64_tls_engine_read os64_tls_read_plaintext
#define os64_tls_engine_flush os64_tls_flush
#define os64_tls_engine_close os64_tls_begin_close
#define os64_tls_engine_eof os64_tls_transport_eof
#define os64_tls_engine_abort os64_tls_abort
#endif

typedef struct { void *p; size_t n; bool engine; } allocation;
static allocation allocations[32];
static size_t calls, fail_allocation, live_allocations;
static bool next_engine;
void *os64_malloc(size_t n)
{
    bool engine = next_engine; next_engine = false;
    if (++calls == fail_allocation) return NULL;
    void *p = malloc(n); assert(p);
    for (size_t i = 0; i < 32; i++) if (!allocations[i].p) {
        allocations[i] = (allocation){p, n, engine}; live_allocations++; return p;
    }
    assert(false); return NULL;
}
void os64_free(void *p)
{
    if (!p) return;
    for (size_t i = 0; i < 32; i++) if (allocations[i].p == p) {
        if (allocations[i].engine)
            for (size_t j = 0; j < allocations[i].n; j++) assert(!((unsigned char *)p)[j]);
        allocations[i].p = NULL; live_allocations--; free(p); return;
    }
    assert(false);
}

static struct {
    int32_t handle;
    size_t at, fragment, fail_at;
    int64_t failure, close_result, clock_result, epoch;
    int32_t timezone;
    unsigned opens, reads, closes, clocks, salt;
    bool live, refuse_open;
    unsigned char *destination;
} io;
static void reset_io(void)
{
    assert(!io.live);
    memset(&io, 0, sizeof io);
    io.fragment = 32; io.fail_at = SIZE_MAX; io.failure = -1;
    io.epoch = 1788868800;
}
int64_t os64_time(os64_time_t *out)
{
    io.clocks++;
    *out = (os64_time_t){.epoch = io.epoch, .tz_offset_minutes = io.timezone,
        .ticks_per_second = 100, .ticks_into_second = 99};
    return io.clock_result;
}
int64_t os64_open(const char *path, const char *mode)
{
    assert(!strcmp(path, "/dev/random") && !strcmp(mode, "r") && !io.live);
    io.opens++;
    if (io.refuse_open) return -1;
    io.live = true; io.at = 0; io.destination = NULL;
    return io.handle;
}
int64_t os64_read(int32_t handle, void *buffer, size_t capacity)
{
    assert(io.live && handle == io.handle && capacity == 32 - io.at);
    if (!io.destination) io.destination = buffer;
    assert(buffer == io.destination + io.at);
    io.reads++;
    if (io.at == io.fail_at) return io.failure;
    size_t n = capacity < io.fragment ? capacity : io.fragment;
    if (n > io.fail_at - io.at) n = io.fail_at - io.at;
    for (size_t i = 0; i < n; i++) ((unsigned char *)buffer)[i] = (unsigned char)(io.at + i + io.salt);
    io.at += n; return (int64_t)n;
}
int64_t os64_close(int32_t handle)
{
    assert(io.live && handle == io.handle);
    io.live = false; io.closes++; return io.close_result;
}
static int64_t read_pem(void *context, void *out, size_t capacity)
{
    size_t *at = context;
    size_t n = tls_fixture_a_pem_length - *at;
    if (n > capacity) n = capacity;
    memcpy(out, tls_fixture_a_pem + *at, n); *at += n; return (int64_t)n;
}
static os64_tls_trust *trust(void)
{
    size_t at = 0; os64_tls_trust *s = NULL;
    assert(os64_tls_trust_load_pem(read_pem, &at, &s, NULL) == TLS_STORE_OK);
    return s;
}
static tls_status create(const tls_os_config *cfg, os64_tls_engine **out)
{
    next_engine = true;
    tls_status result = os64_tls_engine_create_os(cfg, out);
    next_engine = false; return result;
}
static void failure(const tls_os_config *cfg, tls_status expected)
{
    size_t saved = live_allocations;
    os64_tls_engine *c = (void *)1;
    assert(create(cfg, &c) == expected && !c && !io.live);
    assert(live_allocations == saved);
}
static void pump(os64_tls_engine *client, br_ssl_server_context *server)
{
    size_t n;
    unsigned char *p = br_ssl_engine_recvrec_buf(&server->eng, &n);
    if (p && n) {
        if (n > 37) n = 37;
        tls_transfer moved = os64_tls_engine_take(client, p, n);
        if (moved.transferred) br_ssl_engine_recvrec_ack(&server->eng, moved.transferred);
    }
    p = br_ssl_engine_sendrec_buf(&server->eng, &n);
    if (p && n) {
        if (n > 37) n = 37;
        tls_transfer moved = os64_tls_engine_feed(client, p, n);
        if (moved.transferred) br_ssl_engine_sendrec_ack(&server->eng, moved.transferred);
    }
}
static void application_data(os64_tls_engine *client, br_ssl_server_context *server)
{
    const char request[] = "public request", response[] = "authenticated response";
    tls_transfer sent = os64_tls_engine_write(client, request, sizeof request);
    assert(sent.status == TLS_OK && sent.transferred == sizeof request);
    assert(os64_tls_engine_flush(client) == TLS_OK);
    bool replied = false;
    size_t request_at = 0, response_at = 0;
    for (unsigned step = 0; step < 20000 && (request_at < sizeof request || response_at < sizeof response); step++) {
        pump(client, server);
        size_t n;
        unsigned char *p = br_ssl_engine_recvapp_buf(&server->eng, &n);
        if (p && n) {
            assert(request_at + n <= sizeof request && !memcmp(p, request + request_at, n));
            request_at += n; br_ssl_engine_recvapp_ack(&server->eng, n);
        }
        p = br_ssl_engine_sendapp_buf(&server->eng, &n);
        if (!replied && p && n >= sizeof response) {
            memcpy(p, response, sizeof response);
            br_ssl_engine_sendapp_ack(&server->eng, sizeof response);
            br_ssl_engine_flush(&server->eng, 0); replied = true;
        }
        unsigned char got[3];
        tls_transfer received = os64_tls_engine_read(client, got, sizeof got);
        assert(response_at + received.transferred <= sizeof response);
        assert(!memcmp(got, response + response_at, received.transferred));
        response_at += received.transferred;
    }
    assert(request_at == sizeof request && response_at == sizeof response);
    assert(os64_tls_engine_close(client) == TLS_OK);
    for (unsigned step = 0; step < 20000 && os64_tls_engine_state(client).status == TLS_OK; step++) pump(client, server);
    assert(os64_tls_engine_state(client).status == TLS_CLEAN_EOF);
    assert(os64_tls_engine_eof(client) == TLS_CLEAN_EOF);
}
static void handshake(const tls_os_config *cfg, bool success, tls_policy_reason reason)
{
    os64_tls_engine *client;
    assert(create(cfg, &client) == TLS_OK);
    // Release the caller's reference before validation uses its snapshot.
    os64_tls_trust_free(cfg->trust);
    br_ssl_server_context *server = calloc(1, sizeof *server); assert(server);
    unsigned char *records = malloc(BR_SSL_BUFSIZE_BIDI); assert(records);
    br_x509_certificate certificate = {(unsigned char *)tls_fixture_a_leaf, tls_fixture_a_leaf_length};
    unsigned char scalar = 3, seed[32] = {0x61}; // Public fixture key and server seed.
    br_ec_private_key key = {BR_EC_secp256r1, &scalar, 1};
    br_ssl_server_init_full_ec(server, &certificate, 1, BR_KEYTYPE_EC, &key);
    uint16_t suite = BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256;
    br_ssl_engine_set_versions(&server->eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(&server->eng, &suite, 1);
    br_ssl_engine_set_buffer(&server->eng, records, BR_SSL_BUFSIZE_BIDI, 1);
    br_ssl_engine_inject_entropy(&server->eng, seed, sizeof seed);
    assert(br_ssl_server_reset(server));
    bool done = false;
    for (unsigned step = 0; step < 20000; step++) {
        tls_state state = os64_tls_engine_state(client);
        if (state.status != TLS_OK || (state.flags & TLS_HANDSHAKE_DONE)) {
            if ((state.status == TLS_OK) != success)
                fprintf(stderr, "epoch=%lld status=%d policy=%d upstream=%d expected success=%d\n",
                    (long long)io.epoch, state.status, state.policy_reason, state.upstream_error, success);
            assert((state.status == TLS_OK) == success);
            assert(!!(state.flags & TLS_HANDSHAKE_DONE) == success);
            assert(state.policy_reason == reason);
            if (!success) {
                assert(state.status == TLS_CERTIFICATE);
                assert(!(state.flags & (TLS_SEND_PLAIN | TLS_RECV_PLAIN)));
            }
            done = true; break;
        }
        size_t n;
        unsigned char *p = br_ssl_engine_recvrec_buf(&server->eng, &n);
        if (p && n && (state.flags & TLS_SEND_CIPHER)) {
            if (n > 37) n = 37;
            tls_transfer moved = os64_tls_engine_take(client, p, n);
            if (moved.transferred) br_ssl_engine_recvrec_ack(&server->eng, moved.transferred);
        }
        p = br_ssl_engine_sendrec_buf(&server->eng, &n);
        state = os64_tls_engine_state(client);
        if (p && n && (state.flags & TLS_RECV_CIPHER)) {
            if (n > 37) n = 37;
            tls_transfer moved = os64_tls_engine_feed(client, p, n);
            if (moved.transferred) br_ssl_engine_sendrec_ack(&server->eng, moved.transferred);
        }
    }
    assert(done && io.opens == 1 && io.closes == 1 && io.at == 32 && io.clocks == 1);
    if (success) application_data(client, server);
    os64_tls_engine_destroy(client); free(records); free(server);
}
int main(void)
{
    reset_io();
    os64_tls_trust *s = trust();
    tls_name alpn = {"http/1.1", 8};
    tls_os_config cfg = {.hostname = {"example.test", 12}, .trust = s, .alpn = &alpn, .alpn_count = 1};
    failure(NULL, TLS_BAD_ARGUMENT);
    assert(create(&cfg, NULL) == TLS_BAD_ARGUMENT && !io.clocks);
    tls_os_config bad = cfg; bad.trust = NULL; failure(&bad, TLS_BAD_ARGUMENT);
    bad = cfg; bad.hostname = (tls_name){"bad name", 8}; failure(&bad, TLS_BAD_ARGUMENT);
    bad = cfg; bad.alpn = NULL; failure(&bad, TLS_BAD_ARGUMENT);
    assert(!io.opens);
    os64_tls_trust *empty; assert(os64_tls_trust_create(&empty) == TLS_OK);
    bad = cfg; bad.trust = empty; failure(&bad, TLS_BAD_ARGUMENT); os64_tls_trust_free(empty);
    assert(!io.opens);
    reset_io(); io.clock_result = -1; failure(&cfg, TLS_BAD_TIME); assert(!io.opens && io.clocks == 1);
    for (unsigned i = 0; i < 2; i++) {
        reset_io(); io.epoch = i ? INT64_MAX : INT64_MIN;
        failure(&cfg, TLS_BAD_TIME); assert(!io.opens);
    }
    for (unsigned i = 1; i <= 2; i++) {
        reset_io(); fail_allocation = calls + i;
        failure(&cfg, TLS_NO_MEMORY); assert(!io.opens); fail_allocation = 0;
    }
    reset_io(); io.refuse_open = true; failure(&cfg, TLS_ENTROPY_UNAVAILABLE);
    assert(io.opens == 1 && !io.reads && !io.closes);
    for (size_t at = 0; at < 32; at++) for (unsigned kind = 0; kind < 3; kind++) {
        reset_io(); io.fragment = 1; io.fail_at = at;
        io.failure = kind == 0 ? -1 : kind == 1 ? 0 : (int64_t)(33 - at);
        failure(&cfg, TLS_ENTROPY_UNAVAILABLE);
        assert(io.reads == at + 1 && io.opens == 1 && io.closes == 1);
    }
    reset_io(); io.close_result = -1; failure(&cfg, TLS_ENTROPY_UNAVAILABLE);
    assert(io.at == 32 && io.closes == 1);
    puts("PASS input/clock/allocation errors, 96 seed-offset refusals and handle cleanup");
    for (size_t fragment = 1; fragment <= 32; fragment++) {
        reset_io(); io.fragment = fragment; io.handle = fragment == 32 ? INT32_MAX : 0;
        os64_tls_engine *c; assert(create(&cfg, &c) == TLS_OK);
        assert(io.at == 32 && io.reads == (32 + fragment - 1) / fragment && io.closes == 1);
        tls_state state = os64_tls_engine_state(c);
        assert(state.status == TLS_OK && (state.flags & TLS_SEND_CIPHER) && !(state.flags & TLS_HANDSHAKE_DONE));
        assert(os64_tls_engine_abort(c, TLS_CANCELLED) == TLS_CANCELLED);
        os64_tls_engine_destroy(c);
    }
    puts("PASS bounded short-read completion, handle zero, fresh creation and wiped engine cleanup");
    os64_tls_trust_free(s);
    for (unsigned i = 0; i < 5; i++) {
        reset_io(); cfg.trust = trust();
        io.epoch = i == 1 ? 2208988801LL : i == 2 ? -1 : i == 3 ? 1577836800LL : 1788868800;
        io.timezone = i == 3 ? -720 : 840;
        cfg.hostname = i == 4 ? (tls_name){"wrong.test", 10} : (tls_name){"example.test", 12};
        handshake(&cfg, i == 0 || i == 3, i == 4 ? TLS_POLICY_SAN : TLS_POLICY_OK);
    }
    assert(!live_allocations);
    puts("PASS five policy handshakes, bidirectional application data and clean close");
    return 0;
}
