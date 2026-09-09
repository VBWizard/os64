#include "certificate_der.h"
#include "os64/mem.h"
#include "os64/str.h"

struct os64_tls_trust {
    unsigned references;
    bool sealed;
    size_t count, bytes;
    br_x509_trust_anchor anchors[TLS_ANCHORS_MAX];
    void *storage[TLS_ANCHORS_MAX];
};
typedef struct {
    const br_x509_class *vtable;
    br_x509_minimal_context minimal;
    // No anchors: keep checking the supplied chain past the trusted prefix.
    br_x509_minimal_context full_chain;
    os64_tls_trust *trust;
    char hostname[254];
    unsigned char certificate[TLS_CERTIFICATE_MAX];
    size_t expected, received, total, count;
    tls_policy_result result;
    bool started, in_cert, ended, accepted;
} policy_validator;

_Static_assert(__atomic_always_lock_free(sizeof(unsigned), 0), "trust references need inline atomics");

tls_status os64_tls_trust_create(os64_tls_trust **out)
{
    if (!out) return TLS_BAD_ARGUMENT;
    *out = os64_malloc(sizeof **out);
    if (!*out) return TLS_NO_MEMORY;
    os64_memset(*out, 0, sizeof **out);
    (*out)->references = 1;
    return TLS_OK;
}
void os64_tls_trust_free(os64_tls_trust *store)
{
    if (!store || __atomic_sub_fetch(&store->references, 1, __ATOMIC_ACQ_REL)) return;
    for (size_t i = 0; i < store->count; i++) os64_free(store->storage[i]);
    os64_free(store);
}
static bool retain(os64_tls_trust *store)
{
    unsigned refs = __atomic_load_n(&store->references, __ATOMIC_RELAXED);
    do {
        if (!refs || refs == UINT32_MAX) return false;
    } while (!__atomic_compare_exchange_n(&store->references, &refs, refs + 1,
        false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    return true;
}
tls_status os64_tls_trust_add_der(os64_tls_trust *store, const void *der,
                                 size_t length, tls_policy_reason *reason)
{
    if (reason) *reason = TLS_POLICY_OK;
    if (!store || store->sealed || !der || !length) return TLS_BAD_ARGUMENT;
    if (store->count == TLS_ANCHORS_MAX) {
        if (reason) *reason = TLS_POLICY_LIMIT;
        return TLS_LIMIT;
    }
    tls_certificate_view view;
    tls_policy_reason result = os64_tls_certificate_inspect(der, length, 2, NULL, &view);
    if (reason) *reason = result;
    if (result != TLS_POLICY_OK) return result == TLS_POLICY_LIMIT ? TLS_LIMIT : TLS_CERTIFICATE;
    size_t keybytes = view.key.key_type == BR_KEYTYPE_RSA ?
        view.key.key.rsa.nlen + view.key.key.rsa.elen : view.key.key.ec.qlen;
    size_t bytes = view.subject.length + keybytes;
    if (bytes > TLS_ANCHOR_BYTES_MAX - store->bytes) {
        if (reason) *reason = TLS_POLICY_LIMIT;
        return TLS_LIMIT;
    }
    unsigned char *copy = os64_malloc(bytes);
    if (!copy) return TLS_NO_MEMORY;
    os64_memcpy(copy, view.subject.data, view.subject.length);
    br_x509_trust_anchor anchor = {
        .dn = {copy, view.subject.length}, .flags = BR_X509_TA_CA, .pkey = view.key
    };
    unsigned char *key = copy + view.subject.length;
    if (view.key.key_type == BR_KEYTYPE_RSA) {
        os64_memcpy(key, view.key.key.rsa.n, view.key.key.rsa.nlen);
        os64_memcpy(key + view.key.key.rsa.nlen, view.key.key.rsa.e, view.key.key.rsa.elen);
        anchor.pkey.key.rsa.n = key;
        anchor.pkey.key.rsa.e = key + view.key.key.rsa.nlen;
    } else {
        os64_memcpy(key, view.key.key.ec.q, keybytes);
        anchor.pkey.key.ec.q = key;
    }
    store->anchors[store->count] = anchor;
    store->storage[store->count++] = copy;
    store->bytes += bytes;
    return TLS_OK;
}
tls_status os64_tls_trust_seal(os64_tls_trust *store)
{
    if (!store || !store->count) return TLS_BAD_ARGUMENT;
    store->sealed = true;
    return TLS_OK;
}
static void reject(policy_validator *v, tls_policy_reason reason)
{
    if (v->result.reason == TLS_POLICY_OK) v->result.reason = reason;
}
static void start_chain(const br_x509_class **ctx, const char *hostname)
{
    policy_validator *v = (policy_validator *)ctx;
    if (v->started) { reject(v, TLS_POLICY_SEQUENCE); return; }
    v->started = true;
    if (!hostname || os64_strlen(hostname) != os64_strlen(v->hostname)) {
        reject(v, TLS_POLICY_SAN);
    } else {
        for (size_t i = 0; v->hostname[i]; i++)
            if (hostname[i] != v->hostname[i]) { reject(v, TLS_POLICY_SAN); break; }
    }
    v->minimal.vtable->start_chain(&v->minimal.vtable, v->hostname);
    v->full_chain.vtable->start_chain(&v->full_chain.vtable, v->hostname);
}
static void start_cert(const br_x509_class **ctx, uint32_t length)
{
    policy_validator *v = (policy_validator *)ctx;
    if (!v->started || v->in_cert || v->ended) { reject(v, TLS_POLICY_SEQUENCE); return; }
    v->in_cert = true; v->expected = length; v->received = 0;
    if (v->count == TLS_CHAIN_MAX || length > TLS_CERTIFICATE_MAX ||
        length > TLS_CHAIN_MAX * TLS_CERTIFICATE_MAX - v->total) reject(v, TLS_POLICY_LIMIT);
    else { v->count++; v->total += length; }
    if (!length) reject(v, TLS_POLICY_DER);
    v->minimal.vtable->start_cert(&v->minimal.vtable, length);
    v->full_chain.vtable->start_cert(&v->full_chain.vtable, length);
}
static void append(const br_x509_class **ctx, const unsigned char *data, size_t length)
{
    policy_validator *v = (policy_validator *)ctx;
    if (!v->in_cert || v->ended || (length && !data)) { reject(v, TLS_POLICY_SEQUENCE); return; }
    if (length > v->expected - v->received) { reject(v, TLS_POLICY_DER); return; }
    if (v->result.reason != TLS_POLICY_OK || !length) return;
    os64_memcpy(v->certificate + v->received, data, length);
    v->received += length;
    v->minimal.vtable->append(&v->minimal.vtable, data, length);
    v->full_chain.vtable->append(&v->full_chain.vtable, data, length);
}
static void end_cert(const br_x509_class **ctx)
{
    policy_validator *v = (policy_validator *)ctx;
    if (!v->in_cert || v->ended) { reject(v, TLS_POLICY_SEQUENCE); return; }
    v->in_cert = false;
    if (v->received != v->expected) reject(v, TLS_POLICY_DER);
    if (v->result.reason == TLS_POLICY_OK) {
        tls_certificate_view view;
        reject(v, os64_tls_certificate_inspect(v->certificate, v->received,
            v->count == 1 ? 0 : 1, v->hostname, &view));
    }
    v->minimal.vtable->end_cert(&v->minimal.vtable);
    v->full_chain.vtable->end_cert(&v->full_chain.vtable);
}
static unsigned end_chain(const br_x509_class **ctx)
{
    policy_validator *v = (policy_validator *)ctx;
    if (!v->ended) {
        if (!v->started || v->in_cert || !v->count) reject(v, TLS_POLICY_SEQUENCE);
        v->result.upstream_error = v->minimal.vtable->end_chain(&v->minimal.vtable);
        unsigned full_error = v->full_chain.vtable->end_chain(&v->full_chain.vtable);
        // NOT_TRUSTED is the expected completion of the anchor-free check;
        // authentication still requires success from the snapshot-backed one.
        if (!v->result.upstream_error && full_error != BR_ERR_X509_NOT_TRUSTED)
            v->result.upstream_error = full_error ? full_error : BR_ERR_X509_NOT_TRUSTED;
        v->ended = true;
    }
    v->accepted = v->result.reason == TLS_POLICY_OK && !v->result.upstream_error;
    if (v->result.upstream_error) return v->result.upstream_error;
    return v->accepted ? 0 : BR_ERR_X509_NOT_TRUSTED;
}
static const br_x509_pkey *get_pkey(const br_x509_class *const *ctx, unsigned *usages)
{
    const policy_validator *v = (const policy_validator *)ctx;
    if (!v->accepted || v->result.reason != TLS_POLICY_OK) {
        if (usages) *usages = 0;
        return NULL;
    }
    return v->minimal.vtable->get_pkey(&v->minimal.vtable, usages);
}
static const br_x509_class policy_vtable = {
    sizeof(policy_validator), start_chain, start_cert, append, end_cert, end_chain, get_pkey
};
static void minimal_init(br_x509_minimal_context *m, const br_x509_trust_anchor *anchors,
                         size_t count, uint32_t days, uint32_t seconds)
{
    br_x509_minimal_init(m, &br_sha256_vtable, anchors, count);
    br_x509_minimal_set_rsa(m, br_rsa_i31_pkcs1_vrfy);
    br_x509_minimal_set_ecdsa(m, &br_ec_all_m31, br_ecdsa_i31_vrfy_asn1);
    br_x509_minimal_set_minrsa(m, 256);
    br_x509_minimal_set_hash(m, br_sha256_ID, &br_sha256_vtable);
    br_x509_minimal_set_hash(m, br_sha384_ID, &br_sha384_vtable);
    br_x509_minimal_set_hash(m, br_sha512_ID, &br_sha512_vtable);
    br_x509_minimal_set_time(m, days, seconds);
}
static tls_status create(void *context, const char *hostname, uint32_t days,
                         uint32_t seconds, const br_x509_class ***out)
{
    if (!out) return TLS_BAD_ARGUMENT;
    *out = NULL;
    os64_tls_trust *store = context;
    // The engine validates and normalizes DNS syntax before invoking us.
    if (!store || !store->sealed || !hostname || !*hostname || os64_strlen(hostname) > 253)
        return TLS_BAD_ARGUMENT;
    if (!days || seconds >= 86400) return TLS_BAD_TIME;
    policy_validator *v = os64_malloc(sizeof *v);
    if (!v) return TLS_NO_MEMORY;
    os64_memset(v, 0, sizeof *v);
    if (!retain(store)) { os64_free(v); return TLS_LIMIT; }
    v->trust = store; v->vtable = &policy_vtable;
    os64_memcpy(v->hostname, hostname, os64_strlen(hostname) + 1);
    minimal_init(&v->minimal, store->anchors, store->count, days, seconds);
    minimal_init(&v->full_chain, NULL, 0, days, seconds);
    *out = &v->vtable;
    return TLS_OK;
}
static void destroy(const br_x509_class **ctx)
{
    if (!ctx) return;
    policy_validator *v = (policy_validator *)ctx;
    os64_tls_trust_free(v->trust);
    os64_free(v);
}
tls_validator_factory os64_tls_policy_factory(os64_tls_trust *store)
{
    return (tls_validator_factory){create, destroy, store};
}
tls_policy_result os64_tls_policy_result(const br_x509_class *const *ctx)
{
    if (!ctx || *ctx != &policy_vtable) return (tls_policy_result){TLS_POLICY_SEQUENCE, 0};
    return ((const policy_validator *)ctx)->result;
}
