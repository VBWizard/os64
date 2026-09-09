#ifndef OS64_TLS_CLIENT_ENGINE_H
#define OS64_TLS_CLIENT_ENGINE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bearssl.h"
#include "../include/tls/tls.h"

// Private engine boundary, not an installed application API. A production
// caller must provide the policy-enforcing validator described in TLS.md.
typedef struct os64_tls_engine os64_tls_engine;
// Internal spellings share the public value types and constants.
typedef os64_tls_status_t tls_status;
typedef os64_tls_policy_reason_t tls_policy_reason;
typedef os64_tls_transfer_t tls_transfer;
typedef os64_tls_state_t tls_state;
typedef os64_tls_name_t tls_name;
#define TLS_BAD_ARGUMENT OS64_TLS_BAD_ARGUMENT
#define TLS_BAD_TIME OS64_TLS_BAD_TIME
#define TLS_CANCELLED OS64_TLS_CANCELLED
#define TLS_CERTIFICATE OS64_TLS_CERTIFICATE
#define TLS_CLEAN_EOF OS64_TLS_CLEAN_EOF
#define TLS_CLOSING OS64_TLS_CLOSING
#define TLS_ENTROPY_UNAVAILABLE OS64_TLS_ENTROPY_UNAVAILABLE
#define TLS_HANDSHAKE_DONE OS64_TLS_HANDSHAKE_DONE
#define TLS_LIMIT OS64_TLS_LIMIT
#define TLS_NEED_PROGRESS OS64_TLS_NEED_PROGRESS
#define TLS_NO_MEMORY OS64_TLS_NO_MEMORY
#define TLS_OK OS64_TLS_OK
#define TLS_POLICY_ANCHOR OS64_TLS_POLICY_ANCHOR
#define TLS_POLICY_CA OS64_TLS_POLICY_CA
#define TLS_POLICY_CRITICAL OS64_TLS_POLICY_CRITICAL
#define TLS_POLICY_DER OS64_TLS_POLICY_DER
#define TLS_POLICY_DUPLICATE OS64_TLS_POLICY_DUPLICATE
#define TLS_POLICY_EKU OS64_TLS_POLICY_EKU
#define TLS_POLICY_EXTENSION OS64_TLS_POLICY_EXTENSION
#define TLS_POLICY_KEY OS64_TLS_POLICY_KEY
#define TLS_POLICY_KEY_USAGE OS64_TLS_POLICY_KEY_USAGE
#define TLS_POLICY_LIMIT OS64_TLS_POLICY_LIMIT
#define TLS_POLICY_OK OS64_TLS_POLICY_OK
#define TLS_POLICY_SAN OS64_TLS_POLICY_SAN
#define TLS_POLICY_SEQUENCE OS64_TLS_POLICY_SEQUENCE
#define TLS_POLICY_SIGNATURE OS64_TLS_POLICY_SIGNATURE
#define TLS_PROTOCOL OS64_TLS_PROTOCOL
#define TLS_RECV_CIPHER OS64_TLS_RECV_CIPHER
#define TLS_RECV_PLAIN OS64_TLS_RECV_PLAIN
#define TLS_SEND_CIPHER OS64_TLS_SEND_CIPHER
#define TLS_SEND_PLAIN OS64_TLS_SEND_PLAIN
#define TLS_TIMEOUT OS64_TLS_TIMEOUT
#define TLS_TRANSPORT OS64_TLS_TRANSPORT
#define TLS_TRUNCATED OS64_TLS_TRUNCATED
#define TLS_UNSUPPORTED OS64_TLS_UNSUPPORTED

#define TLS_HANDSHAKE_CIPHER_MAX 1048576u

typedef struct {
    // A factory call returns a fresh owned validator. Its destruction function
    // must release any retained immutable trust snapshot. On failure, a
    // non-NULL output is also transferred for cleanup. Factory context is
    // borrowed only during create; the validator owns its later dependencies.
    tls_status (*create)(void *context, const char *hostname,
                         uint32_t days, uint32_t seconds,
                         const br_x509_class ***out);
    void (*destroy)(const br_x509_class **validator);
    void *context;
    // Optional bounded diagnostic from the owned validator; no factory context
    // is retained. Without this callback, engine state reports TLS_POLICY_OK.
    tls_policy_reason (*policy_reason)(const br_x509_class *const *validator);
} tls_validator_factory;

typedef struct {
    tls_name hostname; // ASCII DNS name, copied and lowercased; no IP literals.
    const tls_name *alpn; // Up to 8 nonempty names, 255 bytes each, 1024 total.
    size_t alpn_count;
    int64_t epoch; // Explicit UTC seconds; no clock lookup.
    // Synchronous, fills the entire requested seed on TLS_OK. No retained
    // engine pointers or callback reentry. Partial fills are not success.
    tls_status (*entropy)(void *context, unsigned char *out, size_t length);
    void *entropy_context;
    tls_validator_factory validator;
} tls_engine_config;

// Each connection has one serialized owner. Inputs are borrowed for this
// call; hostname/ALPN and callback results needed later become engine-owned.
// Failure sets *out=NULL and releases acquired resources. No session reuse.
tls_status os64_tls_engine_create(const tls_engine_config *config, os64_tls_engine **out);
void os64_tls_engine_destroy(os64_tls_engine *engine);
tls_state os64_tls_engine_state(os64_tls_engine *engine);

// Copy one accepted prefix. A status and a nonzero count can coexist when
// processing those bytes reaches a terminal state. Preserve that count.
// Zero-length calls do not acknowledge a BearSSL buffer. Programming errors
// return BAD_ARGUMENT without poisoning an otherwise live connection.
tls_transfer os64_tls_engine_feed(os64_tls_engine *engine, const void *data, size_t length);
tls_transfer os64_tls_engine_take(os64_tls_engine *engine, void *data, size_t length);
tls_transfer os64_tls_engine_write(os64_tls_engine *engine, const void *data, size_t length);
tls_transfer os64_tls_engine_read(os64_tls_engine *engine, void *data, size_t length);
tls_status os64_tls_engine_flush(os64_tls_engine *engine);

// Close stops new plaintext writes. Drain already buffered inbound plaintext
// and outgoing records before starting the close exchange. Bytes arriving
// after that point follow BearSSL's close semantics (application data discard).
// Closing during the initial handshake cancels the connection.
tls_status os64_tls_engine_close(os64_tls_engine *engine);
// EOF forbids new ciphertext input/writes, but permits draining authenticated
// plaintext, accepted output and a pending close reply. A bare FIN is not clean EOF.
tls_status os64_tls_engine_eof(os64_tls_engine *engine);
// Abort stops all I/O. The first terminal reason is retained.
tls_status os64_tls_engine_abort(os64_tls_engine *engine, tls_status reason);
#endif
