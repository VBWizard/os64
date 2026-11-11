#ifndef OS64_LIBTLS_TRANSPORT_H
#define OS64_LIBTLS_TRANSPORT_H
#include "tls.h"
#if defined(OS64_TLS_BUILD_PUBLIC)
#define OS64_TLS_TRANSPORT_API __attribute__((visibility("default")))
#else
#define OS64_TLS_TRANSPORT_API
#endif

typedef struct os64_tls_transport os64_tls_transport;
typedef struct { uint64_t handshake_ms, shutdown_ms; } os64_tls_transport_limits_t;
#define OS64_TLS_TRANSPORT_HANDSHAKE_MS 30000u
#define OS64_TLS_TRANSPORT_SHUTDOWN_MS 2000u
// The most ciphertext a transport holds unread: the room release needs.
#define OS64_TLS_TRANSPORT_UNREAD_MAX 4096u

// Adopt a connected TCP handle on success, leaving ownership with the caller
// on failure (*out=NULL). NULL limits select the defaults above; explicit
// limits must be nonzero and finite. DNS/dial are outside these TLS budgets.
// The caller supplies a connected TCP handle; creation does not query its
// type. One serialized owner; do not use the handle or reenter operations.
OS64_TLS_TRANSPORT_API os64_tls_status_t os64_tls_transport_create(const os64_tls_config_t *config,
    int32_t handle, const os64_tls_transport_limits_t *limits, os64_tls_transport **out);
// State inspection enforces deadlines and may close the handle on terminal cleanup.
OS64_TLS_TRANSPORT_API os64_tls_state_t os64_tls_transport_state(os64_tls_transport *transport);

// Try ready network directions, then wait up to timeout_ms, capped by the
// remaining protocol budget and rounded up by the kernel to ticks. When
// both directions need service, cap the wait at 10 ms and alternate them.
// 0 polls; UINT64_MAX is refused. OK means progress; NEED_PROGRESS means an
// empty or interrupted wait, or that plaintext/output needs caller service.
// A caught I/O signal returns NEED_PROGRESS promptly with the connection and
// pending bytes intact. The caller may resume or explicitly abort(CANCELLED).
// Handshake/shutdown expiry aborts the connection.
OS64_TLS_TRANSPORT_API os64_tls_status_t os64_tls_transport_step(os64_tls_transport *transport, uint64_t timeout_ms);

// Nonblocking authenticated plaintext prefixes. Inspect transferred even
// on an error. Accepted output needs flush/step to reach TCP. No implicit
// application-operation deadline: the caller owns its budget across calls.
OS64_TLS_TRANSPORT_API os64_tls_transfer_t os64_tls_transport_read(os64_tls_transport *transport, void *data, size_t length);
OS64_TLS_TRANSPORT_API os64_tls_transfer_t os64_tls_transport_write(os64_tls_transport *transport, const void *data, size_t length);
OS64_TLS_TRANSPORT_API os64_tls_status_t os64_tls_transport_flush(os64_tls_transport *transport);

// Start closure once; repeated calls retain the original shutdown deadline.
// Drain pending plaintext and step until CLEAN_EOF or failure. Closing before
// authentication cancels. Terminal cleanup closes the handle once; free also
// destroys/wipes the client. Abort/free do not wait for a TLS close exchange.
OS64_TLS_TRANSPORT_API os64_tls_status_t os64_tls_transport_begin_close(os64_tls_transport *transport);
OS64_TLS_TRANSPORT_API os64_tls_status_t os64_tls_transport_abort(os64_tls_transport *transport, os64_tls_status_t reason);
OS64_TLS_TRANSPORT_API void os64_tls_transport_free(os64_tls_transport *transport);

// Hand the connection to a caller that will drive the engine itself (tls.h)
// and own the socket: an HTTP/2 connection, whose reader and senders are
// different threads and so cannot be this transport's one owner. On OK the
// client and the handle are the caller's, `unread` holds the ciphertext the
// transport had read from the socket and not yet fed (feed it before
// anything read after), and the transport is wiped and freed with nothing
// closed and no close exchange begun. Refused, with the transport as it was
// and still the caller's to free: NEED_PROGRESS before the handshake has
// completed, or while ciphertext of the transport's own is still owed to
// the socket (step until it is not); the terminal status of a transport
// that has failed or begun to close; BAD_ARGUMENT when `room` is under what
// is unread (OS64_TLS_TRANSPORT_UNREAD_MAX always holds it).
OS64_TLS_TRANSPORT_API os64_tls_status_t os64_tls_transport_release(os64_tls_transport *transport,
    os64_tls_client **client, int32_t *handle, void *unread, size_t room, size_t *unread_len);
#undef OS64_TLS_TRANSPORT_API
#endif
