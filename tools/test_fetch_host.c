// test_fetch_host.c — libfetch's driver against a SCRIPTED PEER.
//
// The seam is libos64's syscall wrappers and libtls's transport calls: every
// one the library reaches the wire through is stubbed here, so fetch.c,
// http.c, transport.c and proxy.c run UNMODIFIED against a table of replies
// keyed by address. That is what lets the harness produce on demand what a
// real network cannot be asked for — a three-hop chain, a self-loop, a
// downgrade, a body cut mid-chunk, a peer that goes silent, a cancellation
// between two reads — and deliver every reply one byte per read and in
// random sizes, because a stream parser's bugs live where a token straddles
// two reads. LIBFETCH.md § Proof before integration.

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fetch/fetch.h"
#include "fetch/transport.h"
#include "os64/os64.h"
#include "pool.h"
#include "fixtures.h"

// ── The heap, with one injectable failure ───────────────────────────────

static bool fail_next_malloc;
void *os64_malloc(size_t size)
{
    if (fail_next_malloc) { fail_next_malloc = false; return NULL; }
    return malloc(size);
}
void os64_free(void *p) { free(p); }
int64_t os64_write(int32_t h, const void *buf, size_t len) { (void)h; (void)buf; return (int64_t)len; }
// The pool's lock never waits here: this harness is one thread.
void os64_yield(void) {}

// ── The clock ───────────────────────────────────────────────────────────

static uint64_t now_ms;
int64_t os64_ticks(os64_ticks_t *t)
{
    *t = (os64_ticks_t){ .ticks = now_ms, .per_second = 1000 };
    return 0;
}

// ── The environment ─────────────────────────────────────────────────────

static struct { const char *name, *value; } env[8];
static void set_env(const char *name, const char *value)
{
    for (size_t i = 0; i < 8; i++)
        if (env[i].name == NULL || strcmp(env[i].name, name) == 0) {
            env[i].name = name; env[i].value = value; return;
        }
    abort();
}
static void clear_env(void) { memset(env, 0, sizeof(env)); }
const char *os64_getenv(const char *key)
{
    for (size_t i = 0; i < 8 && env[i].name; i++)
        if (strcmp(env[i].name, key) == 0) return env[i].value;
    return NULL;
}

// ── The peers, and the connections to them ──────────────────────────────

typedef struct {
    const char *host;
    uint16_t    port;
    const uint8_t *reply;
    size_t      len;
    bool        refuse;        // the dial fails
    bool        silent;        // never answers a read
    size_t      silent_after;  // ...or goes silent after this many bytes
    size_t      early_after;   // expose response once this many request bytes arrive
    size_t      interim_len;   // stop here until the complete upload arrives
    bool        tls_bad_prefix; // authenticated bytes accompanied by protocol failure
    os64_tls_status_t tls_default_error, tls12_error;
    bool        tls_truncate;  // (https) the record layer ends without close_notify
    // KEEP-ALIVE (peer_answers). A server that answers request by request on
    // one connection: the n-th answer is released when the n-th whole
    // request has arrived, and `reply` is all of them in a row. Without
    // answers a peer is the old world: its one reply, then the close.
    size_t      nanswers;
    size_t      answer_len[8];
    size_t      hangup_after;  // closes once it has answered this many (0: keeps the connection)
    size_t      drop_request;  // closes when this request (1 = the first) arrives, unanswered
    size_t      fail_write_at; // a write of this request (1 = the first) breaks
    // How a dropped request's close shows: 0 to anyone who looks, a poll
    // included; 1 a close only a read finds (the request went, then the
    // close), 2 a reset only a read finds.
    int         drop_unseen;
    uint64_t    idle_close_ms; // closes an idle connection after this long (0: never)
} peer_t;

// Room for the pool's whole capacity and one more, each its own peer.
#define NPEERS 40
#define NCONNS 40
static peer_t peers[NPEERS];
static size_t npeers;
static void peer_reset(void) { npeers = 0; }
static peer_t *peer_add(const char *host, uint16_t port, const char *reply)
{
    peer_t *p = &peers[npeers++];
    memset(p, 0, sizeof(*p));
    p->host = host; p->port = port;
    p->reply = (const uint8_t *)reply; p->len = reply ? strlen(reply) : 0;
    return p;
}
static peer_t *peer_add_bytes(const char *host, uint16_t port, const uint8_t *reply, size_t len)
{
    peer_t *p = peer_add(host, port, NULL);
    p->reply = reply; p->len = len;
    return p;
}

typedef struct {
    bool     open;
    const peer_t *peer;
    size_t   served;
    char     request[131072];
    size_t   reqlen;
    int      dialled;          // which dial made it: its entry in history[]
    uint64_t last_ms;          // when it last served a byte
} conn_t;
static conn_t conns[NCONNS];

static const char *find(const char *hay, size_t len, const char *needle)
{
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= len; i++)
        if (memcmp(hay + i, needle, n) == 0) return hay + i;
    return NULL;
}
// How many whole requests have arrived on a connection: each head to its
// blank line, and the body its Content-Length promised.
static size_t requests_in(const conn_t *c)
{
    size_t n = 0, at = 0;
    for (;;) {
        const char *end = find(c->request + at, c->reqlen - at, "\r\n\r\n");
        if (end == NULL) return n;
        size_t head_end = (size_t)(end - c->request) + 4, body = 0;
        const char *cl = find(c->request + at, head_end - at, "Content-Length: ");
        if (cl != NULL) body = strtoul(cl + strlen("Content-Length: "), NULL, 10);
        if (head_end + body > c->reqlen) return n;
        at = head_end + body;
        n++;
    }
}
// The bytes the peer has said so far on this connection.
static size_t released(const conn_t *c)
{
    const peer_t *p = c->peer;
    if (p->nanswers == 0) return p->len;
    size_t k = requests_in(c), n = 0;
    if (p->drop_request && k >= p->drop_request) k = p->drop_request - 1;
    for (size_t i = 0; i < k && i < p->nanswers; i++) n += p->answer_len[i];
    return n;
}
// The peer closed on the request it drops.
static bool dropped(const conn_t *c)
{
    return c->peer->drop_request && requests_in(c) >= c->peer->drop_request;
}
// Everything said and the connection closed by the peer. The old world's
// peer closes after its one reply; a keep-alive peer after `hangup_after`
// answers, on the request it drops, or after `idle_close_ms` idle.
// The idle close alone: the server gave up on a connection nobody used.
static bool idle_closed(const conn_t *c)
{
    const peer_t *p = c->peer;
    return p->nanswers && p->idle_close_ms && c->served > 0 && c->served == released(c) &&
           now_ms - c->last_ms >= p->idle_close_ms;
}
static bool hung_up(const conn_t *c)
{
    const peer_t *p = c->peer;
    if (c->served < released(c)) return false;
    if (p->nanswers == 0) return p->len > 0;
    size_t k = requests_in(c);
    if (dropped(c)) return true;
    if (idle_closed(c)) return true;
    return p->hangup_after && k >= p->hangup_after;
}
// What a poll, or the TLS engine's state, can see of that close: all of
// it, but a dropped request's close that only a read finds.
static bool poll_sees_close(const conn_t *c)
{
    return hung_up(c) && !(c->peer->drop_unseen && dropped(c));
}
// Data the peer has released and not yet sent, on a keep-alive peer only:
// the old world's reply is all there from the start, and the upload's
// lookahead must not see it before the request is sent.
static bool pending(const conn_t *c)
{
    return c->peer->nanswers && c->served < released(c);
}
static int dials, closes;
static char last_dial[300];
static int  chunk_mode;        // 0 = as asked, 1 = one byte, 2 = random
static unsigned rng = 12345;
static size_t chunk_of(size_t asked)
{
    if (chunk_mode == 1) return 1;
    if (chunk_mode == 2) { rng = rng * 1103515245u + 12345u; size_t n = 1 + (rng >> 8) % 37; return n < asked ? n : asked; }
    return asked;
}

// A task's handle table, shrunk: with this many connections open a dial is
// refused as the kernel refuses one with no handle to give (0: no ceiling).
static int open_limit;

int64_t os64_dial(const char *dialstring)
{
    dials++;
    snprintf(last_dial, sizeof(last_dial), "%s", dialstring);
    char host[256]; unsigned port;
    if (sscanf(dialstring, "tcp!%255[^!]!%u", host, &port) != 2) return -1;
    int open = 0;
    for (int h = 0; h < NCONNS; h++) open += conns[h].open;
    if (open_limit && open >= open_limit) return OS64_NET_ERR_NO_RESOURCES;
    for (size_t i = 0; i < npeers; i++) {
        if (strcmp(peers[i].host, host) == 0 && peers[i].port == port) {
            if (peers[i].refuse) return -3;
            for (int h = 0; h < NCONNS; h++)
                if (!conns[h].open) {
                    memset(&conns[h], 0, sizeof(conns[h]));
                    conns[h].open = true; conns[h].peer = &peers[i];
                    conns[h].dialled = dials - 1;
                    return 100 + h;
                }
            abort();
        }
    }
    return -2;
}
const char *os64_dial_reason(int64_t err) { return err == -3 ? "connection refused" : "no such host"; }
// What was sent on each connection, by the order they were dialled, kept
// past its close (slots are reused hop to hop, so `conns[]` alone holds only
// the latest). A kept connection's requests follow one another in its entry.
static char history[NCONNS][131072];
static size_t write_limit, write_stop;
static bool upload_cancel;
int64_t os64_close(int32_t h)
{
    assert(h >= 100 && h < 100 + NCONNS && conns[h - 100].open);
    conns[h - 100].open = false;
    closes++;
    return 0;
}
// What was sent on the i-th connection dialled.
static const char *request_n(int i)
{
    return i >= 0 && i < NCONNS ? history[i] : "";
}
static bool upload_complete(const conn_t *c)
{
    const char *body = strstr(c->request, "\r\n\r\n");
    const char *length = strstr(c->request, "Content-Length: ");
    return body && c->reqlen >= (size_t)(body + 4 - c->request) +
        (length ? strtoul(length + strlen("Content-Length: "), NULL, 10) : 0);
}
static bool early_ready(const conn_t *c)
{
    return c->peer->early_after && c->reqlen >= c->peer->early_after &&
        c->served < c->peer->len &&
        (!c->peer->interim_len || c->served < c->peer->interim_len || upload_complete(c));
}
static int64_t conn_read(conn_t *c, void *buf, size_t cap, uint64_t wait_ms, bool *timed_out)
{
    *timed_out = false;
    if (c->peer->silent || (c->peer->silent_after && c->served >= c->peer->silent_after)) {
        now_ms += wait_ms; *timed_out = true; return 0;
    }
    if (c->peer->interim_len && c->served >= c->peer->interim_len && !upload_complete(c)) {
        now_ms += wait_ms; *timed_out = true; return 0;
    }
    size_t left = released(c) - c->served;
    if (c->peer->interim_len && c->served < c->peer->interim_len && !upload_complete(c))
        left = c->peer->interim_len - c->served;
    if (left == 0 && c->peer->nanswers && !hung_up(c)) {
        now_ms += wait_ms; *timed_out = true; return 0;   // a kept connection, waiting for a request
    }
    if (left == 0 && c->peer->drop_unseen == 2 && dropped(c)) return -1;   // the reset
    if (left == 0) return 0;
    size_t n = chunk_of(cap < left ? cap : left);
    if (c->peer->silent_after && c->served + n > c->peer->silent_after)
        n = c->peer->silent_after - c->served;
    memcpy(buf, c->peer->reply + c->served, n);
    c->served += n;
    c->last_ms = now_ms;
    return (int64_t)n;
}
int64_t os64_read_for(int32_t h, void *buf, size_t cap, uint64_t ms)
{
    conn_t *c = &conns[h - 100];
    assert(c->open);
    // A poll: a byte the peer has said, the close it has made, or nothing.
    if (!ms && !early_ready(c) && !pending(c))
        return poll_sees_close(c) ? 0 : OS64_ERR_TIMEOUT;
    bool timed_out;
    int64_t n = conn_read(c, buf, cap, ms, &timed_out);
    return timed_out ? OS64_ERR_TIMEOUT : n;
}
int64_t os64_write_for(int32_t h, const void *buf, size_t n, uint64_t ms)
{
    (void)ms;
    conn_t *c = &conns[h - 100];
    if (write_stop && c->reqlen >= write_stop) return -1;
    if (c->peer->fail_write_at && requests_in(c) + 1 >= c->peer->fail_write_at) return -1;
    if (write_limit && n > write_limit) n = write_limit;
    assert(c->open && c->reqlen + n < sizeof(c->request));
    memcpy(c->request + c->reqlen, buf, n);
    if (c->dialled < NCONNS)
        memcpy(history[c->dialled] + c->reqlen, buf, n);
    c->reqlen += n;
    return (int64_t)n;
}

// ── TLS: the transport stubbed over the same connections ────────────────

// `heard_ms`: when the engine last read the wire (a step or a read). A
// close an idle server sends after that is ciphertext nobody has read yet,
// and the engine's state does not know it, as a real engine does not.
struct os64_tls_transport { int32_t handle; os64_tls_status_t status; bool closing; uint16_t version;
                            uint64_t heard_ms; };
static int tls_creates, tls_frees, trust_loads, trust_frees;
static os64_tls_trust *fake_trust = (os64_tls_trust *)"trust";
static const os64_tls_trust *last_trust;   // what the last TLS client was verified under
static bool trust_fail_next;               // the next store read fails
os64_tls_status_t os64_tls_transport_create(const os64_tls_config_t *c, int32_t h,
                                            const os64_tls_transport_limits_t *limits,
                                            os64_tls_transport **out)
{
    (void)limits;
    assert(c && c->trust != NULL);
    assert(c->protocol==OS64_TLS_PROTOCOL_DEFAULT || c->protocol==OS64_TLS_PROTOCOL_TLS12_FALLBACK);
    tls_creates++;
    last_trust = c->trust;
    struct os64_tls_transport *t = calloc(1, sizeof(*t));
    t->handle = h;
    t->status = c->protocol == OS64_TLS_PROTOCOL_TLS12_FALLBACK ? conns[h-100].peer->tls12_error : conns[h-100].peer->tls_default_error;
    t->version = c->protocol == OS64_TLS_PROTOCOL_TLS12_FALLBACK ? 0x0303 : 0x0304;
    *out = t;
    return OS64_TLS_OK;
}
os64_tls_state_t os64_tls_transport_state(os64_tls_transport *t)
{
    conn_t *c = &conns[t->handle - 100];
    // A peer that has said everything and closed: authenticated, unless it
    // truncates.
    os64_tls_status_t status = t->status;
    bool unheard = idle_closed(c) && t->heard_ms < c->last_ms + c->peer->idle_close_ms;
    if (status == OS64_TLS_OK && poll_sees_close(c) && !unheard)
        status = c->peer->tls_truncate ? OS64_TLS_TRUNCATED : OS64_TLS_CLEAN_EOF;
    return (os64_tls_state_t){ .status = status, .flags = OS64_TLS_HANDSHAKE_DONE | OS64_TLS_SEND_PLAIN |
                                   (early_ready(c) || pending(c) ? OS64_TLS_RECV_PLAIN | OS64_TLS_SEND_CIPHER : 0),
                               .policy_reason = 0, .upstream_error = 0, .alpn = "http/1.1", .version = t->version };
}
os64_tls_status_t os64_tls_transport_step(os64_tls_transport *t, uint64_t ms)
{
    now_ms += write_limit ? 0 : ms;
    t->heard_ms = now_ms;
    if (t->closing) return t->status = OS64_TLS_CLEAN_EOF;
    return t->status == OS64_TLS_OK ? OS64_TLS_NEED_PROGRESS : t->status;
}
os64_tls_transfer_t os64_tls_transport_write(os64_tls_transport *t, const void *p, size_t n)
{
    int64_t sent = os64_write_for(t->handle, p, n, 0);
    return (os64_tls_transfer_t){ sent < 0 ? OS64_TLS_TRANSPORT : OS64_TLS_OK,
                                 sent < 0 ? 0 : (size_t)sent };
}
os64_tls_transfer_t os64_tls_transport_read(os64_tls_transport *t, void *p, size_t n)
{
    if (t->status != OS64_TLS_OK) return (os64_tls_transfer_t){ t->status, 0 };
    conn_t *c = &conns[t->handle - 100];
    t->heard_ms = now_ms;
    bool timed_out;
    int64_t got = conn_read(c, p, n, 1000, &timed_out);
    if (timed_out) return (os64_tls_transfer_t){ OS64_TLS_NEED_PROGRESS, 0 };
    if (got < 0) return (os64_tls_transfer_t){ t->status = OS64_TLS_TRANSPORT, 0 };
    if (got == 0) {
        t->status = c->peer->tls_truncate ? OS64_TLS_TRUNCATED : OS64_TLS_CLEAN_EOF;
        return (os64_tls_transfer_t){ t->status, 0 };
    }
    if (c->peer->tls_bad_prefix) t->status = OS64_TLS_PROTOCOL;
    return (os64_tls_transfer_t){ t->status, (size_t)got };
}
os64_tls_status_t os64_tls_transport_flush(os64_tls_transport *t) { return t->status; }
os64_tls_status_t os64_tls_transport_begin_close(os64_tls_transport *t) { t->closing = true; return t->status; }
os64_tls_status_t os64_tls_transport_abort(os64_tls_transport *t, os64_tls_status_t s) { return t->status = s; }
void os64_tls_transport_free(os64_tls_transport *t)
{
    tls_frees++;
    os64_close(t->handle);
    free(t);
}
os64_tls_store_status_t os64_tls_trust_reload(os64_tls_trust **current, os64_tls_store_report_t *report)
{
    (void)report;
    trust_loads++;
    if (trust_fail_next) {
        trust_fail_next = false;
        return OS64_TLS_STORE_OPEN;
    }
    *current = fake_trust;
    return OS64_TLS_STORE_OK;
}
void os64_tls_trust_free(os64_tls_trust *store) { assert(store == fake_trust); trust_frees++; }
const char *os64_tls_status_name(os64_tls_status_t s) { (void)s; return "tls-status"; }
const char *os64_tls_error_description(os64_tls_status_t s, os64_tls_policy_reason_t p, int e) { (void)s; (void)p; (void)e; return "tls-explanation"; }
const char *os64_tls_store_status_name(os64_tls_store_status_t s) { (void)s; return "store-status"; }

// ── Helpers ─────────────────────────────────────────────────────────────

static char replies[NPEERS + 8][262144];
static size_t nreplies;
static const char *reply(const char *head, const char *body)
{
    char *r = replies[nreplies++];
    snprintf(r, sizeof(replies[0]), "%s\r\n%s", head, body ? body : "");
    return r;
}
static const char *reply_len(const char *status, const char *extra, const char *body)
{
    char *r = replies[nreplies++];
    snprintf(r, sizeof(replies[0]), "HTTP/1.1 %s\r\nContent-Length: %zu\r\n%s\r\n%s",
             status, strlen(body), extra, body);
    return r;
}
// Chunked framing over `body`, in pieces of `piece` bytes; `cut` > 0 stops
// the reply that many bytes in (mid-chunk when it lands there).
static const char *reply_chunked_bytes(const char *status, const char *extra, const uint8_t *body,
                                       size_t len, size_t piece, size_t *outlen)
{
    char *r = replies[nreplies++];
    size_t at = (size_t)snprintf(r, sizeof(replies[0]), "HTTP/1.1 %s\r\nTransfer-Encoding: chunked\r\n%s\r\n",
                                 status, extra);
    for (size_t off = 0; off < len; off += piece) {
        size_t n = len - off < piece ? len - off : piece;
        at += (size_t)snprintf(r + at, sizeof(replies[0]) - at, "%zx\r\n", n);
        memcpy(r + at, body + off, n); at += n;
        memcpy(r + at, "\r\n", 2); at += 2;
    }
    at += (size_t)snprintf(r + at, sizeof(replies[0]) - at, "0\r\n\r\n");
    *outlen = at;
    return r;
}

// A keep-alive peer: `answers` (NULL-terminated) said one per request, in
// order, on every connection made to it.
static peer_t *peer_answers(const char *host, uint16_t port, const char *const *answers)
{
    char *all = replies[nreplies++];
    peer_t *p = peer_add(host, port, NULL);
    size_t at = 0;
    for (; answers[p->nanswers] != NULL; p->nanswers++) {
        size_t n = strlen(answers[p->nanswers]);
        assert(p->nanswers < 8 && at + n < sizeof(replies[0]));
        memcpy(all + at, answers[p->nanswers], n);
        p->answer_len[p->nanswers] = n;
        at += n;
    }
    p->reply = (const uint8_t *)all;
    p->len = at;
    return p;
}

static void reset(void)
{
    // What an earlier case left in the pool closes first, so its closes are
    // not this case's, and the system's store goes, so its read is.
    fetch_pool_drain();
    fetch_pool_trust_drop();
    peer_reset(); clear_env(); nreplies = 0;
    memset(conns, 0, sizeof(conns));
    memset(history, 0, sizeof(history));
    dials = closes = tls_creates = tls_frees = trust_loads = trust_frees = 0;
    last_trust = NULL; trust_fail_next = false;
    now_ms = 0; chunk_mode = 0; fail_next_malloc = false;
    write_limit = write_stop = 0; upload_cancel = false; open_limit = 0;
}

// Read the whole body into `out`, returning the byte count; the fetch's
// final status is the caller's to check.
static size_t slurp(os64_fetch_t *f, uint8_t *out, size_t cap, size_t piece)
{
    size_t total = 0;
    for (;;) {
        size_t want = piece ? piece : cap - total;
        if (want > cap - total) want = cap - total;
        if (want == 0) want = 1;               // a probe read past a full buffer
        uint8_t scratch[1];
        int64_t n = os64_fetch_read(f, total < cap ? out + total : scratch, want);
        if (n <= 0) return total;
        total += (size_t)n;
    }
}

static const char *request_of(int h) { return conns[h].request; }

static int checks;
#define CHECK(cond) do { checks++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

// ── The cases ───────────────────────────────────────────────────────────

static const char *page = "<html><body>hello, fetch</body></html>\n";

static void case_plain(int mode)
{
    reset(); chunk_mode = mode;
    peer_add("example.test", 80,
             reply_len("200 OK", "Content-Type: Text/HTML; charset=\"UTF-8\"\r\nX-Other: y\r\n", page));
    os64_fetch_options_t opt = { .user_agent = "harness/1", .accept = "text/html",
                                 .extra_headers = "Referer: http://from.test/\r\n" };
    os64_fetch_t *f = os64_fetch_open("http://example.test/index.html", &opt);
    CHECK(f && os64_fetch_status(f) == OS64_FETCH_OK);
    const os64_fetch_head_t *h = os64_fetch_head(f);
    CHECK(h && h->status == 200 && strcmp(h->reason, "OK") == 0);
    CHECK(strcmp(h->content_type, "text/html") == 0 && strcmp(h->charset, "utf-8") == 0);
    CHECK(h->has_length && h->length == strlen(page) && h->encoding[0] == '\0');
    CHECK(strcmp(h->url_text, "http://example.test/index.html") == 0 && h->url.port == 80);
    CHECK(!h->encrypted && !h->via_proxy && h->hops == 0);
    CHECK(h->method == OS64_FETCH_METHOD_GET);
    const char *req = request_of(0);
    CHECK(strstr(req, "GET /index.html HTTP/1.1\r\nHost: example.test\r\n") == req);
    CHECK(strstr(req, "User-Agent: harness/1\r\n") && strstr(req, "Accept: text/html\r\n"));
    CHECK(strstr(req, "Referer: http://from.test/\r\n") && strstr(req, "\r\n\r\n"));
    uint8_t out[4096];
    size_t n = slurp(f, out, sizeof(out), mode == 1 ? 1 : 0);
    CHECK(n == strlen(page) && memcmp(out, page, n) == 0);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
    CHECK(os64_fetch_read(f, out, sizeof(out)) == 0);          // says it again
    const os64_fetch_progress_t *p = os64_fetch_progress(f);
    CHECK(p->wire == n && p->produced == n && p->has_length && p->length == n);
    os64_fetch_close(f);
    // A 1.1 reply framed by its length and read to the end: the connection
    // is kept, not closed (the peer's close is the next take's to find).
    CHECK(closes == 0 && dials == 1 && fetch_pool_idle() == 1);
    // And the request said nothing of its connection.
    CHECK(strstr(req, "Connection") == NULL);
}

static void case_chunked_and_gzip(int mode)
{
    reset(); chunk_mode = mode;
    size_t len;
    const char *r = reply_chunked_bytes("200 OK", "Content-Type: text/plain\r\n",
                                        (const uint8_t *)page, strlen(page), 7, &len);
    peer_add_bytes("chunky.test", 80, (const uint8_t *)r, len);
    os64_fetch_t *f = os64_fetch_open("http://chunky.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && !os64_fetch_head(f)->has_length);
    uint8_t out[4096];
    size_t n = slurp(f, out, sizeof(out), mode ? 3 : 0);
    CHECK(n == strlen(page) && memcmp(out, page, n) == 0 && os64_fetch_status(f) == OS64_FETCH_OK);
    os64_fetch_close(f);

    // gzip inside chunked: the two envelopes come off in the order they went on.
    reset(); chunk_mode = mode;
    r = reply_chunked_bytes("200 OK", "Content-Encoding: gzip\r\nContent-Type: text/plain\r\n",
                            GZ_HELLO, sizeof(GZ_HELLO), 11, &len);
    peer_add_bytes("gz.test", 80, (const uint8_t *)r, len);
    f = os64_fetch_open("http://gz.test/hello.txt", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && strcmp(os64_fetch_head(f)->encoding, "gzip") == 0);
    static uint8_t big[65536];
    n = slurp(f, big, sizeof(big), mode ? 5 : 0);
    CHECK(n == sizeof(GZ_HELLO_PLAIN) - 1 && memcmp(big, GZ_HELLO_PLAIN, n) == 0);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
    const os64_fetch_progress_t *p = os64_fetch_progress(f);
    CHECK(p->wire == sizeof(GZ_HELLO) && p->produced == n);
    os64_fetch_close(f);
}

static void case_cut_and_limits(void)
{
    // A chunked gzip body cut mid-chunk: CUT, with whatever decoded first.
    reset();
    size_t len;
    const char *r = reply_chunked_bytes("200 OK", "Content-Encoding: gzip\r\n", GZ_HELLO, sizeof(GZ_HELLO), 16, &len);
    peer_add_bytes("cut.test", 80, (const uint8_t *)r, len - 40);
    os64_fetch_t *f = os64_fetch_open("http://cut.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
    static uint8_t big[65536];
    slurp(f, big, sizeof(big), 0);
    CHECK(os64_fetch_status(f) == OS64_FETCH_CUT);
    CHECK(strstr(os64_fetch_reason(f), "before its last chunk") != NULL);
    os64_fetch_close(f);

    // A length-framed identity body cut short.
    reset();
    peer_t *p = peer_add("short.test", 80, reply_len("200 OK", "", page));
    p->len -= 10;
    f = os64_fetch_open("http://short.test/", NULL);
    slurp(f, big, sizeof(big), 0);
    CHECK(os64_fetch_status(f) == OS64_FETCH_CUT && strstr(os64_fetch_reason(f), " of ") != NULL);
    os64_fetch_close(f);

    // max_body: exactly the body's size passes; one less refuses BEFORE the
    // byte that would cross, identity and gzip alike, on every chunking.
    for (int mode = 0; mode < 3; mode++) {
        reset(); chunk_mode = mode;
        peer_add("lim.test", 80, reply_len("200 OK", "", page));
        os64_fetch_options_t opt = { .max_body = strlen(page) };
        f = os64_fetch_open("http://lim.test/", &opt);
        size_t n = slurp(f, big, sizeof(big), mode ? 4 : 0);
        CHECK(n == strlen(page) && os64_fetch_status(f) == OS64_FETCH_OK);
        os64_fetch_close(f);

        reset(); chunk_mode = mode;
        peer_add("lim.test", 80, reply_len("200 OK", "", page));
        opt.max_body = strlen(page) - 1;
        f = os64_fetch_open("http://lim.test/", &opt);
        n = slurp(f, big, sizeof(big), mode ? 4 : 0);
        CHECK(n == strlen(page) - 1 && os64_fetch_status(f) == OS64_FETCH_LIMIT);
        CHECK(os64_fetch_progress(f)->produced == strlen(page) - 1);
        os64_fetch_close(f);

        reset(); chunk_mode = mode;
        r = reply_chunked_bytes("200 OK", "Content-Encoding: gzip\r\n", GZ_HELLO, sizeof(GZ_HELLO), 9, &len);
        peer_add_bytes("gzlim.test", 80, (const uint8_t *)r, len);
        opt.max_body = sizeof(GZ_HELLO_PLAIN) - 2;
        f = os64_fetch_open("http://gzlim.test/", &opt);
        n = slurp(f, big, sizeof(big), mode ? 4 : 0);
        CHECK(n == sizeof(GZ_HELLO_PLAIN) - 2 && os64_fetch_status(f) == OS64_FETCH_LIMIT);
        os64_fetch_close(f);
    }

    // The gzip expansion cap: a small wire body that inflates past 100x its
    // length (and the 1 MiB floor) is refused as LIMIT with the decoder's name.
    reset();
    static char head[256];
    snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nContent-Encoding: gzip\r\n\r\n", sizeof(GZ_BOMB));
    static uint8_t bomb[sizeof(GZ_BOMB) + 256];
    memcpy(bomb, head, strlen(head));
    memcpy(bomb + strlen(head), GZ_BOMB, sizeof(GZ_BOMB));
    peer_add_bytes("bomb.test", 80, bomb, strlen(head) + sizeof(GZ_BOMB));
    f = os64_fetch_open("http://bomb.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
    for (;;) { int64_t n = os64_fetch_read(f, big, sizeof(big)); if (n <= 0) break; }
    CHECK(os64_fetch_status(f) == OS64_FETCH_LIMIT && os64_fetch_detail(f)->gzip == OS64_GZIP_LIMIT);
    CHECK(strstr(os64_fetch_reason(f), "safety limit") != NULL);
    os64_fetch_close(f);
}

typedef struct { int calls; os64_fetch_hop_kind_t kinds[8]; os64_fetch_verdict_t answer; char last[OS64_FETCH_URL_MAX]; bool from_via; char from_host[OS64_URL_HOST_MAX]; } hops_t;
static os64_fetch_verdict_t record_hop(void *ctx, const os64_fetch_hop_t *hop)
{
    hops_t *h = ctx;
    if (h->calls < 8) h->kinds[h->calls] = hop->kind;
    h->calls++;
    snprintf(h->last, sizeof(h->last), "%s", hop->whole);
    h->from_via = hop->from_via_proxy;
    snprintf(h->from_host, sizeof(h->from_host), "%s", hop->from_proxy_host);
    return h->answer;
}

static void case_redirects(void)
{
    // A three-hop chain, relative and absolute Locations, ending in a 200.
    reset();
    peer_add("a.test", 80, reply("HTTP/1.1 301 Moved Permanently\r\nLocation: /two\r\nContent-Length: 0\r\n", ""));
    peer_add("b.test", 8080, reply("HTTP/1.1 302 Found\r\nLocation: http://c.test/three?x=1\r\nContent-Length: 0\r\n", ""));
    // a.test's second answer is served by the SAME peer entry (same host:port),
    // so the chain is a.test -> a.test/two -> b.test -> c.test. Give /two its
    // own peer by port instead, to keep the script readable:
    peers[0].reply = (const uint8_t *)reply("HTTP/1.1 301 Moved Permanently\r\nLocation: //b.test:8080/two\r\nContent-Length: 0\r\n", "");
    peers[0].len = strlen((const char *)peers[0].reply);
    peer_add("c.test", 80, reply_len("200 OK", "", page));
    hops_t hops = { .answer = OS64_FETCH_HOP_DEFAULT };
    os64_fetch_options_t opt = { .on_hop = record_hop, .ctx = &hops };
    os64_fetch_t *f = os64_fetch_open("http://a.test/one", &opt);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
    const os64_fetch_head_t *h = os64_fetch_head(f);
    CHECK(h->status == 200 && h->hops == 2 && strcmp(h->url_text, "http://c.test/three?x=1") == 0);
    CHECK(hops.calls == 2 && hops.kinds[0] == OS64_FETCH_HOP_WHOLE && hops.kinds[1] == OS64_FETCH_HOP_WHOLE);
    // Two hops' connections kept (each redirect's body was empty), the
    // third open.
    CHECK(dials == 3 && closes == 0 && fetch_pool_idle() == 2);
    uint8_t out[4096];
    CHECK(slurp(f, out, sizeof(out), 0) == strlen(page));
    os64_fetch_close(f);
    CHECK(closes == 0 && fetch_pool_idle() == 3);

    // A redirect to itself: stopped by default, the head readable; a
    // permissive caller follows it round until the cap.
    reset();
    peer_add("loop.test", 80, reply_len("302 Found", "Location: http://loop.test/\r\n", "courtesy page"));
    hops = (hops_t){ .answer = OS64_FETCH_HOP_DEFAULT };
    f = os64_fetch_open("http://loop.test/", &opt);
    CHECK(os64_fetch_status(f) == OS64_FETCH_REDIRECT_STOPPED);
    CHECK(os64_fetch_detail(f)->hop.kind == OS64_FETCH_HOP_SELF && hops.calls == 1);
    CHECK(os64_fetch_head(f)->status == 302 && os64_fetch_head(f)->has_location);
    CHECK(strstr(os64_fetch_reason(f), "circle") != NULL);
    CHECK(slurp(f, out, sizeof(out), 0) == strlen("courtesy page"));   // the body is still readable
    os64_fetch_close(f);
    reset();
    peer_add("loop.test", 80, reply_len("302 Found", "Location: http://loop.test/\r\n", ""));
    hops = (hops_t){ .answer = OS64_FETCH_HOP_FOLLOW };
    f = os64_fetch_open("http://loop.test/", &opt);
    CHECK(os64_fetch_status(f) == OS64_FETCH_TOO_MANY_HOPS && hops.calls == 6 && dials == 6);
    CHECK(os64_fetch_detail(f)->hop.number == 6);
    CHECK(strstr(os64_fetch_reason(f), "5 times") != NULL);
    os64_fetch_close(f);

    // The rest of the stop kinds, each with its sentence.
    struct { const char *location; os64_fetch_hop_kind_t kind; const char *word; } stops[] = {
        { "",                          OS64_FETCH_HOP_NONE,     "where to" },
        { "mailto:someone@x.test",     OS64_FETCH_HOP_SCHEME,   "http and https" },
        { "http://bad host/",          OS64_FETCH_HOP_UNUSABLE, "usable address" },
    };
    for (size_t i = 0; i < sizeof(stops) / sizeof(stops[0]); i++) {
        reset();
        char head[512];
        snprintf(head, sizeof(head), "HTTP/1.1 301 Moved\r\nLocation: %s\r\nContent-Length: 0\r\n", stops[i].location);
        peer_add("s.test", 80, reply(head, ""));
        f = os64_fetch_open("http://s.test/", NULL);
        CHECK(os64_fetch_status(f) == OS64_FETCH_REDIRECT_STOPPED);
        CHECK(os64_fetch_detail(f)->hop.kind == stops[i].kind);
        CHECK(strstr(os64_fetch_reason(f), stops[i].word) != NULL);
        os64_fetch_close(f);
    }

    // 300/304/305 are answers, not roads: OK with the head, never followed.
    reset();
    peer_add("m.test", 80, reply_len("300 Multiple Choices", "Location: /a\r\n", "pick one"));
    f = os64_fetch_open("http://m.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && os64_fetch_head(f)->status == 300 && dials == 1);
    os64_fetch_close(f);

    // A 404 is an answer too, and its page is the server's to show.
    reset();
    peer_add("nf.test", 80, reply_len("404 Not Found", "Content-Type: text/html\r\n", "<h1>gone</h1>"));
    f = os64_fetch_open("http://nf.test/x", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && os64_fetch_head(f)->status == 404);
    CHECK(slurp(f, out, sizeof(out), 0) == 13 && os64_fetch_status(f) == OS64_FETCH_OK);
    os64_fetch_close(f);
}

static void case_https_and_downgrade(void)
{
    // Direct https: the store is loaded once and freed at close (library-owned).
    reset();
    peer_add("secure.test", 443, reply_len("200 OK", "", page));
    os64_fetch_t *f = os64_fetch_open("https://secure.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && os64_fetch_head(f)->encrypted);
    CHECK(trust_loads == 1 && tls_creates == 1);
    uint8_t out[4096];
    CHECK(slurp(f, out, sizeof(out), 0) == strlen(page) && os64_fetch_status(f) == OS64_FETCH_OK);
    os64_fetch_close(f);
    // The system's store is the process's, kept for the next fetch.
    CHECK(trust_frees == 0 && tls_frees == 1 && closes == 1);

    // ONE SYSTEM STORE FOR EVERY FETCH THAT GIVES NONE: read once, every
    // connection verified under it (the transport is handed it), and a read
    // that failed is not kept — the next fetch reads again.
    reset();
    peer_add("secure.test", 443, reply_len("200 OK", "", page));
    peer_add("other.test", 443, reply_len("200 OK", "", page));
    trust_fail_next = true;
    f = os64_fetch_open("https://secure.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_TLS_FAILED && os64_fetch_detail(f)->store_failed && dials == 0);
    os64_fetch_close(f);
    f = os64_fetch_open("https://secure.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && trust_loads == 2 && last_trust == fake_trust);
    os64_fetch_close(f);
    f = os64_fetch_open("https://other.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && trust_loads == 2 && tls_creates == 2 && last_trust == fake_trust);
    os64_fetch_close(f);
    CHECK(trust_frees == 0);

    // A caller-owned store is never freed.
    reset();
    peer_add("secure.test", 443, reply_len("200 OK", "", page));
    os64_fetch_options_t owned = { .trust = fake_trust };
    f = os64_fetch_open("https://secure.test/", &owned);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && trust_loads == 0);
    os64_fetch_close(f);
    CHECK(trust_frees == 0);

    // A close-framed https body needs the peer's close_notify to count as
    // whole; a bare TCP end under TLS is CUT.
    reset();
    peer_t *p = peer_add("trunc.test", 443, reply("HTTP/1.1 200 OK\r\n", page));
    p->tls_truncate = true;
    f = os64_fetch_open("https://trunc.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
    slurp(f, out, sizeof(out), 0);
    CHECK((os64_fetch_status(f) == OS64_FETCH_CUT || os64_fetch_status(f) == OS64_FETCH_BROKE) &&
          strstr(os64_fetch_reason(f), "TLS") != NULL);
    os64_fetch_close(f);

    // https -> http: refused by default; followed when a person allows it.
    reset();
    peer_add("secure.test", 443, reply("HTTP/1.1 302 Found\r\nLocation: http://plain.test/\r\nContent-Length: 0\r\n", ""));
    peer_add("plain.test", 80, reply_len("200 OK", "", page));
    hops_t hops = { .answer = OS64_FETCH_HOP_DEFAULT };
    os64_fetch_options_t opt = { .on_hop = record_hop, .ctx = &hops };
    f = os64_fetch_open("https://secure.test/", &opt);
    CHECK(os64_fetch_status(f) == OS64_FETCH_REDIRECT_STOPPED);
    CHECK(os64_fetch_detail(f)->hop.kind == OS64_FETCH_HOP_DOWNGRADE && strstr(os64_fetch_reason(f), "downgrade"));
    os64_fetch_close(f);
    reset();
    peer_add("secure.test", 443, reply("HTTP/1.1 302 Found\r\nLocation: http://plain.test/\r\nContent-Length: 0\r\n", ""));
    peer_add("plain.test", 80, reply_len("200 OK", "", page));
    hops = (hops_t){ .answer = OS64_FETCH_HOP_FOLLOW };
    f = os64_fetch_open("https://secure.test/", &opt);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && !os64_fetch_head(f)->encrypted);
    CHECK(strcmp(os64_fetch_head(f)->url_text, "http://plain.test/") == 0 && os64_fetch_head(f)->hops == 1);
    os64_fetch_close(f);
    CHECK(trust_loads == 1 && trust_frees == 0);   // read for the first hop, kept for the process
}

static void case_proxy(void)
{
    // $http_proxy: the connection goes to the proxy, the request line carries
    // the whole address, and the head says so.
    reset();
    set_env("http_proxy", "http://proxy.test:3128/");
    peer_add("proxy.test", 3128, reply_len("200 OK", "", page));
    os64_fetch_t *f = os64_fetch_open("http://origin.test/path", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
    CHECK(strstr(request_of(0), "GET http://origin.test/path HTTP/1.1\r\nHost: origin.test\r\n") == request_of(0));
    CHECK(os64_fetch_head(f)->via_proxy && strcmp(os64_fetch_head(f)->proxy_host, "proxy.test") == 0);
    CHECK(strcmp(last_dial, "tcp!proxy.test!3128") == 0);
    os64_fetch_close(f);

    // $no_proxy bypasses it; the bare "host:port" spelling works; `.no_proxy = true` ignores it all.
    reset();
    set_env("http_proxy", "proxy.test:3128");
    set_env("no_proxy", ".origin.test, other");
    peer_add("origin.test", 80, reply_len("200 OK", "", page));
    f = os64_fetch_open("http://www.origin.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_DIAL_FAILED);       // went direct to www.origin.test: no peer
    CHECK(!os64_fetch_detail(f)->dial_was_proxy && strcmp(last_dial, "tcp!www.origin.test!80") == 0);
    os64_fetch_close(f);
    reset();
    set_env("http_proxy", "proxy.test:3128");
    peer_add("origin.test", 80, reply_len("200 OK", "", page));
    os64_fetch_options_t direct = { .no_proxy = true };
    f = os64_fetch_open("http://origin.test/", &direct);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && !os64_fetch_head(f)->via_proxy);
    os64_fetch_close(f);

    // An unusable setting is refused by name, before any dial.
    reset();
    set_env("http_proxy", "https://proxy.test/");
    f = os64_fetch_open("http://origin.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_PROXY_BAD && dials == 0);
    CHECK(strstr(os64_fetch_reason(f), "$http_proxy") != NULL);
    os64_fetch_close(f);

    // https through a proxy is NOT encrypted, and the head says so.
    reset();
    set_env("https_proxy", "http://proxy.test:3128/");
    peer_add("proxy.test", 3128, reply_len("200 OK", "", page));
    f = os64_fetch_open("https://origin.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && !os64_fetch_head(f)->encrypted && os64_fetch_head(f)->via_proxy);
    CHECK(tls_creates == 0 && trust_loads == 0);
    os64_fetch_close(f);

    // A redirect from http to https is carried by $https_proxy, whatever
    // carried the request that produced it.
    reset();
    set_env("https_proxy", "http://proxy.test:3128/");
    peer_add("plain.test", 80, reply("HTTP/1.1 301 Moved\r\nLocation: https://secure.test/\r\nContent-Length: 0\r\n", ""));
    peer_add("proxy.test", 3128, reply_len("200 OK", "", page));
    f = os64_fetch_open("http://plain.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && os64_fetch_head(f)->hops == 1 && os64_fetch_head(f)->via_proxy);
    os64_fetch_close(f);
}

static bool cancel_now;
static int cancel_after_calls;
static bool cancelled_cb(void *ctx)
{
    (void)ctx;
    if (cancel_after_calls > 0 && --cancel_after_calls == 0) cancel_now = true;
    return cancel_now;
}

static void case_failures(void)
{
    // No road at the first hop, and past it.
    reset();
    peer_t *p = peer_add("down.test", 80, "");
    p->refuse = true;
    os64_fetch_t *f = os64_fetch_open("http://down.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_DIAL_FAILED && os64_fetch_detail(f)->dial_hop == 0);
    CHECK(strstr(os64_fetch_reason(f), "connection refused") != NULL);
    os64_fetch_close(f);
    reset();
    peer_add("up.test", 80, reply("HTTP/1.1 301 Moved\r\nLocation: http://down.test/\r\nContent-Length: 0\r\n", ""));
    p = peer_add("down.test", 80, ""); p->refuse = true;
    f = os64_fetch_open("http://up.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_DIAL_FAILED && os64_fetch_detail(f)->dial_hop == 1);
    os64_fetch_close(f);

    // A peer that never answers: the idle deadline, before the head and after five body bytes.
    reset();
    p = peer_add("quiet.test", 80, reply_len("200 OK", "", page)); p->silent = true;
    f = os64_fetch_open("http://quiet.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_SILENT && !os64_fetch_detail(f)->silent_after_body);
    CHECK(strstr(os64_fetch_reason(f), "30 seconds before") != NULL);
    os64_fetch_close(f);
    reset();
    const char *r = reply_len("200 OK", "", page);
    p = peer_add("quiet.test", 80, r); p->silent_after = strlen(r) - strlen(page) + 5;
    f = os64_fetch_open("http://quiet.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
    uint8_t out[4096];
    CHECK(slurp(f, out, sizeof(out), 0) == 5 && os64_fetch_status(f) == OS64_FETCH_SILENT);
    CHECK(os64_fetch_detail(f)->silent_after_body && strstr(os64_fetch_reason(f), "after 5 bytes"));
    os64_fetch_close(f);

    // Framings and codings this cannot undo, refused by name; a 101; a bad head.
    reset();
    peer_add("br.test", 80, reply_len("200 OK", "Content-Encoding: br\r\n", page));
    f = os64_fetch_open("http://br.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_UNSUPPORTED && strcmp(os64_fetch_detail(f)->unsupported, "br") == 0);
    CHECK(os64_fetch_head(f) != NULL && os64_fetch_read(f, out, sizeof(out)) < 0);
    os64_fetch_close(f);
    reset();
    peer_add("te.test", 80, reply("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n", page));
    f = os64_fetch_open("http://te.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_UNSUPPORTED && strstr(os64_fetch_reason(f), "'gzip'"));
    os64_fetch_close(f);
    reset();
    peer_add("sw.test", 80, reply("HTTP/1.1 101 Switching Protocols\r\nUpgrade: x\r\n", ""));
    f = os64_fetch_open("http://sw.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_UNSUPPORTED && strstr(os64_fetch_reason(f), "switched"));
    os64_fetch_close(f);
    reset();
    peer_add("junk.test", 80, "this is not http at all\r\n\r\n");
    f = os64_fetch_open("http://junk.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_BAD_HEAD && os64_fetch_head(f) == NULL);
    os64_fetch_close(f);
    reset();
    peer_add("chunkjunk.test", 80, reply("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n", "zz\r\nnope"));
    f = os64_fetch_open("http://chunkjunk.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
    CHECK(os64_fetch_read(f, out, sizeof(out)) < 0 && os64_fetch_status(f) == OS64_FETCH_BAD_HEAD);
    CHECK(strstr(os64_fetch_reason(f), "after 0 bytes") != NULL);
    os64_fetch_close(f);

    // Bad gzip: the bytes claimed gzip and were not.
    reset();
    peer_add("notgz.test", 80, reply_len("200 OK", "Content-Encoding: gzip\r\n", "definitely not gzip"));
    f = os64_fetch_open("http://notgz.test/", NULL);
    CHECK(os64_fetch_read(f, out, sizeof(out)) < 0 && os64_fetch_status(f) == OS64_FETCH_CORRUPT);
    CHECK(strstr(os64_fetch_reason(f), "bad gzip") != NULL);
    os64_fetch_close(f);

    // Addresses this cannot take, and headers it will not send.
    reset();
    f = os64_fetch_open("ftp://x.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_UNSUPPORTED_SCHEME && strstr(os64_fetch_reason(f), "ftp"));
    os64_fetch_close(f);
    f = os64_fetch_open("http://x.test:99999/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_BAD_URL && os64_fetch_detail(f)->url == OS64_URL_PORT);
    os64_fetch_close(f);
    os64_fetch_options_t split = { .extra_headers = "X-A: 1\nX-B: 2\r\n" };
    f = os64_fetch_open("http://x.test/", &split);
    CHECK(os64_fetch_status(f) == OS64_FETCH_REQUEST_FAILED && dials == 0);
    os64_fetch_close(f);
    os64_fetch_options_t noend = { .extra_headers = "X-A: 1" };
    f = os64_fetch_open("http://x.test/", &noend);
    CHECK(os64_fetch_status(f) == OS64_FETCH_REQUEST_FAILED && dials == 0);
    os64_fetch_close(f);

    // Cancellation: before the dial, and between two reads.
    reset(); cancel_now = true; cancel_after_calls = 0;
    peer_add("c.test", 80, reply_len("200 OK", "", page));
    os64_fetch_options_t cancel = { .cancelled = cancelled_cb };
    f = os64_fetch_open("http://c.test/", &cancel);
    CHECK(os64_fetch_status(f) == OS64_FETCH_INTERRUPTED && dials == 0);
    os64_fetch_close(f);
    reset(); cancel_now = false; cancel_after_calls = 0;
    peer_add("c.test", 80, reply_len("200 OK", "", page));
    f = os64_fetch_open("http://c.test/", &cancel);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
    CHECK(os64_fetch_read(f, out, 4) == 4);
    cancel_now = true;
    CHECK(os64_fetch_read(f, out, 4) < 0 && os64_fetch_status(f) == OS64_FETCH_INTERRUPTED);
    os64_fetch_close(f);
    CHECK(closes == 1);
    cancel_now = false;

    // No memory for the object itself: NULL, and nothing else to clean up.
    reset(); fail_next_malloc = true;
    CHECK(os64_fetch_open("http://x.test/", NULL) == NULL);
}

static void case_content_type(void)
{
    struct { const char *line; const char *type, *charset; } cases[] = {
        { "Content-Type: text/html\r\n",                                  "text/html", "" },
        { "Content-Type: TEXT/HTML ; Charset = ISO-8859-1\r\n",           "text/html", "iso-8859-1" },
        { "Content-Type: application/xhtml+xml;boundary=x;charset=\"UTF-8\"\r\n", "application/xhtml+xml", "utf-8" },
        { "Content-Type: text/plain\r\nContent-Type: text/html\r\n",      "text/plain", "" },
        { "",                                                             "", "" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset();
        peer_add("ct.test", 80, reply_len("200 OK", cases[i].line, page));
        os64_fetch_t *f = os64_fetch_open("http://ct.test/", NULL);
        CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
        CHECK(strcmp(os64_fetch_head(f)->content_type, cases[i].type) == 0);
        CHECK(strcmp(os64_fetch_head(f)->charset, cases[i].charset) == 0);
        os64_fetch_close(f);
    }
}

// Codex round 1 on PR #92: one case per finding, each of which failed
// against the first push.
static void case_round1(void)
{
    // A header string may not smuggle a request: an empty line, a line with
    // no colon, a name that is not a token — refused before any dial.
    const char *bad[] = { "\r\nGET /admin HTTP/1.1\r\n", "NoColon\r\n", "X A: 1\r\n", ": empty\r\n" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        reset();
        os64_fetch_options_t o = { .extra_headers = bad[i] };
        os64_fetch_t *f = os64_fetch_open("http://x.test/", &o);
        CHECK(os64_fetch_status(f) == OS64_FETCH_REQUEST_FAILED && dials == 0);
        os64_fetch_close(f);
    }
    reset();
    peer_add("x.test", 80, reply_len("200 OK", "", page));
    os64_fetch_options_t good = { .extra_headers = "X-A: 1\r\nX-B: two words\r\n" };
    os64_fetch_t *f = os64_fetch_open("http://x.test/", &good);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && strstr(request_of(0), "X-A: 1\r\nX-B: two words\r\n"));
    os64_fetch_close(f);

    // A sub-second idle budget is a budget, not a zero: a prompt peer is
    // fine, a silent one is SILENT after about that long.
    reset();
    peer_add("q.test", 80, reply_len("200 OK", "", page));
    os64_fetch_options_t quick = { .idle_ms = 500 };
    f = os64_fetch_open("http://q.test/", &quick);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
    uint8_t out[4096];
    CHECK(slurp(f, out, sizeof(out), 0) == strlen(page) && os64_fetch_status(f) == OS64_FETCH_OK);
    os64_fetch_close(f);
    reset();
    peer_t *p = peer_add("q.test", 80, reply_len("200 OK", "", page)); p->silent = true;
    f = os64_fetch_open("http://q.test/", &quick);
    CHECK(os64_fetch_status(f) == OS64_FETCH_SILENT && now_ms >= 500 && now_ms < 2000);
    os64_fetch_close(f);

    // A zero-capacity read is refused and changes nothing.
    reset();
    peer_add("z.test", 80, reply_len("200 OK", "", page));
    f = os64_fetch_open("http://z.test/", NULL);
    CHECK(os64_fetch_read(f, out, 0) == -1 && os64_fetch_status(f) == OS64_FETCH_OK);
    CHECK(slurp(f, out, sizeof(out), 0) == strlen(page) && os64_fetch_status(f) == OS64_FETCH_OK);
    os64_fetch_close(f);

    // A downgrade the caller allowed, whose target's proxy setting is
    // unusable: the hop that stops the fetch says PROXY, not DOWNGRADE.
    reset();
    set_env("http_proxy", "https://proxy.test/");
    peer_add("secure.test", 443, reply("HTTP/1.1 302 Found\r\nLocation: http://plain.test/\r\nContent-Length: 0\r\n", ""));
    hops_t hops = { .answer = OS64_FETCH_HOP_FOLLOW };
    os64_fetch_options_t opt = { .on_hop = record_hop, .ctx = &hops };
    f = os64_fetch_open("https://secure.test/", &opt);
    CHECK(os64_fetch_status(f) == OS64_FETCH_REDIRECT_STOPPED);
    CHECK(os64_fetch_detail(f)->hop.kind == OS64_FETCH_HOP_PROXY);
    CHECK(strstr(os64_fetch_reason(f), "$http_proxy") != NULL && dials == 1);
    os64_fetch_close(f);

    // A cancellation that lands while the trust store is loading must not
    // lose the store: the predicate's second call (after the load) says
    // yes, and the store read is kept for the next fetch, not read again.
    reset(); cancel_now = false; cancel_after_calls = 2;
    peer_add("secure.test", 443, reply_len("200 OK", "", page));
    os64_fetch_options_t cancel = { .cancelled = cancelled_cb };
    f = os64_fetch_open("https://secure.test/", &cancel);
    CHECK(os64_fetch_status(f) == OS64_FETCH_INTERRUPTED && trust_loads == 1);
    os64_fetch_close(f);
    cancel_now = false; cancel_after_calls = 0;
    f = os64_fetch_open("https://secure.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && trust_loads == 1 && trust_frees == 0);
    os64_fetch_close(f);

    // The hop names who carried the hop that ANSWERED, so a caller can warn
    // about a proxied first leg even when the final head went direct.
    reset();
    set_env("http_proxy", "http://proxy.test:3128/");
    peer_add("proxy.test", 3128, reply("HTTP/1.1 301 Moved\r\nLocation: https://secure.test/\r\nContent-Length: 0\r\n", ""));
    peer_add("secure.test", 443, reply_len("200 OK", "", page));
    hops = (hops_t){ .answer = OS64_FETCH_HOP_DEFAULT };
    f = os64_fetch_open("http://plain.test/", &opt);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && os64_fetch_head(f)->encrypted && !os64_fetch_head(f)->via_proxy);
    CHECK(hops.calls == 1 && hops.from_via && strcmp(hops.from_host, "proxy.test") == 0);
    os64_fetch_close(f);
}

// Codex round 2 on PR #92.
static void case_round2(void)
{
    // 304 and 204 carry no body whatever their headers claim: the read
    // answers 0 with OK at once, and the connection is not waited on.
    reset();
    peer_add("nm.test", 80, reply("HTTP/1.1 304 Not Modified\r\nContent-Length: 1234\r\nETag: \"x\"\r\n", ""));
    os64_fetch_options_t cond = { .extra_headers = "If-None-Match: \"x\"\r\n" };
    os64_fetch_t *f = os64_fetch_open("http://nm.test/", &cond);
    uint8_t out[64];
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && os64_fetch_head(f)->status == 304);
    CHECK(os64_fetch_head(f)->has_length && os64_fetch_head(f)->length == 0);
    CHECK(os64_fetch_read(f, out, sizeof(out)) == 0 && os64_fetch_status(f) == OS64_FETCH_OK);
    os64_fetch_close(f);
    // ...and a coding on one describes the representation that was not
    // sent: no decoder, no refusal, the head keeps the word.
    reset();
    peer_add("nm.test", 80, reply("HTTP/1.1 304 Not Modified\r\nContent-Encoding: gzip\r\nContent-Length: 44\r\n", ""));
    f = os64_fetch_open("http://nm.test/", &cond);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && strcmp(os64_fetch_head(f)->encoding, "gzip") == 0);
    CHECK(os64_fetch_read(f, out, sizeof(out)) == 0 && os64_fetch_status(f) == OS64_FETCH_OK);
    os64_fetch_close(f);
    reset();
    peer_add("nm.test", 80, reply("HTTP/1.1 304 Not Modified\r\nContent-Encoding: br\r\n", ""));
    f = os64_fetch_open("http://nm.test/", &cond);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
    CHECK(os64_fetch_read(f, out, sizeof(out)) == 0 && os64_fetch_status(f) == OS64_FETCH_OK);
    os64_fetch_close(f);
    reset();
    peer_add("nc.test", 80, reply("HTTP/1.1 204 No Content\r\nTransfer-Encoding: chunked\r\n", ""));
    f = os64_fetch_open("http://nc.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && os64_fetch_head(f)->status == 204);
    CHECK(os64_fetch_read(f, out, sizeof(out)) == 0 && os64_fetch_status(f) == OS64_FETCH_OK);
    os64_fetch_close(f);

    // A semicolon inside an earlier quoted parameter is data, not a delimiter.
    reset();
    peer_add("ct.test", 80, reply_len("200 OK",
             "Content-Type: text/html; note=\"x;charset=iso-8859-1;y\\\"z\"; charset=utf-8\r\n", page));
    f = os64_fetch_open("http://ct.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
    CHECK(strcmp(os64_fetch_head(f)->content_type, "text/html") == 0);
    CHECK(strcmp(os64_fetch_head(f)->charset, "utf-8") == 0);
    os64_fetch_close(f);
}

// Codex round 4 on PR #92.
static void case_round4(void)
{
    // A sub-second idle budget is named in its own unit, not as "0 seconds".
    reset();
    peer_t *p = peer_add("q.test", 80, reply_len("200 OK", "", page)); p->silent = true;
    os64_fetch_options_t quick = { .idle_ms = 500 };
    os64_fetch_t *f = os64_fetch_open("http://q.test/", &quick);
    CHECK(os64_fetch_status(f) == OS64_FETCH_SILENT && strstr(os64_fetch_reason(f), "500 ms") != NULL);
    os64_fetch_close(f);

    // Credentials follow a redirect that stays on the typed origin and stop
    // at one that leaves it; the rest of the block rides along either way.
    const char *creds = "Cookie: session=s3cret\r\nX-Keep: yes\r\nAuthorization: Bearer t\r\n";
    reset();
    peer_add("same.test", 80, reply("HTTP/1.1 302 Found\r\nLocation: /elsewhere\r\nContent-Length: 0\r\n", ""));
    os64_fetch_options_t o = { .extra_headers = creds };
    f = os64_fetch_open("http://same.test/start", &o);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK || os64_fetch_status(f) == OS64_FETCH_REDIRECT_STOPPED);
    CHECK(dials >= 2);
    CHECK(strstr(request_n(1), "Cookie: session=s3cret\r\n") && strstr(request_n(1), "Authorization: Bearer t\r\n"));
    CHECK(strstr(request_n(1), "X-Keep: yes\r\n"));
    os64_fetch_close(f);
    reset();
    peer_add("first.test", 80, reply("HTTP/1.1 302 Found\r\nLocation: http://other.test/\r\nContent-Length: 0\r\n", ""));
    peer_add("other.test", 80, reply_len("200 OK", "", page));
    f = os64_fetch_open("http://first.test/start", &o);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && dials == 2);
    CHECK(strstr(request_n(0), "Cookie: session=s3cret\r\n") != NULL);          // the typed origin got it
    CHECK(strstr(request_n(1), "Cookie:") == NULL && strstr(request_n(1), "Authorization:") == NULL);
    CHECK(strstr(request_n(1), "X-Keep: yes\r\n") != NULL);
    os64_fetch_close(f);
    // A port change is a different origin too.
    reset();
    peer_add("first.test", 80, reply("HTTP/1.1 302 Found\r\nLocation: http://first.test:8080/\r\nContent-Length: 0\r\n", ""));
    peer_add("first.test", 8080, reply_len("200 OK", "", page));
    f = os64_fetch_open("http://first.test/start", &o);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && strstr(request_n(1), "Cookie:") == NULL);
    os64_fetch_close(f);
}

// Codex round 5 on PR #92.
static void case_round5(void)
{
    // Proxy-Authorization is the PROXY's: never on a direct request, kept
    // across a cross-origin hop the same proxy carries.
    const char *creds = "Proxy-Authorization: Basic cDpw\r\nCookie: c=1\r\nX-Keep: yes\r\n";
    os64_fetch_options_t o = { .extra_headers = creds };
    reset();
    peer_add("direct.test", 80, reply_len("200 OK", "", page));
    os64_fetch_t *f = os64_fetch_open("http://direct.test/", &o);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
    CHECK(strstr(request_n(0), "Proxy-Authorization:") == NULL && strstr(request_n(0), "Cookie: c=1\r\n") != NULL);
    os64_fetch_close(f);
    reset();
    set_env("http_proxy", "http://proxy.test:3128/");
    peer_add("proxy.test", 3128, reply("HTTP/1.1 302 Found\r\nLocation: http://other.test/\r\nContent-Length: 0\r\n", ""));
    f = os64_fetch_open("http://first.test/", &o);
    // Both hops go through the same proxy; its second answer points at itself, which ends it.
    CHECK(os64_fetch_status(f) == OS64_FETCH_REDIRECT_STOPPED && dials == 2);
    CHECK(strstr(request_n(0), "Proxy-Authorization: Basic cDpw\r\n") != NULL && strstr(request_n(0), "Cookie: c=1\r\n") != NULL);
    CHECK(strstr(request_n(1), "Proxy-Authorization: Basic cDpw\r\n") != NULL);   // the proxy still carries it
    CHECK(strstr(request_n(1), "Cookie:") == NULL);                                 // the origin changed
    os64_fetch_close(f);
    // A no_proxy match makes the first hop direct: the proxy credential stays home.
    reset();
    set_env("http_proxy", "http://proxy.test:3128/");
    set_env("no_proxy", "direct.test");
    peer_add("direct.test", 80, reply_len("200 OK", "", page));
    f = os64_fetch_open("http://direct.test/", &o);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && strstr(request_n(0), "Proxy-Authorization:") == NULL);
    os64_fetch_close(f);

    // A scheme-relative reference near the cap, whose dot segments reduce to
    // something short, resolves: the scratch buffer has room for the scheme.
    os64_url_t base = { .scheme = "http", .host = "b.test", .path = "/" };
    static char ref[OS64_URL_REF_MAX];
    size_t n = (size_t)snprintf(ref, sizeof(ref), "//h/");
    while (n + 5 < OS64_URL_REF_MAX - 1) n += (size_t)snprintf(ref + n, sizeof(ref) - n, "a/../");
    char whole[OS64_URL_REF_MAX + 32];
    CHECK(os64_url_absolute(&base, ref, whole, sizeof(whole)) && strcmp(whole, "http://h/") == 0);
}

// Codex round 6 on PR #92.
static void case_round6(void)
{
    // A stopped redirect whose courtesy body is in a coding nobody decodes
    // keeps its verdict; the body is unreadable and the coding is named.
    reset();
    peer_add("secure.test", 443, reply("HTTP/1.1 302 Found\r\nLocation: http://plain.test/\r\nContent-Encoding: br\r\nContent-Length: 3\r\n", "abc"));
    os64_fetch_t *f = os64_fetch_open("https://secure.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_REDIRECT_STOPPED);
    CHECK(os64_fetch_detail(f)->hop.kind == OS64_FETCH_HOP_DOWNGRADE);
    CHECK(strcmp(os64_fetch_detail(f)->unsupported, "br") == 0);
    uint8_t out[16];
    CHECK(os64_fetch_read(f, out, sizeof(out)) == -1 && os64_fetch_status(f) == OS64_FETCH_REDIRECT_STOPPED);
    os64_fetch_close(f);
    // ...while an OK head with the same envelope is UNSUPPORTED, as before.
    reset();
    peer_add("br.test", 80, reply_len("200 OK", "Content-Encoding: br\r\n", page));
    f = os64_fetch_open("http://br.test/", NULL);
    CHECK(os64_fetch_status(f) == OS64_FETCH_UNSUPPORTED);
    os64_fetch_close(f);
}

// Codex round 7 on PR #92.
static void case_round7(void)
{
    // The identity probe at the cap reads ONE byte: a body with many bytes
    // past max_body and a caller buffer far larger than one must refuse by
    // name without touching more than the probe (ASan is the witness).
    for (int mode = 0; mode < 3; mode++) {
        reset(); chunk_mode = mode;
        peer_add("lim.test", 80, reply_len("200 OK", "", page));
        os64_fetch_options_t opt = { .max_body = 5 };
        os64_fetch_t *f = os64_fetch_open("http://lim.test/", &opt);
        static uint8_t big[65536];
        size_t n = slurp(f, big, sizeof(big), 0);
        CHECK(n == 5 && os64_fetch_status(f) == OS64_FETCH_LIMIT);
        CHECK(os64_fetch_progress(f)->produced == 5 && os64_fetch_progress(f)->wire <= 6 + 36);
        os64_fetch_close(f);
    }
}


// Byte-exact uploads, including binary bytes, partial writes and redirects.
static os64_fetch_method_t seen_from, seen_to;
static os64_fetch_verdict_t post_hop(void *ctx, const os64_fetch_hop_t *hop)
{
    (void)ctx;
    seen_from = hop->from_method;
    seen_to = hop->to_method;
    return OS64_FETCH_HOP_DEFAULT;
}
static bool cancel_upload(void *ctx)
{
    (void)ctx;
    return upload_cancel && conns[0].reqlen > 600;
}
static void case_post(void)
{
    static unsigned char body[70000];
    for (size_t i = 0; i < sizeof(body); i++) body[i] = (unsigned char)i;
    const char *type = "multipart/form-data; boundary=abc-123";
    for (int mode = 0; mode < 3; mode++) {
        for (int tls = 0; tls < 2; tls++) {
            reset(); chunk_mode = mode; write_limit = mode == 1 ? 1 : mode == 2 ? 37 : 0;
            peer_add("post.test", tls ? 443 : 80, reply_len("200 OK", "", page));
            os64_fetch_options_t opt = { .method = OS64_FETCH_METHOD_POST,
                .body = body, .body_len = sizeof(body), .content_type = type };
            os64_fetch_t *f = os64_fetch_open(tls ? "https://post.test/form" : "http://post.test/form", &opt);
            CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
            const char *req = request_n(0), *bytes = strstr(req, "\r\n\r\n") + 4;
            CHECK(strncmp(req, "POST /form HTTP/1.1\r\n", strlen("POST /form HTTP/1.1\r\n")) == 0);
            CHECK(strstr(req, "Content-Length: 70000\r\n") != NULL);
            CHECK(strstr(req, "Content-Type: multipart/form-data; boundary=abc-123\r\n") != NULL);
            CHECK(conns[0].reqlen == (size_t)(bytes - req) + sizeof(body));
            CHECK(memcmp(bytes, body, sizeof(body)) == 0);
            os64_fetch_close(f);
            CHECK(dials == 1 && closes == 1);
        }
        int codes[] = {301, 302, 303, 307, 308};
        for (size_t i = 0; i < sizeof(codes)/sizeof(codes[0]); i++) {
            reset(); chunk_mode = mode; write_limit = 17;
            char status[32]; snprintf(status, sizeof(status), "%d Redirect", codes[i]);
            peer_add("post.test", 80, reply_len(status, "Location: http://next.test/\r\n", ""));
            peer_add("next.test", 80, reply_len("200 OK", "", page));
            os64_fetch_options_t opt = { .method = OS64_FETCH_METHOD_POST,
                .body = body, .body_len = sizeof(body), .content_type = type, .on_hop = post_hop };
            os64_fetch_t *f = os64_fetch_open("http://post.test/", &opt);
            CHECK(os64_fetch_status(f) == OS64_FETCH_OK && dials == 2);
            bool replay = codes[i] >= 307;
            CHECK(os64_fetch_head(f)->method == (replay ? OS64_FETCH_METHOD_POST : OS64_FETCH_METHOD_GET));
            CHECK(seen_from == OS64_FETCH_METHOD_POST &&
                  seen_to == (replay ? OS64_FETCH_METHOD_POST : OS64_FETCH_METHOD_GET));
            const char *req = request_n(1), *bytes = strstr(req, "\r\n\r\n") + 4;
            CHECK(strncmp(req, replay ? "POST / HTTP" : "GET / HTTP", replay ? 11 : 10) == 0);
            if (replay) {
                CHECK(memcmp(bytes, body, sizeof(body)) == 0);
                CHECK(strstr(req, "Content-Length: 70000\r\n") != NULL);
            } else {
                CHECK(*bytes == '\0');
                CHECK(strstr(req, "Content-Length:") == NULL && strstr(req, "Content-Type:") == NULL);
            }
            // The redirect's empty body leaves its connection kept; the final
            // reply's, never read, closes its own.
            os64_fetch_close(f); CHECK(closes == 1 && fetch_pool_idle() == 1);
        }
    }
    reset();
    peer_add("post.test", 80, reply_len("303 See Other", "Location: /\r\n", ""));
    os64_fetch_options_t opt = { .method = OS64_FETCH_METHOD_POST, .on_hop = post_hop };
    os64_fetch_t *f = os64_fetch_open("http://post.test/", &opt);
    // POST -> GET follows the same URL. Its next GET -> GET is a loop.
    CHECK(dials == 2 && os64_fetch_detail(f)->hop.kind == OS64_FETCH_HOP_SELF);
    CHECK(strstr(request_n(0), "Content-Length: 0\r\n") != NULL);
    CHECK(strstr(request_n(1), "Content-Length:") == NULL);
    os64_fetch_close(f);

    const char *bad[] = {"Content-Length: 0\r\n", "tRaNsFeR-EnCoDiNg: chunked\r\n",
        "Host: thief.test\r\n", "Content-Type: bad\r\n", "Expect: 100-continue\r\n",
        "Connection: keep-alive\r\n", "Content-Encoding: gzip\r\n", "Trailer: X\r\n"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        reset(); opt = (os64_fetch_options_t){ .extra_headers = bad[i] };
        f = os64_fetch_open("http://post.test/", &opt);
        CHECK(os64_fetch_status(f) == OS64_FETCH_REQUEST_FAILED && dials == 0);
        os64_fetch_close(f);
    }
    for (int i = 0; i < 5; i++) {
        reset(); opt = (os64_fetch_options_t){0};
        if (i == 0) opt.method = (os64_fetch_method_t)99;
        if (i == 1) { opt.method = OS64_FETCH_METHOD_POST; opt.body_len = 1; }
        if (i == 2) opt.body = body;
        if (i == 3) opt.content_type = "text/plain";
        if (i == 4) { opt.method = OS64_FETCH_METHOD_POST; opt.content_type = "x\r\nHost: evil"; }
        f = os64_fetch_open("http://post.test/", &opt);
        CHECK(os64_fetch_status(f) == OS64_FETCH_REQUEST_FAILED && dials == 0);
        os64_fetch_close(f);
    }
    for (int cancel = 0; cancel < 2; cancel++) {
        reset(); write_limit = 17; write_stop = cancel ? 0 : 700; upload_cancel = cancel;
        peer_add("post.test", 80, reply_len("200 OK", "", page));
        opt = (os64_fetch_options_t){ .method = OS64_FETCH_METHOD_POST,
            .body = body, .body_len = sizeof(body), .cancelled = cancel_upload };
        f = os64_fetch_open("http://post.test/", &opt);
        CHECK(os64_fetch_status(f) == (cancel ? OS64_FETCH_INTERRUPTED : OS64_FETCH_REQUEST_FAILED));
        CHECK(dials == 1 && closes == 1); // an uncertain POST is not retried
        os64_fetch_close(f);
    }
}

// A server can answer while ciphertext is pending, or stop reading a plain
// upload. Interim replies must resume exactly; final replies stop sending.
static void case_early_post(void)
{
    static unsigned char body[70000];
    for (size_t i=0; i<sizeof(body); i++) body[i]=(unsigned char)i;
    for (int mode=0; mode<3; mode++) for (int tls=0; tls<2; tls++) {
        for (int kind=0; kind<5; kind++) {
            reset(); chunk_mode=mode; write_limit=37;
            const char *interim = kind==2 ? "HTTP/1.1 100 Continue\r\n\r\n" :
                                          "HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n";
            char reply[512];
            snprintf(reply,sizeof(reply),"%sHTTP/1.1 %s\r\nContent-Length: 5\r\n\r\nhello",
                     kind>=2 ? interim : "", kind==0 ? "401 Unauthorized" :
                     kind==1 || kind==4 ? "413 Content Too Large" : "200 OK");
            peer_t *peer=peer_add("early.test",tls ? 443 : 80,reply);
            peer->early_after=600;
            if(kind==2 || kind==3) peer->interim_len=strlen(interim);
            os64_fetch_options_t opt={.method=OS64_FETCH_METHOD_POST,.body=body,.body_len=sizeof(body)};
            os64_fetch_t *f=os64_fetch_open(tls ? "https://early.test/" : "http://early.test/",&opt);
            CHECK(os64_fetch_status(f)==OS64_FETCH_OK);
            CHECK(os64_fetch_head(f)->status==(kind==0 ? 401 : kind==1 || kind==4 ? 413 : 200));
            CHECK(os64_fetch_head(f)->method==OS64_FETCH_METHOD_POST);
            const char *req=request_n(0), *bytes=strstr(req,"\r\n\r\n")+4;
            size_t uploaded=conns[0].reqlen-(size_t)(bytes-req);
            CHECK((kind==2 || kind==3) ? uploaded==sizeof(body) : uploaded<sizeof(body));
            CHECK(memcmp(bytes,body,uploaded)==0);
            unsigned char out[16];
            CHECK(slurp(f,out,sizeof(out),0)==5 && !memcmp(out,"hello",5));
            CHECK(os64_fetch_status(f)==OS64_FETCH_OK);
            // Kept only when the upload went whole: an answer that stopped it
            // leaves the server reading a body that never ends. (Over TLS this
            // peer's close is authenticated with the reply, so none is kept.)
            os64_fetch_close(f);
            CHECK(dials==1 && closes + (int)fetch_pool_idle()==1);
            CHECK((fetch_pool_idle()==1) == (!tls && (kind==2 || kind==3)));
        }
    }
}

typedef struct {
    unsigned asked, received, hops, headers;
    const char *output;
    bool fail, unterminated, chain, encrypted, cancelled, cancel_header, cancel_cookie;
    char values[8][HTTP_LINE_MAX], hosts[8][OS64_URL_HOST_MAX];
    bool received_encrypted[8];
} cookie_test_t;
static void cookie_received(void *ctx, const os64_url_t *url, bool encrypted, const char *value, size_t len)
{
    cookie_test_t *t = ctx;
    CHECK(t->received < 8 && len < HTTP_LINE_MAX && url->port != 0);
    t->received_encrypted[t->received] = encrypted;
    memcpy(t->values[t->received], value, len);
    t->values[t->received][len] = 0;
    snprintf(t->hosts[t->received], sizeof(t->hosts[0]), "%s", url->host);
    t->received++;
    if (t->cancel_cookie) t->cancelled = true;
}
static bool cookie_headers(void *ctx, const os64_url_t *url, bool encrypted, char *out, size_t cap)
{
    cookie_test_t *t = ctx;
    t->asked++; t->encrypted = encrypted;
    if (t->cancel_header) t->cancelled = true;
    if (t->fail) return false;
    if (t->unterminated) { memset(out, 'x', cap); return true; }
    if (t->chain) {
        if (t->asked == 1) {
            CHECK(strcmp(url->host, "a.test") == 0 && t->received == 0 && t->hops == 0);
            snprintf(out, cap, "Cookie: first=1\r\n");
        } else {
            CHECK(strcmp(url->host, "b.test") == 0 && t->received == 2 && t->hops == 1);
            snprintf(out, cap, "Cookie: second=2\r\n");
        }
    } else {
        snprintf(out, cap, "%s", t->output ? t->output : "");
    }
    return true;
}
static os64_fetch_verdict_t cookie_hop(void *ctx, const os64_fetch_hop_t *hop)
{
    cookie_test_t *t = ctx;
    CHECK(t->received == 2 && hop->status == 302);
    t->hops++;
    return OS64_FETCH_HOP_DEFAULT;
}
static bool cookie_cancel(void *ctx) { return ((cookie_test_t *)ctx)->cancelled; }
static void case_cookies(void)
{
    for (int mode = 0; mode < 3; mode++) {
        reset(); chunk_mode = mode;
        peer_add("a.test", 80, reply_len("302 Found",
            "Set-Cookie: one=1; Expires=Wed, 09 Jun 2027 10:18:14 GMT\r\n"
            "sEt-CoOkIe:\t two=2; Path=/ \t\r\nLocation: http://b.test/next\r\n", ""));
        peer_add("b.test", 80, reply_len("200 OK", "Set-Cookie: three=3\r\n", page));
        cookie_test_t t = { .chain = true };
        os64_fetch_options_t opt = { .headers_for = cookie_headers, .on_set_cookie = cookie_received,
            .on_hop = cookie_hop, .ctx = &t, .extra_headers = "Authorization: secret\r\n" };
        os64_fetch_t *f = os64_fetch_open("http://a.test/login", &opt);
        CHECK(os64_fetch_status(f) == OS64_FETCH_OK && t.asked == 2 && t.received == 3);
        CHECK(strcmp(t.values[0], "one=1; Expires=Wed, 09 Jun 2027 10:18:14 GMT") == 0);
        CHECK(strcmp(t.values[1], "two=2; Path=/") == 0 && strcmp(t.values[2], "three=3") == 0);
        CHECK(strcmp(t.hosts[0], "a.test") == 0 && strcmp(t.hosts[1], "a.test") == 0 && strcmp(t.hosts[2], "b.test") == 0);
        CHECK(strstr(request_n(0), "Cookie: first=1\r\n") && strstr(request_n(0), "Authorization: secret\r\n"));
        CHECK(strstr(request_n(1), "Cookie: second=2\r\n") && !strstr(request_n(1), "Authorization:"));
        os64_fetch_close(f); CHECK(closes == 1 && fetch_pool_idle() == 1);   // the empty redirect's kept

        // 1xx and trailers cannot set a cookie; an oversized cookie is
        // ignored whole rather than shortened into a different value.
        reset(); chunk_mode = mode;
        char wire[HTTP_LINE_MAX + 3000];
        size_t n = (size_t)snprintf(wire, sizeof(wire), "HTTP/1.1 103 Early Hints\r\nSet-Cookie: interim=1\r\n\r\n"
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nSet-Cookie: large=");
        memset(wire + n, 'a', HTTP_LINE_MAX); n += HTTP_LINE_MAX;
        snprintf(wire + n, sizeof(wire) - n, "\r\nSet-Cookie: kept=1\r\n\r\n1\r\nx\r\n0\r\nSet-Cookie: trailer=1\r\n\r\n");
        peer_add("a.test", 80, wire); t = (cookie_test_t){0};
        opt = (os64_fetch_options_t){ .on_set_cookie = cookie_received, .ctx = &t };
        f = os64_fetch_open("http://a.test/", &opt);
        uint8_t out[10];
        CHECK(slurp(f, out, sizeof(out), 0) == 1 && os64_fetch_status(f) == OS64_FETCH_OK);
        CHECK(t.received == 1 && strcmp(t.values[0], "kept=1") == 0);
        os64_fetch_close(f);
    }
    const char *bad[] = {"Cookie: x\n", "Cookie: x\r\n\r\n", "Authorization: leak\r\n",
        "Proxy-Authorization: leak\r\n", "Host: evil\r\n", "Content-Length: 0\r\n",
        "Cookie: a\r\ncookie: b\r\n", "Referer: a\r\nReferer: b\r\n"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]) + 3; i++) {
        reset(); cookie_test_t t = {0};
        if (i < sizeof(bad)/sizeof(bad[0])) t.output = bad[i];
        else if (i == sizeof(bad)/sizeof(bad[0])) t.unterminated = true;
        else if (i == sizeof(bad)/sizeof(bad[0]) + 1) t.fail = true;
        else t.output = "Cookie: duplicate\r\n";
        os64_fetch_options_t opt = { .headers_for = cookie_headers, .ctx = &t };
        if (i == sizeof(bad)/sizeof(bad[0]) + 2) opt.extra_headers = "Cookie: static\r\n";
        os64_fetch_t *f = os64_fetch_open("http://a.test/", &opt);
        CHECK(os64_fetch_status(f) == OS64_FETCH_REQUEST_FAILED && dials == 0 && t.asked == 1);
        os64_fetch_close(f);
    }
    // The lines that say who is asking go per hop too, each name once, in
    // the order written, after the static fields; a name off the list (a
    // Sec- header the list does not name) refuses the hop as any other does.
    {
        static const char who[] =
            "sec-ch-ua: \"B\";v=\"1\"\r\nsec-ch-ua-mobile: ?0\r\nsec-ch-ua-platform: \"Windows\"\r\n"
            "Upgrade-Insecure-Requests: 1\r\nSec-Fetch-Site: none\r\nSec-Fetch-Mode: navigate\r\n"
            "Sec-Fetch-User: ?1\r\nSec-Fetch-Dest: document\r\nAccept-Language: en-US,en;q=0.9\r\n"
            "Priority: u=0, i\r\nCache-Control: max-age=0\r\nCookie: c=1\r\nReferer: http://a.test/\r\n";
        reset();
        peer_add("a.test", 80, reply_len("200 OK", "", page));
        cookie_test_t t = { .output = who };
        os64_fetch_options_t opt = { .headers_for = cookie_headers, .ctx = &t, .extra_headers = "X-Static: 1\r\n" };
        os64_fetch_t *f = os64_fetch_open("http://a.test/", &opt);
        CHECK(os64_fetch_status(f) == OS64_FETCH_OK && t.asked == 1);
        const char *at = strstr(request_n(0), "X-Static: 1\r\n");
        CHECK(at != NULL && strncmp(at + strlen("X-Static: 1\r\n"), who, strlen(who)) == 0);
        os64_fetch_close(f);
        const char *refused[] = {
            "Sec-Fetch-Site: none\r\nsec-fetch-site: none\r\n",     // twice in the block
            "Sec-Purpose: prefetch\r\n",                            // a Sec- name off the list
            "Accept-Language: en\r\n",                              // beside a static one, below
        };
        for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
            reset();
            cookie_test_t r = { .output = refused[i] };
            os64_fetch_options_t ro = { .headers_for = cookie_headers, .ctx = &r };
            if (i == 2) ro.extra_headers = "Accept-Language: fr\r\n";
            f = os64_fetch_open("http://a.test/", &ro);
            CHECK(os64_fetch_status(f) == OS64_FETCH_REQUEST_FAILED && dials == 0 && r.asked == 1);
            os64_fetch_close(f);
        }
    }
    // THE WHOLE PER-HOP ROOM, used: who is asking, an 8 KB Cookie line and
    // a 4 KB Referer, one byte short of OS64_FETCH_HOP_FIELDS_MAX with its
    // NUL. It reaches the wire intact, beside a caller's block at its own
    // limit; a cap any smaller would have cut a line short, and a line cut
    // short is a refused hop.
    {
        static char block[OS64_FETCH_HOP_FIELDS_MAX];
        static char extra[OS64_FETCH_EXTRA_MAX];
        size_t n = (size_t)snprintf(block, sizeof(block), "Accept-Language: en\r\nCookie: ");
        size_t cookie_at = n;
        size_t referer_line = 9 + 14 + 4000 + 2;                // "Referer: " + "http://a.test/" + path + CR LF
        size_t fill = sizeof(block) - 1 - n - 2 - referer_line;
        memset(block + n, 'c', fill);
        n += fill;
        n += (size_t)snprintf(block + n, sizeof(block) - n, "\r\nReferer: http://a.test/");
        memset(block + n, 'r', 4000);
        n += 4000;
        n += (size_t)snprintf(block + n, sizeof(block) - n, "\r\n");
        CHECK(n == sizeof(block) - 1 && fill > 8000);
        size_t e = (size_t)snprintf(extra, sizeof(extra), "X-Big: ");
        memset(extra + e, 'x', sizeof(extra) - e - 3);
        memcpy(extra + sizeof(extra) - 3, "\r\n", 3);
        reset();
        peer_add("a.test", 80, reply_len("200 OK", "", page));
        cookie_test_t t = { .output = block };
        os64_fetch_options_t opt = { .headers_for = cookie_headers, .ctx = &t, .extra_headers = extra };
        os64_fetch_t *f = os64_fetch_open("http://a.test/", &opt);
        CHECK(os64_fetch_status(f) == OS64_FETCH_OK && t.asked == 1);
        CHECK(strstr(request_n(0), block) != NULL && strstr(request_n(0), extra) != NULL);
        CHECK(strncmp(block + cookie_at - 8, "Cookie: c", 9) == 0);
        os64_fetch_close(f);
    }
    // Both sources, secure/direct vs unencrypted HTTP and HTTPS-via-proxy.
    for (int dynamic = 0; dynamic < 2; dynamic++) {
        for (int transport = 0; transport < 3; transport++) {
            reset();
            if (transport == 2) set_env("https_proxy", "http://proxy.test:3128/");
            peer_add(transport == 2 ? "proxy.test" : "a.test", transport == 2 ? 3128 : transport == 1 ? 443 : 80,
                     reply_len("200 OK", "", page));
            cookie_test_t t = { .output = "rEfErEr:\tHtTpS://from.test/private\r\n" };
            os64_fetch_options_t opt = { .ctx = &t };
            if (dynamic) opt.headers_for = cookie_headers;
            else opt.extra_headers = t.output;
            os64_fetch_t *f = os64_fetch_open(transport ? "https://a.test/" : "http://a.test/", &opt);
            CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
            CHECK((strstr(request_n(0), "HtTpS://from.test/private") != NULL) == (transport == 1));
            if (dynamic) CHECK(t.encrypted == (transport == 1));
            os64_fetch_close(f);
        }
    }
    // Set-Cookie must expose wire security even when the URL says HTTPS.
    // No jar lives here: the callback receives the unchanged Secure attribute
    // plus the fact libway needs to decide whether to accept it.
    for (int mode = 0; mode < 3; mode++) for (int transport = 0; transport < 3; transport++) {
        reset(); chunk_mode = mode;
        if (transport == 2) set_env("https_proxy", "http://proxy.test:3128/");
        peer_add(transport == 2 ? "proxy.test" : "a.test", transport == 2 ? 3128 : transport == 1 ? 443 : 80,
                 reply_len("200 OK", "Set-Cookie: session=secret; Secure\r\n", page));
        cookie_test_t t = {0};
        os64_fetch_options_t opt = {.on_set_cookie = cookie_received, .ctx = &t};
        os64_fetch_t *f = os64_fetch_open(transport ? "https://a.test/" : "http://a.test/", &opt);
        CHECK(os64_fetch_status(f) == OS64_FETCH_OK && t.received == 1);
        CHECK(t.received_encrypted[0] == (transport == 1));
        CHECK(os64_fetch_head(f)->encrypted == t.received_encrypted[0]);
        CHECK(strcmp(t.values[0], "session=secret; Secure") == 0 && strcmp(t.hosts[0], "a.test") == 0);
        CHECK(strcmp(os64_fetch_head(f)->url.scheme, transport ? "https" : "http") == 0);
        os64_fetch_close(f);
    }
    // A protocol error accompanying plaintext must not set a cookie from
    // that read, even when the HTTP head itself fits in the prefix.
    reset(); cookie_test_t terminal = {0};
    peer_t *bad_tls = peer_add("a.test", 443, reply_len("200 OK", "Set-Cookie: bad=1\r\n", ""));
    bad_tls->tls_bad_prefix = true;
    os64_fetch_options_t guarded = { .on_set_cookie = cookie_received, .ctx = &terminal };
    os64_fetch_t *guard = os64_fetch_open("https://a.test/", &guarded);
    CHECK(os64_fetch_status(guard) == OS64_FETCH_BAD_HEAD && terminal.received == 0);
    os64_fetch_close(guard);
    for (int where = 0; where < 2; where++) {
        reset(); cookie_test_t stop = { .cancel_header = where == 0, .cancel_cookie = where == 1 };
        peer_add("a.test", 80, reply_len("302 Found", "Set-Cookie: first=1\r\nSet-Cookie: second=2\r\nLocation: http://b.test/\r\n", ""));
        guarded = (os64_fetch_options_t){ .headers_for = cookie_headers, .on_set_cookie = cookie_received,
                                        .cancelled = cookie_cancel, .ctx = &stop };
        guard = os64_fetch_open("http://a.test/", &guarded);
        CHECK(os64_fetch_status(guard) == OS64_FETCH_INTERRUPTED);
        CHECK(dials == where && closes == where && stop.received == (unsigned)where);
        os64_fetch_close(guard);
    }
    // Later malformed fields fail the fetch, but prior cookie delivery is
    // deliberately streaming, not a transaction that can be rolled back.
    reset(); cookie_test_t t = {0};
    peer_add("a.test", 80, "HTTP/1.1 200 OK\r\nSet-Cookie: kept=1\r\nBad Header: x\r\n\r\n");
    os64_fetch_options_t opt = { .on_set_cookie = cookie_received, .ctx = &t, .cancelled = cookie_cancel };
    os64_fetch_t *f = os64_fetch_open("http://a.test/", &opt);
    CHECK(os64_fetch_status(f) == OS64_FETCH_BAD_HEAD && t.received == 1);
    os64_fetch_close(f);
}

// What a reply said about keeping it (fetch.h, os64_fetch_keep_t): each
// field as written, a repeated one joined, a redirect's not carried to its
// target, one too long for its field marked; and a 304 to a caller's
// If-None-Match is a head with an empty body.
// The caching fields each redirect said, as its hop saw them.
static char s_hop_cc[OS64_FETCH_KEEP_FIELD];
static os64_fetch_verdict_t keep_hop(void *ctx, const os64_fetch_hop_t *hop)
{
    (void)ctx;
    snprintf(s_hop_cc, sizeof(s_hop_cc), "%s", hop->keep.cache_control);
    return OS64_FETCH_HOP_DEFAULT;
}

static void case_keep(void)
{
    for (int mode = 0; mode < 3; mode++) {
        reset(); chunk_mode = mode;
        peer_add("a.test", 80, reply_len("301 Moved Permanently",
            "Cache-Control: max-age=60\r\nETag: \"old\"\r\nLocation: http://b.test/pic\r\n", ""));
        peer_add("b.test", 80, reply_len("200 OK",
            "Cache-Control: public\r\ncache-control:  max-age=31536000, immutable\r\n"
            "ETag: \"v1\"\r\nLast-Modified: Wed, 23 Sep 2026 20:36:58 GMT\r\n"
            "Date: Wed, 30 Sep 2026 08:55:09 GMT\r\nAge: 7\r\nVary: Accept-Encoding\r\n", page));
        os64_fetch_options_t opt = { .on_hop = keep_hop };
        s_hop_cc[0] = '\0';
        os64_fetch_t *f = os64_fetch_open("http://a.test/pic", &opt);
        const os64_fetch_head_t *h = os64_fetch_head(f);
        CHECK(h != NULL && h->status == 200 && !h->keep.unreadable);
        // The redirect's own fields reached its hop, and are not the target's.
        CHECK(strcmp(s_hop_cc, "max-age=60") == 0);
        CHECK(strcmp(h->keep.cache_control, "public, max-age=31536000, immutable") == 0);
        CHECK(strcmp(h->keep.etag, "\"v1\"") == 0);
        CHECK(strcmp(h->keep.last_modified, "Wed, 23 Sep 2026 20:36:58 GMT") == 0);
        CHECK(strcmp(h->keep.date, "Wed, 30 Sep 2026 08:55:09 GMT") == 0);
        CHECK(strcmp(h->keep.age, "7") == 0 && strcmp(h->keep.vary, "Accept-Encoding") == 0);
        CHECK(h->keep.expires[0] == '\0' && h->keep.pragma[0] == '\0');
        os64_fetch_close(f);

        // A field longer than its place: marked, never cut short.
        reset(); chunk_mode = mode;
        char extra[600];
        int n = snprintf(extra, sizeof(extra), "Cache-Control: max-age=1, ");
        memset(extra + n, 'x', 250);
        snprintf(extra + n + 250, sizeof(extra) - (size_t)n - 250, "\r\n");
        peer_add("a.test", 80, reply_len("200 OK", extra, page));
        f = os64_fetch_open("http://a.test/", &opt);
        h = os64_fetch_head(f);
        CHECK(h != NULL && h->keep.unreadable && h->keep.cache_control[0] == '\0');
        os64_fetch_close(f);

        // A caching field longer than a header line may be: omitted by the
        // parser, and still marked, so its absence is not permission.
        reset(); chunk_mode = mode;
        char longer[HTTP_LINE_MAX + 200];
        n = snprintf(longer, sizeof(longer), "Cache-Control: no-store, x=\"");
        memset(longer + n, 'x', HTTP_LINE_MAX + 50);
        snprintf(longer + n + HTTP_LINE_MAX + 50, sizeof(longer) - (size_t)n - HTTP_LINE_MAX - 50,
                 "\"\r\n");
        peer_add("a.test", 80, reply_len("200 OK", longer, page));
        f = os64_fetch_open("http://a.test/", &opt);
        h = os64_fetch_head(f);
        CHECK(h != NULL && os64_fetch_status(f) == OS64_FETCH_OK && h->keep.unreadable);
        os64_fetch_close(f);
        // A long line that is no caching field leaves them readable.
        reset(); chunk_mode = mode;
        n = snprintf(longer, sizeof(longer), "X-Padding: ");
        memset(longer + n, 'x', HTTP_LINE_MAX + 50);
        snprintf(longer + n + HTTP_LINE_MAX + 50, sizeof(longer) - (size_t)n - HTTP_LINE_MAX - 50,
                 "\r\nCache-Control: max-age=5\r\n");
        peer_add("a.test", 80, reply_len("200 OK", longer, page));
        f = os64_fetch_open("http://a.test/", &opt);
        h = os64_fetch_head(f);
        CHECK(h != NULL && !h->keep.unreadable && strcmp(h->keep.cache_control, "max-age=5") == 0);
        os64_fetch_close(f);

        // A conditional request is the caller's header, and its 304 has no
        // body whatever it says about one.
        reset(); chunk_mode = mode;
        peer_add("a.test", 80, "HTTP/1.1 304 Not Modified\r\nETag: \"v1\"\r\n"
                               "Cache-Control: max-age=600\r\nContent-Length: 40\r\n\r\n");
        opt = (os64_fetch_options_t){ .extra_headers = "If-None-Match: \"v1\"\r\n" };
        f = os64_fetch_open("http://a.test/pic", &opt);
        h = os64_fetch_head(f);
        CHECK(strstr(request_n(0), "If-None-Match: \"v1\"\r\n") != NULL);
        CHECK(h != NULL && h->status == 304 && strcmp(h->keep.cache_control, "max-age=600") == 0);
        uint8_t out[8];
        CHECK(os64_fetch_read(f, out, sizeof(out)) == 0 && os64_fetch_status(f) == OS64_FETCH_OK);
        os64_fetch_close(f);
    }
}

// A METHOD BY NAME (fetch.h's method_name), for the Fetch standard's job:
// any token on the request line, its body and Content-Length, the body's
// representation headers, no redirect followed, a HEAD's and a 205's reply
// read as bodiless, identity on request, and every final-head field heard.
typedef struct { int fields, omitted, hops; char seen[512]; } header_test_t;
static void header_heard(void *ctx, const char *name, size_t nlen, const char *value, size_t vlen)
{
    header_test_t *t = ctx;
    t->fields++;
    if (value == NULL) {
        t->omitted++;
        return;
    }
    size_t at = strlen(t->seen);
    snprintf(t->seen + at, sizeof(t->seen) - at, "%.*s=%.*s;", (int)nlen, name, (int)vlen, value);
}
static os64_fetch_verdict_t header_hop(void *ctx, const os64_fetch_hop_t *hop)
{
    (void)hop;
    ((header_test_t *)ctx)->hops++;
    return OS64_FETCH_HOP_FOLLOW;               // asked to follow; a named method still stops
}

static void case_methods(void)
{
    // Today's bug, the reason for the rest: a HEAD answered with a length
    // was framed by it, and the read waited for bytes no server sends.
    for (int mode = 0; mode < 3; mode++) {
        reset(); chunk_mode = mode;
        const char *head = "HTTP/1.1 200 OK\r\nContent-Length: 40\r\nContent-Type: text/html\r\n\r\n";
        peer_t *p = peer_add("a.test", 80, head);
        p->silent_after = strlen(head);
        os64_fetch_options_t opt = { .method_name = "HEAD" };
        os64_fetch_t *f = os64_fetch_open("http://a.test/x", &opt);
        CHECK(os64_fetch_status(f) == OS64_FETCH_OK && strncmp(request_n(0), "HEAD /x HTTP/1.1\r\n", 18) == 0);
        CHECK(strstr(request_n(0), "Content-Length") == NULL);
        uint8_t out[8];
        uint64_t before = now_ms;
        CHECK(os64_fetch_read(f, out, sizeof(out)) == 0 && os64_fetch_status(f) == OS64_FETCH_OK && now_ms == before);
        CHECK(os64_fetch_head(f)->has_length && os64_fetch_head(f)->length == 0);
        os64_fetch_close(f);
    }
    // A 205 has no body either.
    reset();
    const char *reset_reply = "HTTP/1.1 205 Reset Content\r\nContent-Length: 10\r\n\r\n";
    peer_t *p = peer_add("a.test", 80, reset_reply);
    p->silent_after = strlen(reset_reply);
    os64_fetch_t *f = os64_fetch_open("http://a.test/", NULL);
    uint8_t out[64];
    CHECK(os64_fetch_read(f, out, sizeof(out)) == 0 && os64_fetch_status(f) == OS64_FETCH_OK);
    os64_fetch_close(f);

    // Any token, its body with a length, and its representation headers.
    reset();
    peer_add("a.test", 80, reply_len("200 OK", "", "done"));
    os64_fetch_options_t opt = { .method_name = "PUT", .body = "{\"a\":1}", .body_len = 7,
                                 .body_headers = "Content-Type: application/json\r\nContent-Language: de\r\n" };
    f = os64_fetch_open("http://a.test/item", &opt);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
    CHECK(slurp(f, out, sizeof(out), 0) == 4 && memcmp(out, "done", 4) == 0);
    os64_fetch_close(f);
    CHECK(strncmp(request_n(0), "PUT /item HTTP/1.1\r\n", 20) == 0);
    CHECK(strstr(request_n(0), "Content-Length: 7\r\n") && strstr(request_n(0), "Content-Type: application/json\r\n") &&
          strstr(request_n(0), "Content-Language: de\r\n") && strstr(request_n(0), "\r\n\r\n{\"a\":1}"));
    const char *names[] = {"DELETE", "OPTIONS", "PATCH", "CHICKEN", "patch"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        reset();
        peer_add("a.test", 80, reply_len("204 No Content", "", ""));
        opt = (os64_fetch_options_t){ .method_name = names[i] };
        f = os64_fetch_open("http://a.test/", &opt);
        char line[32];
        snprintf(line, sizeof(line), "%s / HTTP/1.1\r\n", names[i]);
        CHECK(os64_fetch_status(f) == OS64_FETCH_OK && strncmp(request_n(0), line, strlen(line)) == 0);
        CHECK(strstr(request_n(0), "Content-Length") == NULL);   // no body, and not POST or PUT
        os64_fetch_close(f);
    }
    reset();
    peer_add("a.test", 80, reply_len("200 OK", "", ""));
    opt = (os64_fetch_options_t){ .method_name = "PUT" };
    f = os64_fetch_open("http://a.test/", &opt);
    CHECK(strstr(request_n(0), "Content-Length: 0\r\n") != NULL);        // an empty PUT says so
    os64_fetch_close(f);
    // An EMPTY body (a page's "") is not no body: any method says its
    // length, so the server can tell the two apart. A GET or HEAD refuses one.
    static const char *const empties[] = {"PATCH", "DELETE", "OPTIONS", "PROPFIND"};
    for (size_t i = 0; i < sizeof(empties) / sizeof(empties[0]); i++) {
        reset();
        peer_add("a.test", 80, reply_len("204 No Content", "", ""));
        opt = (os64_fetch_options_t){ .method_name = empties[i], .body = "", .body_len = 0 };
        f = os64_fetch_open("http://a.test/", &opt);
        CHECK(os64_fetch_status(f) == OS64_FETCH_OK && strstr(request_n(0), "Content-Length: 0\r\n") != NULL);
        os64_fetch_close(f);
    }
    {
        http_url_t u;
        CHECK(http_url_parse("http://a.test/", &u) == HTTP_URL_OK);
        char rendered[512];
        http_request_extras_t e = { .method_name = "HEAD", .body_present = true };
        CHECK(!http_request(rendered, sizeof(rendered), &u, false, &e));
        e = (http_request_extras_t){ .body_present = true };
        CHECK(!http_request(rendered, sizeof(rendered), &u, false, &e));
    }
    reset();
    peer_add("a.test", 80, reply_len("200 OK", "", ""));
    opt = (os64_fetch_options_t){ .method_name = "POST", .body = "x=1", .body_len = 3,
                                  .body_headers = "Content-Type: text/plain\r\n" };
    f = os64_fetch_open("http://a.test/", &opt);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && strstr(request_n(0), "Content-Length: 3\r\n") &&
          strstr(request_n(0), "\r\n\r\nx=1"));
    os64_fetch_close(f);

    // What is refused before a dial.
    const os64_fetch_options_t refused[] = {
        { .method_name = "GE T" },
        { .method_name = "" },
        { .method_name = "HEAD", .body = "x", .body_len = 1 },
        { .method_name = "GET", .body = "x", .body_len = 1 },
        { .method_name = "PUT", .content_type = "text/plain", .body = "x", .body_len = 1 },
        { .method_name = "PUT", .method = OS64_FETCH_METHOD_POST },
        { .method_name = "PUT", .body_headers = "X-Foo: 1\r\n" },
        { .method_name = "PUT", .body_headers = "Content-Type: a\nb\r\n" },
        { .method_name = "PUT", .extra_headers = "Content-Type: text/plain\r\n" },
        { .body_headers = "Content-Type: text/plain\r\n" },
        { .method_name = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" },
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        reset();
        peer_add("a.test", 80, reply_len("200 OK", "", ""));
        f = os64_fetch_open("http://a.test/", &refused[i]);
        CHECK(os64_fetch_status(f) == OS64_FETCH_REQUEST_FAILED && dials == 0);
        os64_fetch_close(f);
    }

    // No redirect is followed for a named method, whatever on_hop says: the
    // caller rewrites it by its own rule. Every final field is heard.
    reset();
    peer_add("a.test", 80, reply_len("307 Temporary Redirect", "Location: http://b.test/\r\nX-A: 1\r\n", ""));
    peer_add("b.test", 80, reply_len("200 OK", "", "b"));
    header_test_t t = {0};
    opt = (os64_fetch_options_t){ .method_name = "POST", .body = "x", .body_len = 1, .on_hop = header_hop,
                                  .on_header = header_heard, .ctx = &t };
    f = os64_fetch_open("http://a.test/", &opt);
    CHECK(os64_fetch_status(f) == OS64_FETCH_REDIRECT_STOPPED && dials == 1 && t.hops == 1);
    CHECK(os64_fetch_head(f)->status == 307 && strstr(t.seen, "Location=http://b.test/;") && strstr(t.seen, "X-A=1;"));
    os64_fetch_close(f);

    // The observer hears a line too long to read by its name alone.
    reset();
    static char longer[HTTP_LINE_MAX + 200];
    int n = snprintf(longer, sizeof(longer), "Access-Control-Allow-Origin: http://");
    memset(longer + n, 'x', HTTP_LINE_MAX + 50);
    snprintf(longer + n + HTTP_LINE_MAX + 50, sizeof(longer) - (size_t)n - HTTP_LINE_MAX - 50, "\r\nX-B: 2\r\n");
    peer_add("a.test", 80, reply_len("200 OK", longer, "ok"));
    memset(&t, 0, sizeof(t));
    opt = (os64_fetch_options_t){ .method_name = "GET", .on_header = header_heard, .ctx = &t };
    f = os64_fetch_open("http://a.test/", &opt);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && t.omitted == 1 && strstr(t.seen, "X-B=2;") &&
          strstr(t.seen, "Access-Control") == NULL);
    os64_fetch_close(f);

    // An interim head's fields are never the answer's: a 103 granting
    // Access-Control-Allow-Origin (and a line too long to read, and a
    // Set-Cookie) before a final 200 that grants nothing reaches the
    // observers as the 200's fields alone, for a GET and for an upload
    // whose head is read between its writes. The fetch job's CORS check
    // reads exactly what on_header heard.
    static char interim_wire[HTTP_LINE_MAX + 512];
    n = snprintf(interim_wire, sizeof(interim_wire),
                 "HTTP/1.1 103 Early Hints\r\nAccess-Control-Allow-Origin: *\r\nSet-Cookie: early=1\r\n"
                 "X-Early-Long: ");
    memset(interim_wire + n, 'x', HTTP_LINE_MAX + 10);
    snprintf(interim_wire + n + HTTP_LINE_MAX + 10, sizeof(interim_wire) - (size_t)n - HTTP_LINE_MAX - 10,
             "\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-Final: 1\r\n\r\nok");
    for (int upload = 0; upload < 2; upload++) {
        reset();
        peer_add("a.test", 80, interim_wire);
        memset(&t, 0, sizeof(t));
        cookie_test_t heard = {0};
        opt = (os64_fetch_options_t){ .method_name = upload ? "PUT" : "GET", .body = upload ? "x" : NULL,
                                      .body_len = upload ? 1 : 0, .on_header = header_heard, .ctx = &t };
        f = os64_fetch_open("http://a.test/", &opt);
        CHECK(os64_fetch_status(f) == OS64_FETCH_OK && os64_fetch_head(f)->status == 200);
        CHECK(strcmp(t.seen, "Content-Length=2;X-Final=1;") == 0 && t.omitted == 0);
        os64_fetch_close(f);
        reset();
        peer_add("a.test", 80, interim_wire);
        opt = (os64_fetch_options_t){ .method_name = upload ? "PUT" : "GET", .body = upload ? "x" : NULL,
                                      .body_len = upload ? 1 : 0, .on_set_cookie = cookie_received, .ctx = &heard };
        f = os64_fetch_open("http://a.test/", &opt);
        CHECK(os64_fetch_status(f) == OS64_FETCH_OK && heard.received == 0);
        os64_fetch_close(f);
    }

    // An empty Accept is sent as one; no accept at all sends none.
    reset();
    peer_add("a.test", 80, reply_len("200 OK", "", "ok"));
    opt = (os64_fetch_options_t){ .method_name = "GET", .accept = "" };
    f = os64_fetch_open("http://a.test/", &opt);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && strstr(request_n(0), "\r\nAccept: \r\n") != NULL);
    os64_fetch_close(f);
    reset();
    peer_add("a.test", 80, reply_len("200 OK", "", "ok"));
    opt = (os64_fetch_options_t){ .method_name = "GET" };
    f = os64_fetch_open("http://a.test/", &opt);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && strstr(request_n(0), "\r\nAccept:") == NULL);
    os64_fetch_close(f);

    // identity, for a Range.
    reset();
    peer_add("a.test", 80, reply_len("206 Partial Content", "", "ab"));
    opt = (os64_fetch_options_t){ .method_name = "GET", .identity = true, .extra_headers = "Range: bytes=0-1\r\n" };
    f = os64_fetch_open("http://a.test/", &opt);
    CHECK(strstr(request_n(0), "Accept-Encoding: identity\r\n") && strstr(request_n(0), "Range: bytes=0-1\r\n"));
    os64_fetch_close(f);
    reset();
    peer_add("a.test", 80, reply_len("200 OK", "", "ab"));
    f = os64_fetch_open("http://a.test/", NULL);
    CHECK(strstr(request_n(0), "Accept-Encoding: gzip, identity\r\n"));
    os64_fetch_close(f);

    // The caller's fields may be as long as OS64_FETCH_EXTRA_MAX: a bearer
    // token of 6 KiB goes; one past the cap is refused before a dial.
    static char extra[OS64_FETCH_EXTRA_MAX + 64];
    n = snprintf(extra, sizeof(extra), "Authorization: Bearer ");
    memset(extra + n, 't', 6000);
    snprintf(extra + n + 6000, sizeof(extra) - (size_t)n - 6000, "\r\n");
    reset();
    peer_add("a.test", 80, reply_len("200 OK", "", "ok"));
    opt = (os64_fetch_options_t){ .method_name = "GET", .extra_headers = extra };
    f = os64_fetch_open("http://a.test/", &opt);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK && strstr(request_n(0), extra) != NULL);
    os64_fetch_close(f);
    memset(extra + n, 't', OS64_FETCH_EXTRA_MAX);
    snprintf(extra + OS64_FETCH_EXTRA_MAX, 8, "\r\n");        // one line, a byte past the cap with its NUL
    reset();
    f = os64_fetch_open("http://a.test/", &opt);
    CHECK(os64_fetch_status(f) == OS64_FETCH_REQUEST_FAILED && dials == 0);
    os64_fetch_close(f);
}

static void case_tls_version_fallback(void)
{
    reset();
    peer_t *p=peer_add("version.test",443,"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    p->tls_default_error=OS64_TLS_PEER_CHOSE_TLS12;
    os64_fetch_t *f=os64_fetch_open("https://version.test/",NULL);
    CHECK(os64_fetch_status(f)==OS64_FETCH_OK && dials==2 && closes==1 && tls_creates==2);
    CHECK(os64_fetch_detail(f)->tls_fallback && os64_fetch_detail(f)->tls_version==0x0303);
    CHECK(trust_loads==1 && !strcmp(last_dial,"tcp!version.test!443"));
    os64_fetch_close(f); CHECK(closes==2 && tls_frees==2);
    const os64_tls_status_t failures[]={OS64_TLS_CERTIFICATE,OS64_TLS_PROTOCOL,OS64_TLS_TIMEOUT,OS64_TLS_TRUNCATED};
    for (size_t i=0;i<sizeof failures/sizeof *failures;i++) {
        reset(); p=peer_add("version.test",443,""); p->tls_default_error=failures[i];
        f=os64_fetch_open("https://version.test/",NULL);
        CHECK(os64_fetch_status(f)==OS64_FETCH_TLS_FAILED && dials==1 && closes==1);
        CHECK(os64_fetch_detail(f)->tls==failures[i] && !os64_fetch_detail(f)->tls_fallback);
        os64_fetch_close(f);
        reset(); p=peer_add("version.test",443,"");
        p->tls_default_error=OS64_TLS_PEER_CHOSE_TLS12; p->tls12_error=failures[i];
        f=os64_fetch_open("https://version.test/",NULL);
        CHECK(os64_fetch_status(f)==OS64_FETCH_TLS_FAILED && dials==2 && closes==2);
        CHECK(os64_fetch_detail(f)->tls==failures[i] && os64_fetch_detail(f)->tls_fallback);
        CHECK(conns[0].reqlen==0 && conns[1].reqlen==0);
        os64_fetch_close(f);
    }
    reset(); p=peer_add("version.test",443,"");
    p->tls_default_error=p->tls12_error=OS64_TLS_PEER_CHOSE_TLS12;
    f=os64_fetch_open("https://version.test/",NULL);
    CHECK(os64_fetch_status(f)==OS64_FETCH_TLS_FAILED && dials==2 && closes==2);
    CHECK(os64_fetch_detail(f)->tls_fallback); os64_fetch_close(f);
}

// ── Keep-alive: one connection, many requests (WIRE.md § 2) ─────────────

static const char *const kFirst = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst";
static const char *const kSecond = "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecond";

// One fetch, read to its end and closed: its final status, its body in
// `out`, and its reason in `why`.
static os64_fetch_status_t fetch_all(const char *url, const os64_fetch_options_t *opt, char *out,
                                     size_t cap, char *why, size_t why_cap)
{
    os64_fetch_t *f = os64_fetch_open(url, opt);
    CHECK(f != NULL);
    size_t n = os64_fetch_head(f) ? slurp(f, (uint8_t *)out, cap - 1, 0) : 0;
    out[n] = '\0';
    os64_fetch_status_t st = os64_fetch_status(f);
    if (why != NULL)
        snprintf(why, why_cap, "%s", os64_fetch_reason(f));
    os64_fetch_close(f);
    return st;
}

// How many requests went on the i-th connection dialled.
static int requests_on(int i)
{
    int n = 0;
    for (const char *p = request_n(i); (p = strstr(p, " HTTP/1.1\r\nHost: ")) != NULL; p++) n++;
    return n;
}

static void case_keepalive_reuse(void)
{
    // Each first answer leaves the connection at a message's end, and the
    // second request goes on the same one: a length-framed body, a chunked
    // one with trailers, and the three bodiless answers (HEAD's, a 204, a
    // 304 whose Content-Length describes what was not sent), each closed
    // with and without its body read.
    static const struct { const char *first, *method, *body; } ok[] = {
        { "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst", NULL, "first" },
        { "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nfi\r\n3\r\nrst\r\n0\r\n"
          "X-Trailer: 1\r\nX-Other: 2\r\n\r\n", NULL, "first" },
        { "HTTP/1.1 200 OK\r\nContent-Length: 999\r\n\r\n", "HEAD", "" },
        { "HTTP/1.1 204 No Content\r\n\r\n", NULL, "" },
        { "HTTP/1.1 304 Not Modified\r\nContent-Length: 999\r\nETag: \"v\"\r\n\r\n", NULL, "" },
        { "HTTP/1.1 205 Reset Content\r\nContent-Length: 0\r\n\r\n", NULL, "" },
        { "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", NULL, "" },
        // A HEAD's 205 sends nothing, whatever its length says.
        { "HTTP/1.1 205 Reset Content\r\nContent-Length: 5\r\n\r\n", "HEAD", "" },
    };
    for (int tls = 0; tls < 2; tls++)
        for (int mode = 0; mode < 3; mode++)
            for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
                for (int read = 0; read < 2; read++) {
                    reset(); chunk_mode = mode;
                    const char *answers[] = { ok[i].first, kSecond, NULL };
                    peer_answers("keep.test", tls ? 443 : 80, answers);
                    const char *url = tls ? "https://keep.test/a" : "http://keep.test/a";
                    os64_fetch_options_t opt = { .method_name = ok[i].method };
                    os64_fetch_t *f = os64_fetch_open(url, &opt);
                    CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
                    char out[64] = {0};
                    // Unread, only a body that was never sent is whole.
                    bool empty = ok[i].body[0] == '\0';
                    if (read || !empty) {
                        size_t n = slurp(f, (uint8_t *)out, sizeof(out) - 1, mode == 1 ? 1 : 0);
                        CHECK(n == strlen(ok[i].body) && memcmp(out, ok[i].body, n) == 0);
                        CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
                    }
                    os64_fetch_close(f);
                    CHECK(closes == 0 && fetch_pool_idle() == 1);
                    char why[256];
                    CHECK(fetch_all(url, NULL, out, sizeof(out), why, sizeof(why)) == OS64_FETCH_OK);
                    CHECK(strcmp(out, "second") == 0 && dials == 1 && requests_on(0) == 2);
                    CHECK(strstr(why, "retried") == NULL);
                    CHECK(trust_loads == tls);           // the kept connection needs no store
                    CHECK(fetch_pool_idle() == 1 && closes == 0);
                }
}

static void case_keepalive_refused(void)
{
    // Each of these leaves the connection where it may not carry another
    // request: closed when the fetch is, and the next request dials.
    static const struct { const char *first; size_t read; const char *why; } no[] = {
        { "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 5\r\n\r\nfirst", 99, "the reply's close" },
        { "HTTP/1.1 200 OK\r\nConnection: Keep-Alive, CLOSE\r\nContent-Length: 5\r\n\r\nfirst", 99, "close in a list" },
        { "HTTP/1.1 200 OK\r\nConnection: x\r\nconnection: close\r\nContent-Length: 5\r\n\r\nfirst", 99, "close on a second line" },
        { "HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nfirst", 99, "a 1.0 reply" },
        { "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 5\r\n\r\nfirst", 99, "a 1.0 keep-alive" },
        { "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst", 2, "a body abandoned" },
        { "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nfirst\r\n0\r\n\r\n", 3, "a chunked body abandoned" },
        { "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", 99, "a framing refused" },
        { "HTTP/1.1 200 OK\r\nContent-Encoding: br\r\nContent-Length: 5\r\n\r\nfirst", 99, "a coding refused" },
        { "HTTP/1.1 205 Reset Content\r\nContent-Length: 5\r\n\r\nfirst", 99, "a 205 that sent bytes" },
        { "HTTP/1.1 200 OK\r\n\r\nfirst", 99, "a body read to the close" },
        // Framed whole on the wire, and still a fetch that failed.
        { "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 5\r\n\r\nfirst", 99,
          "a gzip body that did not decode" },
    };
    for (int tls = 0; tls < 2; tls++)
        for (int mode = 0; mode < 3; mode++)
            for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++) {
                reset(); chunk_mode = mode;
                const char *answers[] = { no[i].first, kSecond, NULL };
                peer_t *p = peer_answers("keep.test", tls ? 443 : 80, answers);
                // A body framed by the close ends only at the close.
                if (strstr(no[i].first, "Length") == NULL && strstr(no[i].first, "chunked") == NULL)
                    p->hangup_after = 1;
                const char *url = tls ? "https://keep.test/a" : "http://keep.test/a";
                os64_fetch_t *f = os64_fetch_open(url, NULL);
                if (os64_fetch_head(f) != NULL) {
                    char out[64];
                    size_t at = 0;
                    while (at < no[i].read) {
                        int64_t n = os64_fetch_read(f, out, no[i].read - at < sizeof(out) ? no[i].read - at : sizeof(out));
                        if (n <= 0) break;
                        at += (size_t)n;
                    }
                }
                os64_fetch_close(f);
                if (fetch_pool_idle() != 0 || closes != 1)
                    fprintf(stderr, "kept after %s (tls %d, mode %d)\n", no[i].why, tls, mode);
                CHECK(fetch_pool_idle() == 0 && closes == 1);
                char out[64];
                fetch_all(url, NULL, out, sizeof(out), NULL, 0);
                CHECK(dials == 2);
            }

    // The transport's own failure: authenticated bytes beside a TLS protocol
    // failure are not a connection to keep.
    reset();
    const char *answers[] = { kFirst, kSecond, NULL };
    peer_t *p = peer_answers("keep.test", 443, answers);
    p->tls_bad_prefix = true;
    char out[64];
    fetch_all("https://keep.test/a", NULL, out, sizeof(out), NULL, 0);
    CHECK(fetch_pool_idle() == 0 && closes == 1);

    // The request never says Connection: close, and says keep-alive only
    // when asked.
    reset();
    peer_answers("keep.test", 80, answers);
    os64_fetch_options_t ka = { .keep_alive = true };
    fetch_all("http://keep.test/a", &ka, out, sizeof(out), NULL, 0);
    fetch_all("http://keep.test/b", NULL, out, sizeof(out), NULL, 0);
    CHECK(dials == 1 && strstr(request_n(0), "\r\nConnection: keep-alive\r\n") != NULL);
    CHECK(strstr(strstr(request_n(0), "GET /b"), "Connection") == NULL);
    CHECK(strstr(request_n(0), "close") == NULL);
}

static void case_keepalive_hostile(void)
{
    char out[64], why[256];
    for (int tls = 0; tls < 2; tls++)
        for (int mode = 0; mode < 3; mode++) {
            const char *url = tls ? "https://evil.test/" : "http://evil.test/";
            uint16_t port = tls ? 443 : 80;

            // A SMUGGLED REPLY: a body that says two bytes, and a whole
            // second reply behind it in the same answer. Whether its bytes
            // are already read (leftover at the put) or still on the wire
            // (the probe at the take), the next request never reads it.
            reset(); chunk_mode = mode;
            const char *smuggle[] = { "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi"
                                      "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nevil", kSecond, NULL };
            peer_answers("evil.test", port, smuggle);
            CHECK(fetch_all(url, NULL, out, sizeof(out), NULL, 0) == OS64_FETCH_OK && !strcmp(out, "hi"));
            CHECK(fetch_all(url, NULL, out, sizeof(out), NULL, 0) == OS64_FETCH_OK);
            CHECK(strcmp(out, "evil") != 0 && dials == 2 && requests_on(0) == 1);

            // A LENGTH THAT LIES, past the bytes sent, on a server that keeps
            // the connection: the read waits out its patience, and the
            // connection is not kept.
            reset(); chunk_mode = mode;
            const char *lie[] = { "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello", kSecond, NULL };
            peer_answers("evil.test", port, lie);
            os64_fetch_options_t quick = { .idle_ms = 500 };
            CHECK(fetch_all(url, &quick, out, sizeof(out), NULL, 0) == OS64_FETCH_SILENT);
            CHECK(fetch_pool_idle() == 0 && closes == 1);

            // A 1xx FLOOD: more interim replies than a fetch reads.
            reset(); chunk_mode = mode;
            static char flood[1024];
            size_t at = 0;
            for (int i = 0; i < HTTP_INTERIM_MAX + 1; i++)
                at += (size_t)snprintf(flood + at, sizeof(flood) - at, "HTTP/1.1 103 Early Hints\r\n\r\n");
            snprintf(flood + at, sizeof(flood) - at, "%s", kFirst);
            const char *floods[] = { flood, kSecond, NULL };
            peer_answers("evil.test", port, floods);
            CHECK(fetch_all(url, NULL, out, sizeof(out), NULL, 0) == OS64_FETCH_BAD_HEAD);
            CHECK(fetch_pool_idle() == 0 && closes == 1);
            // ...and exactly as many as it reads is a reply, and kept.
            reset(); chunk_mode = mode;
            at = 0;
            for (int i = 0; i < HTTP_INTERIM_MAX; i++)
                at += (size_t)snprintf(flood + at, sizeof(flood) - at, "HTTP/1.1 103 Early Hints\r\n\r\n");
            snprintf(flood + at, sizeof(flood) - at, "%s", kFirst);
            peer_answers("evil.test", port, floods);
            CHECK(fetch_all(url, NULL, out, sizeof(out), NULL, 0) == OS64_FETCH_OK && !strcmp(out, "first"));
            CHECK(fetch_pool_idle() == 1);

            // A SERVER THAT NEVER CLOSES: a body framed by nothing on a
            // connection the server holds open is never whole; the read waits
            // out its patience and nothing is kept.
            reset(); chunk_mode = mode;
            const char *endless[] = { "HTTP/1.1 200 OK\r\n\r\nand on", kSecond, NULL };
            peer_answers("evil.test", port, endless);
            CHECK(fetch_all(url, &quick, out, sizeof(out), why, sizeof(why)) == OS64_FETCH_SILENT);
            CHECK(fetch_pool_idle() == 0 && closes == 1);
            // ...and one that keeps an idle connection forever is closed by
            // the pool once sixty seconds have gone, not used again.
            reset(); chunk_mode = mode;
            const char *answers[] = { kFirst, kSecond, NULL };
            peer_answers("evil.test", port, answers);
            fetch_all(url, NULL, out, sizeof(out), NULL, 0);
            now_ms += FETCH_POOL_IDLE_MS - 1;
            CHECK(fetch_all(url, NULL, out, sizeof(out), NULL, 0) == OS64_FETCH_OK && !strcmp(out, "second"));
            CHECK(dials == 1);
            now_ms += FETCH_POOL_IDLE_MS;
            CHECK(fetch_all(url, NULL, out, sizeof(out), NULL, 0) == OS64_FETCH_OK && !strcmp(out, "first"));
            CHECK(dials == 2 && closes == 1);
        }
}

static bool flag_cancelled(void *ctx) { return *(bool *)ctx; }

static void case_keepalive_owner(void)
{
    // CANCEL AND STOP: a fetch its owner cancels closes its connection,
    // whether the cancel lands mid-body or after the body was whole.
    char out[64];
    for (int tls = 0; tls < 2; tls++)
        for (int when = 0; when < 2; when++) {
            reset();
            const char *answers[] = { kFirst, kSecond, NULL };
            peer_answers("own.test", tls ? 443 : 80, answers);
            bool stop = false;
            os64_fetch_options_t opt = { .cancelled = flag_cancelled, .ctx = &stop };
            os64_fetch_t *f = os64_fetch_open(tls ? "https://own.test/" : "http://own.test/", &opt);
            CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
            if (when == 0) {
                CHECK(os64_fetch_read(f, out, 2) == 2);
                stop = true;
                CHECK(os64_fetch_read(f, out, sizeof(out)) < 0 && os64_fetch_status(f) == OS64_FETCH_INTERRUPTED);
            } else {
                CHECK(slurp(f, (uint8_t *)out, sizeof(out), 0) == 5 && os64_fetch_status(f) == OS64_FETCH_OK);
                stop = true;           // the window's stop, as the fetch is closed
            }
            os64_fetch_close(f);
            CHECK(fetch_pool_idle() == 0 && closes == 1);
        }

    // A TLS CONNECTION IS KEPT FOR ITS TRUST, and no other: the caller's
    // store, another store, and the library's own loading are three keys.
    static char store_a, store_b;
    os64_tls_trust *a = (os64_tls_trust *)&store_a, *b = (os64_tls_trust *)&store_b;
    reset();
    const char *many[] = { kFirst, kFirst, kFirst, kFirst, kFirst, kFirst, NULL };
    peer_answers("trust.test", 443, many);
    os64_fetch_options_t with_a = { .trust = a }, with_b = { .trust = b };
    fetch_all("https://trust.test/", &with_a, out, sizeof(out), NULL, 0);
    fetch_all("https://trust.test/", &with_b, out, sizeof(out), NULL, 0);
    CHECK(dials == 2);
    fetch_all("https://trust.test/", NULL, out, sizeof(out), NULL, 0);
    CHECK(dials == 3 && trust_loads == 1);
    fetch_all("https://trust.test/", &with_a, out, sizeof(out), NULL, 0);
    fetch_all("https://trust.test/", &with_b, out, sizeof(out), NULL, 0);
    fetch_all("https://trust.test/", NULL, out, sizeof(out), NULL, 0);
    CHECK(dials == 3 && trust_loads == 1 && fetch_pool_idle() == 3);
    CHECK(requests_on(0) == 2 && requests_on(1) == 2 && requests_on(2) == 2);
    // ...and a plain connection to the same host and port is not a TLS one.
    reset();
    peer_answers("trust.test", 443, many);
    fetch_all("https://trust.test/", NULL, out, sizeof(out), NULL, 0);
    fetch_all("http://trust.test:443/", NULL, out, sizeof(out), NULL, 0);
    CHECK(dials == 2);

    // PROXIES: a plain proxy's connection carries any host it serves, and is
    // the proxy's alone: a direct request to another host does not share it.
    reset();
    set_env("http_proxy", "proxy.test:3128");
    peer_answers("proxy.test", 3128, many);
    fetch_all("http://one.test/x", NULL, out, sizeof(out), NULL, 0);
    fetch_all("http://two.test/y", NULL, out, sizeof(out), NULL, 0);
    CHECK(dials == 1 && strstr(request_n(0), "GET http://one.test/x HTTP/1.1\r\nHost: one.test\r\n"));
    CHECK(strstr(request_n(0), "GET http://two.test/y HTTP/1.1\r\nHost: two.test\r\n"));
    CHECK(strstr(request_n(0), "Proxy-Connection") == NULL);
    clear_env();
    peer_answers("one.test", 80, many);
    fetch_all("http://one.test/x", NULL, out, sizeof(out), NULL, 0);
    CHECK(dials == 2 && strcmp(last_dial, "tcp!one.test!80") == 0);
}

static void case_keepalive_caps(void)
{
    char out[64];
    // SIX FOR ONE KEY: seven fetches open at once to one host, closed in
    // order, leave six kept and the first one closed — the least recent.
    reset();
    const char *many[] = { kFirst, kFirst, kFirst, NULL };
    peer_answers("cap.test", 80, many);
    os64_fetch_t *f[FETCH_POOL_PER_KEY + 1];
    for (int i = 0; i <= FETCH_POOL_PER_KEY; i++)
        f[i] = os64_fetch_open("http://cap.test/", NULL);
    for (int i = 0; i <= FETCH_POOL_PER_KEY; i++) {
        CHECK(slurp(f[i], (uint8_t *)out, sizeof(out), 0) == 5);
        now_ms++;
        os64_fetch_close(f[i]);
    }
    CHECK(dials == FETCH_POOL_PER_KEY + 1 && fetch_pool_idle() == FETCH_POOL_PER_KEY);
    CHECK(closes == 1 && !conns[0].open && conns[1].open);
    // The next request takes the most recently kept.
    fetch_all("http://cap.test/", NULL, out, sizeof(out), NULL, 0);
    CHECK(requests_on(FETCH_POOL_PER_KEY) == 2 && requests_on(1) == 1);

    // THE WHOLE POOL'S CAP: one connection to each of one host more than it
    // holds, and the first kept is the one let go.
    reset();
    static char names[FETCH_POOL_TOTAL + 1][32];
    for (int i = 0; i <= FETCH_POOL_TOTAL; i++) {
        snprintf(names[i], sizeof(names[i]), "h%d.test", i);
        peer_answers(names[i], 80, many);
    }
    for (int i = 0; i <= FETCH_POOL_TOTAL; i++) {
        char url[64];
        snprintf(url, sizeof(url), "http://%s/", names[i]);
        now_ms++;
        fetch_all(url, NULL, out, sizeof(out), NULL, 0);
    }
    CHECK(fetch_pool_idle() == FETCH_POOL_TOTAL && closes == 1 && !conns[0].open);
    fetch_all("http://h1.test/", NULL, out, sizeof(out), NULL, 0);
    fetch_all("http://h0.test/", NULL, out, sizeof(out), NULL, 0);
    CHECK(dials == FETCH_POOL_TOTAL + 2 && requests_on(1) == 2);

    // OUT OF HANDLES: the pool at its cap and a task with no handle left
    // for another connection. The dial is refused, a kept connection is let
    // go — the least recent — and the dial tried once more, which succeeds.
    reset();
    for (int i = 0; i <= FETCH_POOL_TOTAL; i++)
        peer_answers(names[i], 80, many);
    for (int i = 0; i < FETCH_POOL_TOTAL; i++) {
        char url[64];
        snprintf(url, sizeof(url), "http://%s/", names[i]);
        now_ms++;
        fetch_all(url, NULL, out, sizeof(out), NULL, 0);
    }
    CHECK(fetch_pool_idle() == FETCH_POOL_TOTAL && closes == 0);
    open_limit = FETCH_POOL_TOTAL;
    char url[64], why[256];
    snprintf(url, sizeof(url), "http://%s/", names[FETCH_POOL_TOTAL]);
    CHECK(fetch_all(url, NULL, out, sizeof(out), why, sizeof(why)) == OS64_FETCH_OK && !strcmp(out, "first"));
    // The least recent (the first host's) went: asking it again dials.
    CHECK(dials == FETCH_POOL_TOTAL + 2 && closes == 1 && fetch_pool_idle() == FETCH_POOL_TOTAL);
    open_limit = 0;
    fetch_all("http://h0.test/", NULL, out, sizeof(out), NULL, 0);
    CHECK(dials == FETCH_POOL_TOTAL + 3);
    // A CLOCK THAT WENT BACKWARDS: a connection kept "later" than now has
    // an age nobody can know, and is closed rather than trusted.
    reset();
    peer_answers("clock.test", 80, many);
    now_ms = 5000;
    fetch_all("http://clock.test/", NULL, out, sizeof(out), NULL, 0);
    CHECK(fetch_pool_idle() == 1);
    now_ms = 4000;
    fetch_all("http://clock.test/", NULL, out, sizeof(out), NULL, 0);
    CHECK(dials == 2 && closes == 1 && requests_on(0) == 1);
    reset();
    for (int i = 0; i <= FETCH_POOL_TOTAL; i++)
        peer_answers(names[i], 80, many);
    // ...and with nothing kept to let go, the refusal is the fetch's.
    fetch_pool_drain();
    open_limit = 1;
    conns[1].open = true;              // a handle the program holds itself
    CHECK(fetch_all(url, NULL, out, sizeof(out), why, sizeof(why)) == OS64_FETCH_DIAL_FAILED);
    conns[1].open = false;
}

// Every final-head field an observer hears, counted.
static void count_header(void *ctx, const char *name, size_t name_len, const char *value, size_t value_len)
{
    (void)name; (void)name_len; (void)value; (void)value_len;
    ((cookie_test_t *)ctx)->headers++;
}

static void case_keepalive_retry(void)
{
    char out[64], why[256];
    for (int tls = 0; tls < 2; tls++) {
        const char *url = tls ? "https://again.test/" : "http://again.test/";
        uint16_t port = tls ? 443 : 80;
        const char *answers[] = { kFirst, kSecond, NULL };

        // A KEPT CONNECTION THE SERVER CLOSED, found by the request: a GET
        // goes once more on a fresh connection, and the reason says so.
        // Four ways to find it: the close seen as the request is written
        // (the upload's poll), a write that breaks, and — the request
        // written whole first — a close or a reset that only the reply's
        // read finds.
        for (int how = 0; how < 4; how++) {
            reset();
            peer_t *p = peer_answers("again.test", port, answers);
            if (how == 1) p->fail_write_at = 2; else p->drop_request = 2;
            p->drop_unseen = how >= 2 ? how - 1 : 0;
            CHECK(fetch_all(url, NULL, out, sizeof(out), why, sizeof(why)) == OS64_FETCH_OK);
            CHECK(strstr(why, "retried") == NULL);
            CHECK(fetch_all(url, NULL, out, sizeof(out), why, sizeof(why)) == OS64_FETCH_OK);
            CHECK(strcmp(out, "first") == 0 && dials == 2);
            CHECK(strstr(why, "retried once on a fresh connection") != NULL);
        }

        // Each idempotent method the same; a POST and a method the list does
        // not name fail as they would have, sent once.
        static const struct { const char *method; bool again; } methods[] = {
            {"GET", true}, {"HEAD", true}, {"OPTIONS", true}, {"PUT", true}, {"DELETE", true},
            {"POST", false}, {"PATCH", false}, {"get", false},
        };
        for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
            reset();
            peer_t *p = peer_answers("again.test", port, answers);
            p->drop_request = 2;
            fetch_all(url, NULL, out, sizeof(out), NULL, 0);
            os64_fetch_options_t opt = { .method_name = methods[i].method };
            os64_fetch_status_t st = fetch_all(url, &opt, out, sizeof(out), why, sizeof(why));
            CHECK((st == OS64_FETCH_OK) == methods[i].again);
            CHECK(dials == (methods[i].again ? 2 : 1));
            CHECK((strstr(why, "retried") != NULL) == methods[i].again);
            CHECK(requests_on(0) == 2);
        }
        // A PUT's body goes again with it, whole: the server that closed may
        // never have read the first.
        reset();
        peer_t *put_peer = peer_answers("again.test", port, answers);
        put_peer->drop_request = 2;
        put_peer->drop_unseen = 1;
        fetch_all(url, NULL, out, sizeof(out), NULL, 0);
        os64_fetch_options_t put = { .method_name = "PUT", .body = "a=1&b=2", .body_len = 7 };
        CHECK(fetch_all(url, &put, out, sizeof(out), why, sizeof(why)) == OS64_FETCH_OK);
        CHECK(dials == 2 && strstr(why, "retried") != NULL);
        const char *again_put = request_n(1);
        CHECK(strncmp(again_put, "PUT / HTTP/1.1\r\n", 16) == 0 && strstr(again_put, "Content-Length: 7\r\n") &&
              strcmp(again_put + strlen(again_put) - 11, "\r\n\r\na=1&b=2") == 0);
        // The POST of the options' own method, with a body, is not sent twice.
        reset();
        peer_t *p = peer_answers("again.test", port, answers);
        p->drop_request = 2;
        fetch_all(url, NULL, out, sizeof(out), NULL, 0);
        os64_fetch_options_t post = { .method = OS64_FETCH_METHOD_POST, .body = "x=1", .body_len = 3 };
        // (Plain, the closed connection is found by the upload's own
        // lookahead, REQUEST_FAILED; over TLS, by the head, BAD_HEAD.)
        CHECK(fetch_all(url, &post, out, sizeof(out), why, sizeof(why)) != OS64_FETCH_OK);
        CHECK(dials == 1 && strstr(why, "retried") == NULL);

        // A FRESH CONNECTION IS NEVER RETRIED, whatever the method.
        reset();
        p = peer_answers("again.test", port, answers);
        p->drop_request = 1;
        CHECK(fetch_all(url, NULL, out, sizeof(out), why, sizeof(why)) != OS64_FETCH_OK);
        CHECK(dials == 1 && strstr(why, "retried") == NULL);

        // TWO REQUESTS, EACH RETRIED: a redirect from one host to another,
        // and a dead kept connection waiting at both. The reason counts them.
        reset();
        const char *to_b[] = { tls ? "HTTP/1.1 302 Found\r\nLocation: https://b.test/\r\nContent-Length: 0\r\n\r\n"
                                   : "HTTP/1.1 302 Found\r\nLocation: http://b.test/\r\nContent-Length: 0\r\n\r\n",
                               NULL };
        peer_t *pa = peer_answers("a.test", port, to_b), *pb = peer_answers("b.test", port, answers);
        pa->drop_request = pb->drop_request = 2;
        pa->drop_unseen = pb->drop_unseen = 1;
        os64_fetch_options_t stop = { .method_name = "GET" };       // a method by name stops at the 302
        fetch_all(tls ? "https://b.test/" : "http://b.test/", NULL, out, sizeof(out), NULL, 0);
        fetch_all(tls ? "https://a.test/" : "http://a.test/", &stop, out, sizeof(out), NULL, 0);
        CHECK(fetch_pool_idle() == 2);
        CHECK(fetch_all(tls ? "https://a.test/" : "http://a.test/", NULL, out, sizeof(out), why, sizeof(why)) ==
              OS64_FETCH_OK && !strcmp(out, "first") && dials == 4);
        CHECK(strstr(why, "(2 requests retried, each once on a fresh connection)") != NULL);

        // A 408 ON THE KEPT CONNECTION: the server timed it out and says
        // so. A GET goes again on a fresh one, and nothing of the 408 —
        // not a header, not a cookie — reaches the caller; a POST hears
        // the 408 as its answer; and a fresh connection's 408 is an answer.
        for (int post = 0; post < 2; post++) {
            reset();
            const char *timed_out[] = { kFirst, "HTTP/1.1 408 Request Timeout\r\nSet-Cookie: stale=1\r\n"
                                                "X-From: the-408\r\nContent-Length: 0\r\n\r\n", NULL };
            peer_answers("again.test", port, timed_out);
            fetch_all(url, NULL, out, sizeof(out), NULL, 0);
            cookie_test_t t = { .output = "" };
            os64_fetch_options_t opt = { .on_set_cookie = cookie_received, .on_header = count_header, .ctx = &t };
            if (post) { opt.method = OS64_FETCH_METHOD_POST; opt.body = "x=1"; opt.body_len = 3; }
            os64_fetch_t *f = os64_fetch_open(url, &opt);
            CHECK(f != NULL && os64_fetch_head(f) != NULL);
            CHECK(os64_fetch_head(f)->status == (post ? 408 : 200));
            // The 408's three fields and its cookie, or kFirst's one field.
            CHECK(dials == (post ? 1 : 2) && t.received == (post ? 1u : 0u) && t.headers == (post ? 3u : 1u));
            CHECK((strstr(os64_fetch_reason(f), "retried") != NULL) == !post);
            os64_fetch_close(f);
        }
        reset();
        const char *fresh408[] = { "HTTP/1.1 408 Request Timeout\r\nContent-Length: 0\r\n\r\n", NULL };
        peer_answers("again.test", port, fresh408);
        CHECK(fetch_all(url, NULL, out, sizeof(out), why, sizeof(why)) == OS64_FETCH_OK && dials == 1 &&
              strstr(why, "retried") == NULL);

        // ONCE: the fresh connection failing too is the fetch's failure.
        reset();
        p = peer_answers("again.test", port, answers);
        p->drop_request = 2;
        fetch_all(url, NULL, out, sizeof(out), NULL, 0);
        p->refuse = true;
        CHECK(fetch_all(url, NULL, out, sizeof(out), why, sizeof(why)) == OS64_FETCH_DIAL_FAILED);
        CHECK(dials == 2 && strstr(why, "retried once on a fresh connection") != NULL);

        // A SILENCE is not a closed connection: the request may be in hand.
        reset();
        const char *one[] = { kFirst, NULL };
        peer_answers("again.test", port, one);
        fetch_all(url, NULL, out, sizeof(out), NULL, 0);
        os64_fetch_options_t quick = { .idle_ms = 500 };
        CHECK(fetch_all(url, &quick, out, sizeof(out), why, sizeof(why)) == OS64_FETCH_SILENT);
        CHECK(dials == 1 && strstr(why, "retried") == NULL);

        // A reply that BEGAN is not retried either: bytes arrived.
        reset();
        const char *cut[] = { kFirst, "HTTP/1.1 200", NULL };
        p = peer_answers("again.test", port, cut);
        p->hangup_after = 2;
        fetch_all(url, NULL, out, sizeof(out), NULL, 0);
        CHECK(fetch_all(url, NULL, out, sizeof(out), why, sizeof(why)) == OS64_FETCH_BAD_HEAD);
        CHECK(dials == 1 && strstr(why, "retried") == NULL);
    }
}

static void case_keepalive_paths(void)
{
    char out[64], why[256];
    const char *answers[] = { kFirst, kSecond, NULL };
    for (int tls = 0; tls < 2; tls++) {
        const char *url = tls ? "https://idle.test/" : "http://idle.test/";
        // THE PROBE AT THE TAKE: a server that closed its idle side after
        // the connection was kept. The probe finds the close (a TLS one by
        // stepping the engine, a plain one by a poll), the connection is
        // closed, and a new one is dialled: no request is sent to find out,
        // and none is retried.
        reset();
        peer_t *p = peer_answers("idle.test", tls ? 443 : 80, answers);
        p->idle_close_ms = 1000;
        CHECK(fetch_all(url, NULL, out, sizeof(out), NULL, 0) == OS64_FETCH_OK && fetch_pool_idle() == 1);
        now_ms += 2000;
        CHECK(fetch_all(url, NULL, out, sizeof(out), why, sizeof(why)) == OS64_FETCH_OK && !strcmp(out, "first"));
        CHECK(dials == 2 && closes == 1 && requests_on(0) == 1 && strstr(why, "retried") == NULL);
    }

    // A REDIRECT'S COURTESY BODY STILL ON ITS WAY: a 302 that promises
    // thirteen bytes and has sent none. Nobody reads them, so the
    // connection is not kept for its key, whatever arrives on it later.
    reset();
    const char *redirect[] = { "HTTP/1.1 302 Found\r\nLocation: http://next.test/\r\nContent-Length: 13\r\n\r\n", NULL };
    peer_answers("from.test", 80, redirect);
    peer_answers("next.test", 80, answers);
    CHECK(fetch_all("http://from.test/", NULL, out, sizeof(out), NULL, 0) == OS64_FETCH_OK && !strcmp(out, "first"));
    CHECK(fetch_pool_idle() == 1 && closes == 1);       // next.test's kept, from.test's closed

    // AN ANSWER BEFORE THE REQUEST'S OWN HEAD WAS WHOLE: a server that
    // replies after fifty bytes of a long head has read a request that
    // never ended, body or no body.
    reset(); write_limit = 17;
    static char pad[400];
    memset(pad, 0, sizeof(pad));
    strcpy(pad, "X-Pad: ");
    memset(pad + 7, 'p', 300);
    strcat(pad, "\r\n");
    peer_t *early = peer_add("early.test", 80, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst");
    early->early_after = 50;
    os64_fetch_options_t padded = { .extra_headers = pad };
    CHECK(fetch_all("http://early.test/", &padded, out, sizeof(out), NULL, 0) == OS64_FETCH_OK);
    CHECK(conns[0].reqlen < 300 && fetch_pool_idle() == 0 && closes == 1);
}

// The rule itself, clause by clause.
static void case_reusable_rule(void)
{
    const fetch_reuse_facts_t base = {
        .request_http11 = true, .request_whole = true, .response_http11 = true,
        .framing = HTTP_FRAMING_LENGTH, .consumed = true, .transport_ok = true,
    };
    fetch_reuse_facts_t x = base;
    CHECK(connection_reusable(&x));
    x = base; x.framing = HTTP_FRAMING_CHUNKED;  CHECK(connection_reusable(&x));
    x = base; x.request_http11 = false;          CHECK(!connection_reusable(&x));
    x = base; x.response_http11 = false;         CHECK(!connection_reusable(&x));
    x = base; x.request_close = true;            CHECK(!connection_reusable(&x));
    x = base; x.response_close = true;           CHECK(!connection_reusable(&x));
    x = base; x.request_whole = false;           CHECK(!connection_reusable(&x));
    x = base; x.consumed = false;                CHECK(!connection_reusable(&x));
    x = base; x.framing = HTTP_FRAMING_CLOSE;    CHECK(!connection_reusable(&x));
    x = base; x.leftover = true;                 CHECK(!connection_reusable(&x));
    x = base; x.transport_ok = false;            CHECK(!connection_reusable(&x));
    // A bodiless reply needs no framing read, and still every other clause.
    x = base; x.bodiless = true; x.framing = HTTP_FRAMING_CLOSE; x.consumed = false;
    CHECK(connection_reusable(&x));
    x.response_close = true;                     CHECK(!connection_reusable(&x));
    x.response_close = false; x.leftover = true; CHECK(!connection_reusable(&x));
}

// The status line's version and the Connection line, as the head reader
// reads them.
static const char *mem_bytes;
static size_t mem_at, mem_len;
static int64_t mem_source(void *ctx, void *buf, size_t cap)
{
    (void)ctx;
    size_t n = mem_len - mem_at < cap ? mem_len - mem_at : cap;
    memcpy(buf, mem_bytes + mem_at, n);
    mem_at += n;
    return (int64_t)n;
}
static void case_reply_connection(void)
{
    static const struct { const char *head; bool http11, close; } cases[] = {
        { "HTTP/1.1 200 OK\r\n\r\n", true, false },
        { "HTTP/1.0 200 OK\r\n\r\n", false, false },
        { "HTTP/1.2 200 OK\r\n\r\n", true, false },
        { "HTTP/2.0 200 OK\r\n\r\n", false, false },
        { "HTTP/1.10 200 OK\r\n\r\n", false, false },
        { "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n", true, true },
        { "HTTP/1.1 200 OK\r\nConnection: CLOSE\r\n\r\n", true, true },
        { "HTTP/1.1 200 OK\r\nConnection:\tupgrade ,close\r\n\r\n", true, true },
        { "HTTP/1.1 200 OK\r\nConnection: closed\r\n\r\n", true, false },
        { "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\n\r\n", true, false },
        { "HTTP/1.1 200 OK\r\nX-Connection: close\r\n\r\n", true, false },
        { "HTTP/1.1 103 Hints\r\nConnection: close\r\n\r\nHTTP/1.1 200 OK\r\n\r\n", true, false },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        mem_bytes = cases[i].head; mem_at = 0; mem_len = strlen(cases[i].head);
        http_stream_t s;
        http_response_t r;
        http_stream_init(&s, mem_source, NULL);
        CHECK(http_head_read(&s, &r) == HTTP_HEAD_OK);
        CHECK(r.http11 == cases[i].http11 && r.connectionClose == cases[i].close);
    }
    // A Connection line too long to read might have said close.
    static char longline[HTTP_LINE_MAX + 200];
    int n = snprintf(longline, sizeof(longline), "HTTP/1.1 200 OK\r\nConnection: ");
    memset(longline + n, 'k', HTTP_LINE_MAX);
    snprintf(longline + n + HTTP_LINE_MAX, sizeof(longline) - (size_t)n - HTTP_LINE_MAX, "\r\n\r\n");
    mem_bytes = longline; mem_at = 0; mem_len = strlen(longline);
    http_stream_t s;
    http_response_t r;
    http_stream_init(&s, mem_source, NULL);
    CHECK(http_head_read(&s, &r) == HTTP_HEAD_OK && r.connectionClose);
}

// The writer's order: Host first, the named lines where the order puts
// them, every other line after in the writer's own order.
static void case_request_order(void)
{
    http_url_t url;
    CHECK(http_url_parse("http://order.test/p", &url) == HTTP_URL_OK);
    char out[2048];
    http_request_extras_t x = {
        .user_agent = "UA", .accept = "*/*", .keep_alive = true,
        .extra_headers = "X-B: 2\r\nReferer: http://r.test/\r\nX-A: 1\r\nAccept-Language: en\r\n",
        .order = "connection, Referer,ACCEPT , x-a, Host, referer, Nobody",
    };
    CHECK(http_request(out, sizeof(out), &url, false, &x));
    CHECK(strcmp(out, "GET /p HTTP/1.1\r\nHost: order.test\r\n"
                      "Connection: keep-alive\r\nReferer: http://r.test/\r\nAccept: */*\r\nX-A: 1\r\n"
                      "User-Agent: UA\r\nAccept-Encoding: gzip, identity\r\n"
                      "X-B: 2\r\nAccept-Language: en\r\n\r\n") == 0);
    // No order is the writer's own, as it always was, less the close.
    x.order = NULL; x.keep_alive = false;
    CHECK(http_request(out, sizeof(out), &url, false, &x));
    CHECK(strcmp(out, "GET /p HTTP/1.1\r\nHost: order.test\r\nUser-Agent: UA\r\nAccept: */*\r\n"
                      "Accept-Encoding: gzip, identity\r\nX-B: 2\r\nReferer: http://r.test/\r\n"
                      "X-A: 1\r\nAccept-Language: en\r\n\r\n") == 0);
    // A body's length goes where it is named, the body's lines too.
    http_request_extras_t put = { .method_name = "PUT", .body_len = 2, .body_present = true,
                                  .body_headers = "Content-Type: text/plain\r\n",
                                  .order = "Content-Length, Content-Type, Accept-Encoding" };
    CHECK(http_request(out, sizeof(out), &url, false, &put));
    CHECK(strcmp(out, "PUT /p HTTP/1.1\r\nHost: order.test\r\nContent-Length: 2\r\n"
                      "Content-Type: text/plain\r\nAccept-Encoding: gzip, identity\r\n\r\n") == 0);
    // An order holding anything but names, commas and blanks is refused.
    x.order = "Accept;q";
    CHECK(!http_request_extras_ok(&x) && !http_request(out, sizeof(out), &url, false, &x));
    x.order = "Accept\r\nX: y";
    CHECK(!http_request_extras_ok(&x));
    // CONNECT, in any case, is refused before a dial: its reply would make
    // the connection a tunnel, and no kept connection may be one.
    static const char *const tunnels[] = {"CONNECT", "connect", "Connect"};
    for (size_t i = 0; i < 3; i++) {
        reset();
        peer_add("order.test", 80, reply_len("200 OK", "", page));
        os64_fetch_options_t tunnel = { .method_name = tunnels[i] };
        os64_fetch_t *t = os64_fetch_open("http://order.test/", &tunnel);
        CHECK(os64_fetch_status(t) == OS64_FETCH_REQUEST_FAILED && dials == 0);
        os64_fetch_close(t);
    }
    http_request_extras_t connect = { .method_name = "CONNECT" };
    CHECK(!http_request_extras_ok(&connect));
    // A fetch's order reaches the per-hop lines too, on every hop.
    reset();
    peer_add("order.test", 80, reply_len("200 OK", "", page));
    cookie_test_t t = { .output = "Cookie: c=1\r\nReferer: http://r.test/\r\n" };
    os64_fetch_options_t opt = { .user_agent = "UA", .headers_for = cookie_headers, .ctx = &t,
                                 .order = "Referer, User-Agent", .keep_alive = true };
    os64_fetch_t *f = os64_fetch_open("http://order.test/p", &opt);
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
    CHECK(strncmp(request_n(0), "GET /p HTTP/1.1\r\nHost: order.test\r\nReferer: http://r.test/\r\n"
                                "User-Agent: UA\r\nAccept-Encoding: gzip, identity\r\n"
                                "Connection: keep-alive\r\nCookie: c=1\r\n\r\n", strlen(request_n(0))) == 0);
    os64_fetch_close(f);
    // ...and an order too long to hold is a request this will not send.
    static char longorder[OS64_FETCH_ORDER_MAX + 8];
    memset(longorder, 'a', sizeof(longorder) - 1);
    os64_fetch_options_t big = { .order = longorder };
    f = os64_fetch_open("http://order.test/p", &big);
    CHECK(os64_fetch_status(f) == OS64_FETCH_REQUEST_FAILED && dials == 1);
    os64_fetch_close(f);
}

int main(int argc, char **argv)
{
    unsigned seed = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 12345u;
    rng = seed;
    printf("test_fetch_host: random-chunk seed %u\n", seed);
    for (int mode = 0; mode < 3; mode++) {
        case_plain(mode);
        case_chunked_and_gzip(mode);
    }
    case_cut_and_limits();
    case_redirects();
    case_https_and_downgrade();
    case_tls_version_fallback();
    case_proxy();
    case_failures();
    case_content_type();
    case_round1();
    case_round2();
    case_round4();
    case_round5();
    case_round6();
    case_round7();
    case_post();
    case_cookies();
    case_early_post();
    case_keep();
    case_methods();
    case_reusable_rule();
    case_reply_connection();
    case_request_order();
    case_keepalive_reuse();
    case_keepalive_refused();
    case_keepalive_hostile();
    case_keepalive_owner();
    case_keepalive_caps();
    case_keepalive_retry();
    case_keepalive_paths();
    // Nothing is left kept at exit: the sanitizer's leak check is the proof
    // that what the pool holds it also lets go.
    fetch_pool_drain();
    fetch_pool_trust_drop();
    CHECK(fetch_pool_idle() == 0);
    printf("test_fetch_host: %d checks passed\n", checks);
    return 0;
}
