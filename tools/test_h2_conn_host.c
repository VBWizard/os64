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
static atomic_int idle_calls;
static bool hook_idle(void *ctx, h2_conn_t *c)
{
    (void)ctx;
    (void)c;
    atomic_fetch_add(&idle_calls, 1);
    return atomic_load(&idle_answer);
}
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
    // A stream that was never opened has no head: the case's CHECK says
    // where, which a crash here would not.
    if (f->stream == NULL)
        return H2_FAILED;
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

// A wait that runs out on a connection that heard nothing marks it quiet
// (HTTP2.md § 2.5), which is its own case. Where a case waits out a patience
// for another reason, the server keeps the connection audible with pings.
static atomic_bool pinging;
static pthread_t pinger_thread;
static void *pinger(void *x)
{
    (void)x;
    while (atomic_load(&pinging)) {
        peer_send_ping();
        nap_ms(40);
    }
    return NULL;
}
static void pings(bool on)
{
    if (on) {
        atomic_store(&pinging, true);
        pthread_create(&pinger_thread, NULL, pinger, NULL);
    } else {
        atomic_store(&pinging, false);
        pthread_join(pinger_thread, NULL);
    }
}

// Read a stream to its end: the bytes into `out`, and how the last read ended.
static int64_t read_all(fetch_t *f, char *out, size_t room, size_t *got)
{
    int64_t n;
    *got = 0;
    while ((n = h2_stream_read(f->stream, out + *got, room - *got, &f->wait)) > 0)
        *got += (size_t)n;
    return n;
}

static bool server_sees_goaway(uint32_t code)
{
    h2_frame_header_t h;
    const uint8_t *payload;
    return peer_frame_of(H2_FRAME_GOAWAY, &h, &payload, 2000) && h.stream == 0 && h.length >= 8 &&
           (((uint32_t)payload[4] << 24 | payload[5] << 16 | payload[6] << 8 | payload[7]) == code);
}

static bool server_sees_rst(uint32_t stream, uint32_t code)
{
    h2_frame_header_t h;
    const uint8_t *payload;
    return peer_frame_of(H2_FRAME_RST_STREAM, &h, &payload, 2000) && h.stream == stream && h.length == 4 &&
           (((uint32_t)payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3]) == code);
}

static bool server_sees_window(uint32_t stream, uint32_t increment)
{
    h2_frame_header_t h;
    const uint8_t *payload;
    return peer_frame_of(H2_FRAME_WINDOW_UPDATE, &h, &payload, 2000) && h.stream == stream &&
           (((uint32_t)payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3]) == increment);
}

// Open three streams and have the server see them: ids 1, 3 and 5.
static void three(fetch_t f[3])
{
    static const char *const kPaths[3] = {"/x", "/y", "/z"};
    for (int i = 0; i < 3; i++) {
        fetch_init(&f[i], 3000);
        CHECK(fetch_get(&f[i], conn, kPaths[i]) == H2_OK);
        CHECK(server_request(kPaths[i]) == (uint32_t)(1 + 2 * i));
    }
}

static void ending_cases(void)
{
    int dead = atomic_load(&dead_calls);
    fetch_t f[3];
    char body[256];
    size_t got;
    h2_ending_t e;

    // THE SERVER RESETS A STREAM MID-BODY: what arrived is read, and then
    // the stream says how it ended. Its neighbours do not hear of it.
    begin();
    three(f);
    server_reply(1, "200", "part", false);
    server_reply(3, "200", "whole", true);
    peer_send_rst(1, H2_ERR_INTERNAL);
    CHECK(fetch_head(&f[0]) == H2_OK);
    CHECK(read_all(&f[0], body, sizeof(body), &got) == -(int64_t)H2_ENDED && got == 4);
    e = h2_stream_ended(f[0].stream);
    CHECK(e.how == H2_END_PEER_RESET && e.code == H2_ERR_INTERNAL && e.head_seen && e.any_seen);
    CHECK(fetch_head(&f[1]) == H2_OK && read_all(&f[1], body, sizeof(body), &got) == 0 && got == 5);
    // Reset before any byte of a response: the retry rule's question.
    peer_send_rst(5, H2_ERR_REFUSED_STREAM);
    CHECK(fetch_head(&f[2]) == H2_ENDED);
    e = h2_stream_ended(f[2].stream);
    CHECK(e.how == H2_END_PEER_RESET && e.code == H2_ERR_REFUSED_STREAM && !e.head_seen && !e.any_seen);
    for (int i = 0; i < 3; i++)
        fetch_done(&f[i]);
    finish(dead);

    // GOAWAY: the stream above its last id was never processed; the one at
    // it runs to its end; no new stream; and the connection ends when the
    // last has left, without being told to.
    dead = atomic_load(&dead_calls);
    begin();
    three(f);
    peer_send_goaway(3, H2_ERR_NO_ERROR);
    CHECK(fetch_head(&f[2]) == H2_ENDED);
    e = h2_stream_ended(f[2].stream);
    CHECK(e.how == H2_END_UNPROCESSED && !e.any_seen);
    CHECK(!h2_conn_room(conn));
    fetch_t late;
    fetch_init(&late, 500);
    CHECK(fetch_get(&late, conn, "/late") == H2_NO_ROOM && late.stream == NULL);
    fetch_done(&late);
    server_reply(1, "200", "one", true);
    server_reply(3, "200", "three", true);
    CHECK(fetch_head(&f[0]) == H2_OK && read_all(&f[0], body, sizeof(body), &got) == 0 && got == 3);
    CHECK(fetch_head(&f[1]) == H2_OK && read_all(&f[1], body, sizeof(body), &got) == 0 && got == 5);
    CHECK(atomic_load(&dead_calls) == dead);
    for (int i = 0; i < 3; i++)
        fetch_done(&f[i]);
    CHECK(died_within(dead, 2000));         // its last stream left
    h2_conn_release(conn);
    for (int i = 0; i < 400 && !peer_engine_freed(); i++)
        nap_ms(5);
    CHECK(peer_end() && host_bell_alive() == 0);

    // A GOAWAY with an error is the connection's death at once, for the
    // streams it had processed too.
    dead = atomic_load(&dead_calls);
    begin();
    three(f);
    server_reply(1, "200", "so far", false);
    peer_send_goaway(5, H2_ERR_ENHANCE_YOUR_CALM);
    CHECK(died_within(dead, 2000));
    CHECK(fetch_head(&f[0]) == H2_OK && read_all(&f[0], body, sizeof(body), &got) == -(int64_t)H2_ENDED && got == 6);
    e = h2_stream_ended(f[0].stream);
    CHECK(e.how == H2_END_CONN_DEAD && e.code == H2_ERR_ENHANCE_YOUR_CALM && e.head_seen);
    CHECK(fetch_head(&f[1]) == H2_ENDED && h2_stream_ended(f[1].stream).how == H2_END_CONN_DEAD);
    for (int i = 0; i < 3; i++)
        fetch_done(&f[i]);
    h2_conn_release(conn);
    for (int i = 0; i < 400 && !peer_engine_freed(); i++)
        nap_ms(5);
    CHECK(peer_end() && host_bell_alive() == 0);

    // THE SERVER HANGS UP under three streams: each is told, the socket is
    // closed once, and a stream may be closed after the connection is dead.
    dead = atomic_load(&dead_calls);
    begin();
    three(f);
    server_reply(3, "200", "half", false);
    nap_ms(50);
    peer_hang_up();
    CHECK(died_within(dead, 2000) && peer_closes() == 1);
    CHECK(fetch_head(&f[0]) == H2_ENDED && h2_stream_ended(f[0].stream).how == H2_END_CONN_DEAD);
    CHECK(fetch_head(&f[1]) == H2_OK && read_all(&f[1], body, sizeof(body), &got) == -(int64_t)H2_ENDED && got == 4);
    CHECK(!h2_conn_room(conn));
    for (int i = 0; i < 3; i++)
        fetch_done(&f[i]);
    h2_conn_release(conn);
    for (int i = 0; i < 400 && !peer_engine_freed(); i++)
        nap_ms(5);
    CHECK(peer_end() && host_bell_alive() == 0);

    // ONE OF THREE IS CANCELLED: the server is told, the others go on, and
    // what it goes on sending for the cancelled one is credited to the
    // connection, whose window would otherwise be spent for good.
    dead = atomic_load(&dead_calls);
    begin();
    three(f);
    fetch_done(&f[1]);
    CHECK(server_sees_rst(3, H2_ERR_CANCEL));
    static char flood[40000];
    memset(flood, 'f', sizeof(flood));
    for (size_t at = 0; at < sizeof(flood); at += 10000)
        peer_send_data(3, flood + at, 10000, false);
    CHECK(server_sees_window(0, 40000));    // past half of 65,535, said back
    server_reply(1, "200", "still here", true);
    CHECK(fetch_head(&f[0]) == H2_OK && read_all(&f[0], body, sizeof(body), &got) == 0 && got == 10);
    // HEADERS for the cancelled stream are decoded all the same: the next
    // head, which leans on the table they changed, still reads right.
    h2_field_t late_head[] = {F(":status", "200"), F("x-late", "for a stream that left")};
    peer_send_headers(3, late_head, 2, true);
    peer_send_headers(5, late_head, 2, true);
    CHECK(fetch_head(&f[2]) == H2_OK && strcmp(f[2].head, ":status: 200\nx-late: for a stream that left\n") == 0);
    fetch_done(&f[0]);
    fetch_done(&f[2]);
    finish(dead);

    // CONDEMNED FROM OUTSIDE: it ends within a slice, its socket closed once
    // and by the reader, its stream told.
    dead = atomic_load(&dead_calls);
    begin();
    fetch_init(&f[0], 3000);
    CHECK(fetch_get(&f[0], conn, "/x") == H2_OK);
    h2_conn_condemn(conn);
    CHECK(!h2_conn_room(conn));
    CHECK(died_within(dead, H2_READER_SLICE_MS + 300) && peer_closes() == 1);
    CHECK(fetch_head(&f[0]) == H2_ENDED && h2_stream_ended(f[0].stream).how == H2_END_CONN_DEAD);
    fetch_done(&f[0]);
    h2_conn_release(conn);
    for (int i = 0; i < 400 && !peer_engine_freed(); i++)
        nap_ms(5);
    CHECK(peer_end() && host_bell_alive() == 0);

    // IDLE, and the pool is done keeping it: closed within a slice of the
    // answer. With a stream it is not idle, whatever the pool would say.
    dead = atomic_load(&dead_calls);
    begin();
    fetch_init(&f[0], 3000);
    CHECK(fetch_get(&f[0], conn, "/x") == H2_OK);
    atomic_store(&idle_answer, true);
    nap_ms(H2_READER_SLICE_MS + 200);
    CHECK(atomic_load(&dead_calls) == dead && h2_conn_room(conn));
    fetch_done(&f[0]);
    CHECK(died_within(dead, H2_READER_SLICE_MS + 300));
    h2_conn_release(conn);
    for (int i = 0; i < 400 && !peer_engine_freed(); i++)
        nap_ms(5);
    CHECK(peer_end() && host_bell_alive() == 0);
}

static void window_cases(void)
{
    int dead = atomic_load(&dead_calls);
    fetch_t a;
    static char big[70000], out[70000];
    size_t got;
    memset(big, 'w', sizeof(big));

    // RECEIVING. A stream window of 1,000: half of it read is said back for
    // the stream; the connection's is said back at half of its 65,535.
    static const h2_setting_t kSmall[] = {{H2_SETTING_INITIAL_WINDOW_SIZE, 1000}};
    static const h2_greeting_t kSmallGreeting = {.settings = kSmall, .count = 1, .window_increment = 0};
    begin_with(&kSmallGreeting, NULL, 0);
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/w") == H2_OK && server_request("/w") == 1);
    server_reply(1, "200", NULL, false);
    peer_send_data(1, big, 1000, false);
    CHECK(fetch_head(&a) == H2_OK);
    CHECK(h2_stream_read(a.stream, out, 400, &a.wait) == 400);      // under half: nothing said
    CHECK(h2_stream_read(a.stream, out, 200, &a.wait) == 200);      // 600 of 1,000: said
    CHECK(server_sees_window(1, 600));
    // What was said back is window again: 600 more arrive and are read, and
    // with the 400 that were waiting they are said back in their turn.
    peer_send_data(1, big, 600, false);
    CHECK(h2_stream_read(a.stream, out, 400, &a.wait) == 400);
    CHECK(h2_stream_read(a.stream, out, 600, &a.wait) == 600);
    CHECK(server_sees_window(1, 1000));
    // One byte past the stream's window is the stream's error, and the
    // connection goes on.
    peer_send_data(1, big, 1001, false);
    CHECK(server_sees_rst(1, H2_ERR_FLOW_CONTROL));
    CHECK(read_all(&a, out, sizeof(out), &got) == -(int64_t)H2_ENDED && got == 0);
    CHECK(h2_stream_ended(a.stream).how == H2_END_FLOW && h2_conn_room(conn));
    fetch_done(&a);
    finish(dead);

    // A body longer than both windows arrives whole, since each is given
    // back as it is read: 70,000 bytes through 65,535.
    dead = atomic_load(&dead_calls);
    begin();
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/long") == H2_OK && server_request("/long") == 1);
    server_reply(1, "200", NULL, false);
    for (size_t at = 0; at < 40000; at += 10000)
        peer_send_data(1, big, 10000, false);
    CHECK(fetch_head(&a) == H2_OK);
    for (got = 0; got < 40000;) {
        int64_t n = h2_stream_read(a.stream, out, 40000 - got, &a.wait);
        CHECK(n > 0);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    for (size_t at = 40000; at < 70000; at += 10000)
        peer_send_data(1, big, 10000, at == 60000);
    CHECK(read_all(&a, out, sizeof(out), &got) == 0 && got == 30000);
    CHECK(h2_stream_ended(a.stream).how == H2_END_CLEAN);
    fetch_done(&a);
    finish(dead);

    // Past the CONNECTION's window is the connection's error.
    dead = atomic_load(&dead_calls);
    begin();
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/w") == H2_OK && server_request("/w") == 1);
    server_reply(1, "200", NULL, false);
    for (size_t at = 0; at < 60000; at += 15000)
        peer_send_data(1, big, 15000, false);
    peer_send_data(1, big, 5536, false);
    CHECK(server_sees_goaway(H2_ERR_FLOW_CONTROL));
    CHECK(died_within(dead, 2000));
    fetch_done(&a);
    h2_conn_release(conn);
    for (int i = 0; i < 400 && !peer_engine_freed(); i++)
        nap_ms(5);
    CHECK(peer_end() && host_bell_alive() == 0);

    // Padding is charged to both windows and has no reader, so it is
    // credited at once: 700 bytes of a frame that carries one.
    dead = atomic_load(&dead_calls);
    begin_with(&kSmallGreeting, NULL, 0);
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/w") == H2_OK && server_request("/w") == 1);
    server_reply(1, "200", NULL, false);
    static uint8_t padded[701];
    padded[0] = 199;                        // the pad length; then one byte of body and 199 of padding
    padded[1] = 'P';
    peer_send_frame(H2_FRAME_DATA, H2_FLAG_PADDED, 1, padded, 201);
    padded[0] = 255;
    peer_send_frame(H2_FRAME_DATA, H2_FLAG_PADDED, 1, padded, 256);  // no body: a length byte and 255 of padding
    peer_send_frame(H2_FRAME_DATA, H2_FLAG_PADDED, 1, padded, 256);
    CHECK(server_sees_window(1, 712));      // 200, 256 and 256: past half of 1,000 with no byte read
    CHECK(fetch_head(&a) == H2_OK && h2_stream_read(a.stream, out, sizeof(out), &a.wait) == 1 && out[0] == 'P');
    fetch_done(&a);
    finish(dead);

    // SENDING. The server gives streams a window of 10: a body of 25 goes
    // ten bytes, and the rest as the window is given.
    dead = atomic_load(&dead_calls);
    static const h2_setting_t kTen[] = {{H2_SETTING_INITIAL_WINDOW_SIZE, 10}};
    begin_with(&kGreeting, kTen, 1);
    pings(true);
    fetch_init(&a, 400);
    h2_field_t post[] = {F(":method", "POST"), F(":scheme", "https"), F(":path", "/up"), F(":authority", "a.test")};
    CHECK(h2_stream_open(conn, post, 4, false, false, &a.wait, &a.stream) == H2_OK && server_request("/up") == 1);
    size_t sent;
    CHECK(h2_stream_send(a.stream, "0123456789abcdefghijklmno", 25, true, &a.wait, &sent) == H2_SILENT && sent == 10);
    h2_frame_header_t h;
    const uint8_t *payload;
    CHECK(peer_frame_of(H2_FRAME_DATA, &h, &payload, 2000) && h.length == 10 && !(h.flags & H2_FLAG_END_STREAM) &&
          memcmp(payload, "0123456789", 10) == 0);
    // The server lowers the initial window to 4: this stream's is 4 - 10
    // below nothing, and six bytes of credit only bring it back to nothing.
    static const h2_setting_t kFour[] = {{H2_SETTING_INITIAL_WINDOW_SIZE, 4}};
    peer_send_settings(kFour, 1);
    CHECK(peer_frame_of(H2_FRAME_SETTINGS, &h, &payload, 2000) && (h.flags & H2_FLAG_ACK));
    peer_send_window(1, 6);
    CHECK(h2_stream_send(a.stream, "abcdefghijklmno", 15, true, &a.wait, &sent) == H2_SILENT && sent == 0);
    a.wait.patience_ms = 3000;
    peer_send_window(1, 100);
    CHECK(h2_stream_send(a.stream, "abcdefghijklmno", 15, true, &a.wait, &sent) == H2_OK && sent == 15);
    size_t seen = 0;
    bool ended = false;
    while (seen < 15 && peer_frame_of(H2_FRAME_DATA, &h, &payload, 2000)) {
        CHECK(memcmp(payload, "abcdefghijklmno" + seen, h.length) == 0);
        seen += h.length;
        ended = (h.flags & H2_FLAG_END_STREAM) != 0;
    }
    CHECK(seen == 15 && ended);
    // The connection's send window is its own: 65,535 less the 25 sent. A
    // second stream with all the stream window it wants stops there.
    fetch_t b;
    fetch_init(&b, 400);
    CHECK(h2_stream_open(conn, post, 4, false, false, &b.wait, &b.stream) == H2_OK && server_request("/up") == 3);
    peer_send_window(3, 1000000);
    CHECK(h2_stream_send(b.stream, big, sizeof(big), true, &b.wait, &sent) == H2_SILENT && sent == 65535 - 25);
    b.wait.patience_ms = 3000;
    peer_send_window(0, 100000);
    size_t more;
    CHECK(h2_stream_send(b.stream, big + sent, sizeof(big) - sent, true, &b.wait, &more) == H2_OK &&
          sent + more == sizeof(big));
    // A window grown past 2^31 - 1 is an error: the stream's for a stream.
    peer_send_window(3, 0x7fffffff);
    CHECK(server_sees_rst(3, H2_ERR_FLOW_CONTROL));
    fetch_done(&a);
    fetch_done(&b);
    pings(false);
    peer_send_window(0, 0x7fffffff);
    CHECK(server_sees_goaway(H2_ERR_FLOW_CONTROL));
    CHECK(died_within(dead, 2000));
    h2_conn_release(conn);
    for (int i = 0; i < 400 && !peer_engine_freed(); i++)
        nap_ms(5);
    CHECK(peer_end() && host_bell_alive() == 0);
}

// The end of a case whose connection has died of its own accord.
static void finish_dead(int dead_before)
{
    CHECK(died_within(dead_before, 3000));
    h2_conn_release(conn);
    conn = NULL;
    for (int i = 0; i < 400 && !peer_engine_freed(); i++)
        nap_ms(5);
    CHECK(peer_end() && host_bell_alive() == 0);
}

static void head_cases(void)
{
    int dead = atomic_load(&dead_calls);
    fetch_t a, b;
    char body[256];
    size_t got;
    static char huge[70000];
    memset(huge, 'h', sizeof(huge) - 1);

    begin();
    // Interim heads are dropped and the wait goes on; trailers end the body
    // and are not handed over.
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/i") == H2_OK && server_request("/i") == 1);
    h2_field_t early[] = {F(":status", "103"), F("link", "</s.css>; rel=preload")};
    peer_send_headers(1, early, 2, false);
    h2_field_t cont[] = {F(":status", "100")};
    peer_send_headers(1, cont, 1, false);
    server_reply(1, "200", "body", false);
    h2_field_t trailers[] = {F("x-checksum", "abc")};
    peer_send_headers(1, trailers, 1, true);
    CHECK(fetch_head(&a) == H2_OK && strncmp(a.head, ":status: 200\n", 13) == 0 && strstr(a.head, "link") == NULL);
    CHECK(read_all(&a, body, sizeof(body), &got) == 0 && got == 4 && h2_stream_ended(a.stream).how == H2_END_CLEAN);
    fetch_done(&a);

    // A head that breaks a rule of RFC 9113 § 8.2 is that stream's end and
    // nobody else's: an uppercase name; an interim head that ends the
    // stream; trailers that do not; trailers with a pseudo-header.
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/bad") == H2_OK && server_request("/bad") == 3);
    h2_field_t shouting[] = {F(":status", "200"), F("Content-Type", "x")};
    peer_send_headers(3, shouting, 2, false);
    CHECK(server_sees_rst(3, H2_ERR_PROTOCOL) && fetch_head(&a) == H2_ENDED);
    CHECK(h2_stream_ended(a.stream).how == H2_END_BAD_HEAD);
    fetch_done(&a);
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/bad") == H2_OK && server_request("/bad") == 5);
    peer_send_headers(5, cont, 1, true);
    CHECK(server_sees_rst(5, H2_ERR_PROTOCOL) && fetch_head(&a) == H2_ENDED);
    fetch_done(&a);
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/bad") == H2_OK && server_request("/bad") == 7);
    server_reply(7, "200", "x", false);
    peer_send_headers(7, trailers, 1, false);
    CHECK(server_sees_rst(7, H2_ERR_PROTOCOL));
    CHECK(fetch_head(&a) == H2_OK && read_all(&a, body, sizeof(body), &got) == -(int64_t)H2_ENDED && got == 1);
    fetch_done(&a);
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/bad") == H2_OK && server_request("/bad") == 9);
    server_reply(9, "200", "x", false);
    h2_field_t status_again[] = {F(":status", "200")};
    peer_send_headers(9, status_again, 1, true);
    CHECK(server_sees_rst(9, H2_ERR_PROTOCOL));
    fetch_done(&a);
    // DATA before any head is the same.
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/bad") == H2_OK && server_request("/bad") == 11);
    peer_send_data(11, "early", 5, false);
    CHECK(server_sees_rst(11, H2_ERR_PROTOCOL) && fetch_head(&a) == H2_ENDED);
    fetch_done(&a);

    // A HEAD OVER THE LIMIT is decoded all the same, the stream is reset
    // (ENHANCE_YOUR_CALM), and the next head, which leans on the table the
    // big one changed, reads right.
    fetch_init(&a, 3000);
    fetch_init(&b, 3000);
    CHECK(fetch_get(&a, conn, "/big") == H2_OK && server_request("/big") == 13);
    CHECK(fetch_get(&b, conn, "/after") == H2_OK && server_request("/after") == 15);
    h2_field_t big_head[] = {F(":status", "200"), F("x-kept", "in the table"), F("x-huge", huge)};
    peer_send_headers(13, big_head, 3, false);
    CHECK(server_sees_rst(13, H2_ERR_ENHANCE_YOUR_CALM) && fetch_head(&a) == H2_ENDED);
    CHECK(h2_stream_ended(a.stream).how == H2_END_HEAD_TOO_LARGE);
    h2_field_t after[] = {F(":status", "200"), F("x-kept", "in the table")};
    peer_send_headers(15, after, 2, true);
    CHECK(fetch_head(&b) == H2_OK && strcmp(b.head, ":status: 200\nx-kept: in the table\n") == 0);
    fetch_done(&a);
    fetch_done(&b);

    // CONTENT-LENGTH AGAINST DATA (RFC 9113 § 8.1.1): short, long, and a
    // head that ends a stream it promised ten bytes on. Each is a reset
    // and none a clean end. What arrived first is still read.
    h2_field_t ten[] = {F(":status", "200"), F("content-length", "10")};
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/len") == H2_OK && server_request("/len") == 17);
    peer_send_headers(17, ten, 2, false);
    peer_send_data(17, "abc", 3, true);
    CHECK(server_sees_rst(17, H2_ERR_PROTOCOL));
    CHECK(fetch_head(&a) == H2_OK && read_all(&a, body, sizeof(body), &got) == -(int64_t)H2_ENDED);
    CHECK(h2_stream_ended(a.stream).how == H2_END_BAD_LENGTH);
    fetch_done(&a);
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/len") == H2_OK && server_request("/len") == 19);
    peer_send_headers(19, ten, 2, false);
    peer_send_data(19, "0123456789A", 11, false);
    CHECK(server_sees_rst(19, H2_ERR_PROTOCOL));
    CHECK(fetch_head(&a) == H2_OK && read_all(&a, body, sizeof(body), &got) == -(int64_t)H2_ENDED && got == 0);
    fetch_done(&a);
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/len") == H2_OK && server_request("/len") == 21);
    peer_send_headers(21, ten, 2, true);
    CHECK(server_sees_rst(21, H2_ERR_PROTOCOL));
    // The head did arrive; it is the body that is not what it promised.
    CHECK(fetch_head(&a) == H2_OK && read_all(&a, body, sizeof(body), &got) == -(int64_t)H2_ENDED && got == 0);
    CHECK(h2_stream_ended(a.stream).how == H2_END_BAD_LENGTH);
    fetch_done(&a);
    // Exactly its length, in two frames, is a clean end.
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/len") == H2_OK && server_request("/len") == 23);
    peer_send_headers(23, ten, 2, false);
    peer_send_data(23, "01234", 5, false);
    peer_send_data(23, "56789", 5, true);
    CHECK(fetch_head(&a) == H2_OK && read_all(&a, body, sizeof(body), &got) == 0 && got == 10);
    fetch_done(&a);
    // A HEAD's reply, and a 304, carry a content-length for content they
    // do not have: left alone.
    fetch_init(&a, 3000);
    h2_field_t head_request[] = {F(":method", "HEAD"), F(":scheme", "https"), F(":path", "/h"), F(":authority", "a.test")};
    CHECK(h2_stream_open(conn, head_request, 4, true, true, &a.wait, &a.stream) == H2_OK && server_request("/h") == 25);
    peer_send_headers(25, ten, 2, true);
    CHECK(fetch_head(&a) == H2_OK && read_all(&a, body, sizeof(body), &got) == 0 && got == 0);
    CHECK(h2_stream_ended(a.stream).how == H2_END_CLEAN);
    fetch_done(&a);
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/n") == H2_OK && server_request("/n") == 27);
    h2_field_t not_modified[] = {F(":status", "304"), F("content-length", "10")};
    peer_send_headers(27, not_modified, 2, true);
    CHECK(fetch_head(&a) == H2_OK && read_all(&a, body, sizeof(body), &got) == 0);
    fetch_done(&a);

    // A WHOLE RESPONSE, THEN A RESET, WHILE THE REQUEST IS STILL GOING (RFC
    // 9113 § 8.1). The response stands and is read to its end; the upload
    // is told to stop; and no reset of ours follows. Both frames arrive
    // before the fetch reads either.
    static char upload[100000];
    fetch_init(&a, 3000);
    h2_field_t post[] = {F(":method", "POST"), F(":scheme", "https"), F(":path", "/up"), F(":authority", "a.test")};
    CHECK(h2_stream_open(conn, post, 4, false, false, &a.wait, &a.stream) == H2_OK && server_request("/up") == 29);
    server_reply(29, "200", "no need", true);
    peer_send_rst(29, H2_ERR_NO_ERROR);
    nap_ms(100);
    size_t sent;
    CHECK(h2_stream_send(a.stream, upload, sizeof(upload), true, &a.wait, &sent) == H2_STOP && sent == 0);
    CHECK(fetch_head(&a) == H2_OK && read_all(&a, body, sizeof(body), &got) == 0 && got == 7);
    h2_ending_t e = h2_stream_ended(a.stream);
    CHECK(e.how == H2_END_CLEAN && e.reset_after_end && e.reset_code == H2_ERR_NO_ERROR);
    fetch_done(&a);
    // The same with the upload already under way, parked for window.
    pings(true);
    fetch_init(&a, 3000);
    CHECK(h2_stream_open(conn, post, 4, false, false, &a.wait, &a.stream) == H2_OK && server_request("/up") == 31);
    CHECK(h2_stream_send(a.stream, upload, 65535, false, &a.wait, &sent) == H2_OK && sent == 65535);
    server_reply(31, "200", "enough", true);
    peer_send_rst(31, H2_ERR_CANCEL);       // whatever the code, the response was whole before it
    CHECK(h2_stream_send(a.stream, upload, sizeof(upload), true, &a.wait, &sent) == H2_STOP && sent == 0);
    CHECK(fetch_head(&a) == H2_OK && read_all(&a, body, sizeof(body), &got) == 0 && got == 6);
    fetch_done(&a);
    pings(false);
    // No reset of ours went out for any of those: the next thing the server
    // hears on a stream is the next request.
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/quiet") == H2_OK);
    h2_frame_header_t h;
    const uint8_t *payload;
    bool reset_seen = false;
    while (peer_frame(&h, &payload, 2000) && h.type != H2_FRAME_HEADERS)
        reset_seen = reset_seen || (h.type == H2_FRAME_RST_STREAM && (h.stream == 29 || h.stream == 31));
    CHECK(!reset_seen && h.type == H2_FRAME_HEADERS && h.stream == 33);
    fetch_done(&a);
    finish(dead);

    // A BLOCK PAST THE HARD CAP, across CONTINUATION frames, ends the
    // connection: the bytes that would keep the table in step are gone.
    dead = atomic_load(&dead_calls);
    begin();
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/x") == H2_OK && server_request("/x") == 1);
    static char filler[16000];
    memset(filler, 0x80 | 2, sizeof(filler));       // ":method: GET", indexed, sixteen thousand times
    peer_send_frame(H2_FRAME_HEADERS, 0, 1, filler, sizeof(filler));
    for (int i = 0; i < 17; i++)
        peer_send_frame(H2_FRAME_CONTINUATION, 0, 1, filler, sizeof(filler));
    CHECK(server_sees_goaway(H2_ERR_ENHANCE_YOUR_CALM));
    fetch_done(&a);
    finish_dead(dead);
}

// What the connection refuses outright, each on a connection of its own.
static void error_cases(void)
{
    static const struct {
        const char *what;
        uint8_t type, flags;
        uint32_t stream;
        uint32_t len;
        uint32_t code;
    } kBad[] = {
        {"a PUSH_PROMISE", H2_FRAME_PUSH_PROMISE, H2_FLAG_END_HEADERS, 1, 5, H2_ERR_PROTOCOL},
        {"a frame past the size we set", H2_FRAME_DATA, 0, 1, 16385, H2_ERR_FRAME_SIZE},
        {"DATA on stream 0", H2_FRAME_DATA, 0, 0, 4, H2_ERR_PROTOCOL},
        {"DATA on a stream never opened", H2_FRAME_DATA, 0, 9, 4, H2_ERR_PROTOCOL},
        {"DATA on a stream of the server's", H2_FRAME_DATA, 0, 2, 4, H2_ERR_PROTOCOL},
        {"a PING of seven bytes", H2_FRAME_PING, 0, 0, 7, H2_ERR_FRAME_SIZE},
        {"a CONTINUATION of nothing", H2_FRAME_CONTINUATION, H2_FLAG_END_HEADERS, 1, 1, H2_ERR_PROTOCOL},
        {"a reset of a stream never opened", H2_FRAME_RST_STREAM, 0, 9, 4, H2_ERR_PROTOCOL},
        {"a window for a stream never opened", H2_FRAME_WINDOW_UPDATE, 0, 9, 4, H2_ERR_PROTOCOL},
        {"a window of nothing for the connection", H2_FRAME_WINDOW_UPDATE, 0, 0, 4, H2_ERR_PROTOCOL},
        {"a block that does not decode", H2_FRAME_HEADERS, H2_FLAG_END_HEADERS, 1, 1, H2_ERR_COMPRESSION},
        {"a head for a stream never opened", H2_FRAME_HEADERS, H2_FLAG_END_HEADERS, 9, 1, H2_ERR_PROTOCOL},
    };
    static uint8_t payload[16385];
    for (size_t i = 0; i < sizeof(kBad) / sizeof(kBad[0]); i++) {
        int dead = atomic_load(&dead_calls);
        fetch_t a;
        begin();
        fetch_init(&a, 3000);
        CHECK(fetch_get(&a, conn, "/x") == H2_OK && server_request("/x") == 1);
        memset(payload, 0, sizeof(payload));
        if (kBad[i].type == H2_FRAME_WINDOW_UPDATE && kBad[i].stream != 0)
            payload[3] = 1;
        if (kBad[i].code == H2_ERR_COMPRESSION)
            payload[0] = 0x80;              // index 0
        else if (kBad[i].type == H2_FRAME_HEADERS)
            payload[0] = 0x88;              // ":status: 200", a block that decodes
        peer_send_frame(kBad[i].type, kBad[i].flags, kBad[i].stream, payload, kBad[i].len);
        if (!server_sees_goaway(kBad[i].code)) {
            fprintf(stderr, "no GOAWAY %u for %s\n", (unsigned)kBad[i].code, kBad[i].what);
            CHECK(false);
        }
        CHECK(fetch_head(&a) == H2_ENDED);
        h2_ending_t e = h2_stream_ended(a.stream);
        CHECK(e.how == H2_END_CONN_DEAD && e.code == kBad[i].code);
        fetch_done(&a);
        finish_dead(dead);
    }

    // A window of nothing for a STREAM is that stream's error alone. A
    // PRIORITY, and a type nobody knows, are dropped.
    int dead = atomic_load(&dead_calls);
    fetch_t a, b;
    char body[16];
    size_t got;
    begin();
    fetch_init(&a, 3000);
    fetch_init(&b, 3000);
    CHECK(fetch_get(&a, conn, "/x") == H2_OK && server_request("/x") == 1);
    CHECK(fetch_get(&b, conn, "/y") == H2_OK && server_request("/y") == 3);
    memset(payload, 0, 8);
    peer_send_frame(H2_FRAME_WINDOW_UPDATE, 0, 1, payload, 4);
    CHECK(server_sees_rst(1, H2_ERR_PROTOCOL) && fetch_head(&a) == H2_ENDED);
    CHECK(h2_stream_ended(a.stream).how == H2_END_BAD_FRAME && h2_stream_ended(a.stream).code == H2_ERR_PROTOCOL);
    peer_send_frame(0x42, 0xff, 3, payload, 8);
    peer_send_frame(H2_FRAME_PRIORITY, 0, 3, payload, 5);
    server_reply(3, "200", "fine", true);
    CHECK(fetch_head(&b) == H2_OK && read_all(&b, body, sizeof(body), &got) == 0 && got == 4);
    fetch_done(&a);
    fetch_done(&b);
    finish(dead);

    // The server's first frame must be SETTINGS.
    dead = atomic_load(&dead_calls);
    os64_tls_client *engine;
    int32_t socket;
    peer_begin(&engine, &socket);
    conn = h2_conn_adopt(engine, socket, NULL, 0, &kGreeting, &kHooks);
    CHECK(conn != NULL && peer_preface(2000));
    peer_send_ping();
    CHECK(server_sees_goaway(H2_ERR_PROTOCOL));
    finish_dead(dead);
    // And a setting with a value it may not have is refused by its code.
    dead = atomic_load(&dead_calls);
    peer_begin(&engine, &socket);
    conn = h2_conn_adopt(engine, socket, NULL, 0, &kGreeting, &kHooks);
    CHECK(conn != NULL && peer_preface(2000));
    static const h2_setting_t kWide[] = {{H2_SETTING_INITIAL_WINDOW_SIZE, 0x80000000u}};
    peer_send_settings(kWide, 1);
    CHECK(server_sees_goaway(H2_ERR_FLOW_CONTROL));
    finish_dead(dead);

    // A greeting this connection cannot keep is not adopted, and the engine
    // and the socket are still the caller's.
    static const h2_setting_t kGreedy[] = {{H2_SETTING_MAX_FRAME_SIZE, 16777215}};
    static const h2_greeting_t kGreedyGreeting = {.settings = kGreedy, .count = 1};
    peer_begin(&engine, &socket);
    CHECK(h2_conn_adopt(engine, socket, NULL, 0, &kGreedyGreeting, &kHooks) == NULL);
    CHECK(peer_closes() == 0 && !peer_engine_freed());
}

// ── The out queue (HTTP2.md § 2.3) ──────────────────────────────────────

typedef struct {
    fetch_t f;
    const char *path;
    h2_result_t result;
    atomic_bool returned;
} opener_t;

static void *opener(void *x)
{
    opener_t *o = x;
    o->result = fetch_get(&o->f, conn, o->path);
    atomic_store(&o->returned, true);
    return NULL;
}

typedef struct {
    atomic_bool stop;
    atomic_int opened, parked;
    atomic_size_t most_queued;
} churn_t;

// Open a stream and leave it, over and over, as fast as the connection lets.
static void *churner(void *x)
{
    churn_t *ch = x;
    while (!atomic_load(&ch->stop)) {
        fetch_t f;
        fetch_init(&f, 30);
        h2_result_t r = fetch_get(&f, conn, "/churn");
        if (r == H2_OK)
            atomic_fetch_add(&ch->opened, 1);
        else
            atomic_fetch_add(&ch->parked, 1);
        fetch_done(&f);
        size_t queued = h2_conn_queued(conn);
        if (queued > atomic_load(&ch->most_queued))
            atomic_store(&ch->most_queued, queued);
    }
    return NULL;
}

static void queue_cases(void)
{
    int dead = atomic_load(&dead_calls);
    fetch_t a, b;
    char body[64];
    size_t got;
    h2_frame_header_t h;
    const uint8_t *payload;

    // A SOCKET THAT TAKES A FEW BYTES AT A TIME: every record still reaches
    // the server whole and in order.
    begin();
    peer_write_cap(7);
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/slow") == H2_OK && server_request("/slow") == 1);
    server_reply(1, "200", "ok", true);
    CHECK(fetch_head(&a) == H2_OK && read_all(&a, body, sizeof(body), &got) == 0 && got == 2);
    fetch_done(&a);
    peer_write_cap(0);
    CHECK(peer_records_sound());

    // WHAT THE ENGINE OWES AFTER A FEED (a KeyUpdate's reply) goes out with
    // nobody sending anything.
    unsigned owed = peer_owed_records();
    peer_send_engine_owes();
    for (int i = 0; i < 400 && peer_owed_records() == owed; i++)
        nap_ms(5);
    CHECK(peer_owed_records() == owed + 1);

    // A SOCKET THAT STOPS TAKING BYTES. A request's HEADERS wait in the
    // queue; the reader goes on delivering, so a response already on its
    // way is read; a fetch that leaves, leaves at once, its reset queued
    // behind. When the socket takes bytes again the reader sends them all,
    // within a slice, with no sender to do it.
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/first") == H2_OK && server_request("/first") == 3);
    peer_stall(true);
    opener_t second = {.path = "/second"};
    fetch_init(&second.f, 3000);
    pthread_t t;
    pthread_create(&t, NULL, opener, &second);      // becomes the drainer, and waits on the socket
    nap_ms(100);
    CHECK(!atomic_load(&second.returned) && h2_conn_queued(conn) != 0);
    server_reply(3, "200", "while stalled", true);
    CHECK(fetch_head(&a) == H2_OK && read_all(&a, body, sizeof(body), &got) == 0 && got == 13);
    fetch_init(&b, 3000);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(fetch_get(&b, conn, "/third") == H2_OK);  // a drainer is at work: its bytes are left to it
    fetch_done(&b);                                 // and it leaves without waiting for anybody
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CHECK((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 < 500);
    peer_stall(false);
    pthread_join(t, NULL);
    CHECK(second.result == H2_OK);
    CHECK(server_request("/second") == 5 && server_request("/third") == 7 && server_sees_rst(7, H2_ERR_CANCEL));
    fetch_done(&a);
    fetch_done(&second.f);
    CHECK(peer_records_sound());

    // A PING is answered with its own eight bytes. This one's answer is
    // refused by a stalled socket when the reader offers it, and goes when
    // the stall ends.
    peer_stall(true);
    peer_send_ping();
    nap_ms(200);
    peer_stall(false);
    CHECK(peer_frame_of(H2_FRAME_PING, &h, &payload, 2000) && (h.flags & H2_FLAG_ACK) && h.length == 8 &&
          memcmp(payload, "pingping", 8) == 0);

    // A stall that ended is forgotten: thirty seconds later the connection
    // is not blamed for it.
    int alive = atomic_load(&dead_calls);
    peer_clock_skip(H2_WRITE_PATIENCE_MS + 1);
    nap_ms(H2_READER_SLICE_MS + 100);
    CHECK(atomic_load(&dead_calls) == alive && h2_conn_room(conn));

    // What a fetch leaves UNREAD in its buffer is credited to the
    // connection when it goes: 40,000 bytes nobody will read.
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/unread") == H2_OK);
    uint32_t id = server_request("/unread");
    server_reply(id, "200", NULL, false);
    static char lot[40000];
    for (size_t at = 0; at < sizeof(lot); at += 10000)
        peer_send_data(id, lot + at, 10000, false);
    CHECK(fetch_head(&a) == H2_OK);
    nap_ms(100);
    fetch_done(&a);
    // (With the few bytes the cases above read on this connection.)
    CHECK(peer_frame_of(H2_FRAME_WINDOW_UPDATE, &h, &payload, 2000) && h.stream == 0);
    uint32_t credited = (uint32_t)payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];
    CHECK(credited >= 40000 && credited < 40100);

    // DATA waits at the queue's mark too: an upload into a stalled socket
    // queues the mark and a frame, and no more, however much it has to send.
    static char upload[600000];
    pings(true);
    fetch_init(&a, 3000);
    h2_field_t post[] = {F(":method", "POST"), F(":scheme", "https"), F(":path", "/up"), F(":authority", "a.test")};
    CHECK(h2_stream_open(conn, post, 4, false, false, &a.wait, &a.stream) == H2_OK);
    id = server_request("/up");
    peer_send_window(id, 1u << 20);
    peer_send_window(0, 1u << 20);
    nap_ms(100);
    peer_stall(true);
    opener_t holder = {.path = "/holder"};
    fetch_init(&holder.f, 3000);
    pthread_create(&t, NULL, opener, &holder);      // the drainer, stuck on the socket
    nap_ms(100);
    size_t sent;
    a.wait.patience_ms = 400;
    CHECK(h2_stream_send(a.stream, upload, sizeof(upload), true, &a.wait, &sent) == H2_SILENT);
    CHECK(sent >= H2_OUT_MARK - 20000 && sent <= H2_OUT_MARK + 20000);
    CHECK(h2_conn_queued(conn) <= H2_OUT_MARK + 20000);
    peer_stall(false);
    pthread_join(t, NULL);
    pings(false);
    fetch_done(&a);
    fetch_done(&holder.f);
    CHECK(server_request("/holder") == id + 2);

    // A reset queued with no drainer at all reaches the wire by the reader,
    // within a slice of the socket taking bytes again.
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/gone") == H2_OK);
    id = server_request("/gone");
    peer_stall(true);
    fetch_done(&a);
    nap_ms(50);
    peer_stall(false);
    CHECK(server_sees_rst(id, H2_ERR_CANCEL));

    // THE QUEUE HAS A CEILING. With the socket stalled and one thread stuck
    // as its drainer, another opens and cancels streams as fast as it can.
    // The queue fills to its mark and that thread is parked at the door; it
    // does not grow with every slot that is freed and used again.
    peer_stall(true);
    opener_t drainer = {.path = "/drainer"};
    fetch_init(&drainer.f, 3000);
    pthread_t stuck_thread;
    pthread_create(&stuck_thread, NULL, opener, &drainer);
    nap_ms(100);
    churn_t ch = {0};
    pthread_create(&t, NULL, churner, &ch);
    for (int i = 0; i < 600 && atomic_load(&ch.parked) == 0; i++)
        nap_ms(10);
    nap_ms(200);
    atomic_store(&ch.stop, true);
    pthread_join(t, NULL);
    CHECK(atomic_load(&ch.opened) > 100 && atomic_load(&ch.parked) > 0);
    // The mark, one stream's HEADERS past it, and a reset for each stream
    // let in: nothing like the megabytes thousands of streams would owe.
    CHECK(atomic_load(&ch.most_queued) <= H2_OUT_MARK + 4096 + (size_t)atomic_load(&ch.opened) * 64);
    CHECK(atomic_load(&ch.most_queued) < H2_OUT_MARK + H2_OUT_CONTROL_MOST);
    CHECK(h2_conn_queued(conn) > H2_OUT_MARK / 2);
    peer_stall(false);
    pthread_join(stuck_thread, NULL);
    CHECK(drainer.result == H2_OK);
    fetch_done(&drainer.f);
    for (int i = 0; i < 600 && h2_conn_queued(conn) != 0; i++)
        nap_ms(5);
    CHECK(h2_conn_queued(conn) == 0 && peer_records_sound());
    finish(dead);

    // THE WRITE PATIENCE. Bytes the socket has refused for thirty seconds
    // are the connection's death, noticed by the drainer waiting on them,
    // and its streams are told.
    dead = atomic_load(&dead_calls);
    begin();
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/x") == H2_OK && server_request("/x") == 1);
    peer_stall(true);
    opener_t stuck = {.path = "/stuck"};
    fetch_init(&stuck.f, 3000);
    pthread_create(&t, NULL, opener, &stuck);
    nap_ms(100);
    CHECK(atomic_load(&dead_calls) == dead);
    peer_clock_skip(H2_WRITE_PATIENCE_MS + 1);
    pthread_join(t, NULL);
    CHECK(died_within(dead, 2000));
    CHECK(fetch_head(&a) == H2_ENDED && h2_stream_ended(a.stream).how == H2_END_CONN_DEAD);
    fetch_done(&a);
    fetch_done(&stuck.f);
    finish_dead(dead);
    // And by the reader, when nobody is waiting on them: a reset left in
    // the queue by a fetch that has gone.
    dead = atomic_load(&dead_calls);
    begin();
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/x") == H2_OK && server_request("/x") == 1);
    peer_stall(true);
    fetch_done(&a);
    nap_ms(H2_READER_SLICE_MS + 100);       // the reader has tried, and been refused
    CHECK(atomic_load(&dead_calls) == dead);
    peer_clock_skip(H2_WRITE_PATIENCE_MS + 1);
    finish_dead(dead);

    // A FLOOD OF PINGS AT A SOCKET THAT WILL NOT TAKE THEIR ANSWERS ends
    // the connection: nothing else would bound what the server can make
    // the queue hold.
    dead = atomic_load(&dead_calls);
    begin();
    peer_stall(true);
    for (int i = 0; i < 70000 && atomic_load(&dead_calls) == dead; i++) {
        peer_send_ping();
        if (i % 2000 == 1999)
            nap_ms(20);
    }
    CHECK(died_within(dead, 5000));
    finish_dead(dead);

    // CIPHERTEXT THAT CAME WITH THE HANDSHAKE (the server's SETTINGS in the
    // same segment as its Finished) is handed over at adoption and read
    // first.
    dead = atomic_load(&dead_calls);
    os64_tls_client *engine;
    int32_t socket;
    uint8_t unread[256];
    peer_begin(&engine, &socket);
    static const h2_setting_t kOne[] = {{H2_SETTING_MAX_CONCURRENT_STREAMS, 1}};
    size_t n = peer_records_of_settings(kOne, 1, unread, sizeof(unread));
    conn = h2_conn_adopt(engine, socket, unread, n, &kGreeting, &kHooks);
    CHECK(n != 0 && conn != NULL && peer_preface(2000));
    CHECK(peer_frame_of(H2_FRAME_SETTINGS, &h, &payload, 2000) && !(h.flags & H2_FLAG_ACK));
    CHECK(peer_frame_of(H2_FRAME_SETTINGS, &h, &payload, 2000) && (h.flags & H2_FLAG_ACK));
    // And its one stream is all it takes: the server's limit is the room,
    // and raising it is news for the pool.
    fetch_init(&a, 3000);
    fetch_init(&b, 3000);
    CHECK(fetch_get(&a, conn, "/x") == H2_OK && !h2_conn_room(conn) && fetch_get(&b, conn, "/y") == H2_NO_ROOM);
    int rooms = atomic_load(&room_calls);
    static const h2_setting_t kTwo[] = {{H2_SETTING_MAX_CONCURRENT_STREAMS, 2}};
    peer_send_settings(kTwo, 1);
    for (int i = 0; i < 400 && atomic_load(&room_calls) == rooms; i++)
        nap_ms(5);
    CHECK(atomic_load(&room_calls) == rooms + 1 && h2_conn_room(conn) && fetch_get(&b, conn, "/y") == H2_OK);
    fetch_done(&a);
    fetch_done(&b);
    finish(dead);
}

// ── Waiting ─────────────────────────────────────────────────────────────

typedef struct {
    fetch_t *f;
    h2_result_t result;
} header_wait_t;

static void *head_waiter(void *x)
{
    header_wait_t *w = x;
    w->result = fetch_head(w->f);
    return NULL;
}

static void wait_cases(void)
{
    int dead = atomic_load(&dead_calls);
    fetch_t a, b;
    pthread_t t;
    struct timespec t0, t1;

    // A STREAM GOES SILENT ON A SILENT CONNECTION: the wait ends, and the
    // connection takes no new stream, since a path that died without a
    // word looks just like this. Its own streams keep their deadlines.
    begin();
    fetch_init(&a, 200);
    CHECK(fetch_get(&a, conn, "/silent") == H2_OK && server_request("/silent") == 1);
    CHECK(h2_conn_room(conn));
    CHECK(fetch_head(&a) == H2_SILENT);
    CHECK(!h2_conn_room(conn));
    fetch_init(&b, 200);
    CHECK(fetch_get(&b, conn, "/after") == H2_NO_ROOM);
    // And when its last stream leaves it ends, as after a GOAWAY: nothing
    // will ever use it again.
    fetch_done(&a);
    fetch_done(&b);
    finish_dead(dead);

    // Silent on ONE stream while frames arrive on the connection is that
    // stream's alone.
    dead = atomic_load(&dead_calls);
    begin();
    pings(true);
    fetch_init(&a, 200);
    CHECK(fetch_get(&a, conn, "/silent") == H2_OK && server_request("/silent") == 1);
    CHECK(fetch_head(&a) == H2_SILENT && h2_conn_room(conn));
    pings(false);
    fetch_done(&a);

    // CANCELLED WHILE IT WAITS, with nothing to ring it: it looks up, and
    // leaves, long before its patience is out.
    pings(true);
    fetch_init(&a, 60000);
    CHECK(fetch_get(&a, conn, "/cancel") == H2_OK && server_request("/cancel") == 3);
    header_wait_t w = {.f = &a};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_create(&t, NULL, head_waiter, &w);
    nap_ms(100);
    atomic_store(&a.cancelled, true);
    pthread_join(t, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CHECK(w.result == H2_INTERRUPTED && t1.tv_sec - t0.tv_sec < 10);
    // And cancelled before it asks, it does not wait at all.
    char byte;
    CHECK(h2_stream_read(a.stream, &byte, 1, &a.wait) == -(int64_t)H2_INTERRUPTED);
    pings(false);
    fetch_done(&a);

    // A fetch that cannot have a bell cannot wait, and says so.
    fetch_init(&a, 1000);
    host_bell_refuse_make = true;
    CHECK(fetch_get(&a, conn, "/nobell") == H2_FAILED && a.stream == NULL);
    host_bell_refuse_make = false;
    fetch_done(&a);

    // A request whose fields encode past the head limit is refused before
    // anything is sent or any table changes.
    static char huge[70000];
    memset(huge, 'h', sizeof(huge) - 1);
    fetch_init(&a, 1000);
    h2_field_t big[] = {F(":method", "GET"), F(":scheme", "https"), F(":path", "/"), F("cookie", huge)};
    CHECK(h2_stream_open(conn, big, 4, true, false, &a.wait, &a.stream) == H2_TOO_LARGE && a.stream == NULL);
    CHECK(fetch_get(&a, conn, "/fine") == H2_OK && server_request("/fine") == 5);
    fetch_done(&a);
    finish(dead);
}

// ── What wakes a wait, and what a dying connection still does ───────────

typedef struct {
    void (*say)(void);
    long after_ms;
} later_t;

// The server's next word, said from another thread once the case's own is
// waiting for it.
static void *later(void *x)
{
    later_t *l = x;
    nap_ms(l->after_ms);
    l->say();
    return NULL;
}

static void say_window(void)
{
    static const h2_setting_t kHundred[] = {{H2_SETTING_INITIAL_WINDOW_SIZE, 100}};
    peer_send_settings(kHundred, 1);
}
static void say_head(void) { server_reply(1, "200", NULL, false); }
static void say_data(void) { peer_send_data(1, "late", 4, true); }

typedef struct {
    uint32_t stream;
    fetch_t *f;
} nudge_t;

// The clock jumps past the fetch's patience, between two WINDOW_UPDATEs for
// its stream: each rings the fetch, and neither is what it waits for. Then
// the fetch is cancelled, which ends a wait that did not end by its patience.
static void *nudger(void *x)
{
    nudge_t *n = x;
    nap_ms(100);
    peer_send_window(n->stream, 1);
    peer_clock_skip(6000);
    peer_send_window(n->stream, 1);
    nap_ms(400);
    atomic_store(&n->f->cancelled, true);
    return NULL;
}

static void wake_cases(void)
{
    int dead = atomic_load(&dead_calls);
    fetch_t a;
    pthread_t t;
    char out[64];
    size_t sent;
    h2_frame_header_t h;
    const uint8_t *payload;

    // A WAIT IS WOKEN BY WHAT IT WAITS FOR. Each patience here is shorter
    // than H2_LOOK_MS, so no wait looks up by itself: the send is woken by
    // the window the server's SETTINGS give, the head's wait by the head,
    // the read by the bytes.
    static const h2_setting_t kTen[] = {{H2_SETTING_INITIAL_WINDOW_SIZE, 10}};
    begin_with(&kGreeting, kTen, 1);
    fetch_init(&a, 900);
    h2_field_t post[] = {F(":method", "POST"), F(":scheme", "https"), F(":path", "/up"), F(":authority", "a.test")};
    CHECK(h2_stream_open(conn, post, 4, false, false, &a.wait, &a.stream) == H2_OK && server_request("/up") == 1);
    later_t l = {say_window, 100};
    pthread_create(&t, NULL, later, &l);
    CHECK(h2_stream_send(a.stream, "0123456789abcdefghijklmno", 25, true, &a.wait, &sent) == H2_OK && sent == 25);
    pthread_join(t, NULL);
    l.say = say_head;
    pthread_create(&t, NULL, later, &l);
    CHECK(fetch_head(&a) == H2_OK);
    pthread_join(t, NULL);
    l.say = say_data;
    pthread_create(&t, NULL, later, &l);
    CHECK(h2_stream_read(a.stream, out, sizeof(out), &a.wait) == 4 && memcmp(out, "late", 4) == 0);
    pthread_join(t, NULL);
    // Both sides finished the stream: leaving it owes the server no reset.
    fetch_done(&a);
    CHECK(!peer_frame_of(H2_FRAME_RST_STREAM, &h, &payload, 300));

    // RUNG FOR NOTHING IT WANTS, with its patience already spent, a wait
    // ends there; and the connection that rang it is not quiet.
    fetch_init(&a, 5000);
    CHECK(fetch_get(&a, conn, "/nudged") == H2_OK);
    nudge_t nudge = {.stream = server_request("/nudged"), .f = &a};
    pthread_create(&t, NULL, nudger, &nudge);
    CHECK(fetch_head(&a) == H2_SILENT && h2_conn_room(conn));
    pthread_join(t, NULL);

    // THE POOL IS NOT ASKED about a connection that has a stream, and is
    // asked every slice about one that has none.
    int asked = atomic_load(&idle_calls);
    nap_ms(2 * H2_READER_SLICE_MS + 200);
    CHECK(atomic_load(&idle_calls) == asked);
    fetch_done(&a);
    CHECK(server_sees_rst(nudge.stream, H2_ERR_CANCEL));
    nap_ms(2 * H2_READER_SLICE_MS + 200);
    CHECK(atomic_load(&idle_calls) > asked);

    // A CONDEMNED CONNECTION SENDS NOTHING MORE: a fetch that leaves its
    // stream after the connection was condemned owes the server no reset.
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/gone") == H2_OK && server_request("/gone") != 0);
    h2_conn_condemn(conn);
    fetch_done(&a);
    CHECK(!peer_frame_of(H2_FRAME_RST_STREAM, &h, &payload, 800));
    finish(dead);

    // A HEAD OF MANY FIELDS, none of them large, is over the limit by their
    // sum: a hundred of a thousand bytes.
    dead = atomic_load(&dead_calls);
    begin();
    static char wide[1001];
    memset(wide, 'w', sizeof(wide) - 1);
    h2_field_t many_fields[101];
    many_fields[0] = F(":status", "200");
    for (int i = 1; i <= 100; i++)
        many_fields[i] = F("x-wide", wide);
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/wide") == H2_OK && server_request("/wide") == 1);
    peer_send_headers(1, many_fields, 101, false);
    CHECK(server_sees_rst(1, H2_ERR_ENHANCE_YOUR_CALM) && fetch_head(&a) == H2_ENDED);
    CHECK(h2_stream_ended(a.stream).how == H2_END_HEAD_TOO_LARGE && h2_conn_room(conn));
    fetch_done(&a);

    // THE READER DOES NOT CLOSE THE SOCKET UNDER A WRITE. A thread is inside
    // a write the stalled socket will not take when the connection is
    // condemned: the socket stays open until that write comes back.
    peer_stall(true);
    opener_t stuck = {.path = "/stuck"};
    fetch_init(&stuck.f, 3000);
    pthread_create(&t, NULL, opener, &stuck);
    nap_ms(100);
    h2_conn_condemn(conn);
    nap_ms(H2_READER_SLICE_MS + 300);
    CHECK(peer_closes() == 0 && atomic_load(&dead_calls) == dead);
    peer_stall(false);
    pthread_join(t, NULL);
    fetch_done(&stuck.f);
    finish_dead(dead);

    // THE SERVER'S CLOSE IN THE SAME READ AS ITS LAST FRAME: the frame is
    // delivered before the connection ends, and the response is whole.
    dead = atomic_load(&dead_calls);
    begin();
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/bye") == H2_OK && server_request("/bye") == 1);
    server_reply(1, "200", NULL, false);
    CHECK(fetch_head(&a) == H2_OK);
    peer_send_data_and_close(1, "whole", 5);
    size_t got;
    CHECK(read_all(&a, out, sizeof(out), &got) == 0 && got == 5 && memcmp(out, "whole", 5) == 0);
    CHECK(h2_stream_ended(a.stream).how == H2_END_CLEAN);
    fetch_done(&a);
    finish_dead(dead);

    // AN UPLOAD WHOSE ANSWER IS WHOLE, on a connection that then died with
    // no reset: the send stops at once, and the response stands.
    dead = atomic_load(&dead_calls);
    begin();
    fetch_init(&a, 3000);
    CHECK(h2_stream_open(conn, post, 4, false, false, &a.wait, &a.stream) == H2_OK && server_request("/up") == 1);
    server_reply(1, "413", NULL, true);
    CHECK(fetch_head(&a) == H2_OK);
    peer_hang_up();
    CHECK(died_within(dead, 2000));
    static char body[70000];
    CHECK(h2_stream_send(a.stream, body, sizeof(body), true, &a.wait, &sent) == H2_STOP && sent == 0);
    CHECK(h2_stream_ended(a.stream).how == H2_END_CLEAN);
    fetch_done(&a);
    finish_dead(dead);

    // A SERVER THAT TURNS PUSH ON has broken RFC 9113 § 6.5.2.
    dead = atomic_load(&dead_calls);
    begin();
    static const h2_setting_t kPush[] = {{H2_SETTING_ENABLE_PUSH, 1}};
    peer_send_settings(kPush, 1);
    CHECK(server_sees_goaway(H2_ERR_PROTOCOL));
    finish_dead(dead);

    // A FIELD BLOCK IS OPEN, and the frame after it is not its
    // CONTINUATION: the connection's error, whatever else is wrong with
    // that frame (here a PRIORITY of four bytes, which alone is its
    // stream's).
    dead = atomic_load(&dead_calls);
    begin();
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/x") == H2_OK && server_request("/x") == 1);
    static const uint8_t kStatus[4] = {0x88};
    peer_send_frame(H2_FRAME_HEADERS, 0, 1, kStatus, 1);
    peer_send_frame(H2_FRAME_PRIORITY, 0, 1, kStatus, 4);
    CHECK(server_sees_goaway(H2_ERR_PROTOCOL));
    fetch_done(&a);
    finish_dead(dead);

    // PLAINTEXT THE ENGINE ALREADY HOLDS when the connection is adopted (the
    // server's SETTINGS, decrypted with its Finished) is read at once: no
    // later read of the socket would bring it.
    dead = atomic_load(&dead_calls);
    os64_tls_client *engine;
    int32_t socket;
    peer_begin(&engine, &socket);
    peer_engine_holds_settings(NULL, 0);
    conn = h2_conn_adopt(engine, socket, NULL, 0, &kGreeting, &kHooks);
    CHECK(conn != NULL && peer_preface(2000));
    CHECK(peer_frame_of(H2_FRAME_SETTINGS, &h, &payload, 2000) && !(h.flags & H2_FLAG_ACK));
    CHECK(peer_frame_of(H2_FRAME_SETTINGS, &h, &payload, 2000) && (h.flags & H2_FLAG_ACK));

    // A FIELD LARGER THAN THE HEADER TABLE is a field all the same: the
    // table bounds what is indexed, the head limit what is taken.
    static char longer[5001];
    memset(longer, 'l', sizeof(longer) - 1);
    h2_field_t long_head[] = {F(":status", "200"), F("x-long", longer)};
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/long") == H2_OK && server_request("/long") == 1);
    peer_send_headers(1, long_head, 2, true);
    CHECK(fetch_head(&a) == H2_OK && h2_stream_ended(a.stream).how == H2_END_CLEAN);

    // CONTENT-LENGTH TWICE: the same number is one length, and two numbers
    // are a head that says nothing true.
    fetch_t b;
    h2_field_t same[] = {F(":status", "200"), F("content-length", "3"), F("content-length", "3")};
    h2_field_t differ[] = {F(":status", "200"), F("content-length", "10"), F("content-length", "3")};
    fetch_init(&b, 3000);
    CHECK(fetch_get(&b, conn, "/same") == H2_OK && server_request("/same") == 3);
    peer_send_headers(3, same, 3, false);
    peer_send_data(3, "abc", 3, true);
    CHECK(fetch_head(&b) == H2_OK && read_all(&b, out, sizeof(out), &got) == 0 && got == 3);
    fetch_done(&b);
    fetch_init(&b, 3000);
    CHECK(fetch_get(&b, conn, "/differ") == H2_OK && server_request("/differ") == 5);
    peer_send_headers(5, differ, 3, false);
    CHECK(server_sees_rst(5, H2_ERR_PROTOCOL) && fetch_head(&b) == H2_ENDED);
    CHECK(h2_stream_ended(b.stream).how == H2_END_BAD_HEAD);
    fetch_done(&b);

    // THE INITIAL WINDOW CHANGES with a stream that has sent its last still
    // in its fetch's hands: stream 1's window, at the most a window may be,
    // is nobody's any more, and one byte more for every stream is no error.
    peer_send_window(1, 0x7fffffff - 65535);
    static const h2_setting_t kOneMore[] = {{H2_SETTING_INITIAL_WINDOW_SIZE, 65536}};
    peer_send_settings(kOneMore, 1);
    fetch_init(&b, 3000);
    CHECK(fetch_get(&b, conn, "/still") == H2_OK && server_request("/still") == 7);
    server_reply(7, "200", "here", true);
    CHECK(fetch_head(&b) == H2_OK && read_all(&b, out, sizeof(out), &got) == 0 && got == 4);
    fetch_done(&b);
    fetch_done(&a);
    // For a stream still sending it is the error RFC 9113 § 6.9.2 says.
    fetch_init(&a, 3000);
    CHECK(h2_stream_open(conn, post, 4, false, false, &a.wait, &a.stream) == H2_OK && server_request("/up") == 9);
    peer_send_window(9, 0x7fffffff - 65536);
    static const h2_setting_t kTwoMore[] = {{H2_SETTING_INITIAL_WINDOW_SIZE, 65537}};
    peer_send_settings(kTwoMore, 1);
    CHECK(server_sees_goaway(H2_ERR_FLOW_CONTROL));
    fetch_done(&a);
    finish_dead(dead);

    // WINDOW GIVEN BACK FOR PADDING, AT A SOCKET THAT TAKES NOTHING: every
    // padded frame of a stream whose window is one byte owes a
    // WINDOW_UPDATE, and the queue's ceiling ends the connection as it does
    // for any other frame a server makes us owe.
    dead = atomic_load(&dead_calls);
    static const h2_setting_t kOneByte[] = {{H2_SETTING_INITIAL_WINDOW_SIZE, 1}};
    static const h2_greeting_t kOneByteGreeting = {.settings = kOneByte, .count = 1, .window_increment = 0};
    begin_with(&kOneByteGreeting, NULL, 0);
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/pad") == H2_OK && server_request("/pad") == 1);
    server_reply(1, "200", NULL, false);
    CHECK(fetch_head(&a) == H2_OK);
    peer_stall(true);
    static const uint8_t kNoPadding[1] = {0};       // a pad length of nothing: one byte of flow, none of body
    for (int i = 0; i < 200000 && atomic_load(&dead_calls) == dead; i++) {
        peer_send_frame(H2_FRAME_DATA, H2_FLAG_PADDED, 1, kNoPadding, 1);
        if (i % 2000 == 1999)
            nap_ms(20);
    }
    CHECK(died_within(dead, 5000));
    // It ended at the ceiling, and not when some other owed frame found the
    // queue far past it.
    CHECK(h2_conn_queued(conn) <= (size_t)H2_OUT_MARK + H2_OUT_CONTROL_MOST + 4096);
    fetch_done(&a);
    finish_dead(dead);

    // And for what the ENGINE owes of its own accord: a KeyUpdate asked for
    // over and over, at a socket that takes no answer.
    dead = atomic_load(&dead_calls);
    begin();
    peer_stall(true);
    for (int i = 0; i < 400000 && atomic_load(&dead_calls) == dead; i++) {
        peer_send_engine_owes();
        if (i % 2000 == 1999)
            nap_ms(20);
    }
    CHECK(died_within(dead, 5000));
    CHECK(h2_conn_queued(conn) <= (size_t)H2_OUT_MARK + H2_OUT_CONTROL_MOST + 8192);
    finish_dead(dead);

    // A RESPONSE THAT MAY HAVE NO CONTENT AND HAS SOME: a HEAD's, and a
    // 204's. The stream is reset and nothing of it is read. An empty DATA
    // frame that ends such a stream is how it is meant to end.
    dead = atomic_load(&dead_calls);
    begin();
    h2_field_t head_request[] = {F(":method", "HEAD"), F(":scheme", "https"), F(":path", "/h"), F(":authority", "a.test")};
    fetch_init(&a, 3000);
    CHECK(h2_stream_open(conn, head_request, 4, true, true, &a.wait, &a.stream) == H2_OK && server_request("/h") == 1);
    server_reply(1, "200", NULL, false);
    peer_send_data(1, "not empty", 9, true);
    CHECK(server_sees_rst(1, H2_ERR_PROTOCOL));
    CHECK(fetch_head(&a) == H2_OK && read_all(&a, out, sizeof(out), &got) == -(int64_t)H2_ENDED && got == 0);
    CHECK(h2_stream_ended(a.stream).how == H2_END_BAD_LENGTH);
    fetch_done(&a);
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/204") == H2_OK && server_request("/204") == 3);
    server_reply(3, "204", NULL, false);
    peer_send_data(3, "x", 1, true);
    CHECK(server_sees_rst(3, H2_ERR_PROTOCOL) && fetch_head(&a) == H2_OK);
    CHECK(read_all(&a, out, sizeof(out), &got) == -(int64_t)H2_ENDED && got == 0);
    fetch_done(&a);
    fetch_init(&a, 3000);
    CHECK(h2_stream_open(conn, head_request, 4, true, true, &a.wait, &a.stream) == H2_OK && server_request("/h") == 5);
    server_reply(5, "200", NULL, false);
    peer_send_data(5, "", 0, true);
    CHECK(fetch_head(&a) == H2_OK && read_all(&a, out, sizeof(out), &got) == 0 && got == 0);
    CHECK(h2_stream_ended(a.stream).how == H2_END_CLEAN);
    fetch_done(&a);

    fetch_done(&a);
    finish(dead);

    // A BUFFER READ EMPTY IS GIVEN BACK when it had grown: with windows of
    // a mebibyte, a stream's buffer grows past 200,000 bytes before any is
    // read, and once all are read it holds none of that while it waits.
    dead = atomic_load(&dead_calls);
    static const h2_setting_t kWideStream[] = {{H2_SETTING_INITIAL_WINDOW_SIZE, 1u << 20}};
    static const h2_greeting_t kWideWindows = {.settings = kWideStream, .count = 1, .window_increment = 1u << 20};
    begin_with(&kWideWindows, NULL, 0);
    static char chunk[10000], taken[200000];
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/buf") == H2_OK && server_request("/buf") == 1);
    server_reply(1, "200", NULL, false);
    CHECK(fetch_head(&a) == H2_OK);
    for (size_t at = 0; at < sizeof(taken); at += sizeof(chunk))
        peer_send_data(1, chunk, sizeof(chunk), false);
    for (int i = 0; i < 400 && h2_conn_buffered(conn) < sizeof(taken); i++)
        nap_ms(5);
    CHECK(h2_conn_buffered(conn) >= sizeof(taken));
    for (got = 0; got < sizeof(taken);) {
        int64_t n = h2_stream_read(a.stream, taken + got, sizeof(taken) - got, &a.wait);
        CHECK(n > 0);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    // (What it keeps is the little a buffer may keep: bytes that arrived
    // after the large one was given back.)
    CHECK(h2_conn_buffered(conn) <= 64u * 1024u);
    // And it takes more afterwards.
    peer_send_data(1, "more", 4, true);
    CHECK(read_all(&a, out, sizeof(out), &got) == 0 && got == 4 && memcmp(out, "more", 4) == 0);
    fetch_done(&a);
    finish(dead);

    // A HEAD WHOSE CONTINUATION NEVER COMES is still a server that
    // answered: the stream ends with the connection, and says a frame of a
    // response was seen, which is what forbids trying the request again.
    dead = atomic_load(&dead_calls);
    begin();
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/half") == H2_OK && server_request("/half") == 1);
    static const uint8_t kHalf[1] = {0x88};
    peer_send_frame(H2_FRAME_HEADERS, 0, 1, kHalf, 1);
    nap_ms(100);
    peer_hang_up();
    CHECK(fetch_head(&a) == H2_ENDED);
    CHECK(h2_stream_ended(a.stream).how == H2_END_CONN_DEAD && h2_stream_ended(a.stream).any_seen &&
          !h2_stream_ended(a.stream).head_seen);
    fetch_done(&a);
    finish_dead(dead);

    // ONE SETTINGS FRAME THAT STATES THE STREAM LIMIT TWICE, up to three
    // and then down to two, leaves room where there was none, and the pool
    // is told.
    dead = atomic_load(&dead_calls);
    static const h2_setting_t kOneStream[] = {{H2_SETTING_MAX_CONCURRENT_STREAMS, 1}};
    begin_with(&kGreeting, kOneStream, 1);
    fetch_init(&a, 3000);
    CHECK(fetch_get(&a, conn, "/x") == H2_OK && server_request("/x") == 1 && !h2_conn_room(conn));
    int rooms = atomic_load(&room_calls);
    static const h2_setting_t kThreeThenTwo[] = {{H2_SETTING_MAX_CONCURRENT_STREAMS, 3},
                                                 {H2_SETTING_MAX_CONCURRENT_STREAMS, 2}};
    peer_send_settings(kThreeThenTwo, 2);
    for (int i = 0; i < 400 && atomic_load(&room_calls) == rooms; i++)
        nap_ms(5);
    CHECK(atomic_load(&room_calls) == rooms + 1 && h2_conn_room(conn));
    fetch_done(&a);

    // A WINDOW PAST 2^31 - 1 FOR A STREAM WHOSE ANSWER IS WHOLE and whose
    // request is still going up: no reset would change how that stream
    // ended, so it is the connection's error.
    fetch_init(&a, 3000);
    CHECK(h2_stream_open(conn, post, 4, false, false, &a.wait, &a.stream) == H2_OK);
    uint32_t up = server_request("/up");
    server_reply(up, "413", NULL, true);
    CHECK(fetch_head(&a) == H2_OK);
    peer_send_window(up, 0x7fffffff - 65535);
    peer_send_window(up, 1);
    CHECK(server_sees_goaway(H2_ERR_FLOW_CONTROL));
    fetch_done(&a);
    finish_dead(dead);

    // A SIGNAL DOES NOT START THE WRITE PATIENCE AGAIN. A write that moved
    // nothing is interrupted twenty seconds in, twice: the second leaves
    // forty seconds without a byte taken, and that is the end.
    dead = atomic_load(&dead_calls);
    begin();
    peer_stall(true);
    peer_interrupt_writes(3);
    opener_t waiting = {.path = "/signalled"};
    fetch_init(&waiting.f, 3000);
    pthread_create(&t, NULL, opener, &waiting);
    CHECK(died_within(dead, 2000));
    peer_stall(false);
    pthread_join(t, NULL);
    fetch_done(&waiting.f);
    finish_dead(dead);
}

// ── Many fetches at once ────────────────────────────────────────────────

#define STRESS_THREADS 8
#define STRESS_ROUNDS 150
static atomic_bool serving;
static atomic_int served, server_resets;

// What "/<n>" answers: n * 37 mod 3000 bytes, the i-th being n + i.
static size_t answer_len(unsigned n) { return (size_t)(n * 37u % 3000u); }

// The server for the many: every request is answered whole, or reset, at
// random, in frames of random sizes.
static void *responder(void *x)
{
    (void)x;
    unsigned seed = 12345;
    static uint8_t body[3000];
    while (atomic_load(&serving)) {
        h2_frame_header_t h;
        const uint8_t *payload;
        const h2_field_t *fields;
        size_t count;
        if (!peer_frame(&h, &payload, 50) || h.type != H2_FRAME_HEADERS)
            continue;
        CHECK(peer_decode(payload, h.length, &fields, &count) && count == 4);
        unsigned n = (unsigned)strtoul((const char *)fields[2].value + 1, NULL, 10);
        size_t len = answer_len(n);
        seed = seed * 1103515245u + 12345u;
        if ((seed >> 16) % 10 == 0) {
            peer_send_rst(h.stream, H2_ERR_INTERNAL);
            atomic_fetch_add(&server_resets, 1);
            continue;
        }
        char length[16];
        snprintf(length, sizeof(length), "%zu", len);
        h2_field_t head[] = {F(":status", "200"), F("content-length", length)};
        peer_send_headers(h.stream, head, 2, len == 0);
        for (size_t i = 0; i < len; i++)
            body[i] = (uint8_t)(n + i);
        for (size_t at = 0; at < len;) {
            seed = seed * 1103515245u + 12345u;
            size_t piece = 1 + (seed >> 16) % 1200;
            if (piece > len - at)
                piece = len - at;
            peer_send_data(h.stream, body + at, piece, at + piece == len);
            at += piece;
        }
        atomic_fetch_add(&served, 1);
    }
    return NULL;
}

static atomic_int whole, reset_seen, left_early;

static void *many(void *arg)
{
    unsigned me = (unsigned)(uintptr_t)arg, seed = me * 7919u;
    static _Thread_local uint8_t got[3000];
    for (unsigned round = 0; round < STRESS_ROUNDS; round++) {
        unsigned n = me * 1000u + round;
        char path[16];
        snprintf(path, sizeof(path), "/%u", n);
        fetch_t f;
        fetch_init(&f, 20000);
        CHECK(fetch_get(&f, conn, path) == H2_OK);
        seed = seed * 1103515245u + 12345u;
        if ((seed >> 16) % 7 == 0) {
            atomic_fetch_add(&left_early, 1);       // leaves before a byte of the answer
            fetch_done(&f);
            continue;
        }
        h2_result_t head = fetch_head(&f);
        if (head == H2_ENDED) {
            CHECK(h2_stream_ended(f.stream).how == H2_END_PEER_RESET);
            atomic_fetch_add(&reset_seen, 1);
            fetch_done(&f);
            continue;
        }
        CHECK(head == H2_OK);
        size_t len = 0;
        int64_t r;
        seed = seed * 1103515245u + 12345u;
        size_t most = (seed >> 16) % 5 == 0 ? 100 : sizeof(got);     // some leave part way through
        while (len < most && (r = h2_stream_read(f.stream, got + len, most - len, &f.wait)) > 0)
            len += (size_t)r;
        bool right = true;
        for (size_t i = 0; i < len; i++)
            right = right && got[i] == (uint8_t)(n + i);
        CHECK(right);
        if (most == sizeof(got)) {
            CHECK(len == answer_len(n) && h2_stream_ended(f.stream).how == H2_END_CLEAN);
            atomic_fetch_add(&whole, 1);
        }
        fetch_done(&f);
    }
    return NULL;
}

static void many_cases(void)
{
    int dead = atomic_load(&dead_calls);
    // Windows the server need not count: this case is about the threads.
    static const h2_setting_t kWide[] = {{H2_SETTING_INITIAL_WINDOW_SIZE, 1u << 30}};
    static const h2_greeting_t kWideGreeting = {.settings = kWide, .count = 1, .window_increment = 1u << 30};
    begin_with(&kWideGreeting, NULL, 0);
    pthread_t server, t[STRESS_THREADS];
    atomic_store(&serving, true);
    pthread_create(&server, NULL, responder, NULL);
    for (uintptr_t i = 0; i < STRESS_THREADS; i++)
        pthread_create(&t[i], NULL, many, (void *)(i + 1));
    for (int i = 0; i < STRESS_THREADS; i++)
        pthread_join(t[i], NULL);
    for (int i = 0; i < 400 && atomic_load(&served) + atomic_load(&server_resets) < STRESS_THREADS * STRESS_ROUNDS; i++)
        nap_ms(5);
    atomic_store(&serving, false);
    pthread_join(server, NULL);
    // Every request reached the server, each kind of ending happened, the
    // connection is still good, and it holds no stream.
    CHECK(atomic_load(&served) + atomic_load(&server_resets) == STRESS_THREADS * STRESS_ROUNDS);
    CHECK(atomic_load(&whole) > 100 && atomic_load(&reset_seen) > 10 && atomic_load(&left_early) > 50);
    CHECK(h2_conn_room(conn) && h2_conn_streams(conn) == 0 && peer_records_sound());
    finish(dead);
}

int main(void)
{
    alarm(SECONDS_MOST);
    basic_cases();
    ending_cases();
    window_cases();
    head_cases();
    error_cases();
    queue_cases();
    wait_cases();
    wake_cases();
    many_cases();
    if (atomic_load(&failures) != 0) {
        fprintf(stderr, "test_h2_conn_host: %d of %d checks failed\n", atomic_load(&failures), atomic_load(&checks));
        return 1;
    }
    printf("test_h2_conn_host: %d checks passed\n", atomic_load(&checks));
    return 0;
}
