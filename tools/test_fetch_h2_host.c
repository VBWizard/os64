// test_fetch_h2_host.c — libfetch's driver and pool with HTTP/2 offered
// (fetch_h2_offer), against scripted servers, with real threads:
// tools/test_fetch_h2_host.sh builds it under ASan and UBSan, then TSan.
// HTTP2.md § 4 H2.6 lists the cases.
//
// THE SEAM is the one tools/test_fetch_host.c cuts (libos64's calls and
// libtls's transport), with tools/h2_peer.c under it: every dial is a wire
// of the peer's, and a SERVER THREAD is started for each. A wire whose TLS
// stand-in answered ALPN `h2` is handed to an HTTP/2 connection and served
// frame by frame; any other is HTTP/1.1 as text. What a server says is a
// table of ROUTES by path, so a case is client code: it starts fetches, on
// threads of their own since a fetch waits, and checks what they were told,
// what was dialled, and what each server saw.
//
// Every case ends in `settle`: no connection left, every socket closed
// once, every engine freed, no record out of order, no bell alive.
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fetch/fetch.h"
#include "fetch/transport.h"
#include "h2_peer.h"
#include "host_bell.h"
#include "os64/os64.h"
#include "pool.h"
#include "fixtures.h"

// A FAILING RUN IS A SHORT ONE: this is run with rules broken on purpose.
#define FAILURES_MOST 50
#define SECONDS_MOST 300
static atomic_int checks, failures;
#define CHECK(x)                                                              \
    do {                                                                      \
        atomic_fetch_add(&checks, 1);                                         \
        if (!(x)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); \
            if (atomic_fetch_add(&failures, 1) + 1 >= FAILURES_MOST) {        \
                fprintf(stderr, "test_fetch_h2_host: %d failures, stopping\n", FAILURES_MOST); \
                exit(1);                                                      \
            }                                                                 \
        }                                                                     \
    } while (0)

static void nap_ms(long ms)
{
    struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

// ── What a server says ──────────────────────────────────────────────────

typedef enum {
    R_REPLY = 0,        // status, fields, body
    R_REFUSE_ONCE,      // RST_STREAM(REFUSED_STREAM) the first time, then the reply
    R_REFUSE,           // ...every time
    R_UNPROCESSED_ONCE, // GOAWAY whose last stream is below this one, the first time
    R_DIE,              // the connection ends with no answer
    R_DIE_ONCE,         // ...the first time
    R_SILENT,           // no answer at all
    R_HEAD_THEN_DIE,    // the head, half the body, and the connection ends
    R_HELD,             // the head now, the body when `release_held` is set
    R_RESET_MID_BODY,   // the head, half the body, RST_STREAM(INTERNAL_ERROR)
    R_HALF_HEAD,        // HEADERS without END_HEADERS, and the connection ends
    R_CANCEL,           // RST_STREAM(CANCEL) before any answer
    R_HUGE_HEAD,        // a head of a hundred fields of a thousand bytes
    R_OPEN,             // the head, and the stream left open with nothing after it
    R_ECHO,             // the request's body back, once it has all come
    R_REFUSE_ONCE_HELD, // RST_STREAM(REFUSED_STREAM) the first time; then the head, and no body
    R_UNENDED,          // the head and the whole body, and the stream left open after it
} route_kind_t;

typedef struct {
    const char *path;
    route_kind_t kind;
    const char *status;             // "200" when NULL
    const char *fields[8][2];       // name, value; NULL name ends
    const uint8_t *body;
    size_t body_len;
    bool no_length;                 // h1: say no Content-Length (h2 never needs one)
    atomic_int hits;
} route_t;

#define ROUTES_MOST 16
static route_t routes[ROUTES_MOST];
static int nroutes;
static atomic_bool release_held;

static route_t *route(const char *path, route_kind_t kind, const char *status, const char *body)
{
    route_t *r = &routes[nroutes++];
    memset(r, 0, sizeof(*r));
    r->path = path;
    r->kind = kind;
    r->status = status;
    r->body = (const uint8_t *)body;
    r->body_len = body != NULL ? strlen(body) : 0;
    return r;
}

static route_t *route_for(const char *path, size_t len)
{
    for (int i = 0; i < nroutes; i++)
        if (strlen(routes[i].path) == len && memcmp(routes[i].path, path, len) == 0)
            return &routes[i];
    return NULL;
}

// ── The dials, and the servers behind them ──────────────────────────────

typedef struct {
    char host[64];
    int port, wire;
    const char *alpn;               // set by the TLS stand-in; NULL until then, and for a plain dial
    atomic_bool tls, served;
    atomic_int requests, resets, bodies;    // what its server saw
    pthread_t thread;
} dial_t;

#define DIALS_MOST 64
static dial_t dials[DIALS_MOST];
static atomic_int ndials;
static atomic_bool stopping;
static atomic_uint sockets_most;

// What each host answers a TLS dial with, and what its HTTP/2 server allows.
static struct {
    const char *host;
    const char *alpn;               // "h2" when NULL
    uint32_t max_streams;           // 0 = says nothing
    atomic_int hang_up_at_handshake;   // this many dials are hung up on as their handshake ends
    atomic_int refuse;                 // this many dials fail
    int handshake_ms;                  // how long its handshake takes
    bool resumes;                      // its handshakes say they resumed a kept session
} hosts[8];
static int nhosts;

static int host_at(const char *name)
{
    for (int i = 0; i < nhosts; i++)
        if (strcmp(hosts[i].host, name) == 0)
            return i;
    return -1;
}

static int host_says(const char *name, const char *alpn, uint32_t max_streams)
{
    hosts[nhosts].host = name;
    hosts[nhosts].alpn = alpn;
    hosts[nhosts].max_streams = max_streams;
    return nhosts++;
}

static void send_reply_h2(uint32_t stream, const route_t *r, bool head_only)
{
    h2_field_t f[10];
    size_t n = 0;
    const char *status = r->status != NULL ? r->status : "200";
    f[n++] = (h2_field_t){.name = (const uint8_t *)":status", .name_len = 7, .value = (const uint8_t *)status,
                          .value_len = 3};
    for (int i = 0; i < 8 && r->fields[i][0] != NULL; i++)
        f[n++] = (h2_field_t){.name = (const uint8_t *)r->fields[i][0], .name_len = (uint32_t)strlen(r->fields[i][0]),
                              .value = (const uint8_t *)r->fields[i][1],
                              .value_len = (uint32_t)strlen(r->fields[i][1])};
    bool no_body = r->body_len == 0;
    peer_send_headers(stream, f, n, no_body && !head_only);
    if (head_only || no_body)
        return;
    for (size_t at = 0; at < r->body_len;) {
        size_t piece = r->body_len - at < 9000 ? r->body_len - at : 9000;
        peer_send_data(stream, r->body + at, piece, at + piece == r->body_len);
        at += piece;
    }
}

// One HTTP/2 connection's server: the hello, then every request by its route.
static void serve_h2(dial_t *d)
{
    h2_frame_header_t h;
    const uint8_t *payload;
    uint32_t held[64];
    int nheld = 0;
    // An upload in progress: its stream, and what has come of its body.
    uint32_t echo_stream = 0;
    static _Thread_local uint8_t echo[4096];
    size_t echo_len = 0;
    int at = host_at(d->host);
    uint32_t most = at >= 0 ? hosts[at].max_streams : 0;
    peer_use(d->wire);
    if (!peer_preface(3000))
        return;
    h2_setting_t limit = {H2_SETTING_MAX_CONCURRENT_STREAMS, most};
    peer_send_settings(&limit, most != 0 ? 1 : 0);
    while (!atomic_load(&stopping) && peer_closes() == 0) {
        if (atomic_load(&release_held) && nheld != 0) {
            for (int i = 0; i < nheld; i++)
                peer_send_data(held[i], "held body", 9, true);
            nheld = 0;
        }
        if (!peer_frame(&h, &payload, 20))
            continue;
        if (h.type == H2_FRAME_RST_STREAM)
            atomic_fetch_add(&d->resets, 1);
        if (h.type == H2_FRAME_DATA && h.stream == echo_stream) {
            size_t n = h.length < sizeof(echo) - echo_len ? h.length : sizeof(echo) - echo_len;
            memcpy(echo + echo_len, payload, n);
            echo_len += n;
            if (h.flags & H2_FLAG_END_STREAM) {
                route_t back = {.status = "200", .body = echo, .body_len = echo_len};
                send_reply_h2(h.stream, &back, false);
                echo_stream = 0;
            }
        }
        if (h.type == H2_FRAME_DATA && (h.flags & H2_FLAG_END_STREAM))
            atomic_fetch_add(&d->bodies, 1);
        if (h.type != H2_FRAME_HEADERS)
            continue;
        const h2_field_t *fields;
        size_t count;
        if (!peer_decode(payload, h.length, &fields, &count))
            continue;
        const route_t *r = NULL;
        for (size_t i = 0; i < count; i++)
            if (fields[i].name_len == 5 && memcmp(fields[i].name, ":path", 5) == 0)
                r = route_for((const char *)fields[i].value, fields[i].value_len);
        atomic_fetch_add(&d->requests, 1);
        if (r == NULL) {
            static const route_t kNone = {.status = "404"};
            send_reply_h2(h.stream, &kNone, false);
            continue;
        }
        int hit = atomic_fetch_add(&((route_t *)r)->hits, 1);
        switch (r->kind) {
        case R_REFUSE_ONCE:
        case R_REFUSE:
        case R_REFUSE_ONCE_HELD:
            if (hit == 0 || r->kind == R_REFUSE) {
                peer_send_rst(h.stream, H2_ERR_REFUSED_STREAM);
                break;
            }
            send_reply_h2(h.stream, r, r->kind == R_REFUSE_ONCE_HELD);
            break;
        case R_UNPROCESSED_ONCE:
            if (hit == 0) {
                peer_send_goaway(h.stream - 2, H2_ERR_NO_ERROR);
                break;
            }
            send_reply_h2(h.stream, r, false);
            break;
        case R_DIE_ONCE:
            if (hit != 0) {
                send_reply_h2(h.stream, r, false);
                break;
            }
            peer_hang_up();
            return;
        case R_DIE:
            peer_hang_up();
            return;
        case R_SILENT:
            break;
        case R_CANCEL:
            peer_send_rst(h.stream, H2_ERR_CANCEL);
            break;
        case R_OPEN:
            send_reply_h2(h.stream, r, true);
            break;
        case R_UNENDED:
            send_reply_h2(h.stream, r, true);
            for (size_t at = 0; at < r->body_len; at += 9000)
                peer_send_data(h.stream, r->body + at, r->body_len - at < 9000 ? r->body_len - at : 9000, false);
            break;
        case R_ECHO:
            echo_stream = h.stream;
            echo_len = 0;
            if (h.flags & H2_FLAG_END_STREAM) {
                route_t none = {.status = "200", .body = (const uint8_t *)"no body came", .body_len = 12};
                send_reply_h2(h.stream, &none, false);
                echo_stream = 0;
            }
            break;
        case R_HUGE_HEAD: {
            static char wide[1001];
            h2_field_t many[101];
            memset(wide, 'w', sizeof(wide) - 1);
            many[0] = (h2_field_t){.name = (const uint8_t *)":status", .name_len = 7, .value = (const uint8_t *)"200",
                                   .value_len = 3};
            for (int i = 1; i <= 100; i++)
                many[i] = (h2_field_t){.name = (const uint8_t *)"x-wide", .name_len = 6,
                                       .value = (const uint8_t *)wide, .value_len = 1000};
            peer_send_headers(h.stream, many, 101, true);
            break;
        }
        case R_HALF_HEAD: {
            static const uint8_t kStatus[1] = {0x88};
            peer_send_frame(H2_FRAME_HEADERS, 0, h.stream, kStatus, 1);
            nap_ms(100);
            peer_hang_up();
            return;
        }
        case R_HEAD_THEN_DIE:
        case R_RESET_MID_BODY:
            send_reply_h2(h.stream, r, true);
            peer_send_data(h.stream, r->body, r->body_len / 2, false);
            if (r->kind == R_RESET_MID_BODY) {
                peer_send_rst(h.stream, H2_ERR_INTERNAL);
                break;
            }
            nap_ms(100);
            peer_hang_up();
            return;
        case R_HELD:
            send_reply_h2(h.stream, r, true);
            if (nheld < 64)
                held[nheld++] = h.stream;
            break;
        default:
            send_reply_h2(h.stream, r, false);
            break;
        }
    }
}

// One HTTP/1.1 connection's server, plain or under the TLS stand-in: each
// request's reply as text, the connection kept.
static void serve_h1(dial_t *d)
{
    static _Thread_local char in[16384];
    size_t have = 0;
    peer_use(d->wire);
    while (!atomic_load(&stopping) && peer_closes() == 0) {
        size_t n = peer_raw_take(in + have, sizeof(in) - 1 - have, 20);
        have += n;
        in[have] = '\0';
        char *end = strstr(in, "\r\n\r\n");
        if (end == NULL)
            continue;
        char *path = strchr(in, ' ');
        size_t path_len = path != NULL ? strcspn(path + 1, " ") : 0;
        const route_t *r = path != NULL ? route_for(path + 1, path_len) : NULL;
        bool head = strncmp(in, "HEAD ", 5) == 0;
        atomic_fetch_add(&d->requests, 1);
        in[have] = '\0';
        have = 0;
        char out[2048];
        if (r == NULL) {
            peer_raw_say("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n", 45);
            continue;
        }
        atomic_fetch_add(&((route_t *)r)->hits, 1);
        route_t back;
        if (r->kind == R_ECHO) {
            // The body follows the head: as many bytes as Content-Length says.
            static _Thread_local char body[4096];
            const char *said = strcasestr(in, "content-length:");
            size_t want = said != NULL ? (size_t)strtoul(said + 15, NULL, 10) : 0, got = 0;
            size_t after = (size_t)(in + strlen(in) - (end + 4));
            memcpy(body, end + 4, after < sizeof(body) ? after : sizeof(body));
            got = after;
            while (got < want && got < sizeof(body) && !atomic_load(&stopping))
                got += peer_raw_take(body + got, sizeof(body) - got, 20);
            back = (route_t){.status = "200", .body = (const uint8_t *)body, .body_len = got};
            r = &back;
        }
        int len = snprintf(out, sizeof(out), "HTTP/1.1 %s Status\r\n", r->status != NULL ? r->status : "200");
        for (int i = 0; i < 8 && r->fields[i][0] != NULL; i++)
            len += snprintf(out + len, sizeof(out) - (size_t)len, "%s: %s\r\n", r->fields[i][0], r->fields[i][1]);
        bool has_length = false;
        for (int i = 0; i < 8 && r->fields[i][0] != NULL; i++)
            has_length = has_length || strcasecmp(r->fields[i][0], "content-length") == 0;
        if (!has_length)
            len += snprintf(out + len, sizeof(out) - (size_t)len, "Content-Length: %zu\r\n", r->body_len);
        len += snprintf(out + len, sizeof(out) - (size_t)len, "\r\n");
        peer_raw_say(out, (size_t)len);
        if (!head && r->body_len != 0)
            peer_raw_say(r->body, r->body_len);
    }
}

static void *server(void *x)
{
    dial_t *d = x;
    // A TLS dial is one protocol or the other once its handshake stand-in
    // has answered; a plain dial is HTTP/1.1.
    if (d->port == 443) {
        peer_use(d->wire);
        while (!atomic_load(&d->tls) && !atomic_load(&stopping) && peer_closes() == 0)
            nap_ms(1);
        if (!atomic_load(&d->tls)) {
            atomic_store(&d->served, true);
            return NULL;
        }
    }
    if (d->alpn != NULL && strcmp(d->alpn, "h2") == 0)
        serve_h2(d);
    else
        serve_h1(d);
    atomic_store(&d->served, true);
    return NULL;
}

// ── The OS and TLS, as libfetch reaches them ────────────────────────────

const char *os64_getenv(const char *key) { (void)key; return NULL; }
int64_t os64_write(int32_t h, const void *buf, size_t len) { (void)h; (void)buf; return (int64_t)len; }
const char *os64_dial_reason(int64_t err) { (void)err; return "connection refused"; }

static pthread_mutex_t dial_mu = PTHREAD_MUTEX_INITIALIZER;

int64_t os64_dial(const char *dialstring)
{
    char host[64];
    int port = 0;
    if (sscanf(dialstring, "tcp!%63[^!]!%d", host, &port) != 2)
        abort();
    int at = host_at(host);
    if (at >= 0 && atomic_load(&hosts[at].refuse) > 0) {
        atomic_fetch_sub(&hosts[at].refuse, 1);
        return OS64_NET_ERR_REFUSED;
    }
    pthread_mutex_lock(&dial_mu);
    int n = atomic_load(&ndials);
    if (n == DIALS_MOST)
        abort();
    dial_t *d = &dials[n];
    memset(d, 0, sizeof(*d));
    snprintf(d->host, sizeof(d->host), "%s", host);
    d->port = port;
    d->wire = peer_open();
    unsigned open_now = peer_sockets_open();
    if (open_now > atomic_load(&sockets_most))
        atomic_store(&sockets_most, open_now);
    pthread_create(&d->thread, NULL, server, d);
    atomic_store(&ndials, n + 1);
    pthread_mutex_unlock(&dial_mu);
    return peer_socket(d->wire);
}

static dial_t *dial_of(int32_t socket)
{
    int n = atomic_load(&ndials);
    for (int i = n - 1; i >= 0; i--)
        if (peer_socket(dials[i].wire) == socket)
            return &dials[i];
    abort();
}

struct os64_tls_transport {
    int32_t handle;
    const char *alpn;
    os64_tls_status_t status;
    bool closing, resumed;
};
static atomic_int tls_made, tls_freed, tls_released;
static os64_tls_trust *const kTrust = (os64_tls_trust *)"trust";

os64_tls_status_t os64_tls_transport_create(const os64_tls_config_t *c, int32_t h,
                                            const os64_tls_transport_limits_t *limits, os64_tls_transport **out)
{
    (void)limits;
    dial_t *d = dial_of(h);
    int at = host_at(d->host);
    const char *answer = at >= 0 && hosts[at].alpn != NULL ? hosts[at].alpn : "h2";
    bool offered = false;
    for (size_t i = 0; i < c->alpn_count; i++)
        offered = offered || (c->alpn[i].length == strlen(answer) && memcmp(c->alpn[i].data, answer, c->alpn[i].length) == 0);
    if (at >= 0 && hosts[at].handshake_ms != 0)
        nap_ms(hosts[at].handshake_ms);
    struct os64_tls_transport *t = calloc(1, sizeof(*t));
    t->handle = h;
    t->alpn = offered ? answer : "http/1.1";
    t->status = OS64_TLS_OK;
    t->resumed = at >= 0 && hosts[at].resumes;
    d->alpn = t->alpn;
    atomic_store(&d->tls, true);
    atomic_fetch_add(&tls_made, 1);
    if (at >= 0 && atomic_load(&hosts[at].hang_up_at_handshake) > 0) {
        atomic_fetch_sub(&hosts[at].hang_up_at_handshake, 1);
        peer_use(d->wire);
        peer_hang_up();
    }
    *out = t;
    return OS64_TLS_OK;
}

os64_tls_state_t os64_tls_transport_state(os64_tls_transport *t)
{
    unsigned flags = OS64_TLS_HANDSHAKE_DONE | OS64_TLS_SEND_PLAIN;
    if (t->resumed)
        flags |= OS64_TLS_RESUMED;
    if (t->status == OS64_TLS_OK && peer_readable(t->handle, 0))
        flags |= OS64_TLS_RECV_PLAIN;
    return (os64_tls_state_t){.status = t->status, .flags = flags, .alpn = t->alpn, .version = 0x0304};
}

os64_tls_status_t os64_tls_transport_step(os64_tls_transport *t, uint64_t ms)
{
    if (t->closing)
        return t->status = OS64_TLS_CLEAN_EOF;
    if (t->status != OS64_TLS_OK)
        return t->status;
    return peer_readable(t->handle, ms < 20 ? ms : 20) ? OS64_TLS_OK : OS64_TLS_NEED_PROGRESS;
}

os64_tls_transfer_t os64_tls_transport_write(os64_tls_transport *t, const void *p, size_t n)
{
    int64_t sent = os64_write_for(t->handle, p, n, 0);
    return (os64_tls_transfer_t){sent < 0 ? OS64_TLS_TRANSPORT : OS64_TLS_OK, sent < 0 ? 0 : (size_t)sent};
}

os64_tls_transfer_t os64_tls_transport_read(os64_tls_transport *t, void *p, size_t n)
{
    if (t->status != OS64_TLS_OK)
        return (os64_tls_transfer_t){t->status, 0};
    int64_t got = os64_read_for(t->handle, p, n, 0);
    if (got > 0)
        return (os64_tls_transfer_t){OS64_TLS_OK, (size_t)got};
    if (got == 0)
        return (os64_tls_transfer_t){t->status = OS64_TLS_CLEAN_EOF, 0};
    return (os64_tls_transfer_t){OS64_TLS_NEED_PROGRESS, 0};
}

os64_tls_status_t os64_tls_transport_flush(os64_tls_transport *t) { return t->status; }
os64_tls_status_t os64_tls_transport_begin_close(os64_tls_transport *t) { t->closing = true; return t->status; }
os64_tls_status_t os64_tls_transport_abort(os64_tls_transport *t, os64_tls_status_t s) { return t->status = s; }
void os64_tls_transport_free(os64_tls_transport *t)
{
    atomic_fetch_add(&tls_freed, 1);
    os64_close(t->handle);
    free(t);
}

os64_tls_status_t os64_tls_transport_release(os64_tls_transport *t, os64_tls_client **client, int32_t *handle,
                                             void *unread, size_t room, size_t *unread_len)
{
    (void)unread;
    if (room < OS64_TLS_TRANSPORT_UNREAD_MAX)
        abort();
    *client = peer_engine(peer_index_of(t->handle));
    *handle = t->handle;
    *unread_len = 0;
    atomic_fetch_add(&tls_released, 1);
    free(t);
    return OS64_TLS_OK;
}

os64_tls_store_status_t os64_tls_trust_reload(os64_tls_trust **current, os64_tls_store_report_t *report)
{
    (void)report;
    *current = kTrust;
    return OS64_TLS_STORE_OK;
}
void os64_tls_trust_free(os64_tls_trust *store) { (void)store; }
const char *os64_tls_status_name(os64_tls_status_t s) { (void)s; return "tls-status"; }
const char *os64_tls_error_description(os64_tls_status_t s, os64_tls_policy_reason_t p, int e)
{
    (void)s; (void)p; (void)e;
    return "tls-explanation";
}
const char *os64_tls_store_status_name(os64_tls_store_status_t s) { (void)s; return "store-status"; }

// ── A fetch, on a thread of its own ─────────────────────────────────────

typedef struct {
    const char *url;
    os64_fetch_options_t opt;
    // What came of it.
    os64_fetch_status_t status;         // after open
    os64_fetch_status_t ended;          // after the body
    int32_t http_status;
    bool encrypted;
    uint32_t hops;
    size_t body_len;
    char body[4096];
    char heard[2048];                   // "name: value\n" for each field on_header was given
    char reason[512];
    os64_fetch_detail_t detail;
    char cookies[512];                  // "E:value\n" or "P:value\n", by whether the hop was encrypted
    atomic_bool cancel, done;
    bool keep_open;                     // stop after the head; `f` is the case's to read and close
    os64_fetch_t *f;
    pthread_t thread;
} job_t;

static void job_header(void *ctx, const char *name, size_t name_len, const char *value, size_t value_len)
{
    job_t *j = ctx;
    size_t at = strlen(j->heard);
    snprintf(j->heard + at, sizeof(j->heard) - at, "%.*s: %.*s\n", (int)name_len, name, (int)value_len,
             value != NULL ? value : "(too long)");
}

static void job_cookie(void *ctx, const os64_url_t *url, bool encrypted, const char *value, size_t len)
{
    job_t *j = ctx;
    (void)url;
    size_t at = strlen(j->cookies);
    snprintf(j->cookies + at, sizeof(j->cookies) - at, "%c:%.*s\n", encrypted ? 'E' : 'P', (int)len, value);
}

static bool job_cancelled(void *ctx) { return atomic_load(&((job_t *)ctx)->cancel); }

static void job_read(job_t *j)
{
    for (;;) {
        int64_t n = os64_fetch_read(j->f, j->body + j->body_len, sizeof(j->body) - j->body_len);
        if (n <= 0)
            break;
        j->body_len += (size_t)n;
    }
    j->ended = os64_fetch_status(j->f);
    snprintf(j->reason, sizeof(j->reason), "%s", os64_fetch_reason(j->f));
    j->detail = *os64_fetch_detail(j->f);
}

static void *job_run(void *x)
{
    job_t *j = x;
    j->opt.ctx = j;
    j->opt.on_header = job_header;
    j->opt.on_set_cookie = job_cookie;
    j->opt.cancelled = job_cancelled;
    j->opt.no_proxy = true;
    j->f = os64_fetch_open(j->url, &j->opt);
    j->status = os64_fetch_status(j->f);
    const os64_fetch_head_t *head = os64_fetch_head(j->f);
    if (head != NULL) {
        j->http_status = head->status;
        j->encrypted = head->encrypted;
        j->hops = head->hops;
    }
    j->ended = j->status;
    snprintf(j->reason, sizeof(j->reason), "%s", os64_fetch_reason(j->f));
    j->detail = *os64_fetch_detail(j->f);
    if (!j->keep_open) {
        if (j->status == OS64_FETCH_OK)
            job_read(j);
        os64_fetch_close(j->f);
        j->f = NULL;
    }
    atomic_store(&j->done, true);
    return NULL;
}

// A fetch that should have ended whole: when it did not, how it ended.
#define JOB_OK(j)                                                             \
    do {                                                                      \
        if ((j)->ended != OS64_FETCH_OK)                                      \
            fprintf(stderr, "%s:%d: %s ended %s: %s\n", __FILE__, __LINE__, (j)->url,                 \
                    os64_fetch_status_name((j)->ended), (j)->reason);        \
        CHECK((j)->ended == OS64_FETCH_OK);                                   \
    } while (0)

static void job_start(job_t *j, const char *url)
{
    j->url = url;
    pthread_create(&j->thread, NULL, job_run, j);
}
static void job_join(job_t *j) { pthread_join(j->thread, NULL); }
static void job_go(job_t *j, const char *url)
{
    job_start(j, url);
    job_join(j);
}

// ── Between cases ───────────────────────────────────────────────────────

static void begin(void)
{
    nroutes = nhosts = 0;
    memset(hosts, 0, sizeof(hosts));
    atomic_store(&release_held, false);
    atomic_store(&stopping, false);
    atomic_store(&ndials, 0);
    atomic_store(&sockets_most, 0);
    peer_forget_all();
    peer_thread_head_start(0);
}

static int h2_dials(void)
{
    int n = 0;
    for (int i = 0; i < atomic_load(&ndials); i++)
        n += dials[i].alpn != NULL && strcmp(dials[i].alpn, "h2") == 0;
    return n;
}

static int requests_seen(void)
{
    int n = 0;
    for (int i = 0; i < atomic_load(&ndials); i++)
        n += atomic_load(&dials[i].requests);
    return n;
}

// The end of a case: nothing is left. Every kept connection is closed, an
// HTTP/2 one by its reader within a slice; every socket was closed once;
// every engine handed to an HTTP/2 connection was freed, which is every
// hold on it let go; and no record crossed a wire out of order.
static void settle(void)
{
    fetch_pool_drain();
    for (int i = 0; i < 600 && (fetch_pool_closing() != 0 || peer_sockets_open() != 0); i++)
        nap_ms(5);
    if (fetch_pool_closing() != 0 || peer_sockets_open() != 0) {
        // Nothing after this could be trusted, and a connection that never
        // closes makes every later case wait: stop here.
        fprintf(stderr, "test_fetch_h2_host: %zu connections still closing and %u sockets open at a case's end\n",
                fetch_pool_closing(), peer_sockets_open());
        exit(1);
    }
    size_t kept, live;
    fetch_pool_counts(&kept, &live);
    CHECK(kept == 0 && live == 0);
    atomic_store(&stopping, true);
    for (int i = 0; i < atomic_load(&ndials); i++) {
        pthread_join(dials[i].thread, NULL);
        peer_use(dials[i].wire);
        CHECK(peer_closes() == 1);
        if (dials[i].alpn != NULL && strcmp(dials[i].alpn, "h2") == 0) {
            for (int k = 0; k < 400 && !peer_engine_freed(); k++)
                nap_ms(5);
            CHECK(peer_engine_freed() && peer_records_sound());
        }
    }
    CHECK(atomic_load(&sockets_most) <= FETCH_POOL_TOTAL);
    CHECK(host_bell_alive() == 0);
}

// ── Cases ───────────────────────────────────────────────────────────────

static const char kPage[] = "the page, as it was sent\n";

// The same exchange over HTTP/2 and over HTTP/1.1 is the same fetch: the
// head a caller reads, what its observers hear, the bytes.
static void same_as_h1(const char *what, const char *path, const os64_fetch_options_t *opt, int32_t status)
{
    static job_t two[2];
    char url[2][128];
    snprintf(url[0], sizeof(url[0]), "https://h2.same.test%s", path);
    snprintf(url[1], sizeof(url[1]), "https://h1.same.test%s", path);
    for (int i = 0; i < 2; i++) {
        memset(&two[i], 0, sizeof(two[i]));
        if (opt != NULL)
            two[i].opt = *opt;
        job_go(&two[i], url[i]);
    }
    bool same = two[0].status == two[1].status && two[0].ended == two[1].ended &&
                two[0].http_status == two[1].http_status && two[0].body_len == two[1].body_len &&
                memcmp(two[0].body, two[1].body, two[0].body_len) == 0 && two[0].encrypted && two[1].encrypted;
    if (!same || two[0].http_status != status || two[0].ended != OS64_FETCH_OK) {
        fprintf(stderr, "%s: h2 %d/%d status %d, %zu bytes (%s); h1 %d/%d status %d, %zu bytes (%s)\n", what,
                (int)two[0].status, (int)two[0].ended, (int)two[0].http_status, two[0].body_len, two[0].reason,
                (int)two[1].status, (int)two[1].ended, (int)two[1].http_status, two[1].body_len, two[1].reason);
        CHECK(false);
    }
    // What the observers heard: every field the route sent, in order, on
    // both (the h1 text adds a Content-Length of its own when the route
    // gave none, which is the one line h2 need not carry).
    route_t *r = route_for(path, strlen(path));
    for (int i = 0; r != NULL && i < 8 && r->fields[i][0] != NULL; i++) {
        char want[256];
        snprintf(want, sizeof(want), "%s: %s\n", r->fields[i][0], r->fields[i][1]);
        CHECK(strcasestr(two[0].heard, want) != NULL && strcasestr(two[1].heard, want) != NULL);
    }
    CHECK(strcasestr(two[0].heard, ":status") == NULL);
    CHECK(two[0].detail.tls_version == 0x0304);
}

static void basic_cases(void)
{
    begin();
    host_says("h2.same.test", "h2", 0);
    host_says("h1.same.test", "http/1.1", 0);
    route_t *r = route("/page", R_REPLY, "200", kPage);
    r->fields[0][0] = "content-type";
    r->fields[0][1] = "text/html; charset=utf-8";
    r->fields[1][0] = "x-custom";
    r->fields[1][1] = "one value";
    // A name longer than any the fetch itself reads is an observer's still.
    r->fields[2][0] = "x-a-field-name-of-seventy-bytes-which-is-longer-than-any-name-this-reads";
    r->fields[2][1] = "heard";
    r = route("/gzip", R_REPLY, "200", NULL);
    r->body = GZ_HELLO;
    r->body_len = sizeof(GZ_HELLO);
    r->fields[0][0] = "content-encoding";
    r->fields[0][1] = "gzip";
    r = route("/empty", R_REPLY, "204", NULL);
    r = route("/head", R_REPLY, "200", kPage);
    r = route("/post", R_ECHO, "200", NULL);

    same_as_h1("a GET", "/page", NULL, 200);
    same_as_h1("a gzip body", "/gzip", NULL, 200);
    same_as_h1("a 204", "/empty", NULL, 204);
    os64_fetch_options_t head = {.method_name = "HEAD"};
    same_as_h1("a HEAD", "/head", &head, 200);
    os64_fetch_options_t post = {.method = OS64_FETCH_METHOD_POST, .body = "name=value", .body_len = 10,
                                 .content_type = "application/x-www-form-urlencoded"};
    same_as_h1("a POST", "/post", &post, 200);
    // (The server echoes what it was sent, so the upload is what is compared.)
    static job_t posted;
    memset(&posted, 0, sizeof(posted));
    posted.opt = post;
    job_go(&posted, "https://h2.same.test/post");
    CHECK(posted.ended == OS64_FETCH_OK && posted.body_len == 10 && memcmp(posted.body, "name=value", 10) == 0);
    same_as_h1("a path nobody serves", "/nowhere", NULL, 404);

    // One HTTP/2 connection carried all six of its host's.
    CHECK(h2_dials() == 1 && atomic_load(&dials[0].requests) == 7 && requests_seen() == 13);
    size_t kept, live;
    fetch_pool_counts(&kept, &live);
    CHECK(kept >= 2 && live == 0);
    settle();
    CHECK(atomic_load(&tls_released) == 1);
}

// ── Sharing ─────────────────────────────────────────────────────────────

#define MANY 6
static void sharing_cases(void)
{
    static job_t jobs[2 * MANY];
    // SIX AT ONCE TO A KEY WHOSE CONNECTION IS LIVE: no dial, six streams.
    begin();
    route("/warm", R_REPLY, "200", "warm");
    route("/held", R_HELD, "200", NULL);
    job_t warm = {0};
    job_go(&warm, "https://share.test/warm");
    CHECK(warm.ended == OS64_FETCH_OK && atomic_load(&ndials) == 1);
    memset(jobs, 0, sizeof(jobs));
    for (int i = 0; i < MANY; i++) {
        jobs[i].keep_open = true;
        job_start(&jobs[i], "https://share.test/held");
    }
    for (int i = 0; i < MANY; i++)
        job_join(&jobs[i]);
    size_t kept, live;
    fetch_pool_counts(&kept, &live);
    CHECK(atomic_load(&ndials) == 1 && kept == 0 && live == 1);
    atomic_store(&release_held, true);
    for (int i = 0; i < MANY; i++) {
        CHECK(jobs[i].status == OS64_FETCH_OK && jobs[i].detail.tls_version == 0x0304);
        job_read(&jobs[i]);
        CHECK(jobs[i].ended == OS64_FETCH_OK && jobs[i].body_len == 9 && memcmp(jobs[i].body, "held body", 9) == 0);
        os64_fetch_close(jobs[i].f);
    }
    fetch_pool_counts(&kept, &live);
    CHECK(kept == 1 && live == 0 && atomic_load(&dials[0].requests) == 1 + MANY);
    settle();

    // THE FIRST VISIT'S BURST dials for each fetch that finds no connection
    // (three here, as a browser's does without a stored property). Once the
    // key has answered h2 it is remembered: with its connections gone and a
    // dial in flight, three more wait for that dial and dial nothing.
    begin();
    int slow = host_says("burst.test", "h2", 0);
    hosts[slow].handshake_ms = 150;     // every fetch asks before any dial has an answer
    route("/held", R_HELD, "200", NULL);
    memset(jobs, 0, sizeof(jobs));
    for (int i = 0; i < 3; i++) {
        jobs[i].keep_open = true;
        job_start(&jobs[i], "https://burst.test/held");
    }
    for (int i = 0; i < 3; i++)
        job_join(&jobs[i]);
    int first = atomic_load(&ndials);
    CHECK(first == 3 && h2_dials() == 3);
    atomic_store(&release_held, true);
    for (int i = 0; i < 3; i++) {
        job_read(&jobs[i]);
        JOB_OK(&jobs[i]);
        os64_fetch_close(jobs[i].f);
    }
    settle();
    // (The pool's memory of the key outlives its connections.)
    begin();
    slow = host_says("burst.test", "h2", 0);
    hosts[slow].handshake_ms = 150;
    route("/held", R_HELD, "200", NULL);
    memset(jobs, 0, sizeof(jobs));
    for (int i = 0; i < 3; i++) {
        jobs[i].keep_open = true;
        job_start(&jobs[i], "https://burst.test/held");
        if (i == 0)
            for (int k = 0; k < 400 && atomic_load(&ndials) == 0; k++)
                nap_ms(1);      // the first's dial is in flight before the others ask
    }
    for (int i = 0; i < 3; i++)
        job_join(&jobs[i]);
    CHECK(atomic_load(&ndials) == 1 && atomic_load(&dials[0].requests) == 3);
    atomic_store(&release_held, true);
    for (int i = 0; i < 3; i++) {
        job_read(&jobs[i]);
        JOB_OK(&jobs[i]);
        os64_fetch_close(jobs[i].f);
    }
    settle();

    // A KEY REMEMBERED AS h2 THAT ANSWERS HTTP/1.1 THIS TIME: the fetches
    // that waited on the dial's verdict dial for themselves.
    begin();
    host_says("burst.test", "http/1.1", 0);
    route("/page", R_REPLY, "200", kPage);
    memset(jobs, 0, sizeof(jobs));
    for (int i = 0; i < 3; i++)
        job_start(&jobs[i], "https://burst.test/page");
    for (int i = 0; i < 3; i++) {
        job_join(&jobs[i]);
        CHECK(jobs[i].ended == OS64_FETCH_OK && jobs[i].body_len == sizeof(kPage) - 1);
    }
    CHECK(h2_dials() == 0 && atomic_load(&ndials) >= 1 && requests_seen() == 3);
    settle();

    // THE SERVER'S LIMIT IS ONE STREAM. A second fetch finds the connection
    // without room: it dials another, and never spins. With the first gone,
    // the next shares again.
    begin();
    host_says("one.test", "h2", 1);
    route("/held", R_HELD, "200", NULL);
    route("/page", R_REPLY, "200", kPage);
    memset(jobs, 0, sizeof(jobs));
    jobs[0].keep_open = true;
    job_go(&jobs[0], "https://one.test/held");
    // (Its server's SETTINGS have been applied once its head has come.)
    nap_ms(50);
    job_go(&jobs[1], "https://one.test/page");
    CHECK(jobs[1].ended == OS64_FETCH_OK && atomic_load(&ndials) == 2);
    atomic_store(&release_held, true);
    job_read(&jobs[0]);
    CHECK(jobs[0].ended == OS64_FETCH_OK);
    os64_fetch_close(jobs[0].f);
    job_go(&jobs[2], "https://one.test/page");
    CHECK(jobs[2].ended == OS64_FETCH_OK && atomic_load(&ndials) == 2);
    settle();
}

// ── What is tried again, and what each ending is called ─────────────────

static void retry_cases(void)
{
    static job_t j, k;
    begin();
    route("/warm", R_REPLY, "200", "warm");
    route("/refused-once", R_REFUSE_ONCE, "200", "second time");
    route("/refused", R_REFUSE, "200", NULL);
    route("/unprocessed", R_UNPROCESSED_ONCE, "200", "after the goaway");
    route("/die-once", R_DIE_ONCE, "200", "after the death");
    route("/reset-mid", R_RESET_MID_BODY, "200", "0123456789");
    route("/die-mid", R_HEAD_THEN_DIE, "200", "0123456789");
    route("/half", R_HALF_HEAD, "200", NULL);
    os64_fetch_options_t post = {.method = OS64_FETCH_METHOD_POST, .body = "x", .body_len = 1};

    // REFUSED_STREAM before any answer: once more, whatever the method, and
    // on the same connection, which refused one stream and no more.
    memset(&j, 0, sizeof(j));
    j.opt = post;
    job_go(&j, "https://retry.test/refused-once");
    CHECK(j.ended == OS64_FETCH_OK && j.body_len == 11 && memcmp(j.body, "second time", 11) == 0);
    // (Said without "on a fresh connection": the second try rode the
    // connection that refused the first.)
    CHECK(strstr(j.reason, "(retried once)") != NULL);
    CHECK(atomic_load(&ndials) == 1 && atomic_load(&dials[0].requests) == 2);
    // Refused twice is the caller's, in the server's own word.
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://retry.test/refused");
    CHECK(j.status == OS64_FETCH_REQUEST_FAILED && j.detail.h2 && j.detail.h2_coded &&
          j.detail.h2_code == H2_ERR_REFUSED_STREAM && !j.detail.h2_code_ours && j.detail.h2_stream != 0);
    CHECK(strstr(j.reason, "the server reset the stream: REFUSED_STREAM") != NULL &&
          strstr(j.reason, "retried once") != NULL);
    CHECK(atomic_load(&ndials) == 1);
    settle();

    // A GOAWAY THAT STOPS BELOW THE STREAM never processed it: once more on
    // a connection dialled for it, a POST among them.
    begin();
    route("/warm", R_REPLY, "200", "warm");
    route("/unprocessed", R_UNPROCESSED_ONCE, "200", "after the goaway");
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://goaway.test/warm");
    memset(&j, 0, sizeof(j));
    j.opt = post;
    job_go(&j, "https://goaway.test/unprocessed");
    CHECK(j.ended == OS64_FETCH_OK && j.body_len == 16 && memcmp(j.body, "after the goaway", 16) == 0);
    CHECK(atomic_load(&ndials) == 2 && strstr(j.reason, "retried once") != NULL);
    settle();

    // THE CONNECTION DIES UNDER A REQUEST IT WAS NOT DIALLED FOR, before any
    // answer: an idempotent one goes once more; a POST is the caller's.
    begin();
    route("/warm", R_REPLY, "200", "warm");
    route("/die-once", R_DIE_ONCE, "200", "after the death");
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://death.test/warm");
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://death.test/die-once");
    CHECK(j.ended == OS64_FETCH_OK && j.body_len == 15 && atomic_load(&ndials) == 2);
    CHECK(strstr(j.reason, "retried once") != NULL);
    settle();
    begin();
    route("/warm", R_REPLY, "200", "warm");
    route("/die", R_DIE, "200", NULL);
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://death2.test/warm");
    memset(&j, 0, sizeof(j));
    j.opt = post;
    job_go(&j, "https://death2.test/die");
    CHECK(j.status == OS64_FETCH_REQUEST_FAILED && atomic_load(&ndials) == 1);
    CHECK(strstr(j.reason, "the connection ended under the request") != NULL && strstr(j.reason, "retried") == NULL);
    CHECK(j.detail.h2 && !j.detail.h2_coded);
    // And one that dies under the request it was dialled for is the
    // caller's too, idempotent or not: nothing says the next would differ.
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://death2.test/die");
    CHECK(j.status == OS64_FETCH_REQUEST_FAILED && atomic_load(&ndials) == 2 && strstr(j.reason, "retried") == NULL);
    settle();

    // MID-BODY: a reset and a death are each BROKE, with what was read
    // before them and the reason in the sentence's tail.
    begin();
    route("/reset-mid", R_RESET_MID_BODY, "200", "0123456789");
    route("/die-mid", R_HEAD_THEN_DIE, "200", "0123456789");
    route("/half", R_HALF_HEAD, "200", NULL);
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://mid.test/reset-mid");
    CHECK(j.status == OS64_FETCH_OK && j.ended == OS64_FETCH_BROKE && j.body_len == 5);
    CHECK(strstr(j.reason, "(HTTP/2: the server reset the stream: INTERNAL_ERROR)") != NULL);
    CHECK(j.detail.h2_coded && j.detail.h2_code == H2_ERR_INTERNAL && !j.detail.h2_code_ours);
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://mid.test/die-mid");
    CHECK(j.status == OS64_FETCH_OK && j.ended == OS64_FETCH_BROKE && j.body_len == 5);
    CHECK(strstr(j.reason, "(HTTP/2: the connection ended under the request)") != NULL);
    // A head that began and never finished is an answer begun: the request
    // is not sent again, though it is idempotent and joined a connection.
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://mid.test/reset-mid");
    memset(&k, 0, sizeof(k));
    job_go(&k, "https://mid.test/half");
    CHECK(k.status == OS64_FETCH_REQUEST_FAILED && strstr(k.reason, "retried") == NULL);
    settle();

    // A RESET THAT IS NOT A REFUSAL is not "never processed": the caller's,
    // though nothing of an answer came and the request is idempotent.
    begin();
    route("/warm", R_REPLY, "200", "warm");
    route("/cancelled", R_CANCEL, "200", NULL);
    route("/huge", R_HUGE_HEAD, "200", NULL);
    route_t *brief = route("/short", R_REPLY, "200", "12345");
    brief->fields[0][0] = "content-length";
    brief->fields[0][1] = "10";
    route("/open204", R_OPEN, "204", NULL);
    route("/held", R_HELD, "200", NULL);
    route_t *late = route("/408", R_REPLY, "408", "too slow");
    late->fields[0][0] = "x-seen";
    late->fields[0][1] = "yes";
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://words.test/warm");
    // A 408 ON A JOINED CONNECTION is an answer like any other: HTTP/1.1's
    // "the server timed the idle connection out" has no meaning on a
    // connection whose reader has watched it the whole time.
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://words.test/408");
    CHECK(j.ended == OS64_FETCH_OK && j.http_status == 408 && strstr(j.heard, "x-seen: yes") != NULL &&
          strstr(j.reason, "retried") == NULL && j.body_len == 8);
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://words.test/cancelled");
    CHECK(j.status == OS64_FETCH_REQUEST_FAILED && strstr(j.reason, "the server reset the stream: CANCEL") != NULL &&
          strstr(j.reason, "retried") == NULL && atomic_load(&ndials) == 1);
    // A HEAD LARGER THAN THIS READS is a legal thing refused, by the word
    // HTTP/1.1's over-long framing header has.
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://words.test/huge");
    CHECK(j.status == OS64_FETCH_UNSUPPORTED && strcmp(j.detail.unsupported, "head") == 0);
    CHECK(j.detail.h2 && j.detail.h2_code_ours && j.detail.h2_code == H2_ERR_ENHANCE_YOUR_CALM);
    CHECK(strstr(j.reason, "too long to read") != NULL && strstr(j.reason, "(HTTP/2: we reset the stream") != NULL);
    // A BODY SHORTER THAN ITS LENGTH is CUT, as HTTP/1.1's is.
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://words.test/short");
    CHECK(j.status == OS64_FETCH_OK && j.ended == OS64_FETCH_CUT);
    CHECK(strstr(j.reason, "body is not what its head promised") != NULL && j.detail.h2_code_ours);
    // A 204 WHOSE STREAM THE SERVER LEAVES OPEN is whole all the same: no
    // content may follow it, and none is waited for.
    memset(&j, 0, sizeof(j));
    j.opt.idle_ms = 400;
    job_go(&j, "https://words.test/open204");
    CHECK(j.ended == OS64_FETCH_OK && j.http_status == 204 && j.body_len == 0);
    // WHAT AN EARLIER TRY LEFT IS NOT THE LATER ONE'S: refused once, the
    // request goes again and its body then goes silent. The sentence and
    // the detail say the silence, and nothing of the refusal but that it
    // was retried.
    route("/refused-then-silent", R_REFUSE_ONCE_HELD, "200", NULL);
    memset(&j, 0, sizeof(j));
    j.opt.idle_ms = 300;
    job_go(&j, "https://words.test/refused-then-silent");
    CHECK(j.status == OS64_FETCH_OK && j.ended == OS64_FETCH_SILENT);
    CHECK(strstr(j.reason, "REFUSED_STREAM") == NULL && strstr(j.reason, "(retried once)") != NULL);
    CHECK(j.detail.h2 && !j.detail.h2_coded && j.detail.h2_code == 0);
    // AN ANSWER BEFORE THE UPLOAD IS DONE: a 413 to a body larger than the
    // server's window, whole, with no reset and no window given. The fetch
    // has its answer and does not wait out the idle limit for window.
    route("/early413", R_REPLY, "413", "too large");
    static char upload[100000];
    memset(upload, 'u', sizeof(upload));
    memset(&j, 0, sizeof(j));
    j.opt = (os64_fetch_options_t){.method = OS64_FETCH_METHOD_POST, .body = upload, .body_len = sizeof(upload),
                                   .idle_ms = 2000};
    job_go(&j, "https://words.test/early413");
    CHECK(j.ended == OS64_FETCH_OK && j.http_status == 413 && j.body_len == 9 && memcmp(j.body, "too large", 9) == 0);
    // SILENT MID-BODY: the head came, the body never does, and the fetch
    // says how long it waited and where.
    memset(&j, 0, sizeof(j));
    j.opt.idle_ms = 300;
    job_go(&j, "https://words.test/held");
    CHECK(j.status == OS64_FETCH_OK && j.ended == OS64_FETCH_SILENT && j.detail.silent_after_body);
    CHECK(strstr(j.reason, "went silent for 300 ms after 0 bytes") != NULL);
    // (Two dials: the silence before this one left its connection quiet,
    // and this fetch dialled past it.)
    CHECK(atomic_load(&ndials) == 2);
    settle();
}

// ── Leaving a stream ────────────────────────────────────────────────────

static os64_fetch_verdict_t follow(void *ctx, const os64_fetch_hop_t *hop)
{
    (void)ctx;
    (void)hop;
    return OS64_FETCH_HOP_FOLLOW;
}

static void leaving_cases(void)
{
    static job_t j, k;
    begin();
    route("/page", R_REPLY, "200", kPage);
    route("/held", R_HELD, "200", NULL);
    // (Its stream is left open after its bytes, so that a fetch which leaves
    // it has left a stream neither side finished, however fast the bytes
    // came: that is what makes the resets below certain.)
    route_t *big = route("/big", R_UNENDED, "200", NULL);
    static uint8_t lots[60000];
    memset(lots, 'b', sizeof(lots));
    big->body = lots;
    big->body_len = sizeof(lots);

    // THE BODY CAP: the fetch ends LIMIT at the byte it names, its stream is
    // reset, and the connection carries the next fetch.
    memset(&j, 0, sizeof(j));
    j.opt.max_body = 1000;
    job_go(&j, "https://leave.test/big");
    CHECK(j.ended == OS64_FETCH_LIMIT && j.body_len == 1000);
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://leave.test/page");
    CHECK(j.ended == OS64_FETCH_OK && atomic_load(&ndials) == 1);

    // CLOSED WITH THE BODY UNREAD, and CANCELLED WHILE IT WAITS for one:
    // each resets its stream and no more.
    memset(&j, 0, sizeof(j));
    j.keep_open = true;
    job_go(&j, "https://leave.test/big");
    CHECK(j.status == OS64_FETCH_OK);
    os64_fetch_close(j.f);
    memset(&j, 0, sizeof(j));
    j.keep_open = true;
    job_go(&j, "https://leave.test/held");
    CHECK(j.status == OS64_FETCH_OK);
    atomic_store(&j.cancel, true);
    job_read(&j);
    CHECK(j.ended == OS64_FETCH_INTERRUPTED);
    os64_fetch_close(j.f);
    memset(&k, 0, sizeof(k));
    job_go(&k, "https://leave.test/page");
    CHECK(k.ended == OS64_FETCH_OK && atomic_load(&ndials) == 1);
    for (int i = 0; i < 400 && atomic_load(&dials[0].resets) < 3; i++)
        nap_ms(5);
    CHECK(atomic_load(&dials[0].resets) == 3);
    size_t kept, live;
    fetch_pool_counts(&kept, &live);
    CHECK(kept == 1 && live == 0);
    settle();

    // A REDIRECT FROM https OVER HTTP/2 TO plain http AND BACK: each hop's
    // stream or connection is left as its protocol leaves one, and the head
    // says which hop was encrypted. A Secure cookie's fate follows the hop,
    // for the fetch that dialled and for one that joined.
    begin();
    route_t *r = route("/start", R_REPLY, "302", NULL);
    r->fields[0][0] = "location";
    r->fields[0][1] = "http://plain.test/middle";
    r->fields[1][0] = "set-cookie";
    r->fields[1][1] = "first=1; Secure";
    r = route("/middle", R_REPLY, "302", NULL);
    r->fields[0][0] = "location";
    r->fields[0][1] = "https://cookie.test/end";
    r->fields[1][0] = "set-cookie";
    r->fields[1][1] = "second=2; Secure";
    r = route("/end", R_REPLY, "200", "the end");
    r->fields[0][0] = "set-cookie";
    r->fields[0][1] = "third=3; Secure";
    for (int pass = 0; pass < 2; pass++) {
        memset(&j, 0, sizeof(j));
        j.opt.on_hop = follow;
        job_go(&j, "https://cookie.test/start");
        CHECK(j.ended == OS64_FETCH_OK && j.hops == 2 && j.encrypted && j.body_len == 7);
        // E: the hop was encrypted, and a Secure cookie may be kept.
        CHECK(strcmp(j.cookies, "E:first=1; Secure\nP:second=2; Secure\nE:third=3; Secure\n") == 0);
    }
    // One HTTP/2 connection for the four https hops; the second pass joined it.
    CHECK(h2_dials() == 1);
    for (int i = 0; i < atomic_load(&ndials); i++)
        if (dials[i].alpn != NULL)
            CHECK(atomic_load(&dials[i].requests) == 4);
    settle();
}

// ── The pool's edges ────────────────────────────────────────────────────

static void pool_cases(void)
{
    static job_t j, k;
    static job_t many[FETCH_POOL_TOTAL + 4];

    // THE DIALLER'S CONNECTION DIES BEFORE ITS ADOPTION RETURNS: the server
    // hangs up as the handshake ends, and the new reader is given a head
    // start, so its last word comes first. Nothing is freed under the
    // dialler: it is refused a stream, gives the connection back, and dials
    // again.
    begin();
    int at = host_says("early.test", "h2", 0);
    atomic_store(&hosts[at].hang_up_at_handshake, 1);
    route("/page", R_REPLY, "200", kPage);
    peer_thread_head_start(150);
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://early.test/page");
    peer_thread_head_start(0);
    CHECK(j.ended == OS64_FETCH_OK && atomic_load(&ndials) == 2 && atomic_load(&dials[1].requests) == 1);
    settle();

    // A TAKE WHILE A CONNECTION IS BEING ADOPTED: the dialler holds it, its
    // reader is starting (held back here), and it is nobody else's yet. A
    // second fetch to a key not yet known to speak h2 dials its own.
    begin();
    route("/page", R_REPLY, "200", kPage);
    peer_thread_head_start(200);
    memset(&j, 0, sizeof(j));
    memset(&k, 0, sizeof(k));
    job_start(&j, "https://adopting.test/page");
    nap_ms(70);
    job_start(&k, "https://adopting.test/page");
    job_join(&j);
    job_join(&k);
    peer_thread_head_start(0);
    JOB_OK(&j);
    JOB_OK(&k);
    CHECK(atomic_load(&ndials) == 2);
    settle();

    // A SERVER WHOSE EVERY NEW CONNECTION ENDS AS IT BEGINS: the dialler is
    // refused a stream on the connection it dialled, dials once more, and
    // the second such refusal is the answer. It does not dial for ever.
    begin();
    at = host_says("never.test", "h2", 0);
    atomic_store(&hosts[at].hang_up_at_handshake, 8);
    route("/page", R_REPLY, "200", kPage);
    peer_thread_head_start(150);
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://never.test/page");
    peer_thread_head_start(0);
    CHECK(j.status == OS64_FETCH_REQUEST_FAILED && atomic_load(&ndials) == 2 && j.detail.h2);
    CHECK(strstr(j.reason, "took no request, twice") != NULL);
    settle();

    // A QUIET CONNECTION: a stream goes silent on a connection that said
    // nothing, and the next fetch dials past it.
    begin();
    route("/silent", R_SILENT, "200", NULL);
    route("/page", R_REPLY, "200", kPage);
    memset(&j, 0, sizeof(j));
    j.opt.idle_ms = 300;
    job_go(&j, "https://quiet.test/page");
    CHECK(j.ended == OS64_FETCH_OK);
    nap_ms(100);        // the server's SETTINGS and the reply are behind it: the connection is quiet now
    memset(&j, 0, sizeof(j));
    j.opt.idle_ms = 300;
    job_go(&j, "https://quiet.test/silent");
    CHECK(j.status == OS64_FETCH_SILENT && strstr(j.reason, "went silent for 300 ms before the reply was whole") != NULL);
    memset(&k, 0, sizeof(k));
    job_go(&k, "https://quiet.test/page");
    CHECK(k.ended == OS64_FETCH_OK && atomic_load(&ndials) == 2);
    settle();

    // A TAKE AGAINST THE IDLE CLOSE: with the clock past the idle time,
    // either the take finds the connection and it lives, or its reader has
    // struck it and the take dials. Never a stream on a closed connection.
    begin();
    route("/page", R_REPLY, "200", kPage);
    for (int round = 0; round < 6; round++) {
        memset(&j, 0, sizeof(j));
        job_go(&j, "https://idle.test/page");
        CHECK(j.ended == OS64_FETCH_OK);
        peer_clock_skip(FETCH_POOL_IDLE_MS + 1);
        nap_ms(round * 110);    // the reader looks every half second: before, at and after its look
    }
    CHECK(atomic_load(&ndials) >= 2 && requests_seen() == 6);
    settle();

    // IDLE TIME IS FOR A CONNECTION WITH NO FETCH ON IT: with one held open
    // past the idle time, the connection stays, and the next fetch joins it.
    begin();
    route("/held", R_HELD, "200", NULL);
    route("/page", R_REPLY, "200", kPage);
    memset(&j, 0, sizeof(j));
    j.keep_open = true;
    job_go(&j, "https://busy.test/held");
    peer_clock_skip(FETCH_POOL_IDLE_MS + 1);
    nap_ms(2 * H2_READER_SLICE_MS + 100);
    memset(&k, 0, sizeof(k));
    job_go(&k, "https://busy.test/page");
    CHECK(k.ended == OS64_FETCH_OK && atomic_load(&ndials) == 1);
    atomic_store(&release_held, true);
    job_read(&j);
    CHECK(j.ended == OS64_FETCH_OK);
    os64_fetch_close(j.f);
    settle();

    // EVICTION AFTER EVICTION, EVERY CLOSE HELD BACK A SLICE: the table is
    // full of idle HTTP/2 connections and more hosts are fetched. Each
    // evicts one, and each waits for a reader to close before it dials, so
    // the sockets open never pass the table's total (settle checks).
    begin();
    route("/page", R_REPLY, "200", kPage);
    char urls[FETCH_POOL_TOTAL + 4][64];
    memset(many, 0, sizeof(many));
    for (int i = 0; i < FETCH_POOL_TOTAL; i++) {
        snprintf(urls[i], sizeof(urls[i]), "https://full%d.test/page", i);
        job_go(&many[i], urls[i]);
        CHECK(many[i].ended == OS64_FETCH_OK);
    }
    size_t kept, live;
    fetch_pool_counts(&kept, &live);
    CHECK(kept == FETCH_POOL_TOTAL && peer_sockets_open() == FETCH_POOL_TOTAL);
    for (int i = FETCH_POOL_TOTAL; i < FETCH_POOL_TOTAL + 4; i++) {
        snprintf(urls[i], sizeof(urls[i]), "https://full%d.test/page", i);
        job_start(&many[i], urls[i]);
    }
    for (int i = FETCH_POOL_TOTAL; i < FETCH_POOL_TOTAL + 4; i++) {
        job_join(&many[i]);
        CHECK(many[i].ended == OS64_FETCH_OK);
    }
    CHECK(atomic_load(&ndials) == FETCH_POOL_TOTAL + 4 && atomic_load(&sockets_most) == FETCH_POOL_TOTAL);
    settle();

    // FREED BY THE LAST OF THREE: the connection is struck, its last fetch
    // leaves, and its reader says its last word, in each order a fetch can
    // meet. (settle's engine check is the proof it was freed, and ASan's
    // that it was freed once.)
    begin();
    route("/held", R_HELD, "200", NULL);
    route("/die-mid", R_HEAD_THEN_DIE, "200", "0123456789");
    // The reader's word first (the server ends it), the fetch's put last.
    memset(&j, 0, sizeof(j));
    j.keep_open = true;
    job_go(&j, "https://last.test/die-mid");
    CHECK(j.status == OS64_FETCH_OK);
    for (int i = 0; i < 400 && peer_sockets_open() != 0; i++)
        nap_ms(5);
    CHECK(peer_sockets_open() == 0);
    job_read(&j);
    CHECK(j.ended == OS64_FETCH_BROKE);
    os64_fetch_close(j.f);
    // The fetch's put first, then the drain strikes it, the reader's word last.
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://last2.test/held");
    settle();
    // Struck by the drain with a fetch still on it is not possible (a drain
    // leaves a connection in use alone); struck by its own death with one
    // on it was the first; and the idle close strikes it with none.
    begin();
    route("/page", R_REPLY, "200", kPage);
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://last3.test/page");
    peer_clock_skip(FETCH_POOL_IDLE_MS + 1);
    for (int i = 0; i < 400 && peer_sockets_open() != 0; i++)
        nap_ms(5);
    CHECK(peer_sockets_open() == 0);
    fetch_pool_counts(&kept, &live);
    CHECK(kept == 0 && live == 0);
    settle();
}

// With the offer off, every one of these hosts is dialled as HTTP/1.1 and
// nothing of HTTP/2's is touched.
static void offer_off_cases(void)
{
    static job_t j[3];
    begin();
    route("/page", R_REPLY, "200", kPage);
    fetch_h2_offer = false;
    int released = atomic_load(&tls_released);
    memset(j, 0, sizeof(j));
    for (int i = 0; i < 3; i++) {
        job_go(&j[i], "https://off.test/page");
        CHECK(j[i].ended == OS64_FETCH_OK && j[i].body_len == sizeof(kPage) - 1 && !j[i].detail.h2);
    }
    CHECK(h2_dials() == 0 && atomic_load(&tls_released) == released && requests_seen() == 3);
    fetch_h2_offer = true;
    settle();
}

// ── What the fetches cost the wire (os64_fetch_counts) ──────────────────

// What the counters have moved by since `then`, which is brought up to now.
static os64_fetch_counts_t since(os64_fetch_counts_t *then)
{
    os64_fetch_counts_t now, by;
    os64_fetch_counts(&now);
    by = (os64_fetch_counts_t){
        .connections_h1 = now.connections_h1 - then->connections_h1,
        .connections_h2 = now.connections_h2 - then->connections_h2,
        .handshakes = now.handshakes - then->handshakes,
        .handshakes_resumed = now.handshakes_resumed - then->handshakes_resumed,
        .requests_h1 = now.requests_h1 - then->requests_h1,
        .requests_h2 = now.requests_h2 - then->requests_h2,
        .waits = now.waits - then->waits,
        .wait_ms = now.wait_ms - then->wait_ms,
    };
    *then = now;
    return by;
}

static bool counted(os64_fetch_counts_t by, uint64_t c1, uint64_t c2, uint64_t hs, uint64_t resumed, uint64_t r1,
                    uint64_t r2, uint64_t waits)
{
    bool same = by.connections_h1 == c1 && by.connections_h2 == c2 && by.handshakes == hs &&
                by.handshakes_resumed == resumed && by.requests_h1 == r1 && by.requests_h2 == r2 && by.waits == waits;
    if (!same)
        fprintf(stderr, "counts moved by: connections %lu h1 %lu h2, handshakes %lu (%lu resumed), requests %lu h1 "
                "%lu h2, %lu waits of %lu ms\n", (unsigned long)by.connections_h1, (unsigned long)by.connections_h2,
                (unsigned long)by.handshakes, (unsigned long)by.handshakes_resumed, (unsigned long)by.requests_h1,
                (unsigned long)by.requests_h2, (unsigned long)by.waits, (unsigned long)by.wait_ms);
    return same && (waits != 0) == (by.wait_ms != 0);
}

static void counts_cases(void)
{
    static job_t j, held, second;
    os64_fetch_counts_t then;
    os64_fetch_counts(&then);

    // HTTP/2: the first fetch makes a connection (one handshake) and sends
    // a request; the second sends a request and makes nothing.
    begin();
    route("/page", R_REPLY, "200", kPage);
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://count2.test/page");
    JOB_OK(&j);
    CHECK(counted(since(&then), 0, 1, 1, 0, 0, 1, 0));
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://count2.test/page");
    JOB_OK(&j);
    CHECK(counted(since(&then), 0, 0, 0, 0, 0, 1, 0));
    settle();

    // HTTP/1.1 under TLS, on a host whose handshakes resume: the same, by
    // its own counters, and the handshake is counted as resumed. A plain
    // http fetch makes a connection and no handshake.
    begin();
    int h1 = host_says("count1.test", "http/1.1", 0);
    hosts[h1].resumes = true;
    route("/page", R_REPLY, "200", kPage);
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://count1.test/page");
    JOB_OK(&j);
    CHECK(counted(since(&then), 1, 0, 1, 1, 1, 0, 0));
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://count1.test/page");
    JOB_OK(&j);
    CHECK(counted(since(&then), 0, 0, 0, 0, 1, 0, 0));
    memset(&j, 0, sizeof(j));
    job_go(&j, "http://plain.test/page");
    JOB_OK(&j);
    CHECK(counted(since(&then), 1, 0, 0, 0, 1, 0, 0));
    settle();

    // A FETCH THAT WAITS: the host is remembered as HTTP/2 and its
    // handshake is slow, so a second fetch waits for the first's dial. One
    // connection, two requests, one fetch that waited, and for some time.
    begin();
    int slow = host_says("countw.test", "h2", 0);
    route("/page", R_REPLY, "200", kPage);
    route("/held", R_HELD, "200", NULL);
    memset(&j, 0, sizeof(j));
    job_go(&j, "https://countw.test/page");
    JOB_OK(&j);
    fetch_pool_drain();
    for (int i = 0; i < 600 && fetch_pool_closing() != 0; i++)
        nap_ms(5);
    since(&then);
    hosts[slow].handshake_ms = 200;
    memset(&held, 0, sizeof(held));
    memset(&second, 0, sizeof(second));
    held.keep_open = true;
    job_start(&held, "https://countw.test/held");
    for (int i = 0; i < 200 && atomic_load(&ndials) < 2; i++)
        nap_ms(2);
    job_go(&second, "https://countw.test/page");
    job_join(&held);
    JOB_OK(&second);
    CHECK(counted(since(&then), 0, 1, 1, 0, 0, 2, 1));
    atomic_store(&release_held, true);
    job_read(&held);
    CHECK(held.ended == OS64_FETCH_OK);
    os64_fetch_close(held.f);
    settle();
}

int main(void)
{
    alarm(SECONDS_MOST);
    fetch_h2_offer = true;
    basic_cases();
    sharing_cases();
    retry_cases();
    leaving_cases();
    pool_cases();
    counts_cases();
    offer_off_cases();
    if (atomic_load(&failures) != 0) {
        fprintf(stderr, "test_fetch_h2_host: %d of %d checks failed\n", atomic_load(&failures), atomic_load(&checks));
        return 1;
    }
    printf("test_fetch_h2_host: %d checks passed\n", atomic_load(&checks));
    return 0;
}
