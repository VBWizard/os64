#ifndef OS64_TLS_CERTIFICATE_POLICY_H
#define OS64_TLS_CERTIFICATE_POLICY_H
#include "client_engine.h"

// Private policy boundary. Extension rules and ownership are in
// TLS_CERTIFICATE_POLICY.md; no application-supplied bypass is exposed here.
#define TLS_CERTIFICATE_MAX 32768u
#define TLS_CHAIN_MAX 8u
#define TLS_ANCHORS_MAX 256u
#define TLS_ANCHOR_BYTES_MAX 1048576u
// Keep a live owner reference while using the factory to create validators.
// Successful validators retain the sealed snapshot independently.
tls_validator_factory os64_tls_policy_factory(os64_tls_trust *store);
typedef struct { tls_policy_reason reason; unsigned upstream_error; } tls_policy_result;
tls_policy_result os64_tls_policy_result(const br_x509_class *const *validator);
#endif
