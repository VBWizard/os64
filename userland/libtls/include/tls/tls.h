#ifndef OS64_LIBTLS_TLS_H
#define OS64_LIBTLS_TLS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Define only while building libtls; private fixture objects stay hidden.
#if defined(OS64_TLS_BUILD_PUBLIC)
#define OS64_TLS_API __attribute__((visibility("default")))
#else
#define OS64_TLS_API
#endif

typedef struct os64_tls_client os64_tls_client;
typedef struct os64_tls_trust os64_tls_trust;
typedef enum {
    OS64_TLS_OK, OS64_TLS_NEED_PROGRESS, OS64_TLS_CLEAN_EOF, OS64_TLS_BAD_ARGUMENT, OS64_TLS_NO_MEMORY,
    OS64_TLS_LIMIT, OS64_TLS_ENTROPY_UNAVAILABLE, OS64_TLS_BAD_TIME, OS64_TLS_CERTIFICATE,
    OS64_TLS_UNSUPPORTED, OS64_TLS_PROTOCOL, OS64_TLS_TRUNCATED, OS64_TLS_TRANSPORT, OS64_TLS_TIMEOUT,
    OS64_TLS_CANCELLED
} os64_tls_status_t;
typedef enum {
    OS64_TLS_POLICY_OK, OS64_TLS_POLICY_DER, OS64_TLS_POLICY_LIMIT, OS64_TLS_POLICY_DUPLICATE,
    OS64_TLS_POLICY_EXTENSION, OS64_TLS_POLICY_CRITICAL, OS64_TLS_POLICY_SAN,
    OS64_TLS_POLICY_EKU, OS64_TLS_POLICY_CA, OS64_TLS_POLICY_KEY_USAGE, OS64_TLS_POLICY_KEY,
    OS64_TLS_POLICY_SIGNATURE, OS64_TLS_POLICY_ANCHOR, OS64_TLS_POLICY_SEQUENCE
} os64_tls_policy_reason_t;

enum {
    OS64_TLS_RECV_CIPHER = 1u << 0, OS64_TLS_SEND_CIPHER = 1u << 1,
    OS64_TLS_RECV_PLAIN = 1u << 2, OS64_TLS_SEND_PLAIN = 1u << 3,
    OS64_TLS_HANDSHAKE_DONE = 1u << 4, OS64_TLS_CLOSING = 1u << 5
};
typedef struct { os64_tls_status_t status; size_t transferred; } os64_tls_transfer_t;
typedef struct {
    os64_tls_status_t status;
    unsigned flags;
    int upstream_error; // Opaque diagnostic detail; its values are not a stable ABI.
    os64_tls_policy_reason_t policy_reason;
    const char *alpn; // Client-owned; valid until os64_tls_free, or NULL.
} os64_tls_state_t;
typedef struct { const char *data; size_t length; } os64_tls_name_t;

#define OS64_TLS_STORE_PATH_MAX 256u
#define OS64_TLS_STORE_PEM_MAX (2u * 1024u * 1024u)
#define OS64_TLS_STORE_LINE_MAX 1024u
#define OS64_TLS_STORE_CONFIG_MAX 8191u
#define OS64_TLS_STORE_DEFAULT "/etc/certs/roots.pem"
typedef enum {
    OS64_TLS_STORE_OK, OS64_TLS_STORE_BAD_ARGUMENT, OS64_TLS_STORE_NO_MEMORY, OS64_TLS_STORE_LIMIT,
    OS64_TLS_STORE_CONFIG, OS64_TLS_STORE_FORMAT, OS64_TLS_STORE_CERTIFICATE,
    OS64_TLS_STORE_OPEN, OS64_TLS_STORE_IO
} os64_tls_store_status_t;
typedef struct {
    os64_tls_store_status_t status;
    os64_tls_policy_reason_t policy_reason;
    size_t line, certificates;
} os64_tls_store_detail_t;
typedef struct {
    // True means failure occurred while selecting/reading/parsing config.
    bool config_stage;
    char config_path[OS64_TLS_STORE_PATH_MAX], bundle_path[OS64_TLS_STORE_PATH_MAX];
    os64_tls_store_detail_t detail;
} os64_tls_store_report_t;


typedef struct {
    os64_tls_name_t hostname; // ASCII DNS name; copied, lowercased, no IP literals.
    const os64_tls_name_t *alpn; // Up to 8 names, 255 bytes each, 1024 total.
    size_t alpn_count;
    os64_tls_trust *trust; // Borrowed sealed snapshot; retained by the client.
} os64_tls_config_t;

// One serialized owner per client. Creation samples OS UTC and fresh entropy;
// failure clears *out. Creation does not acquire a transport handle.
OS64_TLS_API os64_tls_status_t os64_tls_client_create(const os64_tls_config_t *config, os64_tls_client **out);
OS64_TLS_API void os64_tls_free(os64_tls_client *client);
OS64_TLS_API os64_tls_state_t os64_tls_state(os64_tls_client *client);

// Copy an accepted prefix. A nonzero count and terminal error may coexist.
// The caller retains unaccepted input and any ciphertext taken but not sent.
// Plaintext is gated on authentication. Zero-length calls make no progress;
// zero progress while live means the caller must drive the available directions.
OS64_TLS_API os64_tls_transfer_t os64_tls_feed_ciphertext(os64_tls_client *client, const void *data, size_t length);
OS64_TLS_API os64_tls_transfer_t os64_tls_take_ciphertext(os64_tls_client *client, void *data, size_t length);
OS64_TLS_API os64_tls_transfer_t os64_tls_write_plaintext(os64_tls_client *client, const void *data, size_t length);
OS64_TLS_API os64_tls_transfer_t os64_tls_read_plaintext(os64_tls_client *client, void *data, size_t length);
OS64_TLS_API os64_tls_status_t os64_tls_flush(os64_tls_client *client);

// Close stops new writes and drains buffered plaintext/output before the close
// exchange; closing during the handshake cancels. Drive ciphertext to finish.
// EOF forbids new input/writes but permits draining; a bare FIN is not clean EOF.
// Abort accepts TRANSPORT, TIMEOUT or CANCELLED, stops I/O and preserves the
// first terminal reason. Free never blocks and wipes client secrets. None of these operations closes a transport handle.
OS64_TLS_API os64_tls_status_t os64_tls_begin_close(os64_tls_client *client);
OS64_TLS_API os64_tls_status_t os64_tls_input_eof(os64_tls_client *client);
OS64_TLS_API os64_tls_status_t os64_tls_abort(os64_tls_client *client, os64_tls_status_t reason);
OS64_TLS_API const char *os64_tls_status_name(os64_tls_status_t status);
OS64_TLS_API const char *os64_tls_license(void);

// A builder has one serialized owner. Failed additions leave existing anchors
// intact. Sealed snapshots cannot be changed; clients retain their own reference.
OS64_TLS_API os64_tls_status_t os64_tls_trust_create(os64_tls_trust **out);
OS64_TLS_API os64_tls_status_t os64_tls_trust_add_der(os64_tls_trust *store, const void *der,
                                                   size_t length, os64_tls_policy_reason_t *reason);
OS64_TLS_API os64_tls_status_t os64_tls_trust_seal(os64_tls_trust *store);
OS64_TLS_API void os64_tls_trust_free(os64_tls_trust *store);

// A synchronous borrowed reader returns EOF (0), an error (<0), or at most
// capacity bytes. No callback reentry or retained destination pointer.
typedef int64_t (*os64_tls_store_reader_t)(void *context, void *data, size_t capacity);
// Complete success produces a sealed snapshot; failure clears *out.
OS64_TLS_API os64_tls_store_status_t os64_tls_trust_load_pem(os64_tls_store_reader_t read, void *context,
                                                         os64_tls_trust **out, os64_tls_store_detail_t *detail);
// Caller serializes reload and creation, and owns *current (or NULL).
// Failure leaves *current unchanged. Paths are absolute; a bundle replaces
// the complete store. The no-path form selects tls.conf through the OS ladder.
OS64_TLS_API os64_tls_store_status_t os64_tls_trust_reload(os64_tls_trust **current, os64_tls_store_report_t *report);
OS64_TLS_API os64_tls_store_status_t os64_tls_trust_reload_path(os64_tls_trust **current, const char *path,
                                                             os64_tls_store_report_t *report);
OS64_TLS_API const char *os64_tls_store_status_name(os64_tls_store_status_t status);
#undef OS64_TLS_API
#endif
