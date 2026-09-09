#ifndef OS64_TLS_CLIENT_ENGINE_H
#define OS64_TLS_CLIENT_ENGINE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bearssl.h"

// Private engine boundary, not an installed application API. A production
// caller must provide the policy-enforcing validator described in TLS.md.
typedef struct os64_tls_engine os64_tls_engine;
typedef enum {
    TLS_OK, TLS_NEED_PROGRESS, TLS_CLEAN_EOF, TLS_BAD_ARGUMENT, TLS_NO_MEMORY,
    TLS_LIMIT, TLS_ENTROPY_UNAVAILABLE, TLS_BAD_TIME, TLS_CERTIFICATE,
    TLS_UNSUPPORTED, TLS_PROTOCOL, TLS_TRUNCATED, TLS_TRANSPORT, TLS_TIMEOUT,
    TLS_CANCELLED
} tls_status;
typedef enum {
    TLS_POLICY_OK, TLS_POLICY_DER, TLS_POLICY_LIMIT, TLS_POLICY_DUPLICATE,
    TLS_POLICY_EXTENSION, TLS_POLICY_CRITICAL, TLS_POLICY_SAN,
    TLS_POLICY_EKU, TLS_POLICY_CA, TLS_POLICY_KEY_USAGE, TLS_POLICY_KEY,
    TLS_POLICY_SIGNATURE, TLS_POLICY_ANCHOR, TLS_POLICY_SEQUENCE
} tls_policy_reason;

#define TLS_HANDSHAKE_CIPHER_MAX 1048576u

enum {
    TLS_RECV_CIPHER = 1u << 0, TLS_SEND_CIPHER = 1u << 1,
    TLS_RECV_PLAIN = 1u << 2, TLS_SEND_PLAIN = 1u << 3,
    TLS_HANDSHAKE_DONE = 1u << 4, TLS_CLOSING = 1u << 5
};
typedef struct { tls_status status; size_t transferred; } tls_transfer;
typedef struct {
    tls_status status;
    unsigned flags;
    int upstream_error;
    tls_policy_reason policy_reason;
    const char *alpn; // Connection-owned; valid until destroy, or NULL.
} tls_state;
typedef struct { const char *data; size_t length; } tls_name;

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
