#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../userland/libtls/port/certificate_der.h"

typedef struct { const unsigned char *data; size_t length; } blob;
typedef struct {
    const char *name;
    const blob *root, *chain[9];
    size_t count;
    tls_policy_reason reason;
    bool success;
    const char *hostname;
    int upstream;
    uint32_t days;
} policy_case;
typedef struct { const char *name; const blob *der; tls_policy_reason reason; } anchor_case;
#include "policy_corpus.h"

// Host-only atomic accounting; allocation injection is used outside threads.
static size_t allocated, freed, allocation_calls, fail_allocation;
void *os64_malloc(size_t n)
{
    size_t call = __atomic_add_fetch(&allocation_calls, 1, __ATOMIC_RELAXED);
    if (call == fail_allocation) return NULL;
    void *p = malloc(n); assert(p);
    __atomic_add_fetch(&allocated, 1, __ATOMIC_RELAXED);
    return p;
}
void os64_free(void *p)
{
    if (!p) return;
    __atomic_add_fetch(&freed, 1, __ATOMIC_RELAXED);
    free(p);
}
static os64_tls_trust *trust(const blob *root)
{
    os64_tls_trust *s = NULL;
    assert(os64_tls_trust_create(&s) == TLS_OK);
    tls_policy_reason reason;
    tls_status status = os64_tls_trust_add_der(s, root->data, root->length, &reason);
    if (status != TLS_OK) fprintf(stderr, "root add: %d / %d\n", status, reason);
    assert(status == TLS_OK);
    assert(os64_tls_trust_seal(s) == TLS_OK);
    return s;
}
static unsigned drive(const policy_case *c, const br_x509_class **v, size_t fragment, bool gated)
{
    (*v)->start_chain(v, c->hostname);
    for (size_t i = 0; i < c->count; i++) {
        const blob *b = c->chain[i];
        (*v)->start_cert(v, (uint32_t)b->length);
        for (size_t at = 0; at < b->length;) {
            size_t n = b->length - at;
            if (n > fragment) n = fragment;
            (*v)->append(v, b->data + at, n); at += n;
        }
        if (gated) (*v)->append(v, NULL, 0);
        (*v)->end_cert(v);
        if (gated) {
            unsigned usage = 123;
            assert(!(*v)->get_pkey(v, &usage) && !usage);
        }
    }
    return (*v)->end_chain(v);
}
static void raw_upstream(const policy_case *c)
{
    if (c->upstream < 0) return;
    tls_certificate_view view;
    assert(os64_tls_certificate_inspect(c->root->data, c->root->length, 2, NULL, &view) == TLS_POLICY_OK);
    br_x509_trust_anchor anchor = {
        .dn = {(unsigned char *)view.subject.data, view.subject.length},
        .flags = BR_X509_TA_CA, .pkey = view.key
    };
    br_x509_minimal_context *raw = malloc(sizeof *raw); assert(raw);
    br_x509_minimal_init_full(raw, &anchor, 1);
    br_x509_minimal_set_time(raw, c->days, 0);
    unsigned error = drive(c, &raw->vtable, 37, false);
    if ((!error) != (bool)c->upstream) fprintf(stderr, "%s upstream error %u\n", c->name, error);
    assert((!error) == (bool)c->upstream);
    free(raw);
}
static void check_case(const policy_case *c, size_t fragment)
{
    os64_tls_trust *s = trust(c->root);
    tls_validator_factory factory = os64_tls_policy_factory(s);
    const br_x509_class **v;
    assert(factory.create(s, c->hostname, c->days, 0, &v) == TLS_OK);
    // The validator must survive destruction of the store's owner reference.
    os64_tls_trust_free(s);
    size_t calls = allocation_calls;
    unsigned error = drive(c, v, fragment, true);
    tls_policy_result result = os64_tls_policy_result(v);
    if ((!error) != c->success || result.reason != c->reason)
        fprintf(stderr, "%s: error=%u reason=%d expected success=%d reason=%d\n",
            c->name, error, result.reason, c->success, c->reason);
    assert((!error) == c->success && result.reason == c->reason);
    assert((*v)->end_chain(v) == error);
    assert(((*v)->get_pkey(v, NULL) != NULL) == c->success);
    assert(allocation_calls == calls); // No peer-length-driven allocations.
    factory.destroy(v);
}
static void anchors_and_failures(void)
{
    os64_tls_trust *s = NULL;
    fail_allocation = allocation_calls + 1;
    assert(os64_tls_trust_create(&s) == TLS_NO_MEMORY && !s);
    fail_allocation = 0;
    for (size_t i = 0; i < sizeof anchor_cases / sizeof anchor_cases[0]; i++) {
        const anchor_case *c = &anchor_cases[i];
        assert(os64_tls_trust_create(&s) == TLS_OK);
        tls_policy_reason reason;
        tls_status status = os64_tls_trust_add_der(s, c->der->data, c->der->length, &reason);
        if (reason != c->reason) fprintf(stderr, "%s: reason=%d expected=%d\n", c->name, reason, c->reason);
        assert(reason == c->reason && (status == TLS_OK) == (reason == TLS_POLICY_OK));
        assert((os64_tls_trust_seal(s) == TLS_OK) == (status == TLS_OK));
        os64_tls_trust_free(s);
    }
    const blob *root = cases[0].root;
    assert(os64_tls_trust_create(&s) == TLS_OK);
    assert(os64_tls_trust_seal(s) == TLS_BAD_ARGUMENT);
    tls_validator_factory f = os64_tls_policy_factory(s);
    const br_x509_class **v = (void *)1;
    assert(f.create(s, "example.test", 740232, 0, &v) == TLS_BAD_ARGUMENT && !v);
    fail_allocation = allocation_calls + 1;
    assert(os64_tls_trust_add_der(s, root->data, root->length, NULL) == TLS_NO_MEMORY);
    assert(os64_tls_trust_seal(s) == TLS_BAD_ARGUMENT);
    fail_allocation = 0;
    unsigned char *copy = malloc(root->length); assert(copy);
    memcpy(copy, root->data, root->length);
    assert(os64_tls_trust_add_der(s, copy, root->length, NULL) == TLS_OK);
    memset(copy, 0xa5, root->length); free(copy);
    assert(os64_tls_trust_seal(s) == TLS_OK);
    assert(os64_tls_trust_add_der(s, root->data, root->length, NULL) == TLS_BAD_ARGUMENT);
    fail_allocation = allocation_calls + 1;
    assert(f.create(s, "example.test", 740232, 0, &v) == TLS_NO_MEMORY && !v);
    fail_allocation = 0;
    assert(f.create(s, "example.test", 740232, 86400, &v) == TLS_BAD_TIME && !v);
    assert(f.create(s, "example.test", 740232, 0, &v) == TLS_OK);
    assert(!drive(&cases[0], v, 1, true));
    f.destroy(v); os64_tls_trust_free(s);
    assert(os64_tls_trust_create(&s) == TLS_OK);
    for (size_t i = 0; i < TLS_ANCHORS_MAX; i++)
        assert(os64_tls_trust_add_der(s, root->data, root->length, NULL) == TLS_OK);
    assert(os64_tls_trust_add_der(s, root->data, root->length, NULL) == TLS_LIMIT);
    os64_tls_trust_free(s);
    assert(os64_tls_trust_create(&s) == TLS_OK);
    size_t count = 0;
    tls_status status;
    while ((status = os64_tls_trust_add_der(s, large_root->data, large_root->length, NULL)) == TLS_OK) count++;
    assert(status == TLS_LIMIT);
    assert(count > 0 && count < TLS_ANCHORS_MAX);
    assert(os64_tls_trust_seal(s) == TLS_OK);
    os64_tls_trust_free(s);
}
static void *concurrent(void *context)
{
    os64_tls_trust *s = context;
    tls_validator_factory f = os64_tls_policy_factory(s);
    for (size_t i = 0; i < 100; i++) {
        const br_x509_class **v;
        assert(f.create(s, "example.test", 740232, 0, &v) == TLS_OK);
        assert(!drive(&cases[0], v, 37, true));
        f.destroy(v);
    }
    return NULL;
}
static void ownership_and_sequence(void)
{
    os64_tls_trust *s = trust(cases[0].root);
    pthread_t threads[4];
    for (size_t i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, concurrent, s));
    for (size_t i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    tls_validator_factory f = os64_tls_policy_factory(s);
    for (unsigned test = 0; test < 6; test++) {
        const br_x509_class **v;
        assert(f.create(s, "example.test", 740232, 0, &v) == TLS_OK);
        if (test != 0) (*v)->start_chain(v, "example.test");
        if (test == 1) (*v)->append(v, (const unsigned char *)"x", 1);
        if (test == 2) { (*v)->start_cert(v, 1); (*v)->append(v, (const unsigned char *)"xx", 2); (*v)->end_cert(v); }
        if (test == 3) { (*v)->start_cert(v, 100); (*v)->end_cert(v); }
        if (test == 4) (*v)->start_chain(v, "example.test");
        if (test == 5) (*v)->start_cert(v, UINT32_MAX);
        assert((*v)->end_chain(v));
        assert(!(*v)->get_pkey(v, NULL));
        f.destroy(v);
    }
    os64_tls_trust_free(s);
}

static tls_status entropy(void *context, unsigned char *out, size_t length)
{
    (void)context;
    memset(out, 0x61, length);
    return TLS_OK;
}
static void handshake(const policy_case *c)
{
    os64_tls_trust *s = trust(c->root);
    tls_engine_config cfg = {
        .hostname = {c->hostname, strlen(c->hostname)}, .epoch = ((int64_t)c->days - 719528) * 86400,
        .entropy = entropy, .validator = os64_tls_policy_factory(s)
    };
    os64_tls_engine *client;
    assert(os64_tls_engine_create(&cfg, &client) == TLS_OK);
    os64_tls_trust_free(s);
    br_ssl_server_context *server = calloc(1, sizeof *server); assert(server);
    unsigned char *records = malloc(BR_SSL_BUFSIZE_BIDI); assert(records);
    br_x509_certificate chain[9];
    for (size_t i = 0; i < c->count; i++) chain[i] = (br_x509_certificate){(unsigned char *)c->chain[i]->data, c->chain[i]->length};
    br_ec_private_key key = {BR_EC_secp256r1, (unsigned char *)leaf_scalar, sizeof leaf_scalar};
    br_ssl_server_init_full_ec(server, chain, c->count, BR_KEYTYPE_EC, &key);
    uint16_t suite = BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256;
    br_ssl_engine_set_versions(&server->eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(&server->eng, &suite, 1);
    br_ssl_engine_set_buffer(&server->eng, records, BR_SSL_BUFSIZE_BIDI, 1);
    unsigned char seed[32] = {0x34};
    br_ssl_engine_inject_entropy(&server->eng, seed, sizeof seed);
    assert(br_ssl_server_reset(server));
    bool done = false;
    for (unsigned step = 0; step < 20000; step++) {
        tls_state state = os64_tls_engine_state(client);
        if (state.status != TLS_OK || (state.flags & TLS_HANDSHAKE_DONE)) {
            assert((state.status == TLS_OK) == c->success);
            assert(!!(state.flags & TLS_HANDSHAKE_DONE) == c->success);
            if (!c->success) {
                assert(state.status == TLS_CERTIFICATE);
                assert(!(state.flags & (TLS_SEND_PLAIN | TLS_RECV_PLAIN)));
                assert(!os64_tls_engine_write(client, "secret", 6).transferred);
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
    if (!done) fprintf(stderr, "handshake stalled: %s server error=%d\n", c->name, br_ssl_engine_last_error(&server->eng));
    assert(done);
    os64_tls_engine_destroy(client); free(server); free(records);
}
int main(void)
{
    tls_certificate_view view;
    const blob *leaf = cases[0].chain[0];
    for (size_t length = 0; length < leaf->length; length++)
        assert(os64_tls_certificate_inspect(leaf->data, length, 0, "example.test", &view) != TLS_POLICY_OK);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        raw_upstream(&cases[i]);
        check_case(&cases[i], 1);
        check_case(&cases[i], 37);
        check_case(&cases[i], 65536);
        if (!strcmp(cases[i].name, "valid") || !strcmp(cases[i].name, "cn-only") ||
            !strcmp(cases[i].name, "client-auth") || !strcmp(cases[i].name, "critical-eku") ||
            !strcmp(cases[i].name, "trailing-restriction-after-trust")) handshake(&cases[i]);
        printf("policy: %s PASS\n", cases[i].name);
    }
    anchors_and_failures();
    ownership_and_sequence();
    assert(allocated == freed);
    printf("TLS policy: %zu chain cases, %zu anchor cases, five TLS handshake gates, fragmented input, failure atomicity, owned snapshots, concurrent references PASS\n",
        sizeof cases / sizeof cases[0], sizeof anchor_cases / sizeof anchor_cases[0]);
    return 0;
}
