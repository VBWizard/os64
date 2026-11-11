// test_h2_conn_host.c — one HTTP/2 connection (userland/libfetch/h2_conn.c)
// against a scripted server (tools/h2_peer.c), with real threads:
// tools/test_h2_conn_host.sh builds it under ASan and UBSan, then TSan.
//
// A case adopts a connection, plays the server's side a frame at a time, and
// checks both what the server saw and what a fetch's thread was told. Every
// case ends through `finish`, which checks the socket was closed once and by
// nobody but the reader, the engine was freed (so every hold was let go),
// and no record reached the wire out of order.
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "h2_conn.h"
#include "h2_peer.h"
#include "host_bell.h"

// A FAILING RUN IS A SHORT ONE: this test is run with rules broken on
// purpose, and a broken rule can fail a check on every pass of a loop.
#define FAILURES_MOST 50
#define SECONDS_MOST 300
static atomic_int checks, failures;
#define CHECK(x)                                                              \
    do {                                                                      \
        atomic_fetch_add(&checks, 1);                                         \
        if (!(x)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); \
            if (atomic_fetch_add(&failures, 1) + 1 >= FAILURES_MOST) {        \
                fprintf(stderr, "test_h2_conn_host: too many failed checks; stopping\n"); \
                exit(1);                                                      \
            }                                                                 \
        }                                                                     \
    } while (0)

static void nap_ms(long ms)
{
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

static h2_field_t F(const char *name, const char *value)
{
    return (h2_field_t){.name = (const uint8_t *)name, .value = (const uint8_t *)value,
                        .name_len = (uint32_t)strlen(name), .value_len = (uint32_t)strlen(value)};
}

// ── The pool's side, as far as a connection is concerned ────────────────

static atomic_int dead_calls, room_calls;
static atomic_bool idle_answer;
static void hook_dead(void *ctx, h2_conn_t *c) { (void)ctx; (void)c; atomic_fetch_add(&dead_calls, 1); }
static void hook_room(void *ctx, h2_conn_t *c) { (void)ctx; (void)c; atomic_fetch_add(&room_calls, 1); }
static bool hook_idle(void *ctx, h2_conn_t *c) { (void)ctx; (void)c; return atomic_load(&idle_answer); }
static const h2_conn_hooks_t kHooks = {.idle_out = hook_idle, .room = hook_room, .dead = hook_dead};

// A greeting with a stream window of 65,535 and a connection window of the
// same: small enough that a case can fill either.
static const h2_setting_t kSettings[] = {{H2_SETTING_HEADER_TABLE_SIZE, 4096}, {H2_SETTING_ENABLE_PUSH, 0}};
static const h2_greeting_t kGreeting = {.settings = kSettings, .count = 2, .window_increment = 0};

// ── A fetch, as far as a connection is concerned ────────────────────────

typedef struct {
    os64_bell_t bell;
    h2_wait_t wait;
    h2_stream_t *stream;
    atomic_bool cancelled;
    char head[2048];            // "name: value\n" for each field handed over
} fetch_t;

static bool fetch_cancelled(void *ctx) { return atomic_load(&((fetch_t *)ctx)->cancelled); }

static void fetch_init(fetch_t *f, uint32_t patience_ms)
{
    memset(f, 0, sizeof(*f));
    f->bell = (os64_bell_t)OS64_BELL_NONE;
    f->wait = (h2_wait_t){.bell = &f->bell, .patience_ms = patience_ms, .cancelled = fetch_cancelled, .cancel_ctx = f};
}

static void fetch_done(fetch_t *f)
{
    if (f->stream != NULL)
        h2_stream_close(f->stream);
    f->stream = NULL;
    os64_bell_close(&f->bell);
}

static bool note_field(void *ctx, const h2_field_t *field)
{
    fetch_t *f = ctx;
    size_t at = strlen(f->head);
    snprintf(f->head + at, sizeof(f->head) - at, "%.*s: %.*s\n", (int)field->name_len, (const char *)field->name,
             (int)field->value_len, (const char *)field->value);
    return true;
}

static h2_result_t fetch_get(fetch_t *f, h2_conn_t *c, const char *path)
{
    h2_field_t request[] = {F(":method", "GET"), F(":scheme", "https"), F(":path", path), F(":authority", "a.test")};
    return h2_stream_open(c, request, 4, true, false, &f->wait, &f->stream);
}

static h2_result_t fetch_head(fetch_t *f)
{
    f->head[0] = '\0';
    return h2_stream_head(f->stream, &f->wait, note_field, f);
}

// ── A connection and its server ─────────────────────────────────────────

static h2_conn_t *conn;

// The server's side of one request: the HEADERS the client sent, decoded.
static uint32_t server_request(const char *want_path)
{
    h2_frame_header_t h;
    const uint8_t *payload;
    const h2_field_t *fields;
    size_t count;
    CHECK(peer_frame_of(H2_FRAME_HEADERS, &h, &payload, 2000));
    CHECK((h.flags & H2_FLAG_END_HEADERS) != 0 && peer_decode(payload, h.length, &fields, &count) && count == 4);
    CHECK(count == 4 && fields[2].value_len == strlen(want_path) && memcmp(fields[2].value, want_path, strlen(want_path)) == 0);
    return h.stream;
}

static void server_reply(uint32_t stream, const char *status, const char *body, bool end)
{
    h2_field_t head[] = {F(":status", status), F("content-type", "text/plain")};
    peer_send_headers(stream, head, 2, body == NULL && end);
    if (body != NULL)
        peer_send_data(stream, body, strlen(body), end);
}

// Adopt a connection and trade settings: the server sees the preface, the
// greeting byte for byte, and an acknowledgement of its own SETTINGS.
static void begin_with(const h2_greeting_t *greeting, const h2_setting_t *servers, uint32_t count)
{
    os64_tls_client *engine;
    int32_t socket;
    h2_frame_header_t h;
    const uint8_t *payload;
    peer_begin(&engine, &socket);
    atomic_store(&idle_answer, false);
    conn = h2_conn_adopt(engine, socket, NULL, 0, greeting, &kHooks);
    CHECK(conn != NULL);
    CHECK(peer_preface(2000));
    CHECK(peer_frame(&h, &payload, 2000) && h.type == H2_FRAME_SETTINGS && h.flags == 0 && h.stream == 0 &&
          h.length == greeting->count * 6);
    for (uint32_t i = 0; i < greeting->count; i++) {
        const uint8_t *p = payload + i * 6;
        CHECK((uint32_t)(p[0] << 8 | p[1]) == greeting->settings[i].identifier);
        CHECK(((uint32_t)p[2] << 24 | p[3] << 16 | p[4] << 8 | p[5]) == greeting->settings[i].value);
    }
    if (greeting->window_increment != 0) {
        CHECK(peer_frame(&h, &payload, 2000) && h.type == H2_FRAME_WINDOW_UPDATE && h.stream == 0 && h.length == 4);
        CHECK(((uint32_t)payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3]) == greeting->window_increment);
    }
    peer_send_settings(servers, count);
    CHECK(peer_frame(&h, &payload, 2000) && h.type == H2_FRAME_SETTINGS && (h.flags & H2_FLAG_ACK) && h.length == 0);
}

static void begin(void) { begin_with(&kGreeting, NULL, 0); }

// Wait for the reader's last word, at most `ms`.
static bool died_within(int before, long ms)
{
    for (long waited = 0; waited < ms && atomic_load(&dead_calls) == before; waited += 5)
        nap_ms(5);
    return atomic_load(&dead_calls) == before + 1;
}

// The end of a case: condemn the connection if it lives, let the adopter's
// hold go, and see that the reader closed the socket once and everything
// was let go.
static void finish(int dead_before)
{
    h2_conn_condemn(conn);
    h2_conn_release(conn);
    conn = NULL;
    CHECK(died_within(dead_before, 3000));
    for (int i = 0; i < 400 && !peer_engine_freed(); i++)
        nap_ms(5);
    CHECK(peer_end());
    CHECK(host_bell_alive() == 0);
}

// ── Cases ───────────────────────────────────────────────────────────────

static void basic_cases(void)
{
    int dead = atomic_load(&dead_calls);
    fetch_t a, b;
    char body[64];
    begin();
    CHECK(h2_conn_room(conn) && h2_conn_streams(conn) == 0);

    // One request and its response, read to its end; no reset is owed.
    fetch_init(&a, 2000);
    CHECK(fetch_get(&a, conn, "/one") == H2_OK && h2_conn_streams(conn) == 1);
    uint32_t one = server_request("/one");
    CHECK(one == 1);
    server_reply(one, "200", "hello", true);
    CHECK(fetch_head(&a) == H2_OK && strcmp(a.head, ":status: 200\ncontent-type: text/plain\n") == 0);
    CHECK(h2_stream_read(a.stream, body, sizeof(body), &a.wait) == 5 && memcmp(body, "hello", 5) == 0);
    CHECK(h2_stream_read(a.stream, body, sizeof(body), &a.wait) == 0);
    h2_ending_t ending = h2_stream_ended(a.stream);
    CHECK(ending.how == H2_END_CLEAN && ending.head_seen && ending.any_seen && !ending.reset_after_end);
    int rooms = atomic_load(&room_calls);
    fetch_done(&a);
    CHECK(h2_conn_streams(conn) == 0 && atomic_load(&room_calls) == rooms + 1);

    // Two at once, their frames interleaved: ids rise by two, and each
    // stream gets its own bytes.
    fetch_init(&a, 2000);
    fetch_init(&b, 2000);
    CHECK(fetch_get(&a, conn, "/a") == H2_OK && fetch_get(&b, conn, "/b") == H2_OK);
    uint32_t ia = server_request("/a"), ib = server_request("/b");
    CHECK(ia == 3 && ib == 5);
    server_reply(ib, "200", "bbb", false);
    server_reply(ia, "404", "aa", false);
    peer_send_data(ib, "BBB", 3, true);
    peer_send_data(ia, "AA", 2, true);
    CHECK(fetch_head(&a) == H2_OK && strncmp(a.head, ":status: 404\n", 13) == 0);
    CHECK(fetch_head(&b) == H2_OK && strncmp(b.head, ":status: 200\n", 13) == 0);
    size_t got = 0;
    int64_t n;
    while ((n = h2_stream_read(b.stream, body + got, sizeof(body) - got, &b.wait)) > 0)
        got += (size_t)n;
    CHECK(n == 0 && got == 6 && memcmp(body, "bbbBBB", 6) == 0);
    got = 0;
    while ((n = h2_stream_read(a.stream, body + got, sizeof(body) - got, &a.wait)) > 0)
        got += (size_t)n;
    CHECK(n == 0 && got == 4 && memcmp(body, "aaAA", 4) == 0);
    fetch_done(&a);
    fetch_done(&b);

    // Left with its body unread: the server is told (CANCEL), and the
    // connection carries on.
    fetch_init(&a, 2000);
    CHECK(fetch_get(&a, conn, "/left") == H2_OK);
    uint32_t left = server_request("/left");
    server_reply(left, "200", "unread", false);
    CHECK(fetch_head(&a) == H2_OK);
    fetch_done(&a);
    h2_frame_header_t h;
    const uint8_t *payload;
    CHECK(peer_frame_of(H2_FRAME_RST_STREAM, &h, &payload, 2000) && h.stream == left && payload[3] == H2_ERR_CANCEL);
    CHECK(h2_conn_room(conn));
    finish(dead);
}

int main(void)
{
    alarm(SECONDS_MOST);
    basic_cases();
    if (atomic_load(&failures) != 0) {
        fprintf(stderr, "test_h2_conn_host: %d of %d checks failed\n", atomic_load(&failures), atomic_load(&checks));
        return 1;
    }
    printf("test_h2_conn_host: %d checks passed\n", atomic_load(&checks));
    return 0;
}
