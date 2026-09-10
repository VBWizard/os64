#include "include/tls/transport.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/proc.h"
#include "os64/signal.h"

#define CIPHER_CAP 4096u
#define WAIT_SLICE_MS 10u
struct os64_tls_transport {
    os64_tls_client *client;
    int32_t handle;
    os64_tls_status_t terminal;
    uint64_t last_tick, deadline, shutdown_ms;
    uint32_t rate;
    bool handshake_done, closing, wait_read;
    unsigned char input[CIPHER_CAP], output[CIPHER_CAP];
    size_t input_at, input_end, output_at, output_end;
};

static void wipe(void *data, size_t size)
{
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}
static bool fatal(os64_tls_status_t status)
{
    return status != OS64_TLS_OK && status != OS64_TLS_NEED_PROGRESS && status != OS64_TLS_CLEAN_EOF;
}
static uint64_t deadline(uint64_t now, uint32_t rate, uint64_t ms)
{
    uint64_t seconds = ms / 1000, fraction = ((ms % 1000) * rate + 999) / 1000;
    if (seconds > (UINT64_MAX - fraction) / rate) return UINT64_MAX;
    uint64_t ticks = seconds * rate + fraction;
    return ticks > UINT64_MAX - now ? UINT64_MAX : now + ticks;
}
static uint64_t remaining_ms(uint64_t ticks, uint32_t rate)
{
    uint64_t seconds = ticks / rate, fraction = ((ticks % rate) * 1000 + rate - 1) / rate;
    return seconds > (UINT64_MAX - fraction) / 1000 ? UINT64_MAX : seconds * 1000 + fraction;
}
static void release_handle(os64_tls_transport *t)
{
    if (t->handle < 0) return;
    int32_t handle = t->handle;
    t->handle = -1;
    if (os64_close(handle) < 0 && t->terminal == OS64_TLS_OK) {
        t->terminal = OS64_TLS_TRANSPORT;
        os64_tls_abort(t->client, OS64_TLS_TRANSPORT);
    }
}
static os64_tls_status_t fail(os64_tls_transport *t, os64_tls_status_t reason)
{
    if (t->terminal == OS64_TLS_OK) {
        os64_tls_state_t state = os64_tls_state(t->client);
        t->terminal = fatal(state.status) ? state.status : reason;
        os64_tls_abort(t->client, reason == OS64_TLS_TIMEOUT || reason == OS64_TLS_CANCELLED ? reason : OS64_TLS_TRANSPORT);
        wipe(t->input, sizeof t->input); wipe(t->output, sizeof t->output);
        t->input_at = t->input_end = t->output_at = t->output_end = 0;
        release_handle(t);
    }
    return t->terminal;
}

// Check time before retiring a deadline on newly observed TLS progress.
static os64_tls_state_t inspect(os64_tls_transport *t)
{
    os64_tls_state_t state = {.status = OS64_TLS_BAD_ARGUMENT};
    if (!t) return state;
    state = os64_tls_state(t->client);
    if (t->terminal == OS64_TLS_OK && fatal(state.status)) fail(t, state.status);
    if (t->terminal == OS64_TLS_OK && t->handle >= 0) {
        os64_ticks_t now;
        if (os64_ticks(&now) < 0 || now.per_second != t->rate || now.ticks < t->last_tick)
            fail(t, OS64_TLS_BAD_TIME);
        else {
            t->last_tick = now.ticks;
            if ((!t->handshake_done || t->closing) && now.ticks >= t->deadline)
                fail(t, OS64_TLS_TIMEOUT);
            else {
                if (!t->closing && state.status == OS64_TLS_CLEAN_EOF &&
                    ((state.flags & OS64_TLS_SEND_CIPHER) || t->output_at < t->output_end)) {
                    t->closing = true;
                    t->deadline = deadline(now.ticks, t->rate, t->shutdown_ms);
                }
                if ((state.flags & OS64_TLS_HANDSHAKE_DONE) &&
                    !(state.flags & OS64_TLS_SEND_CIPHER) && t->output_at == t->output_end)
                    t->handshake_done = true;
            }
        }
    }
    if (t->terminal != OS64_TLS_OK) {
        state.status = t->terminal;
        state.flags &= OS64_TLS_HANDSHAKE_DONE | OS64_TLS_CLOSING;
        return state;
    }
    if (t->output_at < t->output_end) state.flags |= OS64_TLS_SEND_CIPHER;
    if (!t->handshake_done) state.flags &= ~(OS64_TLS_HANDSHAKE_DONE | OS64_TLS_SEND_PLAIN);
    if (t->closing) state.flags |= OS64_TLS_CLOSING;
    if (state.status == OS64_TLS_CLEAN_EOF) {
        if (state.flags & OS64_TLS_SEND_CIPHER) state.status = OS64_TLS_OK;
        else {
            release_handle(t);
            state.flags &= OS64_TLS_HANDSHAKE_DONE | OS64_TLS_CLOSING;
            if (t->terminal != OS64_TLS_OK) state.status = t->terminal;
        }
    }
    return state;
}

os64_tls_status_t os64_tls_transport_create(const os64_tls_config_t *config, int32_t handle,
    const os64_tls_transport_limits_t *limits, os64_tls_transport **out)
{
    if (!out) return OS64_TLS_BAD_ARGUMENT;
    *out = NULL;
    uint64_t handshake = limits ? limits->handshake_ms : OS64_TLS_TRANSPORT_HANDSHAKE_MS;
    uint64_t shutdown = limits ? limits->shutdown_ms : OS64_TLS_TRANSPORT_SHUTDOWN_MS;
    if (!config || handle < 0 || !handshake || !shutdown || handshake == UINT64_MAX || shutdown == UINT64_MAX)
        return OS64_TLS_BAD_ARGUMENT;
    os64_ticks_t now;
    if (os64_ticks(&now) < 0 || !now.per_second) return OS64_TLS_BAD_TIME;
    os64_tls_transport *t = os64_malloc(sizeof *t);
    if (!t) return OS64_TLS_NO_MEMORY;
    os64_memset(t, 0, sizeof *t);
    t->handle = -1; t->last_tick = now.ticks; t->rate = now.per_second;
    t->deadline = deadline(now.ticks, t->rate, handshake); t->shutdown_ms = shutdown;
    os64_tls_status_t status = os64_tls_client_create(config, &t->client);
    if (status == OS64_TLS_OK) {
        if (os64_ticks(&now) < 0 || now.per_second != t->rate || now.ticks < t->last_tick) status = OS64_TLS_BAD_TIME;
        else if (now.ticks >= t->deadline) status = OS64_TLS_TIMEOUT;
        else t->last_tick = now.ticks;
    }
    if (status != OS64_TLS_OK) {
        os64_tls_free(t->client); wipe(t, sizeof *t); os64_free(t); return status;
    }
    t->handle = handle; *out = t;
    return OS64_TLS_OK;
}

os64_tls_state_t os64_tls_transport_state(os64_tls_transport *t) { return inspect(t); }

static bool feed(os64_tls_transport *t)
{
    if (t->input_at == t->input_end || !(inspect(t).flags & OS64_TLS_RECV_CIPHER)) return false;
    os64_tls_transfer_t r = os64_tls_feed_ciphertext(t->client, t->input + t->input_at, t->input_end - t->input_at);
    t->input_at += r.transferred;
    if (fatal(r.status)) fail(t, r.status);
    return r.transferred != 0;
}
static bool take(os64_tls_transport *t)
{
    if (t->output_at != t->output_end || !(inspect(t).flags & OS64_TLS_SEND_CIPHER)) return false;
    os64_tls_transfer_t r = os64_tls_take_ciphertext(t->client, t->output, sizeof t->output);
    t->output_at = 0; t->output_end = r.transferred;
    if (fatal(r.status)) fail(t, r.status);
    return r.transferred != 0;
}
static bool can_read(os64_tls_transport *t)
{
    os64_tls_state_t state = inspect(t);
    return state.status == OS64_TLS_OK && t->handle >= 0 &&
           t->input_at == t->input_end && (state.flags & OS64_TLS_RECV_CIPHER);
}
static bool can_write(os64_tls_transport *t)
{
    return !fatal(inspect(t).status) && t->output_at < t->output_end;
}
static bool receive(os64_tls_transport *t, uint64_t ms, bool *interrupted)
{
    if (!can_read(t)) return false;
    int64_t n = os64_read_for(t->handle, t->input, sizeof t->input, ms);
    if (n == OS64_INTERRUPTED) { *interrupted = true; return false; }
    if (n == OS64_ERR_TIMEOUT) return false;
    if (n < 0 || (uint64_t)n > sizeof t->input) {
        fail(t, OS64_TLS_TRANSPORT); return false;
    }
    t->input_at = 0; t->input_end = (size_t)n;
    if (!n) os64_tls_input_eof(t->client);
    return true;
}
static bool send(os64_tls_transport *t, uint64_t ms, bool *interrupted)
{
    if (!can_write(t)) return false;
    size_t length = t->output_end - t->output_at;
    int64_t n = os64_write_for(t->handle, t->output + t->output_at, length, ms);
    if (n == OS64_INTERRUPTED) { *interrupted = true; return false; }
    if (n == OS64_ERR_TIMEOUT) return false;
    if (n <= 0 || (uint64_t)n > length) {
        fail(t, OS64_TLS_TRANSPORT); return false;
    }
    t->output_at += (size_t)n;
    return true;
}
static bool poll(os64_tls_transport *t, bool *interrupted)
{
    bool progress = feed(t);
    progress |= take(t);
    progress |= send(t, 0, interrupted);
    if (*interrupted) return progress;
    progress |= receive(t, 0, interrupted);
    if (*interrupted) return progress;
    progress |= feed(t);
    return progress;
}

os64_tls_status_t os64_tls_transport_step(os64_tls_transport *t, uint64_t timeout_ms)
{
    if (!t || timeout_ms == UINT64_MAX) return OS64_TLS_BAD_ARGUMENT;
    os64_tls_state_t state = inspect(t);
    if (state.status != OS64_TLS_OK) return state.status;
    bool interrupted = false;
    bool progress = poll(t, &interrupted);
    state = inspect(t);
    if (state.status != OS64_TLS_OK) return state.status;
    // A caught signal gives control back to the caller without another park.
    if (interrupted) return OS64_TLS_NEED_PROGRESS;
    if (progress) return OS64_TLS_OK;
    if (!timeout_ms || (state.flags & OS64_TLS_RECV_PLAIN)) return OS64_TLS_NEED_PROGRESS;
    bool reading = can_read(t), writing = can_write(t);
    if (!reading && !writing) {
        state = inspect(t);
        return state.status == OS64_TLS_OK ? OS64_TLS_NEED_PROGRESS : state.status;
    }
    uint64_t wait = timeout_ms;
    if (reading && writing && wait > WAIT_SLICE_MS) wait = WAIT_SLICE_MS;
    if (!t->handshake_done || t->closing) {
        os64_tls_status_t status = inspect(t).status;
        if (status != OS64_TLS_OK) return status;
        uint64_t ticks = t->deadline - t->last_tick;
        uint64_t remaining = remaining_ms(ticks, t->rate);
        if (remaining < wait) wait = remaining;
    }
    if (reading && (!writing || t->wait_read)) progress = receive(t, wait, &interrupted);
    else progress = send(t, wait, &interrupted);
    if (reading && writing) t->wait_read = !t->wait_read;
    if (!interrupted) progress |= poll(t, &interrupted);
    state = inspect(t);
    return state.status != OS64_TLS_OK ? state.status :
           interrupted ? OS64_TLS_NEED_PROGRESS : progress ? OS64_TLS_OK : OS64_TLS_NEED_PROGRESS;
}

static os64_tls_transfer_t plaintext(os64_tls_transport *t, void *data, size_t length, bool writing)
{
    os64_tls_transfer_t result = {OS64_TLS_BAD_ARGUMENT, 0};
    if (!t || (length && !data)) return result;
    result.status = inspect(t).status;
    if (result.status != OS64_TLS_OK) return result;
    if (writing && !t->handshake_done && length) {
        result.status = OS64_TLS_NEED_PROGRESS; return result;
    }
    result = writing ? os64_tls_write_plaintext(t->client, data, length) : os64_tls_read_plaintext(t->client, data, length);
    os64_tls_status_t after = inspect(t).status;
    if (after != OS64_TLS_OK) result.status = after;
    return result;
}
os64_tls_transfer_t os64_tls_transport_read(os64_tls_transport *t, void *p, size_t n) { return plaintext(t, p, n, false); }
os64_tls_transfer_t os64_tls_transport_write(os64_tls_transport *t, const void *p, size_t n) { return plaintext(t, (void *)p, n, true); }
os64_tls_status_t os64_tls_transport_flush(os64_tls_transport *t)
{
    os64_tls_state_t state = inspect(t);
    if (state.status != OS64_TLS_OK) return state.status;
    os64_tls_flush(t->client);
    return inspect(t).status;
}
os64_tls_status_t os64_tls_transport_begin_close(os64_tls_transport *t)
{
    os64_tls_state_t state = inspect(t);
    if (state.status != OS64_TLS_OK) return state.status;
    if (!t->closing) {
        t->closing = true;
        t->deadline = deadline(t->last_tick, t->rate, t->shutdown_ms);
        os64_tls_begin_close(t->client);
    }
    return inspect(t).status;
}
os64_tls_status_t os64_tls_transport_abort(os64_tls_transport *t, os64_tls_status_t reason)
{
    if (!t || (reason != OS64_TLS_CANCELLED && reason != OS64_TLS_TIMEOUT && reason != OS64_TLS_TRANSPORT))
        return OS64_TLS_BAD_ARGUMENT;
    os64_tls_state_t state = inspect(t);
    return state.status == OS64_TLS_OK ? fail(t, reason) : state.status;
}
void os64_tls_transport_free(os64_tls_transport *t)
{
    if (!t) return;
    os64_tls_transport_abort(t, OS64_TLS_CANCELLED);
    release_handle(t);
    os64_tls_free(t->client); wipe(t, sizeof *t); os64_free(t);
}
