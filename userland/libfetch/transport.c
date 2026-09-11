#include "fetch/transport.h"
#include "os64/os64.h"

typedef struct {
    uint64_t start, last;
    uint32_t rate;
} budget_t;

static bool live(os64_tls_status_t status)
{
    return status == OS64_TLS_OK || status == OS64_TLS_NEED_PROGRESS;
}

static void remember(fetch_transport_t *io, os64_tls_status_t status)
{
    if (!live(status) && live(io->error.status)) {
        if (io->tls) {
            os64_tls_state_t state = os64_tls_transport_state(io->tls);
            io->error.policy_reason = state.policy_reason;
            io->error.upstream_error = state.upstream_error;
        }
        io->error.status = status;
    }
    if (status == OS64_TLS_TIMEOUT) io->silent = true;
}

static bool cancelled(fetch_transport_t *io)
{
    if (io->cancelled == NULL || !io->cancelled(io->cancel_ctx)) return false;
    if (io->tls) os64_tls_transport_abort(io->tls, OS64_TLS_CANCELLED);
    io->error.status = OS64_TLS_CANCELLED;
    return true;
}

static bool start_budget(fetch_transport_t *io, budget_t *budget)
{
    os64_ticks_t now;
    if (os64_ticks(&now) < 0 || !now.per_second) {
        remember(io, OS64_TLS_BAD_TIME); return false;
    }
    *budget = (budget_t){now.ticks, now.ticks, now.per_second};
    return true;
}

// Fixed 30-second operations need no absolute tick addition (and therefore
// no overflow near the end of the clock). Round remaining patience upward.
static uint64_t patience(fetch_transport_t *io, budget_t *budget)
{
    os64_ticks_t now;
    if (cancelled(io)) return 0;
    if (os64_ticks(&now) < 0 || now.per_second != budget->rate || now.ticks < budget->last) {
        remember(io, OS64_TLS_BAD_TIME); return 0;
    }
    budget->last = now.ticks;
    uint64_t elapsed = now.ticks - budget->start;
    uint64_t limit = (uint64_t)budget->rate * (io->idle_ms / 1000u);
    if (elapsed >= limit) {
        remember(io, OS64_TLS_TIMEOUT); return 0;
    }
    return ((limit - elapsed) * 1000u + budget->rate - 1u) / budget->rate;
}

bool fetch_transport_open(fetch_transport_t *io, int32_t handle,
                          const os64_tls_config_t *config, uint32_t idle_ms,
                          bool (*cancel_fn)(void *ctx), void *cancel_ctx)
{
    *io = (fetch_transport_t){.handle = handle, .encrypted = config != NULL,
                              .idle_ms = idle_ms ? idle_ms : FETCH_IDLE_MS_DEFAULT,
                              .cancelled = cancel_fn, .cancel_ctx = cancel_ctx};
    if (cancelled(io)) goto fail;
    if (!config) return true;
    os64_tls_status_t status = os64_tls_transport_create(config, handle, NULL, &io->tls);
    remember(io, status);
    if (status != OS64_TLS_OK) goto fail;
    io->handle = -1; // The transport adopted the handle.
    for (;;) {
        if (cancelled(io)) goto fail;
        os64_tls_state_t state = os64_tls_transport_state(io->tls);
        remember(io, state.status);
        if (!live(state.status)) goto fail;
        if (state.flags & OS64_TLS_HANDSHAKE_DONE) return true;
        remember(io, os64_tls_transport_step(io->tls, io->idle_ms));
        if (!live(io->error.status)) goto fail;
    }
fail:
    fetch_transport_close(io, false);
    return false;
}

bool fetch_transport_write(fetch_transport_t *io, const void *data, size_t length)
{
    budget_t budget;
    if (!start_budget(io, &budget)) return false;
    const unsigned char *bytes = data;
    size_t sent = 0;
    bool flushed = false;
    for (;;) {
        uint64_t wait = patience(io, &budget);
        if (!wait || !live(io->error.status)) return false;
        if (!io->tls) {
            if (sent == length) return true;
            int64_t n = os64_write_for(io->handle, bytes + sent, length - sent, wait);
            if (n == OS64_INTERRUPTED || n == OS64_ERR_TIMEOUT) continue;
            if (n <= 0 || (uint64_t)n > length - sent) {
                remember(io, OS64_TLS_TRANSPORT); return false;
            }
            sent += (size_t)n;
            continue;
        }
        if (sent < length) {
            os64_tls_transfer_t moved = os64_tls_transport_write(io->tls, bytes + sent, length - sent);
            sent += moved.transferred;
            remember(io, moved.status);
            if (!live(moved.status)) return false;
        }
        if (sent == length && !flushed) {
            remember(io, os64_tls_transport_flush(io->tls));
            flushed = true;
        }
        os64_tls_state_t state = os64_tls_transport_state(io->tls);
        remember(io, state.status);
        if (!live(io->error.status) || !patience(io, &budget)) return false;
        if (flushed && !(state.flags & OS64_TLS_SEND_CIPHER)) return true;
        // A peer responding before the request is submitted must not force
        // us to discard response bytes or spin on caller-service backpressure.
        if (state.flags & OS64_TLS_RECV_PLAIN) {
            remember(io, OS64_TLS_LIMIT); return false;
        }
        wait = patience(io, &budget);
        if (!wait) return false;
        remember(io, os64_tls_transport_step(io->tls, wait));
    }
}

int64_t fetch_transport_read(void *context, void *data, size_t capacity)
{
    fetch_transport_t *io = context;
    if (cancelled(io)) return OS64_INTERRUPTED;
    if (io->error.status == OS64_TLS_CLEAN_EOF) return 0;
    if (!live(io->error.status)) return -1;
    budget_t budget;
    if (!start_budget(io, &budget)) return -1;
    for (;;) {
        uint64_t wait = patience(io, &budget);
        if (!wait) return -1;
        if (!io->tls) {
            int64_t n = os64_read_for(io->handle, data, capacity, wait);
            if (n == OS64_INTERRUPTED || n == OS64_ERR_TIMEOUT) continue;
            if (!patience(io, &budget)) return -1;
            if (n < 0 || (uint64_t)n > capacity) {
                remember(io, OS64_TLS_TRANSPORT); return -1;
            }
            return n;
        }
        os64_tls_transfer_t moved = os64_tls_transport_read(io->tls, data, capacity);
        remember(io, moved.status);
        if (cancelled(io)) return OS64_INTERRUPTED;
        if (!patience(io, &budget)) return -1;
        if (moved.transferred) return (int64_t)moved.transferred;
        if (moved.status == OS64_TLS_CLEAN_EOF) return 0;
        if (!live(moved.status)) return -1;
        wait = patience(io, &budget);
        if (!wait) return -1;
        remember(io, os64_tls_transport_step(io->tls, wait));
        if (!live(io->error.status))
            return io->error.status == OS64_TLS_CLEAN_EOF ? 0 : -1;
    }
}

bool fetch_transport_complete(fetch_transport_t *io, bool independently_framed)
{
    if (cancelled(io)) return false;
    if (io->tls) remember(io, os64_tls_transport_state(io->tls).status);
    os64_tls_status_t status = io->error.status;
    // A complete length/chunk-framed message survives missing TLS closure;
    // a close-framed message needs authenticated EOF (RFC 9112 section 9.8).
    if (independently_framed && (status == OS64_TLS_TRUNCATED || status == OS64_TLS_TRANSPORT)) return true;
    return live(status) || status == OS64_TLS_CLEAN_EOF;
}

void fetch_transport_close(fetch_transport_t *io, bool normal)
{
    if (io->tls) {
        if (normal && !cancelled(io)) {
            os64_tls_status_t status = os64_tls_transport_begin_close(io->tls);
            // Shutdown has its own retained two-second deadline. Drain extra
            // plaintext only for closure; it never reaches the selected file.
            unsigned char discard[1024];
            while (live(status) && !cancelled(io)) {
                os64_tls_transfer_t moved = os64_tls_transport_read(io->tls, discard, sizeof discard);
                status = moved.status;
                if (live(status)) status = os64_tls_transport_step(io->tls, OS64_TLS_TRANSPORT_SHUTDOWN_MS);
            }
        }
        os64_tls_transport *transport = io->tls;
        io->tls = NULL;
        os64_tls_transport_free(transport);
    }
    if (io->handle >= 0) {
        int32_t handle = io->handle;
        io->handle = -1;
        os64_close(handle);
    }
}

bool fetch_transport_tls_failed(const fetch_transport_t *io)
{
    return io->encrypted && !live(io->error.status) && io->error.status != OS64_TLS_CLEAN_EOF;
}
