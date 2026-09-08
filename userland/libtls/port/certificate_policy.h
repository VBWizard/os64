#ifndef OS64_TLS_CERTIFICATE_POLICY_H
#define OS64_TLS_CERTIFICATE_POLICY_H
#include "client_engine.h"

// Private policy boundary. Extension rules and ownership are in
// TLS_CERTIFICATE_POLICY.md; no application-supplied bypass is exposed here.
#define TLS_CERTIFICATE_MAX 32768u
#define TLS_CHAIN_MAX 8u
#define TLS_ANCHORS_MAX 256u
#define TLS_ANCHOR_BYTES_MAX 1048576u
typedef enum {
    TLS_POLICY_OK, TLS_POLICY_DER, TLS_POLICY_LIMIT, TLS_POLICY_DUPLICATE,
    TLS_POLICY_EXTENSION, TLS_POLICY_CRITICAL, TLS_POLICY_SAN,
    TLS_POLICY_EKU, TLS_POLICY_CA, TLS_POLICY_KEY_USAGE, TLS_POLICY_KEY,
    TLS_POLICY_SIGNATURE, TLS_POLICY_ANCHOR, TLS_POLICY_SEQUENCE
} tls_policy_reason;
typedef struct os64_tls_trust os64_tls_trust;

tls_status os64_tls_trust_create(os64_tls_trust **out);
// A builder has one serialized owner. Failure leaves its existing anchors intact.
tls_status os64_tls_trust_add_der(os64_tls_trust *store, const void *der,
                                 size_t length, tls_policy_reason *reason);
tls_status os64_tls_trust_seal(os64_tls_trust *store);
void os64_tls_trust_free(os64_tls_trust *store);
// Keep a live owner reference while using the factory to create validators.
// Successful validators retain the sealed snapshot independently.
tls_validator_factory os64_tls_policy_factory(os64_tls_trust *store);
typedef struct { tls_policy_reason reason; unsigned upstream_error; } tls_policy_result;
tls_policy_result os64_tls_policy_result(const br_x509_class *const *validator);
#endif
