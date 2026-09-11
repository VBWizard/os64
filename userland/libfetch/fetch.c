// fetch.c — the driver: dial, ask, read the head, follow the hops, hand the
// body over with its framing and coding taken off. LIBFETCH.md is the
// design record; fetch.h is the contract. This file was os64get's
// url_ask_with_trust + receive_url_body with the file, the prose and the
// exit codes taken out, and the buffers moved off BSS and into the object.

#include "fetch/fetch.h"

#include "fetch/transport.h"
#include "proxy.h"

#include "os64/dial.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/str.h"

// The wire staging buffer for a gzip body: the decoder wants an input run
// and the framing hands over whatever arrived, so a run is accumulated here
// before decoding. Identity bodies never touch it — they are read straight
// into the caller's buffer.
#define FETCH_WIRE_CHUNK 65536

// A dial string: "tcp!" + the longest host the resolver accepts + "!65535".
// The bound is checked anyway, because a truncated dial string fails with
// the WRONG complaint — a port nobody mistyped (Codex review, 2026-08-22).
#define FETCH_DIAL_MAX (OS64_RESOLVE_NAME_MAX + 16)

struct os64_fetch {
    os64_fetch_options_t opt;
    char user_agent[OS64_FETCH_AGENT_MAX];
    char accept[OS64_FETCH_ACCEPT_MAX];
    char extra[OS64_FETCH_EXTRA_MAX];

    // One conversation with the world: the connection, the stream reading
    // it, and the head that came back. A redirect throws all three away and
    // starts another; the body is read from the LAST one. The stream holds a
    // pointer to `io`, so these live here and are never copied.
    fetch_transport_t io;
    http_stream_t     stream;
    http_response_t   reply;
    http_body_t       body;
    http_url_t        current;      // the address being asked
    http_url_t        origin;       // the address the caller typed: what its credentials are FOR
    fetch_proxy_t     proxy;        // who is carrying it

    os64_fetch_head_t     head;
    os64_fetch_progress_t progress;
    os64_fetch_detail_t   detail;
    os64_fetch_status_t   status;
    bool have_head;                 // `head` is filled and the connection is positioned at the body
    bool connected;                 // io holds a live connection
    bool over;                      // read has given its final answer
    int64_t final_answer;           // ...and this is it (0 or -1)

    os64_tls_trust *trust;
    bool trust_owned;

    // gzip: the decoder, its cap, and the wire staging run.
    bool gzip;
    os64_gzip_t *gz;
    uint64_t gz_limit;
    bool gz_final;                  // the framing said the body is WHOLE
    uint8_t *wire;
    size_t wire_have, wire_next;

    char reason[OS64_FETCH_WHY_MAX + OS64_FETCH_URL_MAX];
};

// ── Small things ────────────────────────────────────────────────────────

static bool cancelled(os64_fetch_t *f)
{
    return f->opt.cancelled != NULL && f->opt.cancelled(f->opt.ctx);
}

static bool cancel_predicate(void *ctx)
{
    return cancelled((os64_fetch_t *)ctx);
}

static void proxy_facts(const fetch_proxy_t *p, bool *via, char *host, size_t cap, uint16_t *port)
{
    *via = p->inUse;
    if (p->inUse) {
        os64_strcopy(host, cap, p->host);
        *port = p->port;
    } else {
        host[0] = '\0';
        *port = 0;
    }
}

// The same ORIGIN: scheme, host and port — the web's unit of trust.
static bool same_address_origin(const http_url_t *a, const http_url_t *b)
{
    return a->port == b->port &&
           os64_streq(a->scheme, b->scheme) &&
           os64_streq(a->host, b->host);
}

static bool same_address(const http_url_t *a, const http_url_t *b)
{
    return same_address_origin(a, b) && os64_streq(a->path, b->path);
}

// 301/302 and 307/308 differ only in what a client may do to the METHOD:
// the older pair were so widely implemented as "retry it as a GET" that the
// newer pair had to be invented to mean "and keep the method you had". This
// library only ever sends GET, so all four say the same thing to it — and
// 303 (See Other), whose entire meaning is "GET this other thing instead",
// says it too.
//
// The 3xx codes deliberately NOT here, each a final answer the caller sees:
//   300 Multiple Choices — a list for a person to pick from; its Location is
//       a hint, and choosing on somebody's behalf is not a fetcher's job.
//   304 Not Modified — an answer to a conditional request nothing here makes.
//   305 Use Proxy — a stranger telling this machine to route its traffic
//       through a machine of the stranger's choosing. Every browser dropped
//       it for that reason; so does this.
static bool redirect_is_followed(int32_t status)
{
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

// The library's URL, with the port the fetch actually used. fetch.h
// promises a filled-in port here (a consumer that dials again wants it),
// which is the opposite of what os64_url_absolute wants — http_url_to_os64
// is for that direction.
static void url_filled(const http_url_t *in, os64_url_t *out)
{
    http_url_to_os64(in, out);
    out->port = in->port;
}

// ── Redirect arithmetic ─────────────────────────────────────────────────

// THE WHOLE ADDRESS FIRST, AND EVERY QUESTION IS ASKED OF THAT. A Location
// may be root-relative (`/login`), scheme-relative (`//cdn.example/x`) or
// relative to the page (`index.html`), and none of those parses as a URL on
// its own — so a judgement made on the raw header judged an empty host,
// asked the proxy policy about nothing, and let a malformed relative path
// through to be offered as a command os64get itself would refuse. Resolve,
// then parse, then decide. (Codex review round 6, 2026-09-03.)
static void hop_read(os64_fetch_t *f, os64_fetch_hop_t *hop, http_url_t *target,
                     fetch_proxy_t *carrier)
{
    os64_memset(hop, 0, sizeof(*hop));
    hop->status = f->reply.status;
    os64_strcopy(hop->reason, sizeof(hop->reason), f->reply.reason);
    hop->number = f->head.hops + 1;
    proxy_facts(&f->proxy, &hop->from_via_proxy, hop->from_proxy_host,
                sizeof(hop->from_proxy_host), &hop->from_proxy_port);

    const char *location = f->reply.location;
    if (location[0] == '\0') {
        hop->kind = OS64_FETCH_HOP_NONE;
        return;
    }
    os64_url_t base;
    http_url_to_os64(&f->current, &base);
    if (!os64_url_absolute(&base, location, hop->whole, sizeof(hop->whole))) {
        hop->kind = OS64_FETCH_HOP_TOO_LONG;
        return;
    }

    http_url_result_t parse = http_url_parse(hop->whole, target);
    if (parse == HTTP_URL_SCHEME || parse == HTTP_URL_NOT_A_URL) {
        // HTTP_URL_NOT_A_URL does not mean here what it means on a command
        // line. What resolution hands back is either `scheme://...` or a
        // reference that named its own scheme and was copied through
        // untouched, so a refusal at this point is the second kind —
        // `mailto:`, `data:`, `tel:` — an address of a sort this library
        // does not fetch, rather than a bare word meant for something else.
        hop->kind = OS64_FETCH_HOP_SCHEME;
        return;
    }
    if (parse != HTTP_URL_OK) {
        hop->kind = OS64_FETCH_HOP_UNUSABLE;
        // http's refusals are the library's, one for one (http.c's
        // url_result_from), so the public detail speaks the library's.
        switch (parse) {
            case HTTP_URL_NO_HOST:    hop->parse = OS64_URL_NO_HOST;    break;
            case HTTP_URL_HOST_CHARS: hop->parse = OS64_URL_HOST_CHARS; break;
            case HTTP_URL_PATH_CHARS: hop->parse = OS64_URL_PATH_CHARS; break;
            case HTTP_URL_PORT:       hop->parse = OS64_URL_PORT;       break;
            default:                  hop->parse = OS64_URL_TOO_LONG;   break;
        }
        return;
    }
    url_filled(target, &hop->target);

    // A REDIRECT TO THE ADDRESS THAT JUST ANSWERED is a server that has lost
    // its place, and the hop limit would eventually say so — five requests
    // later, in words about counting rather than about what happened.
    if (same_address(&f->current, target)) {
        hop->kind = OS64_FETCH_HOP_SELF;
        return;
    }
    if (os64_streq(f->current.scheme, "https") && os64_streq(target->scheme, "http")) {
        hop->kind = OS64_FETCH_HOP_DOWNGRADE;
        return;
    }

    // THE PROXY ASKED ABOUT IS THE TARGET'S, NOT THIS FETCH'S. They are
    // different questions and the scheme decides each one separately — a
    // redirect from http to https is carried by $https_proxy no matter what
    // carried the request that produced it. Passing the current fetch's
    // proxy in here looked obviously right and was obviously wrong the first
    // time a plain-HTTP page redirected to https with $https_proxy set: the
    // answer said "out of reach" about an address one hop away.
    carrier->inUse = false;
    if (!f->opt.no_proxy &&
        !fetch_proxy_for(target, carrier, hop->why, sizeof(hop->why))) {
        hop->kind = OS64_FETCH_HOP_PROXY;
        return;
    }
    proxy_facts(carrier, &hop->via_proxy, hop->proxy_host, sizeof(hop->proxy_host),
                &hop->proxy_port);
    hop->kind = OS64_FETCH_HOP_WHOLE;
}

// The caller's verdict, or the policy's. A FOLLOW where there is nothing to
// follow is a STOP (fetch.h says so), and a DOWNGRADE or SELF the caller
// did not explicitly allow stops too.
// The hop is the caller's to see and, past the verdict, the library's to
// amend: a DOWNGRADE the caller allowed can still turn out to have no road
// (a bad proxy setting for the target), and the hop that ends the fetch
// must then SAY so rather than read as "your FOLLOW was not honoured"
// (Codex, PR #92).
static bool hop_followed(os64_fetch_t *f, os64_fetch_hop_t *hop, fetch_proxy_t *carrier)
{
    os64_fetch_verdict_t verdict = OS64_FETCH_HOP_DEFAULT;
    if (f->opt.on_hop != NULL)
        verdict = f->opt.on_hop(f->opt.ctx, hop);
    if (verdict == OS64_FETCH_HOP_STOP)
        return false;
    switch (hop->kind) {
        case OS64_FETCH_HOP_WHOLE:
            return true;
        case OS64_FETCH_HOP_DOWNGRADE:
        case OS64_FETCH_HOP_SELF:
            if (verdict != OS64_FETCH_HOP_FOLLOW)
                return false;
            // The road to an explicitly allowed target is worked out now —
            // hop_read stopped before asking, since the default never goes.
            carrier->inUse = false;
            if (!f->opt.no_proxy) {
                http_url_t target;
                if (http_url_parse(hop->whole, &target) != HTTP_URL_OK) {
                    hop->kind = OS64_FETCH_HOP_UNUSABLE;   // cannot happen: hop_read parsed it
                    return false;
                }
                if (!fetch_proxy_for(&target, carrier, hop->why, sizeof(hop->why))) {
                    hop->kind = OS64_FETCH_HOP_PROXY;
                    return false;
                }
                proxy_facts(carrier, &hop->via_proxy, hop->proxy_host,
                            sizeof(hop->proxy_host), &hop->proxy_port);
            }
            return true;
        default:
            return false;
    }
}

// A CREDENTIAL IS FOR THE ORIGIN THE CALLER TYPED, NOT FOR WHOEVER A
// REDIRECT NAMES. `Cookie`, `Authorization` and `Proxy-Authorization` in
// extra_headers are sent to the typed origin and to any hop that stays on
// it (same scheme, host and port); a hop that leaves it gets the block
// with those fields removed, which is curl's rule and the Fetch standard's
// for Authorization. Without it a 302 from an attacker's page on the
// origin — or an origin that has simply been taken over — would collect
// the caller's session. Everything else in the block (Referer, Range,
// Accept-Language) is not origin-bound and rides along. (Codex, PR #92
// rd4.) The block is already validated line by line, so the walk below
// can trust its shape.
static bool origin_bound(const char *line, size_t nameLen)
{
    static const char *const bound[] = { "cookie", "authorization", "proxy-authorization" };
    for (size_t i = 0; i < sizeof(bound) / sizeof(bound[0]); i++) {
        size_t n = os64_strlen(bound[i]);
        if (n != nameLen)
            continue;
        size_t k = 0;
        while (k < n) {
            char c = line[k];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c + ('a' - 'A'));
            if (c != bound[i][k])
                break;
            k++;
        }
        if (k == n)
            return true;
    }
    return false;
}

static void extras_for_origin(const os64_fetch_t *f, char *out, size_t cap)
{
    out[0] = '\0';
    if (f->extra[0] == '\0')
        return;
    if (same_address_origin(&f->origin, &f->current)) {
        os64_strcopy(out, cap, f->extra);
        return;
    }
    size_t used = 0;
    const char *p = f->extra;
    while (*p != '\0') {
        const char *line = p;
        const char *colon = p;
        while (*colon != ':')
            colon++;
        const char *end = colon;
        while (!(end[0] == '\r' && end[1] == '\n'))
            end++;
        end += 2;
        size_t len = (size_t)(end - line);
        if (!origin_bound(line, (size_t)(colon - line)) && used + len < cap) {
            for (size_t i = 0; i < len; i++)
                out[used + i] = line[i];
            used += len;
            out[used] = '\0';
        }
        p = end;
    }
}

// ── The head: dial, ask, read, and follow ───────────────────────────────

static void head_fill(os64_fetch_t *f)
{
    os64_fetch_head_t *h = &f->head;
    h->status = f->reply.status;
    os64_strcopy(h->reason, sizeof(h->reason), f->reply.reason);
    os64_strcopy(h->content_type, sizeof(h->content_type), f->reply.contentType);
    os64_strcopy(h->charset, sizeof(h->charset), f->reply.charset);
    h->has_length = f->reply.hasLength;
    h->length = f->reply.length;
    os64_strcopy(h->encoding, sizeof(h->encoding), f->reply.contentEncoding);
    h->has_location = f->reply.hasLocation;
    h->location = f->reply.location;
    url_filled(&f->current, &h->url);
    http_url_render(&f->current, h->url_text, sizeof(h->url_text));
    h->encrypted = f->io.encrypted;
    proxy_facts(&f->proxy, &h->via_proxy, h->proxy_host, sizeof(h->proxy_host), &h->proxy_port);
    f->progress.has_length = h->has_length;
    f->progress.length = h->length;
    f->have_head = true;
}

static void tls_snapshot(os64_fetch_t *f)
{
    f->detail.tls = f->io.error.status;
    f->detail.tls_policy = f->io.error.policy_reason;
    f->detail.tls_engine = f->io.error.upstream_error;
}

static os64_fetch_status_t drop_connection(os64_fetch_t *f, os64_fetch_status_t status)
{
    if (f->connected) {
        fetch_transport_close(&f->io, false);
        f->connected = false;
    }
    if (status == OS64_FETCH_TLS_FAILED)
        tls_snapshot(f);
    if (cancelled(f))
        return OS64_FETCH_INTERRUPTED;
    return status;
}

// Ask, and keep asking wherever the answers point, until an answer is the
// thing itself or a verdict says stop. On OK the connection is open and
// positioned at the first byte of the body and `current` names who served
// it; REDIRECT_STOPPED and TOO_MANY_HOPS leave the last head readable the
// same way; on anything else the connection is closed.
static os64_fetch_status_t ask(os64_fetch_t *f)
{
    for (;;) {
        if (cancelled(f))
            return OS64_FETCH_INTERRUPTED;

        bool encrypted = os64_streq(f->current.scheme, "https") && !f->proxy.inUse;
        if (encrypted && f->trust == NULL) {
            os64_tls_store_status_t loaded =
                os64_tls_trust_reload(&f->trust, &f->detail.store_report);
            // Ownership is recorded the instant the load succeeds, BEFORE
            // anything can return: a cancellation that lands during the
            // load would otherwise leave a store nobody frees (Codex, #92).
            if (loaded == OS64_TLS_STORE_OK)
                f->trust_owned = true;
            if (cancelled(f))
                return OS64_FETCH_INTERRUPTED;
            if (loaded != OS64_TLS_STORE_OK) {
                f->detail.store_failed = true;
                f->detail.store = loaded;
                return OS64_FETCH_TLS_FAILED;
            }
        }

        // ── Dial: the proxy if there is one, the origin if not ──────────
        // The CONNECTION goes to whoever is answering; the ADDRESS stays in
        // the request line. That split is the whole of proxying.
        const char *peerHost = f->proxy.inUse ? f->proxy.host : f->current.host;
        uint16_t    peerPort = f->proxy.inUse ? f->proxy.port : f->current.port;

        char dialstring[FETCH_DIAL_MAX];
        int32_t dn = os64_snprintf(dialstring, sizeof(dialstring), "tcp!%s!%u",
                                   peerHost, (unsigned)peerPort);
        if (dn < 0 || (size_t)dn >= sizeof(dialstring)) {
            os64_snprintf(f->detail.why, sizeof(f->detail.why),
                          "the host name is too long to dial (limit %d)", OS64_RESOLVE_NAME_MAX);
            return OS64_FETCH_REQUEST_FAILED;
        }

        // Who is being asked, recorded before the attempt so a DIAL_FAILED
        // and a TLS_FAILED can both name the peer (for TLS the peer IS the
        // origin: a proxied hop is never encrypted).
        f->detail.dial_was_proxy = f->proxy.inUse;
        os64_strcopy(f->detail.dial_host, sizeof(f->detail.dial_host), peerHost);
        f->detail.dial_port = peerPort;
        f->detail.dial_hop = f->head.hops;

        int64_t conn = os64_dial(dialstring);
        if (cancelled(f)) {
            if (conn >= 0)
                os64_close((int32_t)conn);
            return OS64_FETCH_INTERRUPTED;
        }
        if (conn < 0) {
            f->detail.dial = conn;
            return OS64_FETCH_DIAL_FAILED;
        }

        const os64_tls_name_t alpn = {"http/1.1", 8};
        os64_tls_config_t config = {
            .hostname = {f->current.host, os64_strlen(f->current.host)},
            .alpn = &alpn, .alpn_count = 1, .trust = f->trust
        };
        if (!fetch_transport_open(&f->io, (int32_t)conn, encrypted ? &config : NULL,
                                  f->opt.idle_ms, cancel_predicate, f)) {
            // fetch_transport_open closed the handle itself on failure.
            tls_snapshot(f);
            return cancelled(f) ? OS64_FETCH_INTERRUPTED : OS64_FETCH_TLS_FAILED;
        }
        f->connected = true;

        // ── Ask ─────────────────────────────────────────────────────────
        char request[HTTP_LINE_MAX + OS64_FETCH_AGENT_MAX + OS64_FETCH_ACCEPT_MAX + OS64_FETCH_EXTRA_MAX];
        char extraForHop[OS64_FETCH_EXTRA_MAX];
        extras_for_origin(f, extraForHop, sizeof(extraForHop));
        http_request_extras_t extras = {
            .user_agent    = f->user_agent[0] ? f->user_agent : NULL,
            .accept        = f->accept[0] ? f->accept : NULL,
            .extra_headers = extraForHop[0] ? extraForHop : NULL,
        };
        if (!http_request(request, sizeof(request), &f->current, f->proxy.inUse, &extras)) {
            os64_strcopy(f->detail.why, sizeof(f->detail.why),
                         "the request does not fit, or a header holds a byte a header cannot");
            return drop_connection(f, OS64_FETCH_REQUEST_FAILED);
        }
        if (!fetch_transport_write(&f->io, request, os64_strlen(request))) {
            os64_strcopy(f->detail.why, sizeof(f->detail.why), "could not send the request");
            tls_snapshot(f);
            return drop_connection(f, OS64_FETCH_REQUEST_FAILED);
        }

        // ── The reply's head ────────────────────────────────────────────
        http_stream_init(&f->stream, fetch_transport_read, &f->io);
        http_head_result_t hrc = http_head_read(&f->stream, &f->reply);
        if (hrc != HTTP_HEAD_OK) {
            f->detail.head = hrc;
            tls_snapshot(f);
            if (hrc == HTTP_HEAD_SOURCE && f->io.silent) {
                f->detail.silent_after_body = false;
                return drop_connection(f, OS64_FETCH_SILENT);
            }
            // A framing header too long to read, or a 101 that hands the
            // connection to another protocol, is not a MALFORMED reply — it
            // is a legal one this library cannot honestly act on, the same
            // answer the coding refusals give, reached a different way.
            if (hrc == HTTP_HEAD_FRAMING || hrc == HTTP_HEAD_SWITCHED) {
                os64_strcopy(f->detail.unsupported, sizeof(f->detail.unsupported),
                             hrc == HTTP_HEAD_SWITCHED ? "101" : "head");
                return drop_connection(f, OS64_FETCH_UNSUPPORTED);
            }
            return drop_connection(f, OS64_FETCH_BAD_HEAD);
        }

        // A final authenticated prefix can accompany a terminal TLS error.
        // Check it before acting on headers, including redirect destinations.
        // It is a BAD_HEAD, not a TLS_FAILED: the handshake succeeded and the
        // head arrived, and what is untrustworthy is the reply as a whole —
        // the reason carries the transport's verdict in its tail.
        if (!fetch_transport_complete(&f->io, true)) {
            f->detail.head = HTTP_HEAD_SOURCE;
            tls_snapshot(f);
            return drop_connection(f, OS64_FETCH_BAD_HEAD);
        }

        head_fill(f);
        if (!redirect_is_followed(f->reply.status))
            return OS64_FETCH_OK;

        os64_fetch_hop_t hop;
        http_url_t target;
        fetch_proxy_t carrier = {0};
        hop_read(f, &hop, &target, &carrier);
        bool followed = hop_followed(f, &hop, &carrier);
        f->detail.hop = hop;                        // AFTER the verdict: it may have amended the kind
        if (!followed)
            return OS64_FETCH_REDIRECT_STOPPED;     // the head stays readable
        if (hop.number > f->opt.max_hops)
            return OS64_FETCH_TOO_MANY_HOPS;        // so does this one

        // The redirect's own body is a courtesy page for a browser to
        // display, and it goes with the connection: keep-alive is not
        // spoken here, so the next hop is a fresh dial whatever is left
        // unread on this one.
        fetch_transport_close(&f->io, false);
        f->connected = false;
        f->have_head = false;
        if (hop.kind == OS64_FETCH_HOP_WHOLE)
            f->current = target;
        else
            http_url_parse(hop.whole, &f->current);   // DOWNGRADE/SELF: parsed OK in hop_read
        f->proxy = carrier;
        f->head.hops = hop.number;
    }
}

// ── The body: framing off, coding off ───────────────────────────────────

// The wire length is only known in advance under a Content-Length; a chunked
// or close-framed gzip body gets the ceiling and nothing finer.
static uint64_t gzip_limit_for(const http_response_t *reply)
{
    if (!reply->hasLength)
        return OS64_FETCH_GZIP_OUTPUT_MAX;
    uint64_t limit;
    if (reply->length > OS64_FETCH_GZIP_OUTPUT_MAX / OS64_FETCH_GZIP_RATIO_MAX)
        limit = OS64_FETCH_GZIP_OUTPUT_MAX;
    else
        limit = reply->length * OS64_FETCH_GZIP_RATIO_MAX;
    if (limit < OS64_FETCH_GZIP_OUTPUT_FLOOR)
        limit = OS64_FETCH_GZIP_OUTPUT_FLOOR;
    if (limit > OS64_FETCH_GZIP_OUTPUT_MAX)
        limit = OS64_FETCH_GZIP_OUTPUT_MAX;
    return limit;
}

// A FRAMING OR A CODING THIS LIBRARY CANNOT UNDO MUST NEVER BECOME BYTES
// FOR A CONSUMER. What would land is the envelope wearing the letter's
// name, and a page full of chunk lengths or of a compression nothing here
// speaks is worse than no page at all, because it looks like a successful
// fetch. The rule outlives the list: whatever this library learns to read
// moves out of these branches by being handled (chunked and gzip did), and
// whatever it has not learned is refused by name.
static os64_fetch_status_t body_prepare(os64_fetch_t *f)
{
    // A 204 and a 304 carry NO body whatever their headers say (RFC 9112
    // §6.3: a Content-Length or a Content-Encoding on a 304 describes the
    // representation that was NOT sent), so both the framing and the
    // coding are cleared here — the framing forced to zero bytes so the
    // body reader cannot wait for bytes that will never come, the coding
    // dropped so no decoder is built for an empty body and no coding is
    // refused for one (Codex, PR #92 rd2 and rd3). The head keeps the
    // headers as metadata; head_fill has already copied them. A 304 is
    // reachable: a caller may send If-Modified-Since in extra_headers. 1xx
    // never reaches here — the head reader consumes interim replies and
    // refuses 101.
    if (f->reply.status == 204 || f->reply.status == 304) {
        f->reply.hasLength = true;
        f->reply.length = 0;
        f->reply.transferEncoding[0] = '\0';
        f->reply.contentEncoding[0] = '\0';
        f->head.has_length = true;
        f->head.length = 0;
        f->progress.has_length = true;
        f->progress.length = 0;
    }
    if (!http_body_open(&f->body, &f->stream, &f->reply)) {
        os64_strcopy(f->detail.unsupported, sizeof(f->detail.unsupported),
                     f->reply.transferEncoding);
        return OS64_FETCH_UNSUPPORTED;
    }
    // gzip is the one content coding this library undoes (BROWSER.md 3(d)),
    // and it is undone DOWNSTREAM of the framing: a gzip body may arrive
    // chunked, and the two envelopes come off in the order they went on.
    f->gzip = os64_streq(f->reply.contentEncoding, "gzip");
    if (f->reply.contentEncoding[0] != '\0' &&
        !os64_streq(f->reply.contentEncoding, "identity") && !f->gzip) {
        os64_strcopy(f->detail.unsupported, sizeof(f->detail.unsupported),
                     f->reply.contentEncoding);
        return OS64_FETCH_UNSUPPORTED;
    }
    if (f->gzip) {
        f->gz_limit = gzip_limit_for(&f->reply);
        f->detail.gzip_limit = f->gz_limit;
        f->gz = os64_gzip_create(f->gz_limit);
        f->wire = (uint8_t *)os64_malloc(FETCH_WIRE_CHUNK);
        if (f->gz == NULL || f->wire == NULL)
            return OS64_FETCH_NO_MEMORY;
    }
    return OS64_FETCH_OK;
}

// read's final word, remembered so a second read after the end says the
// same thing instead of asking a closed connection.
static int64_t finish(os64_fetch_t *f, os64_fetch_status_t status, int64_t answer)
{
    f->over = true;
    f->status = status;
    f->final_answer = answer;
    return answer;
}

// What the framing's 0 or < 0 means, in the library's vocabulary.
static int64_t framing_ended(os64_fetch_t *f, int64_t n)
{
    f->detail.body = f->body.result;
    if (n == 0) {
        if (f->body.result == HTTP_BODY_DONE) {
            // HTTP removes framing before gzip decoding. For a close-framed
            // HTTPS body, the transport requires authenticated TLS closure;
            // plain HTTP has no way to tell a complete reply from a raw FIN.
            if (!fetch_transport_complete(&f->io, f->body.framing != HTTP_FRAMING_CLOSE)) {
                tls_snapshot(f);
                return cancelled(f) ? finish(f, OS64_FETCH_INTERRUPTED, -1) : finish(f, OS64_FETCH_CUT, 0);
            }
            return finish(f, OS64_FETCH_OK, 0);
        }
        return finish(f, OS64_FETCH_CUT, 0);
    }
    if (cancelled(f))
        return finish(f, OS64_FETCH_INTERRUPTED, -1);
    switch (f->body.result) {
        case HTTP_BODY_BROKE:
            tls_snapshot(f);
            if (f->io.silent) {
                f->detail.silent_after_body = true;
                return finish(f, OS64_FETCH_SILENT, -1);
            }
            return finish(f, OS64_FETCH_BROKE, -1);
        default:
            // The server's chunk framing stopped being HTTP: "that was not
            // speech", the same verdict a broken head earns. The transport's
            // verdict is snapshotted too: an authenticated prefix can arrive
            // beside a terminal TLS status, and the reason's tail names it.
            tls_snapshot(f);
            return finish(f, OS64_FETCH_BAD_HEAD, -1);
    }
}

// How many bytes may still be handed over under max_body; SIZE_MAX = no cap.
static size_t room(const os64_fetch_t *f)
{
    if (f->opt.max_body == 0)
        return (size_t)-1;
    if (f->progress.produced >= f->opt.max_body)
        return 0;
    uint64_t left = f->opt.max_body - f->progress.produced;
    return left > (size_t)-1 ? (size_t)-1 : (size_t)left;
}

static int64_t read_identity(os64_fetch_t *f, uint8_t *buf, size_t cap)
{
    size_t want = room(f);
    uint8_t probe;
    uint8_t *dst = buf;
    if (want == 0) {
        // At the cap exactly. One more byte from the wire is the LIMIT; none
        // is a body that fit — the probe is never handed over either way.
        dst = &probe;
        want = 1;
    } else if (want < cap) {
        cap = want;
    }
    int64_t n = http_body_read(&f->body, dst, cap);
    if (n <= 0)
        return framing_ended(f, n);
    f->progress.wire += (uint64_t)n;
    if (dst == &probe)
        return finish(f, OS64_FETCH_LIMIT, -1);
    f->progress.produced += (uint64_t)n;
    return n;
}

static os64_fetch_status_t gzip_verdict(os64_gzip_status_t status)
{
    // An unsupported method and a local expansion policy are both honest
    // "this reply cannot be decoded here" answers. Every other terminal gzip
    // result says the bytes claimed to be gzip but did not form one complete,
    // verified gzip stream, which is corruption rather than missing support.
    if (status == OS64_GZIP_UNSUPPORTED)
        return OS64_FETCH_UNSUPPORTED;
    if (status == OS64_GZIP_LIMIT)
        return OS64_FETCH_LIMIT;
    return OS64_FETCH_CORRUPT;
}

static int64_t read_gzip(os64_fetch_t *f, uint8_t *buf, size_t cap)
{
    for (;;) {
        if (cancelled(f))
            return finish(f, OS64_FETCH_INTERRUPTED, -1);

        // Decode what is staged, or the final empty run once the framing
        // has said the body is whole (the decoder needs to be TOLD, so it
        // can verify the trailer and refuse trailing bytes).
        if (f->wire_next < f->wire_have || f->gz_final) {
            size_t want = room(f);
            uint8_t probe;
            uint8_t *dst = buf;
            size_t space = cap;
            if (want == 0) {
                dst = &probe;
                space = 1;
            } else if (want < space) {
                space = want;
            }
            const uint8_t *input = f->wire + f->wire_next;
            size_t inputLeft = f->wire_have - f->wire_next;
            uint8_t *output = dst;
            size_t outputLeft = space;
            os64_gzip_status_t st = os64_gzip_process(f->gz, &input, &inputLeft,
                                                      &output, &outputLeft, f->gz_final);
            f->wire_next = f->wire_have - inputLeft;
            size_t produced = space - outputLeft;
            f->detail.gzip = st;
            if (produced != 0) {
                if (dst == &probe)
                    return finish(f, OS64_FETCH_LIMIT, -1);
                f->progress.produced += produced;
                return (int64_t)produced;
            }
            if (st == OS64_GZIP_DONE) {
                if (!fetch_transport_complete(&f->io, f->body.framing != HTTP_FRAMING_CLOSE)) {
                    tls_snapshot(f);
                    return cancelled(f) ? finish(f, OS64_FETCH_INTERRUPTED, -1) : finish(f, OS64_FETCH_CUT, 0);
                }
                return finish(f, OS64_FETCH_OK, 0);
            }
            if (st == OS64_GZIP_NEED_OUTPUT)
                continue;                        // space was the probe byte; try again
            if (st == OS64_GZIP_NEED_INPUT) {
                // Asked for more with the whole body already given is a
                // stream that ends before its trailer; asked for more while
                // holding some is the decoder breaking its word.
                if (f->gz_final) {
                    f->detail.gzip = OS64_GZIP_TRUNCATED;
                    return finish(f, OS64_FETCH_CORRUPT, -1);
                }
                if (inputLeft != 0) {
                    f->detail.gzip = OS64_GZIP_BAD_ARGUMENT;
                    return finish(f, OS64_FETCH_CORRUPT, -1);
                }
                // fall through to refill
            } else {
                return finish(f, gzip_verdict(st), -1);
            }
        }

        // Refill the staging run from the framing.
        int64_t n = http_body_read(&f->body, f->wire, FETCH_WIRE_CHUNK);
        if (n > 0) {
            f->wire_have = (size_t)n;
            f->wire_next = 0;
            f->progress.wire += (uint64_t)n;
            continue;
        }
        if (n == 0 && f->body.result == HTTP_BODY_DONE) {
            // The framing is satisfied: tell the decoder, and let the next
            // pass verify the trailer. The transport's closure is checked
            // when the decoder says DONE, not here.
            f->gz_final = true;
            f->wire_have = f->wire_next = 0;
            continue;
        }
        // CUT or a failure: the decoder's work is unfinished, and that is
        // the FRAMING's verdict to give, not the decoder's — a truncated
        // transfer must read as "cut short", the same as an identity body.
        return framing_ended(f, n);
    }
}

// ── The calls ───────────────────────────────────────────────────────────

os64_fetch_t *os64_fetch_open(const char *url, const os64_fetch_options_t *opt)
{
    os64_fetch_t *f = (os64_fetch_t *)os64_malloc(sizeof(*f));
    if (f == NULL)
        return NULL;
    os64_memset(f, 0, sizeof(*f));
    if (opt != NULL)
        f->opt = *opt;
    if (f->opt.max_hops == 0)
        f->opt.max_hops = OS64_FETCH_HOPS_DEFAULT;
    f->trust = f->opt.trust;              // caller-owned: trust_owned stays false
    f->io.handle = -1;

    // The caller's strings, copied now so they need not outlive the call;
    // one that does not fit is a header this library will not send.
    if ((opt && opt->user_agent &&
         os64_strcopy(f->user_agent, sizeof(f->user_agent), opt->user_agent) >= sizeof(f->user_agent)) ||
        (opt && opt->accept &&
         os64_strcopy(f->accept, sizeof(f->accept), opt->accept) >= sizeof(f->accept)) ||
        (opt && opt->extra_headers &&
         os64_strcopy(f->extra, sizeof(f->extra), opt->extra_headers) >= sizeof(f->extra))) {
        os64_strcopy(f->detail.why, sizeof(f->detail.why), "a request header is longer than this will send");
        f->status = OS64_FETCH_REQUEST_FAILED;
        return f;
    }
    // ...and one that holds a byte a header cannot is refused HERE, before a
    // dial: a header composed from page content is a header an attacker
    // composes, and the refusal must not cost a connection to find out.
    http_request_extras_t extras = {
        .user_agent    = f->user_agent[0] ? f->user_agent : NULL,
        .accept        = f->accept[0] ? f->accept : NULL,
        .extra_headers = f->extra[0] ? f->extra : NULL,
    };
    if (!http_request_extras_ok(&extras)) {
        os64_strcopy(f->detail.why, sizeof(f->detail.why),
                     "a request header holds a byte a header cannot (a bare CR or LF, or a control byte)");
        f->status = OS64_FETCH_REQUEST_FAILED;
        return f;
    }

    http_url_result_t urc = http_url_parse(url, &f->current);
    if (urc == HTTP_URL_SCHEME) {
        os64_strcopy(f->detail.scheme, sizeof(f->detail.scheme), f->current.scheme);
        f->status = OS64_FETCH_UNSUPPORTED_SCHEME;
        return f;
    }
    if (urc != HTTP_URL_OK) {
        switch (urc) {
            case HTTP_URL_NOT_A_URL:  f->detail.url = OS64_URL_NOT_A_URL;  break;
            case HTTP_URL_NO_HOST:    f->detail.url = OS64_URL_NO_HOST;    break;
            case HTTP_URL_HOST_CHARS: f->detail.url = OS64_URL_HOST_CHARS; break;
            case HTTP_URL_PATH_CHARS: f->detail.url = OS64_URL_PATH_CHARS; break;
            case HTTP_URL_PORT:       f->detail.url = OS64_URL_PORT;       break;
            default:                  f->detail.url = OS64_URL_TOO_LONG;   break;
        }
        f->status = OS64_FETCH_BAD_URL;
        return f;
    }

    f->origin = f->current;          // what the caller's credentials are for, whatever the hops do

    if (!f->opt.no_proxy &&
        !fetch_proxy_for(&f->current, &f->proxy, f->detail.why, sizeof(f->detail.why))) {
        f->status = OS64_FETCH_PROXY_BAD;
        return f;
    }

    f->status = ask(f);
    if (f->have_head) {
        // OK, REDIRECT_STOPPED, TOO_MANY_HOPS: a head and a positioned
        // connection. The body is prepared now so read() has nothing left
        // to decide; a framing or coding refusal outranks the head's status
        // because a page nobody can read is not a page.
        os64_fetch_status_t prepared = body_prepare(f);
        if (prepared != OS64_FETCH_OK) {
            f->status = drop_connection(f, prepared);
            f->over = true;
            f->final_answer = -1;
        }
    }
    return f;
}

os64_fetch_status_t os64_fetch_status(const os64_fetch_t *f) { return f->status; }
const os64_fetch_detail_t *os64_fetch_detail(const os64_fetch_t *f) { return &f->detail; }
const os64_fetch_head_t *os64_fetch_head(const os64_fetch_t *f)
{
    return f->have_head ? &f->head : NULL;
}
const os64_fetch_progress_t *os64_fetch_progress(const os64_fetch_t *f) { return &f->progress; }

int64_t os64_fetch_read(os64_fetch_t *f, void *buf, size_t cap)
{
    if (f->over)
        return f->final_answer;
    if (!f->have_head || !f->connected)
        return finish(f, f->status == OS64_FETCH_OK ? OS64_FETCH_BROKE : f->status, -1);
    // A zero-capacity read is a caller's mistake, refused without touching
    // anything: answering 0 would read as "the body ended whole" to the
    // loop that made it (Codex, PR #92). fetch.h says so.
    if (cap == 0)
        return -1;
    if (cancelled(f))
        return finish(f, OS64_FETCH_INTERRUPTED, -1);
    return f->gzip ? read_gzip(f, (uint8_t *)buf, cap)
                   : read_identity(f, (uint8_t *)buf, cap);
}

const char *os64_fetch_status_name(os64_fetch_status_t status)
{
    switch (status) {
        case OS64_FETCH_OK:                 return "ok";
        case OS64_FETCH_BAD_URL:            return "bad-url";
        case OS64_FETCH_UNSUPPORTED_SCHEME: return "unsupported-scheme";
        case OS64_FETCH_DIAL_FAILED:        return "dial-failed";
        case OS64_FETCH_PROXY_BAD:          return "proxy-bad";
        case OS64_FETCH_TLS_FAILED:         return "tls-failed";
        case OS64_FETCH_REQUEST_FAILED:     return "request-failed";
        case OS64_FETCH_BAD_HEAD:           return "bad-head";
        case OS64_FETCH_UNSUPPORTED:        return "unsupported";
        case OS64_FETCH_SILENT:             return "silent";
        case OS64_FETCH_REDIRECT_STOPPED:   return "redirect-stopped";
        case OS64_FETCH_TOO_MANY_HOPS:      return "too-many-hops";
        case OS64_FETCH_CUT:                return "cut";
        case OS64_FETCH_BROKE:              return "broke";
        case OS64_FETCH_CORRUPT:            return "corrupt";
        case OS64_FETCH_LIMIT:              return "limit";
        case OS64_FETCH_INTERRUPTED:        return "interrupted";
        case OS64_FETCH_NO_MEMORY:          return "no-memory";
    }
    return "unknown";
}

const char *os64_fetch_reason(os64_fetch_t *f)
{
    const os64_fetch_detail_t *d = &f->detail;
    char *out = f->reason;
    size_t cap = sizeof(f->reason);
    switch (f->status) {
        case OS64_FETCH_OK:
            os64_strcopy(out, cap, "no problem");
            break;
        case OS64_FETCH_BAD_URL:
            os64_snprintf(out, cap, "%s", os64_url_reason(d->url));
            break;
        case OS64_FETCH_UNSUPPORTED_SCHEME:
            os64_snprintf(out, cap, "%s is not a scheme this fetches (http and https are)", d->scheme);
            break;
        case OS64_FETCH_DIAL_FAILED:
            os64_snprintf(out, cap, "cannot reach %s%s:%u: %s",
                          d->dial_was_proxy ? "the proxy at " : "", d->dial_host,
                          (unsigned)d->dial_port, os64_dial_reason(d->dial));
            break;
        case OS64_FETCH_PROXY_BAD:
        case OS64_FETCH_REQUEST_FAILED:
            os64_snprintf(out, cap, "%s", d->why);
            break;
        case OS64_FETCH_TLS_FAILED:
            if (d->store_failed)
                os64_snprintf(out, cap, "TLS trust %s (%s: %s, line %lu, policy %u)",
                              os64_tls_store_status_name(d->store),
                              d->store_report.config_stage ? "config" : "bundle",
                              d->store_report.config_stage ? d->store_report.config_path
                                                           : d->store_report.bundle_path,
                              (unsigned long)d->store_report.detail.line,
                              (unsigned)d->store_report.detail.policy_reason);
            else
                os64_snprintf(out, cap, "TLS %s: %s (policy %u, engine %d)",
                              os64_tls_status_name(d->tls),
                              os64_tls_error_description(d->tls, d->tls_policy, d->tls_engine),
                              (unsigned)d->tls_policy, d->tls_engine);
            break;
        case OS64_FETCH_BAD_HEAD:
            if (d->head != HTTP_HEAD_OK)
                os64_snprintf(out, cap, "%s", http_head_reason(d->head));
            else
                os64_snprintf(out, cap, "after %lu bytes, %s",
                              (unsigned long)f->progress.wire, http_body_reason(d->body));
            break;
        case OS64_FETCH_UNSUPPORTED:
            if (os64_streq(d->unsupported, "101"))
                os64_strcopy(out, cap, "the server switched protocols, and the bytes that follow are not HTTP");
            else if (os64_streq(d->unsupported, "head"))
                os64_strcopy(out, cap, "a header the fetch depends on was too long to read");
            else if (d->gzip == OS64_GZIP_UNSUPPORTED)
                os64_strcopy(out, cap, "the gzip reply uses a method this cannot decode");
            else
                os64_snprintf(out, cap, "the reply is framed or coded as '%s', which this does not read",
                              d->unsupported);
            break;
        case OS64_FETCH_SILENT:
        {
            // The budget in the caller's own unit: whole seconds when it is
            // whole seconds, milliseconds otherwise — "0 seconds" for a
            // 500 ms budget was a false duration (Codex, PR #92 rd4).
            char idle[32];
            if (f->io.idle_ms % 1000u == 0)
                os64_snprintf(idle, sizeof(idle), "%u seconds", f->io.idle_ms / 1000u);
            else
                os64_snprintf(idle, sizeof(idle), "%u ms", f->io.idle_ms);
            if (d->silent_after_body)
                os64_snprintf(out, cap, "the server went silent for %s after %lu bytes",
                              idle, (unsigned long)f->progress.wire);
            else
                os64_snprintf(out, cap, "the server went silent for %s before the reply was whole",
                              idle);
            break;
        }
        case OS64_FETCH_REDIRECT_STOPPED:
            switch (d->hop.kind) {
                case OS64_FETCH_HOP_WHOLE:
                    os64_snprintf(out, cap, "%ld %s -> %s, and the caller said stop",
                                  (long)d->hop.status, f->head.reason, d->hop.whole);
                    break;
                case OS64_FETCH_HOP_NONE:
                    os64_snprintf(out, cap, "%ld %s, and it does not say where to",
                                  (long)d->hop.status, f->head.reason);
                    break;
                case OS64_FETCH_HOP_TOO_LONG:
                    os64_snprintf(out, cap, "%ld %s, and the address it points at is longer than this will hold",
                                  (long)d->hop.status, f->head.reason);
                    break;
                case OS64_FETCH_HOP_UNUSABLE:
                    os64_snprintf(out, cap, "it points at %s, which is not a usable address: %s",
                                  d->hop.whole, os64_url_reason(d->hop.parse));
                    break;
                case OS64_FETCH_HOP_SCHEME:
                    os64_snprintf(out, cap, "it points at %s, and this fetches http and https addresses",
                                  d->hop.whole);
                    break;
                case OS64_FETCH_HOP_DOWNGRADE:
                    os64_snprintf(out, cap, "refusing HTTPS-to-HTTP downgrade to %s; the target is unencrypted",
                                  d->hop.whole);
                    break;
                case OS64_FETCH_HOP_PROXY:
                    os64_snprintf(out, cap, "it points at %s, and %s", d->hop.whole, d->hop.why);
                    break;
                case OS64_FETCH_HOP_SELF:
                    os64_snprintf(out, cap, "it points back at %s, the address that just answered; the server is going in a circle",
                                  d->hop.whole);
                    break;
            }
            break;
        case OS64_FETCH_TOO_MANY_HOPS:
            os64_snprintf(out, cap, "sent somewhere else %u times and still going (next: %s)",
                          (unsigned)f->opt.max_hops, d->hop.whole);
            break;
        case OS64_FETCH_CUT:
            if (f->head.has_length)
                os64_snprintf(out, cap, "the reply ended after %lu of %lu bytes",
                              (unsigned long)f->progress.wire, (unsigned long)f->head.length);
            else
                os64_snprintf(out, cap, "the reply ended after %lu bytes, before its last chunk",
                              (unsigned long)f->progress.wire);
            break;
        case OS64_FETCH_BROKE:
            os64_snprintf(out, cap, "the connection broke after %lu bytes", (unsigned long)f->progress.wire);
            break;
        case OS64_FETCH_CORRUPT:
            os64_snprintf(out, cap, "bad gzip reply: %s", os64_gzip_status_name(d->gzip));
            break;
        case OS64_FETCH_LIMIT:
            if (d->gzip == OS64_GZIP_LIMIT)
                os64_snprintf(out, cap, "the gzip reply expands past its %lu-byte safety limit"
                              " (at most %lux wire size and %lu MiB overall)",
                              (unsigned long)d->gzip_limit, (unsigned long)OS64_FETCH_GZIP_RATIO_MAX,
                              (unsigned long)(OS64_FETCH_GZIP_OUTPUT_MAX / 1024 / 1024));
            else
                os64_snprintf(out, cap, "the body is larger than the %lu bytes asked for",
                              (unsigned long)f->opt.max_body);
            break;
        case OS64_FETCH_INTERRUPTED:
            os64_strcopy(out, cap, "interrupted");
            break;
        case OS64_FETCH_NO_MEMORY:
            os64_strcopy(out, cap, "out of memory");
            break;
    }
    // Every sentence is ASCII: the glass is Latin-1/CP437 and draws a UTF-8
    // dash as three glyphs, and these sentences are headed for a status line.
    // A body that ended badly on an encrypted transport has the transport's
    // verdict behind it (a truncation the peer never authenticated, an
    // engine error), and the numbers are worth the tail of the sentence.
    if ((f->status == OS64_FETCH_CUT || f->status == OS64_FETCH_BROKE ||
         f->status == OS64_FETCH_SILENT || f->status == OS64_FETCH_BAD_HEAD) &&
        fetch_transport_tls_failed(&f->io)) {
        size_t have = os64_strlen(out);
        if (have < cap)
            os64_snprintf(out + have, cap - have, " (TLS %s: %s; policy %u, engine %d)",
                          os64_tls_status_name(d->tls),
                          os64_tls_error_description(d->tls, d->tls_policy, d->tls_engine),
                          (unsigned)d->tls_policy, d->tls_engine);
    }
    return f->reason;
}

void os64_fetch_close(os64_fetch_t *f)
{
    if (f == NULL)
        return;
    if (f->connected) {
        // A bounded TLS closure only for a body that ended whole; a fetch
        // that stopped anywhere else does not wait on the peer.
        fetch_transport_close(&f->io, f->over && f->status == OS64_FETCH_OK);
        f->connected = false;
    }
    if (f->gz != NULL)
        os64_gzip_destroy(f->gz);
    if (f->wire != NULL)
        os64_free(f->wire);
    if (f->trust_owned && f->trust != NULL)
        os64_tls_trust_free(f->trust);
    os64_free(f);
}
