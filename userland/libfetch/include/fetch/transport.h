#ifndef FETCH_TRANSPORT_H
#define FETCH_TRANSPORT_H
// transport.h — the byte source under the HTTP parser: one connected socket,
// plain or wrapped in TLS, with the idle deadline and the caller's
// cancellation applied to every wait. fetch.c owns one per hop; the host
// harness (tools/test_fetch_transport_host.sh) drives it with scripted
// os64_read_for/os64_tls_transport_* stubs.
#include "tls/transport.h"

#define FETCH_IDLE_MS_DEFAULT 30000u

// One owner, referenced in place by the HTTP stream. Error detail survives
// cleanup so the HTTP caller can explain a failed read after releasing I/O.
//
// CANCELLATION IS THE CALLER'S PREDICATE, asked before every wait and after
// every interrupted one. The thing that cancels is a signal handler or a UI
// thread, and both already keep a flag of their own; a library-side flag
// would be that flag written twice. NULL = never cancelled.
typedef struct {
    int32_t handle;
    os64_tls_transport *tls;
    struct {
        os64_tls_status_t status;
        os64_tls_policy_reason_t policy_reason;
        int upstream_error;
    } error;
    bool encrypted, silent;
    uint32_t idle_ms;                // the deadline every wait is measured against
    bool (*cancelled)(void *ctx);
    void *cancel_ctx;
} fetch_transport_t;

// Takes ownership of the connected handle, including on failure. NULL config
// selects plain TCP. TLS success includes authentication and flight submission.
// `idle_ms` 0 = FETCH_IDLE_MS_DEFAULT.
bool fetch_transport_open(fetch_transport_t *io, int32_t handle,
                          const os64_tls_config_t *config, uint32_t idle_ms,
                          bool (*cancel_fn)(void *ctx), void *cancel_ctx);
bool fetch_transport_write(fetch_transport_t *io, const void *data, size_t length);
// http_source_fn's shape: `context` is the fetch_transport_t.
int64_t fetch_transport_read(void *context, void *data, size_t capacity);
// Validate transport status at a completed HTTP boundary (headers or body).
// Independent framing can survive missing closure; protocol failures cannot.
bool fetch_transport_complete(fetch_transport_t *io, bool independently_framed);
// Normal completion attempts bounded TLS closure. Failure/redirect cleanup
// does not wait. Repeated cleanup is harmless; it does not clear diagnostics.
void fetch_transport_close(fetch_transport_t *io, bool normal);
// Was the transport's failure a TLS one worth naming? (The status is only
// meaningful on an encrypted transport that did not end cleanly.)
bool fetch_transport_tls_failed(const fetch_transport_t *io);
#endif
