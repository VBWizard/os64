// h2_peer.c — see h2_peer.h.
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "h2_peer.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/signal.h"
#include "os64/thread.h"

// A record: its kind, its number, its payload's length, its payload.
#define REC_HEAD 7u
#define REC_MOST 4096u
enum { REC_DATA = 1, REC_OWED = 2, REC_MAKE_OWE = 3, REC_CLOSE = 4 };

#define SOCKET_FIRST 100
#define PEER_WIRES 24
#define THREAD_HANDLE 1000

typedef struct {
    uint8_t *bytes;
    size_t at, len, room;
} run_t;

struct os64_tls_client {
    uint8_t out[1u << 18];
    size_t out_at, out_len;
    uint32_t out_seq, in_seq;
    uint8_t rec[REC_HEAD + REC_MOST];
    size_t rec_len;
    uint8_t plain[1u << 16];
    size_t plain_at, plain_len;
    atomic_bool failed;
    bool eof;                   // a close_notify was fed
    atomic_int inside;
};

// One connection: the socket's two directions, the client's engine, and the
// server's half of everything (its record numbers, its HPACK tables, what it
// has read). A case speaks for the server of the wire it has in hand (`W`,
// peer_use); the library's threads find a wire by its socket or its engine.
struct wire {
    run_t to_server, to_client, server_plain;
    bool stalled, hung_up, records_bad;
    size_t write_most;
    unsigned write_interrupts;
    unsigned closes, owed;
    uint32_t server_in_seq, server_out_seq;
    struct os64_tls_client engine;
    atomic_bool freed;
    bool open;
    h2_hpack_encoder_t server_encoder;
    h2_hpack_decoder_t server_decoder;
    uint32_t encoder_store[H2_HPACK_STORAGE(4096) / 4 + 1], decoder_store[H2_HPACK_STORAGE(4096) / 4 + 1];
    uint8_t decoder_scratch[4096];
};

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t moved = PTHREAD_COND_INITIALIZER;
static struct wire wires[PEER_WIRES];
static _Thread_local struct wire *W = &wires[0];
static _Atomic uint64_t skipped_ms;

static _Thread_local uint8_t decoded_bytes[1u << 17];
static _Thread_local h2_field_t decoded[512];
static _Thread_local size_t decoded_len, decoded_count;

static struct wire *wire_of(int32_t handle)
{
    return handle >= SOCKET_FIRST && handle < SOCKET_FIRST + PEER_WIRES ? &wires[handle - SOCKET_FIRST] : NULL;
}

static struct wire *wire_of_engine(const os64_tls_client *c)
{
    for (int i = 0; i < PEER_WIRES; i++)
        if (&wires[i].engine == c)
            return &wires[i];
    abort();
}

static void put(run_t *r, const void *bytes, size_t len)
{
    if (r->at == r->len)
        r->at = r->len = 0;
    if (r->room - r->len < len) {
        r->room = (r->len + len) * 2;
        r->bytes = realloc(r->bytes, r->room);
    }
    memcpy(r->bytes + r->len, bytes, len);
    r->len += len;
}

static void record(uint8_t *head, uint8_t kind, uint32_t seq, size_t len)
{
    head[0] = kind;
    head[1] = (uint8_t)(seq >> 24);
    head[2] = (uint8_t)(seq >> 16);
    head[3] = (uint8_t)(seq >> 8);
    head[4] = (uint8_t)seq;
    head[5] = (uint8_t)(len >> 8);
    head[6] = (uint8_t)len;
}

static uint32_t record_seq(const uint8_t *head) { return (uint32_t)head[1] << 24 | head[2] << 16 | head[3] << 8 | head[4]; }
static size_t record_len(const uint8_t *head) { return (size_t)head[5] << 8 | head[6]; }

// ── The OS, as the connection sees it ───────────────────────────────────

static uint64_t real_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static void until(struct timespec *t, uint64_t ms)
{
    clock_gettime(CLOCK_REALTIME, t);
    t->tv_sec += (time_t)(ms / 1000u);
    t->tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (t->tv_nsec >= 1000000000L) {
        t->tv_sec++;
        t->tv_nsec -= 1000000000L;
    }
}

void *os64_malloc(size_t size) { return malloc(size); }
void os64_free(void *p) { free(p); }
void os64_yield(void) { sched_yield(); }

int64_t os64_ticks(os64_ticks_t *t)
{
    *t = (os64_ticks_t){.ticks = real_ms() + atomic_load(&skipped_ms), .per_second = 1000};
    return 0;
}

int64_t os64_read_for(int32_t handle, void *buf, size_t len, uint64_t timeout_ms)
{
    struct wire *w = wire_of(handle);
    if (w == NULL)
        abort();
    struct timespec end;
    until(&end, timeout_ms);
    pthread_mutex_lock(&mu);
    if (w->closes != 0)
        abort();                // a read of a socket that was closed
    while (w->to_client.at == w->to_client.len && !w->hung_up)
        if (pthread_cond_timedwait(&moved, &mu, &end) != 0)
            break;
    int64_t got = OS64_ERR_TIMEOUT;
    if (w->to_client.at != w->to_client.len) {
        size_t n = w->to_client.len - w->to_client.at < len ? w->to_client.len - w->to_client.at : len;
        memcpy(buf, w->to_client.bytes + w->to_client.at, n);
        w->to_client.at += n;
        got = (int64_t)n;
    } else if (w->hung_up)
        got = 0;
    pthread_mutex_unlock(&mu);
    return got;
}

int64_t os64_write_for(int32_t handle, const void *buf, size_t len, uint64_t timeout_ms)
{
    struct wire *w = wire_of(handle);
    if (w == NULL || len == 0)
        abort();
    os64_ticks_t began, now;
    os64_ticks(&began);
    pthread_mutex_lock(&mu);
    if (w->closes != 0)
        abort();                // a write to a socket that was closed
    // A signal, twenty seconds into a write that moved nothing.
    if (w->stalled && timeout_ms != 0 && w->write_interrupts != 0) {
        w->write_interrupts--;
        atomic_fetch_add(&skipped_ms, 20000);
        pthread_mutex_unlock(&mu);
        return OS64_INTERRUPTED;
    }
    // Stalled: wait out the patience by the case's clock, which it may skip.
    for (;;) {
        os64_ticks(&now);
        if (!w->stalled || now.ticks - began.ticks >= timeout_ms)
            break;
        struct timespec nap;
        until(&nap, 5);
        pthread_cond_timedwait(&moved, &mu, &nap);
    }
    int64_t wrote = OS64_ERR_TIMEOUT;
    if (!w->stalled) {
        size_t n = w->write_most != 0 && w->write_most < len ? w->write_most : len;
        put(&w->to_server, buf, n);
        wrote = (int64_t)n;
        pthread_cond_broadcast(&moved);
    }
    pthread_mutex_unlock(&mu);
    return wrote;
}

int64_t os64_close(int32_t handle)
{
    struct wire *w = wire_of(handle);
    if (w != NULL) {
        pthread_mutex_lock(&mu);
        w->closes++;
        pthread_cond_broadcast(&moved);
        pthread_mutex_unlock(&mu);
    }
    return 0;
}

typedef struct {
    int64_t (*fn)(void *);
    void *arg;
} start_t;

static void *start(void *x)
{
    start_t s = *(start_t *)x;
    free(x);
    s.fn(s.arg);
    return NULL;
}

static atomic_uint thread_head_start_ms;
void peer_thread_head_start(unsigned ms) { atomic_store(&thread_head_start_ms, ms); }

int64_t os64_thread(int64_t (*fn)(void *), void *arg)
{
    pthread_t t;
    start_t *s = malloc(sizeof(*s));
    *s = (start_t){fn, arg};
    if (pthread_create(&t, NULL, start, s) != 0)
        return -1;
    pthread_detach(t);
    // A head start for the new thread, when a case wants what it does first
    // to be over before its maker goes on.
    unsigned ahead = atomic_load(&thread_head_start_ms);
    if (ahead != 0) {
        struct timespec nap = {.tv_sec = ahead / 1000u, .tv_nsec = (long)(ahead % 1000u) * 1000000L};
        nanosleep(&nap, NULL);
    }
    return THREAD_HANDLE;
}

// ── The engine stand-in ─────────────────────────────────────────────────

// One thread at a time: the connection drives its engine under its lock.
static void enter(os64_tls_client *c)
{
    if (atomic_exchange(&c->inside, 1) != 0)
        c->failed = true;
}
static void leave(os64_tls_client *c) { atomic_store(&c->inside, 0); }

static bool engine_owes(os64_tls_client *c, uint8_t kind, const void *payload, size_t len)
{
    if (c->out_at == c->out_len)
        c->out_at = c->out_len = 0;
    if (sizeof(c->out) - c->out_len < REC_HEAD + len)
        return false;
    record(c->out + c->out_len, kind, c->out_seq++, len);
    if (len != 0)
        memcpy(c->out + c->out_len + REC_HEAD, payload, len);
    c->out_len += REC_HEAD + len;
    return true;
}

os64_tls_state_t os64_tls_state(os64_tls_client *c)
{
    enter(c);
    // CLEAN_EOF from the moment the close is fed, plaintext still unread or not.
    os64_tls_state_t state = {.status = c->failed ? OS64_TLS_TRANSPORT : c->eof ? OS64_TLS_CLEAN_EOF : OS64_TLS_OK,
                              .flags = OS64_TLS_HANDSHAKE_DONE | OS64_TLS_SEND_PLAIN | OS64_TLS_RECV_CIPHER};
    if (c->out_at != c->out_len)
        state.flags |= OS64_TLS_SEND_CIPHER;
    if (c->plain_at != c->plain_len)
        state.flags |= OS64_TLS_RECV_PLAIN;
    leave(c);
    return state;
}

os64_tls_transfer_t os64_tls_write_plaintext(os64_tls_client *c, const void *data, size_t length)
{
    enter(c);
    size_t n = length < REC_MOST ? length : REC_MOST;
    if (n != 0 && !engine_owes(c, REC_DATA, data, n))
        n = 0;
    os64_tls_transfer_t r = {c->failed ? OS64_TLS_TRANSPORT : OS64_TLS_OK, n};
    leave(c);
    return r;
}

os64_tls_transfer_t os64_tls_take_ciphertext(os64_tls_client *c, void *data, size_t length)
{
    enter(c);
    size_t n = c->out_len - c->out_at < length ? c->out_len - c->out_at : length;
    memcpy(data, c->out + c->out_at, n);
    c->out_at += n;
    os64_tls_transfer_t r = {c->failed ? OS64_TLS_TRANSPORT : OS64_TLS_OK, n};
    leave(c);
    return r;
}

os64_tls_transfer_t os64_tls_feed_ciphertext(os64_tls_client *c, const void *data, size_t length)
{
    enter(c);
    const uint8_t *in = data;
    size_t fed = 0;
    while (fed < length && !c->failed && !c->eof) {
        if (c->plain_at == c->plain_len)
            c->plain_at = c->plain_len = 0;
        // A record is begun only with room for the most it can carry.
        if (c->rec_len == 0 && sizeof(c->plain) - c->plain_len < REC_MOST)
            break;
        c->rec[c->rec_len++] = in[fed++];
        if (c->rec_len < REC_HEAD || c->rec_len < REC_HEAD + record_len(c->rec))
            continue;
        size_t len = record_len(c->rec);
        if (len > REC_MOST || record_seq(c->rec) != c->in_seq++)
            c->failed = true;
        else if (c->rec[0] == REC_DATA) {
            memcpy(c->plain + c->plain_len, c->rec + REC_HEAD, len);
            c->plain_len += len;
        } else if (c->rec[0] == REC_MAKE_OWE)
            engine_owes(c, REC_OWED, NULL, 0);
        else if (c->rec[0] == REC_CLOSE)
            c->eof = true;
        else
            c->failed = true;
        c->rec_len = 0;
    }
    os64_tls_transfer_t r = {c->failed ? OS64_TLS_TRANSPORT : OS64_TLS_OK, fed};
    leave(c);
    return r;
}

os64_tls_transfer_t os64_tls_read_plaintext(os64_tls_client *c, void *data, size_t length)
{
    enter(c);
    size_t n = c->plain_len - c->plain_at < length ? c->plain_len - c->plain_at : length;
    memcpy(data, c->plain + c->plain_at, n);
    c->plain_at += n;
    os64_tls_transfer_t r = {c->failed ? OS64_TLS_TRANSPORT : OS64_TLS_OK, n};
    leave(c);
    return r;
}

os64_tls_status_t os64_tls_flush(os64_tls_client *c)
{
    (void)c;
    return OS64_TLS_OK;
}

void os64_tls_free(os64_tls_client *c)
{
    if (c != NULL)
        atomic_store(&wire_of_engine(c)->freed, true);
}

// ── The connection's two ends ───────────────────────────────────────────

// Under `mu`: a wire as new, its buffers kept for the next use.
static void wire_reset(struct wire *w)
{
    run_t a = w->to_server, b = w->to_client, c = w->server_plain;
    memset(w, 0, sizeof(*w));
    w->to_server = (run_t){.bytes = a.bytes, .room = a.room};
    w->to_client = (run_t){.bytes = b.bytes, .room = b.room};
    w->server_plain = (run_t){.bytes = c.bytes, .room = c.room};
    w->open = true;
    h2_hpack_encoder_init(&w->server_encoder, w->encoder_store, sizeof(w->encoder_store), 4096);
    h2_hpack_decoder_init(&w->server_decoder, w->decoder_store, sizeof(w->decoder_store), w->decoder_scratch,
                          sizeof(w->decoder_scratch), 4096);
}

void peer_begin(os64_tls_client **out_engine, int32_t *socket)
{
    pthread_mutex_lock(&mu);
    wire_reset(&wires[0]);
    pthread_mutex_unlock(&mu);
    W = &wires[0];
    *out_engine = &wires[0].engine;
    *socket = SOCKET_FIRST;
}

void peer_forget_all(void)
{
    pthread_mutex_lock(&mu);
    for (int i = 0; i < PEER_WIRES; i++)
        wires[i].open = false;
    pthread_mutex_unlock(&mu);
}

int peer_open(void)
{
    int at = -1;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < PEER_WIRES && at < 0; i++)
        if (!wires[i].open) {
            wire_reset(&wires[i]);
            at = i;
        }
    pthread_mutex_unlock(&mu);
    if (at < 0)
        abort();                // a case opened more connections than this has wires
    return at;
}

void peer_use(int index) { W = &wires[index]; }
int peer_index_of(int32_t socket) { return socket - SOCKET_FIRST; }

unsigned peer_sockets_open(void)
{
    unsigned n = 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < PEER_WIRES; i++)
        n += wires[i].open && wires[i].closes == 0;
    pthread_mutex_unlock(&mu);
    return n;
}

bool peer_readable(int32_t socket, uint64_t timeout_ms)
{
    struct wire *w = wire_of(socket);
    struct timespec end;
    until(&end, timeout_ms);
    pthread_mutex_lock(&mu);
    while (w->to_client.at == w->to_client.len && !w->hung_up)
        if (pthread_cond_timedwait(&moved, &mu, &end) != 0)
            break;
    bool ready = w->to_client.at != w->to_client.len || w->hung_up;
    pthread_mutex_unlock(&mu);
    return ready;
}

size_t peer_raw_take(void *out, size_t room, int timeout_ms)
{
    struct timespec end;
    until(&end, (uint64_t)timeout_ms);
    pthread_mutex_lock(&mu);
    while (W->to_server.at == W->to_server.len)
        if (pthread_cond_timedwait(&moved, &mu, &end) != 0)
            break;
    size_t n = W->to_server.len - W->to_server.at < room ? W->to_server.len - W->to_server.at : room;
    memcpy(out, W->to_server.bytes + W->to_server.at, n);
    W->to_server.at += n;
    pthread_mutex_unlock(&mu);
    return n;
}

void peer_raw_say(const void *bytes, size_t len)
{
    pthread_mutex_lock(&mu);
    put(&W->to_client, bytes, len);
    pthread_cond_broadcast(&moved);
    pthread_mutex_unlock(&mu);
}
os64_tls_client *peer_engine(int index) { return &wires[index].engine; }
int32_t peer_socket(int index) { return SOCKET_FIRST + index; }

// The server's side of the wire: every whole record the client has written,
// checked for its order and unwrapped.
static void pull(void)
{
    while (W->to_server.len - W->to_server.at >= REC_HEAD) {
        const uint8_t *head = W->to_server.bytes + W->to_server.at;
        size_t len = record_len(head);
        if (len > REC_MOST) {
            W->records_bad = true;
            W->to_server.at = W->to_server.len;
            return;
        }
        if (W->to_server.len - W->to_server.at < REC_HEAD + len)
            return;
        if (record_seq(head) != W->server_in_seq++ || (head[0] != REC_DATA && head[0] != REC_OWED))
            W->records_bad = true;
        else if (head[0] == REC_DATA)
            put(&W->server_plain, head + REC_HEAD, len);
        else
            W->owed++;
        W->to_server.at += REC_HEAD + len;
    }
}

bool peer_records_sound(void)
{
    pthread_mutex_lock(&mu);
    pull();
    bool sound = !W->records_bad && !W->engine.failed;
    pthread_mutex_unlock(&mu);
    return sound;
}

unsigned peer_closes(void)
{
    pthread_mutex_lock(&mu);
    unsigned n = W->closes;
    pthread_mutex_unlock(&mu);
    return n;
}

bool peer_engine_freed(void) { return atomic_load(&W->freed); }

bool peer_end(void) { return peer_closes() == 1 && peer_engine_freed() && peer_records_sound(); }

unsigned peer_owed_records(void)
{
    pthread_mutex_lock(&mu);
    pull();
    unsigned n = W->owed;
    pthread_mutex_unlock(&mu);
    return n;
}

void peer_stall(bool on)
{
    pthread_mutex_lock(&mu);
    W->stalled = on;
    pthread_cond_broadcast(&moved);
    pthread_mutex_unlock(&mu);
}

void peer_write_cap(size_t most)
{
    pthread_mutex_lock(&mu);
    W->write_most = most;
    pthread_mutex_unlock(&mu);
}

void peer_interrupt_writes(unsigned times)
{
    pthread_mutex_lock(&mu);
    W->write_interrupts = times;
    pthread_mutex_unlock(&mu);
}

void peer_hang_up(void)
{
    pthread_mutex_lock(&mu);
    W->hung_up = true;
    pthread_cond_broadcast(&moved);
    pthread_mutex_unlock(&mu);
}

void peer_clock_skip(uint64_t ms)
{
    atomic_fetch_add(&skipped_ms, ms);
    pthread_mutex_lock(&mu);
    pthread_cond_broadcast(&moved);
    pthread_mutex_unlock(&mu);
}

// Wait until the server has `need` bytes of plaintext it has not looked at.
// Called and returning with `mu` held.
static bool have(size_t need, int timeout_ms)
{
    struct timespec end;
    until(&end, (uint64_t)timeout_ms);
    for (;;) {
        pull();
        if (W->server_plain.len - W->server_plain.at >= need)
            return true;
        if (pthread_cond_timedwait(&moved, &mu, &end) != 0) {
            pull();
            return W->server_plain.len - W->server_plain.at >= need;
        }
    }
}

bool peer_preface(int timeout_ms)
{
    pthread_mutex_lock(&mu);
    bool ok = have(H2_PREFACE_LEN, timeout_ms) &&
              memcmp(W->server_plain.bytes + W->server_plain.at, h2_preface, H2_PREFACE_LEN) == 0;
    if (ok)
        W->server_plain.at += H2_PREFACE_LEN;
    pthread_mutex_unlock(&mu);
    return ok;
}

bool peer_frame(h2_frame_header_t *header, const uint8_t **payload, int timeout_ms)
{
    static uint8_t held[1u << 20];
    pthread_mutex_lock(&mu);
    bool ok = have(H2_FRAME_HEADER_LEN, timeout_ms);
    if (ok) {
        h2_frame_header_read(W->server_plain.bytes + W->server_plain.at, header);
        ok = header->length <= sizeof(held) && have(H2_FRAME_HEADER_LEN + header->length, timeout_ms);
    }
    if (ok) {
        memcpy(held, W->server_plain.bytes + W->server_plain.at + H2_FRAME_HEADER_LEN, header->length);
        W->server_plain.at += H2_FRAME_HEADER_LEN + header->length;
        *payload = held;
    }
    pthread_mutex_unlock(&mu);
    return ok;
}

bool peer_frame_of(uint8_t type, h2_frame_header_t *header, const uint8_t **payload, int timeout_ms)
{
    uint64_t end = real_ms() + (uint64_t)timeout_ms;
    for (;;) {
        uint64_t now = real_ms();
        if (now >= end || !peer_frame(header, payload, (int)(end - now)))
            return false;
        if (header->type == type)
            return true;
    }
}

static bool keep(void *ctx, const h2_field_t *f)
{
    (void)ctx;
    if (decoded_count == 512 || sizeof(decoded_bytes) - decoded_len < (size_t)f->name_len + f->value_len)
        return false;
    uint8_t *at = decoded_bytes + decoded_len;
    memcpy(at, f->name, f->name_len);
    memcpy(at + f->name_len, f->value, f->value_len);
    decoded[decoded_count++] = (h2_field_t){.name = at, .value = at + f->name_len, .name_len = f->name_len,
                                            .value_len = f->value_len, .sensitive = f->sensitive};
    decoded_len += (size_t)f->name_len + f->value_len;
    return true;
}

bool peer_decode(const uint8_t *block, size_t len, const h2_field_t **fields, size_t *count)
{
    decoded_len = decoded_count = 0;
    bool ok = h2_hpack_decode(&W->server_decoder, block, len, keep, NULL) == H2_HPACK_OK;
    *fields = decoded;
    *count = decoded_count;
    return ok;
}

// The server's plaintext as records onto `r`.
static void wrap(run_t *r, const uint8_t *plain, size_t len)
{
    for (size_t at = 0; at < len;) {
        size_t n = len - at < REC_MOST ? len - at : REC_MOST;
        uint8_t head[REC_HEAD];
        record(head, REC_DATA, W->server_out_seq++, n);
        put(r, head, REC_HEAD);
        put(r, plain + at, n);
        at += n;
    }
}

static void say(const uint8_t *plain, size_t len)
{
    pthread_mutex_lock(&mu);
    wrap(&W->to_client, plain, len);
    pthread_cond_broadcast(&moved);
    pthread_mutex_unlock(&mu);
}

void peer_send_frame(uint8_t type, uint8_t flags, uint32_t stream, const void *payload, size_t len)
{
    uint8_t *frame = malloc(H2_FRAME_HEADER_LEN + len);
    h2_frame_header_write(frame, &(h2_frame_header_t){.length = (uint32_t)len, .type = type, .flags = flags,
                                                      .stream = stream});
    if (len != 0)
        memcpy(frame + H2_FRAME_HEADER_LEN, payload, len);
    say(frame, H2_FRAME_HEADER_LEN + len);
    free(frame);
}

static size_t settings_frame(const h2_setting_t *settings, uint32_t count, uint8_t *out, size_t room)
{
    return h2_write_settings(out, room, settings, count);
}

void peer_send_settings(const h2_setting_t *settings, uint32_t count)
{
    uint8_t frame[H2_FRAME_HEADER_LEN + 64 * 6];
    say(frame, settings_frame(settings, count, frame, sizeof(frame)));
}

size_t peer_records_of_settings(const h2_setting_t *settings, uint32_t count, uint8_t *out, size_t room)
{
    uint8_t frame[H2_FRAME_HEADER_LEN + 64 * 6];
    size_t n = settings_frame(settings, count, frame, sizeof(frame));
    run_t r = {0};
    pthread_mutex_lock(&mu);
    wrap(&r, frame, n);
    pthread_mutex_unlock(&mu);
    size_t len = r.len <= room ? r.len : 0;
    memcpy(out, r.bytes, len);
    free(r.bytes);
    return len;
}

void peer_send_headers_split(uint32_t stream, const h2_field_t *fields, size_t count, bool end_stream, size_t piece)
{
    size_t bound = h2_hpack_encode_bound(fields, count), len;
    uint8_t *block = malloc(bound);
    h2_hpack_encode(&W->server_encoder, fields, count, block, bound, &len);
    size_t at = len < piece ? len : piece;
    peer_send_frame(H2_FRAME_HEADERS, (end_stream ? H2_FLAG_END_STREAM : 0) | (at == len ? H2_FLAG_END_HEADERS : 0),
                    stream, block, at);
    while (at < len) {
        size_t n = len - at < piece ? len - at : piece;
        peer_send_frame(H2_FRAME_CONTINUATION, at + n == len ? H2_FLAG_END_HEADERS : 0, stream, block + at, n);
        at += n;
    }
    free(block);
}

void peer_send_headers(uint32_t stream, const h2_field_t *fields, size_t count, bool end_stream)
{
    peer_send_headers_split(stream, fields, count, end_stream, H2_FRAME_SIZE_LEAST);
}

void peer_send_data(uint32_t stream, const void *bytes, size_t len, bool end_stream)
{
    peer_send_frame(H2_FRAME_DATA, end_stream ? H2_FLAG_END_STREAM : 0, stream, bytes, len);
}

void peer_send_window(uint32_t stream, uint32_t increment)
{
    uint8_t four[4] = {(uint8_t)(increment >> 24), (uint8_t)(increment >> 16), (uint8_t)(increment >> 8),
                       (uint8_t)increment};
    peer_send_frame(H2_FRAME_WINDOW_UPDATE, 0, stream, four, 4);
}

void peer_send_rst(uint32_t stream, uint32_t code)
{
    uint8_t four[4] = {(uint8_t)(code >> 24), (uint8_t)(code >> 16), (uint8_t)(code >> 8), (uint8_t)code};
    peer_send_frame(H2_FRAME_RST_STREAM, 0, stream, four, 4);
}

void peer_send_goaway(uint32_t last_stream, uint32_t code)
{
    uint8_t eight[8] = {(uint8_t)(last_stream >> 24), (uint8_t)(last_stream >> 16), (uint8_t)(last_stream >> 8),
                        (uint8_t)last_stream, (uint8_t)(code >> 24), (uint8_t)(code >> 16), (uint8_t)(code >> 8),
                        (uint8_t)code};
    peer_send_frame(H2_FRAME_GOAWAY, 0, 0, eight, 8);
}

void peer_send_ping(void)
{
    static const uint8_t eight[8] = {'p', 'i', 'n', 'g', 'p', 'i', 'n', 'g'};
    peer_send_frame(H2_FRAME_PING, 0, 0, eight, 8);
}

void peer_send_data_and_close(uint32_t stream, const void *bytes, size_t len)
{
    uint8_t frame[H2_FRAME_HEADER_LEN + 256], head[REC_HEAD];
    if (len > 256)
        abort();
    h2_frame_header_write(frame, &(h2_frame_header_t){.length = (uint32_t)len, .type = H2_FRAME_DATA,
                                                      .flags = H2_FLAG_END_STREAM, .stream = stream});
    memcpy(frame + H2_FRAME_HEADER_LEN, bytes, len);
    pthread_mutex_lock(&mu);
    wrap(&W->to_client, frame, H2_FRAME_HEADER_LEN + len);
    record(head, REC_CLOSE, W->server_out_seq++, 0);
    put(&W->to_client, head, REC_HEAD);
    pthread_cond_broadcast(&moved);
    pthread_mutex_unlock(&mu);
}

void peer_engine_holds_settings(const h2_setting_t *settings, uint32_t count)
{
    pthread_mutex_lock(&mu);
    W->engine.plain_len += settings_frame(settings, count, W->engine.plain + W->engine.plain_len,
                                         sizeof(W->engine.plain) - W->engine.plain_len);
    pthread_mutex_unlock(&mu);
}

void peer_send_engine_owes(void)
{
    uint8_t head[REC_HEAD];
    pthread_mutex_lock(&mu);
    record(head, REC_MAKE_OWE, W->server_out_seq++, 0);
    put(&W->to_client, head, REC_HEAD);
    pthread_cond_broadcast(&moved);
    pthread_mutex_unlock(&mu);
}
