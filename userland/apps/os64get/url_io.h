#ifndef OS64GET_URL_IO_H
#define OS64GET_URL_IO_H
#include "tls/transport.h"

#define URL_IDLE_MS 30000u

// One owner, referenced in place by the HTTP stream. Error detail survives
// cleanup so the HTTP caller can explain a failed read after releasing I/O.
typedef struct {
    int32_t handle;
    os64_tls_transport *tls;
    os64_tls_state_t error;
    bool encrypted, silent;
} url_io_t;

// Takes ownership of the connected handle, including on failure. NULL config
// selects plain TCP. TLS success includes authentication and flight submission.
bool url_io_open(url_io_t *io, int32_t handle, const os64_tls_config_t *config);
bool url_io_write(url_io_t *io, const void *data, size_t length);
int64_t url_io_read(void *context, void *data, size_t capacity);
// Validate transport status at a completed HTTP boundary (headers or body).
// Independent framing can survive missing closure; protocol failures cannot.
bool url_io_complete(url_io_t *io, bool independently_framed);
// Normal completion attempts bounded TLS closure. Failure/redirect cleanup
// does not wait. Repeated cleanup is harmless; it does not clear diagnostics.
void url_io_close(url_io_t *io, bool normal);
void url_io_report(const url_io_t *io);
#endif
