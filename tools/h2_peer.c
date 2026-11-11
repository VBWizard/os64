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

#define SOCKET 7
#define THREAD_HANDLE 1000

typedef struct {
    uint8_t *bytes;
    size_t at, len, room;
} run_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t moved = PTHREAD_COND_INITIALIZER;
static run_t to_server, to_client, server_plain;
static bool stalled, hung_up, records_bad;
static size_t write_most;
static unsigned write_interrupts;
static unsigned closes, owed;
static uint32_t server_in_seq, server_out_seq;
static _Atomic uint64_t skipped_ms;

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
static struct os64_tls_client engine;
static atomic_bool engine_freed;

static h2_hpack_encoder_t server_encoder;
static h2_hpack_decoder_t server_decoder;
static uint32_t encoder_store[H2_HPACK_STORAGE(4096) / 4 + 1], decoder_store[H2_HPACK_STORAGE(4096) / 4 + 1];
static uint8_t decoder_scratch[4096];
static uint8_t decoded_bytes[1u << 17];
static h2_field_t decoded[512];
static size_t decoded_len, decoded_count;

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
    if (handle != SOCKET)
        abort();
    struct timespec end;
    until(&end, timeout_ms);
    pthread_mutex_lock(&mu);
    if (closes != 0)
        abort();                // a read of a socket that was closed
    while (to_client.at == to_client.len && !hung_up)
        if (pthread_cond_timedwait(&moved, &mu, &end) != 0)
            break;
    int64_t got = OS64_ERR_TIMEOUT;
    if (to_client.at != to_client.len) {
        size_t n = to_client.len - to_client.at < len ? to_client.len - to_client.at : len;
        memcpy(buf, to_client.bytes + to_client.at, n);
        to_client.at += n;
        got = (int64_t)n;
    } else if (hung_up)
        got = 0;
    pthread_mutex_unlock(&mu);
    return got;
}

int64_t os64_write_for(int32_t handle, const void *buf, size_t len, uint64_t timeout_ms)
{
    if (handle != SOCKET || len == 0)
        abort();
    os64_ticks_t began, now;
    os64_ticks(&began);
    pthread_mutex_lock(&mu);
    if (closes != 0)
        abort();                // a write to a socket that was closed
    // A signal, twenty seconds into a write that moved nothing.
    if (stalled && timeout_ms != 0 && write_interrupts != 0) {
        write_interrupts--;
        atomic_fetch_add(&skipped_ms, 20000);
        pthread_mutex_unlock(&mu);
        return OS64_INTERRUPTED;
    }
    // Stalled: wait out the patience by the case's clock, which it may skip.
    for (;;) {
        os64_ticks(&now);
        if (!stalled || now.ticks - began.ticks >= timeout_ms)
            break;
        struct timespec nap;
        until(&nap, 5);
        pthread_cond_timedwait(&moved, &mu, &nap);
    }
    int64_t wrote = OS64_ERR_TIMEOUT;
    if (!stalled) {
        size_t n = write_most != 0 && write_most < len ? write_most : len;
        put(&to_server, buf, n);
        wrote = (int64_t)n;
        pthread_cond_broadcast(&moved);
    }
    pthread_mutex_unlock(&mu);
    return wrote;
}

int64_t os64_close(int32_t handle)
{
    if (handle == SOCKET) {
        pthread_mutex_lock(&mu);
        closes++;
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

int64_t os64_thread(int64_t (*fn)(void *), void *arg)
{
    pthread_t t;
    start_t *s = malloc(sizeof(*s));
    *s = (start_t){fn, arg};
    if (pthread_create(&t, NULL, start, s) != 0)
        return -1;
    pthread_detach(t);
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
        atomic_store(&engine_freed, true);
}

// ── The connection's two ends ───────────────────────────────────────────

void peer_begin(os64_tls_client **out_engine, int32_t *socket)
{
    pthread_mutex_lock(&mu);
    to_server.at = to_server.len = to_client.at = to_client.len = server_plain.at = server_plain.len = 0;
    stalled = hung_up = records_bad = false;
    write_most = 0;
    write_interrupts = 0;
    closes = owed = 0;
    server_in_seq = server_out_seq = 0;
    memset(&engine, 0, sizeof(engine));
    atomic_store(&engine_freed, false);
    h2_hpack_encoder_init(&server_encoder, encoder_store, sizeof(encoder_store), 4096);
    h2_hpack_decoder_init(&server_decoder, decoder_store, sizeof(decoder_store), decoder_scratch,
                          sizeof(decoder_scratch), 4096);
    pthread_mutex_unlock(&mu);
    *out_engine = &engine;
    *socket = SOCKET;
}

// The server's side of the wire: every whole record the client has written,
// checked for its order and unwrapped.
static void pull(void)
{
    while (to_server.len - to_server.at >= REC_HEAD) {
        const uint8_t *head = to_server.bytes + to_server.at;
        size_t len = record_len(head);
        if (len > REC_MOST) {
            records_bad = true;
            to_server.at = to_server.len;
            return;
        }
        if (to_server.len - to_server.at < REC_HEAD + len)
            return;
        if (record_seq(head) != server_in_seq++ || (head[0] != REC_DATA && head[0] != REC_OWED))
            records_bad = true;
        else if (head[0] == REC_DATA)
            put(&server_plain, head + REC_HEAD, len);
        else
            owed++;
        to_server.at += REC_HEAD + len;
    }
}

bool peer_records_sound(void)
{
    pthread_mutex_lock(&mu);
    pull();
    bool sound = !records_bad && !engine.failed;
    pthread_mutex_unlock(&mu);
    return sound;
}

unsigned peer_closes(void)
{
    pthread_mutex_lock(&mu);
    unsigned n = closes;
    pthread_mutex_unlock(&mu);
    return n;
}

bool peer_engine_freed(void) { return atomic_load(&engine_freed); }

bool peer_end(void) { return peer_closes() == 1 && peer_engine_freed() && peer_records_sound(); }

unsigned peer_owed_records(void)
{
    pthread_mutex_lock(&mu);
    pull();
    unsigned n = owed;
    pthread_mutex_unlock(&mu);
    return n;
}

void peer_stall(bool on)
{
    pthread_mutex_lock(&mu);
    stalled = on;
    pthread_cond_broadcast(&moved);
    pthread_mutex_unlock(&mu);
}

void peer_write_cap(size_t most)
{
    pthread_mutex_lock(&mu);
    write_most = most;
    pthread_mutex_unlock(&mu);
}

void peer_interrupt_writes(unsigned times)
{
    pthread_mutex_lock(&mu);
    write_interrupts = times;
    pthread_mutex_unlock(&mu);
}

void peer_hang_up(void)
{
    pthread_mutex_lock(&mu);
    hung_up = true;
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
        if (server_plain.len - server_plain.at >= need)
            return true;
        if (pthread_cond_timedwait(&moved, &mu, &end) != 0) {
            pull();
            return server_plain.len - server_plain.at >= need;
        }
    }
}

bool peer_preface(int timeout_ms)
{
    pthread_mutex_lock(&mu);
    bool ok = have(H2_PREFACE_LEN, timeout_ms) &&
              memcmp(server_plain.bytes + server_plain.at, h2_preface, H2_PREFACE_LEN) == 0;
    if (ok)
        server_plain.at += H2_PREFACE_LEN;
    pthread_mutex_unlock(&mu);
    return ok;
}

bool peer_frame(h2_frame_header_t *header, const uint8_t **payload, int timeout_ms)
{
    static uint8_t held[1u << 20];
    pthread_mutex_lock(&mu);
    bool ok = have(H2_FRAME_HEADER_LEN, timeout_ms);
    if (ok) {
        h2_frame_header_read(server_plain.bytes + server_plain.at, header);
        ok = header->length <= sizeof(held) && have(H2_FRAME_HEADER_LEN + header->length, timeout_ms);
    }
    if (ok) {
        memcpy(held, server_plain.bytes + server_plain.at + H2_FRAME_HEADER_LEN, header->length);
        server_plain.at += H2_FRAME_HEADER_LEN + header->length;
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
    bool ok = h2_hpack_decode(&server_decoder, block, len, keep, NULL) == H2_HPACK_OK;
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
        record(head, REC_DATA, server_out_seq++, n);
        put(r, head, REC_HEAD);
        put(r, plain + at, n);
        at += n;
    }
}

static void say(const uint8_t *plain, size_t len)
{
    pthread_mutex_lock(&mu);
    wrap(&to_client, plain, len);
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
    h2_hpack_encode(&server_encoder, fields, count, block, bound, &len);
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
    wrap(&to_client, frame, H2_FRAME_HEADER_LEN + len);
    record(head, REC_CLOSE, server_out_seq++, 0);
    put(&to_client, head, REC_HEAD);
    pthread_cond_broadcast(&moved);
    pthread_mutex_unlock(&mu);
}

void peer_engine_holds_settings(const h2_setting_t *settings, uint32_t count)
{
    pthread_mutex_lock(&mu);
    engine.plain_len += settings_frame(settings, count, engine.plain + engine.plain_len,
                                       sizeof(engine.plain) - engine.plain_len);
    pthread_mutex_unlock(&mu);
}

void peer_send_engine_owes(void)
{
    uint8_t head[REC_HEAD];
    pthread_mutex_lock(&mu);
    record(head, REC_MAKE_OWE, server_out_seq++, 0);
    put(&to_client, head, REC_HEAD);
    pthread_cond_broadcast(&moved);
    pthread_mutex_unlock(&mu);
}
