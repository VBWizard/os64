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
    bool        tls_truncate;  // (https) the record layer ends without close_notify
} peer_t;

static peer_t peers[16];
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
    char     request[8192];
    size_t   reqlen;
} conn_t;
static conn_t conns[16];
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

int64_t os64_dial(const char *dialstring)
{
    dials++;
    snprintf(last_dial, sizeof(last_dial), "%s", dialstring);
    char host[256]; unsigned port;
    if (sscanf(dialstring, "tcp!%255[^!]!%u", host, &port) != 2) return -1;
    for (size_t i = 0; i < npeers; i++) {
        if (strcmp(peers[i].host, host) == 0 && peers[i].port == port) {
            if (peers[i].refuse) return -3;
            for (int h = 0; h < 16; h++)
                if (!conns[h].open) {
                    memset(&conns[h], 0, sizeof(conns[h]));
                    conns[h].open = true; conns[h].peer = &peers[i];
                    return 100 + h;
                }
            abort();
        }
    }
    return -2;
}
const char *os64_dial_reason(int64_t err) { return err == -3 ? "connection refused" : "no such host"; }
int64_t os64_close(int32_t h)
{
    assert(h >= 100 && h < 116 && conns[h - 100].open);
    conns[h - 100].open = false;
    closes++;
    return 0;
}
static int64_t conn_read(conn_t *c, void *buf, size_t cap, uint64_t wait_ms, bool *timed_out)
{
    *timed_out = false;
    if (c->peer->silent || (c->peer->silent_after && c->served >= c->peer->silent_after)) {
        now_ms += wait_ms; *timed_out = true; return 0;
    }
    size_t left = c->peer->len - c->served;
    if (left == 0) return 0;
    size_t n = chunk_of(cap < left ? cap : left);
    if (c->peer->silent_after && c->served + n > c->peer->silent_after)
        n = c->peer->silent_after - c->served;
    memcpy(buf, c->peer->reply + c->served, n);
    c->served += n;
    return (int64_t)n;
}
int64_t os64_read_for(int32_t h, void *buf, size_t cap, uint64_t ms)
{
    conn_t *c = &conns[h - 100];
    assert(c->open);
    bool timed_out;
    int64_t n = conn_read(c, buf, cap, ms, &timed_out);
    return timed_out ? OS64_ERR_TIMEOUT : n;
}
int64_t os64_write_for(int32_t h, const void *buf, size_t n, uint64_t ms)
{
    (void)ms;
    conn_t *c = &conns[h - 100];
    assert(c->open && c->reqlen + n < sizeof(c->request));
    memcpy(c->request + c->reqlen, buf, n);
    c->reqlen += n;
    return (int64_t)n;
}

// ── TLS: the transport stubbed over the same connections ────────────────

struct os64_tls_transport { int32_t handle; os64_tls_status_t status; bool closing; };
static int tls_creates, tls_frees, trust_loads, trust_frees;
static os64_tls_trust *fake_trust = (os64_tls_trust *)"trust";
os64_tls_status_t os64_tls_transport_create(const os64_tls_config_t *c, int32_t h,
                                            const os64_tls_transport_limits_t *limits,
                                            os64_tls_transport **out)
{
    (void)limits;
    assert(c && c->trust != NULL);
    tls_creates++;
    struct os64_tls_transport *t = calloc(1, sizeof(*t));
    t->handle = h; t->status = OS64_TLS_OK;
    *out = t;
    return OS64_TLS_OK;
}
os64_tls_state_t os64_tls_transport_state(os64_tls_transport *t)
{
    return (os64_tls_state_t){ .status = t->status, .flags = OS64_TLS_HANDSHAKE_DONE | OS64_TLS_SEND_PLAIN,
                               .policy_reason = 0, .upstream_error = 0, .alpn = "http/1.1" };
}
os64_tls_status_t os64_tls_transport_step(os64_tls_transport *t, uint64_t ms)
{
    now_ms += ms;
    if (t->closing) return t->status = OS64_TLS_CLEAN_EOF;
    return t->status == OS64_TLS_OK ? OS64_TLS_NEED_PROGRESS : t->status;
}
os64_tls_transfer_t os64_tls_transport_write(os64_tls_transport *t, const void *p, size_t n)
{
    os64_write_for(t->handle, p, n, 0);
    return (os64_tls_transfer_t){ OS64_TLS_OK, n };
}
os64_tls_transfer_t os64_tls_transport_read(os64_tls_transport *t, void *p, size_t n)
{
    if (t->status != OS64_TLS_OK) return (os64_tls_transfer_t){ t->status, 0 };
    conn_t *c = &conns[t->handle - 100];
    bool timed_out;
    int64_t got = conn_read(c, p, n, 1000, &timed_out);
    if (timed_out) return (os64_tls_transfer_t){ OS64_TLS_NEED_PROGRESS, 0 };
    if (got == 0) {
        t->status = c->peer->tls_truncate ? OS64_TLS_TRUNCATED : OS64_TLS_CLEAN_EOF;
        return (os64_tls_transfer_t){ t->status, 0 };
    }
    return (os64_tls_transfer_t){ OS64_TLS_OK, (size_t)got };
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
    *current = fake_trust;
    return OS64_TLS_STORE_OK;
}
void os64_tls_trust_free(os64_tls_trust *store) { assert(store == fake_trust); trust_frees++; }
const char *os64_tls_status_name(os64_tls_status_t s) { (void)s; return "tls-status"; }
const char *os64_tls_store_status_name(os64_tls_store_status_t s) { (void)s; return "store-status"; }

// ── Helpers ─────────────────────────────────────────────────────────────

static char replies[32][262144];
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

static void reset(void)
{
    peer_reset(); clear_env(); nreplies = 0;
    memset(conns, 0, sizeof(conns));
    dials = closes = tls_creates = tls_frees = trust_loads = trust_frees = 0;
    now_ms = 0; chunk_mode = 0; fail_next_malloc = false;
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
    CHECK(closes == 1 && dials == 1);
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

typedef struct { int calls; os64_fetch_hop_kind_t kinds[8]; os64_fetch_verdict_t answer; char last[OS64_FETCH_URL_MAX]; } hops_t;
static os64_fetch_verdict_t record_hop(void *ctx, const os64_fetch_hop_t *hop)
{
    hops_t *h = ctx;
    if (h->calls < 8) h->kinds[h->calls] = hop->kind;
    h->calls++;
    snprintf(h->last, sizeof(h->last), "%s", hop->whole);
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
    CHECK(dials == 3 && closes == 2);                 // two hops closed, the third open
    uint8_t out[4096];
    CHECK(slurp(f, out, sizeof(out), 0) == strlen(page));
    os64_fetch_close(f);
    CHECK(closes == 3);

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
    CHECK(trust_frees == 1 && tls_frees == 1 && closes == 1);

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
    CHECK(trust_frees == 1);   // loaded for the first hop, freed at close
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
    // leak the store: the predicate's second call (after the load) says yes.
    reset(); cancel_now = false; cancel_after_calls = 2;
    peer_add("secure.test", 443, reply_len("200 OK", "", page));
    os64_fetch_options_t cancel = { .cancelled = cancelled_cb };
    f = os64_fetch_open("https://secure.test/", &cancel);
    CHECK(os64_fetch_status(f) == OS64_FETCH_INTERRUPTED && trust_loads == 1);
    os64_fetch_close(f);
    CHECK(trust_frees == 1);
    cancel_now = false; cancel_after_calls = 0;
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
    case_proxy();
    case_failures();
    case_content_type();
    case_round1();
    printf("test_fetch_host: %d checks passed\n", checks);
    return 0;
}
