// fetch_job.c — a script's request, hop by hop (fetch_job.h).
//
// THE LOOP IS THE FETCH STANDARD'S § 4.1 main fetch, re-entered for every
// redirect (§ 4.5's last step), with § 4.4's checks on each response. Every
// question it asks is origin.c's; this file does the I/O and the order.
// libfetch is asked one hop at a time with a method by name, so it follows
// no redirect itself: its own rule for credentials across redirects (keep
// Cookie and Authorization to the TYPED origin) never fires inside this job,
// because each hop is typed afresh. The standard's rule replaces it here:
// cookies go only when the hop's includeCredentials says so (hop_cookies),
// and Authorization is struck by way_redirect_follow the moment a redirect
// leaves the current origin, for good. Nothing here may rely on libfetch's.
//
// THE ORDER IS PART OF THE RULE. A hop's Set-Cookie is heard while libfetch
// reads its head, inside os64_fetch_open, BEFORE the CORS check below can
// fail it: § 4.7 stores cookies and § 4.4 checks afterward, and § 3.3.6 says
// a credentialed answer's cookies stand even when the page may not read it.
//
// LOCKS. A hop takes the jar's lock (choosing cookies, hearing them) and the
// preflight cache's (asking, storing, clearing), each briefly and never one
// inside the other, and holds neither across libfetch or across a call into
// this file. A synchronous XHR runs this on the window thread, so nothing on
// the window thread may hold either lock while it runs one: the window takes
// them only through these same calls.

#include "fetch_job.h"

#include "os64/date.h"
#include "os64/fmt.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/str.h"
#include "ticker.h"

// ── A hop's conversation with libfetch ──────────────────────────────────

typedef struct {
    yonder_fetch_job_t *job;
    way_request_t *r;
    const char *url;                // this hop's address
    bool preflight;                 // a preflight: no cookies go, none are heard
    // The request's Fetch Metadata, stepped at every hop of its URL list,
    // and the lines the page set of its own (WAY_GIVEN_: Accept-Language,
    // Priority), which the browser's then does not double.
    way_metadata_t metadata;
    unsigned page_given;
    bool (*cancelled)(void *ctx);
    void *cancel_ctx;
    // Where the cookies a hop left out for want of room are noted: the
    // product's (yonder_fetched_t.cookies_short).
    way_cookies_short_t *cookies_short;
    // The head as libfetch read it: its lines as a header block, and the
    // names of the lines it could not read for their length.
    char *block;
    size_t len, cap;
    char *omitted;
    size_t olen, ocap;
    bool too_long;                  // past YONDER_FETCH_HEAD_MAX
    bool no_memory;
} Hop;

static int64_t now_utc(void)
{
    os64_time_t t;
    return os64_time(&t) == 0 ? t.epoch : 0;
}

static bool hop_cancelled(void *ctx)
{
    Hop *h = ctx;
    return h->cancelled != NULL && h->cancelled(h->cancel_ctx);
}

// THE ONE PLACE that chooses which cookies a hop carries: the jar's for the
// hop's own address and transport, HttpOnly ones included (§ 3.1.1), and
// only when the hop's includeCredentials is true (§ 4.6). SameSite is not
// enforced here: the jar records it and does not choose by it, so under
// `include` a cross-site script request carries Lax and Strict cookies a
// browser would not send (DEBTS, paid with the public-suffix list).
static size_t hop_cookies(const Hop *h, const os64_url_t *to, bool encrypted, char *out, size_t cap)
{
    static const char kCookie[] = "Cookie: ";
    // The line within WAY_COOKIE_LINE_MAX, as a page's own hops (jar.h).
    if (cap > WAY_COOKIE_LINE_MAX)
        cap = WAY_COOKIE_LINE_MAX;
    if (h->preflight || h->job->jar == NULL || !way_credentials_included(h->r) ||
        cap <= sizeof(kCookie) + 2)
        return 0;
    size_t room = cap - (sizeof(kCookie) - 1) - 2;
    int32_t left = 0;
    size_t n = way_jar_cookies(h->job->jar, to, encrypted, now_utc(), out + sizeof(kCookie) - 1, room, &left);
    way_cookies_short_note(h->cookies_short, to, left);
    if (n == 0) {
        out[0] = '\0';
        return 0;
    }
    os64_memcpy(out, kCookie, sizeof(kCookie) - 1);
    size_t used = sizeof(kCookie) - 1 + n;
    out[used++] = '\r';
    out[used++] = '\n';
    out[used] = '\0';
    return used;
}

// The lines that say who is asking, for a hop to `to`: the request's
// metadata stepped there, or the preflight's for it; the agent's preset
// lines beside them. The length written, within WAY_IDENTITY_MAX: 0 when
// they do not fit, which sends the hop without them.
static size_t hop_identity(Hop *h, const os64_url_t *to, char *out, size_t cap)
{
    way_origin_t where;
    way_origin_of_url(to, 0, &where);
    way_metadata_hop(&h->metadata, &where);
    way_metadata_t m = h->preflight ? way_metadata_preflight(&h->metadata) : h->metadata;
    // A preflight carries none of the page's headers, so the browser's
    // Accept-Language and Priority go with it whatever the page set.
    unsigned given = h->preflight ? 0 : h->page_given;
    if (!way_identity_lines(h->job->identity, &m, &where, given, out,
                            cap < WAY_IDENTITY_MAX ? cap : WAY_IDENTITY_MAX))
        return 0;
    return os64_strlen(out);
}

// libfetch's per-hop fields: who is asking, the cookies above, then the
// Referer the request's policy gives this hop, when it fits after them.
static bool hop_fields(void *ctx, const os64_url_t *to, bool encrypted, char *out, size_t cap)
{
    Hop *h = ctx;
    out[0] = '\0';
    size_t used = hop_identity(h, to, out, cap);
    used += hop_cookies(h, to, encrypted, out + used, cap - used);
    char referrer[OS64_FETCH_URL_MAX];
    if (way_referrer_for(h->r, h->job->referrer, h->url, referrer, sizeof(referrer))) {
        size_t n = os64_strlen(referrer);
        if (used + 9 + n + 3 <= cap) {
            os64_memcpy(out + used, "Referer: ", 9);
            os64_memcpy(out + used + 9, referrer, n);
            used += 9 + n;
            out[used++] = '\r';
            out[used++] = '\n';
            out[used] = '\0';
        }
    }
    return true;
}

static void hop_cookie_heard(void *ctx, const os64_url_t *from, bool encrypted, const char *value, size_t len)
{
    Hop *h = ctx;
    if (!h->preflight && h->job->jar != NULL && way_credentials_included(h->r))
        way_jar_hear(h->job->jar, from, encrypted, value, len, now_utc());
}

// `n` bytes onto a growing buffer, within YONDER_FETCH_HEAD_MAX.
static bool grow_append(Hop *h, char **buf, size_t *len, size_t *cap, const char *a, size_t n)
{
    if (*len + n + 1 > YONDER_FETCH_HEAD_MAX) {
        h->too_long = true;
        return false;
    }
    if (*len + n + 1 > *cap) {
        size_t want = *cap ? *cap * 2 : 1024;
        while (want < *len + n + 1)
            want *= 2;
        if (want > YONDER_FETCH_HEAD_MAX)
            want = YONDER_FETCH_HEAD_MAX;
        char *grown = os64_realloc(*buf, want);
        if (grown == NULL) {
            h->no_memory = true;
            return false;
        }
        *buf = grown;
        *cap = want;
    }
    os64_memcpy(*buf + *len, a, n);
    *len += n;
    (*buf)[*len] = '\0';
    return true;
}

static void hop_header(void *ctx, const char *name, size_t nlen, const char *value, size_t vlen)
{
    Hop *h = ctx;
    if (h->too_long || h->no_memory)
        return;
    if (value == NULL) {
        if (grow_append(h, &h->omitted, &h->olen, &h->ocap, name, nlen))
            grow_append(h, &h->omitted, &h->olen, &h->ocap, "\r\n", 2);
        return;
    }
    if (grow_append(h, &h->block, &h->len, &h->cap, name, nlen) &&
        grow_append(h, &h->block, &h->len, &h->cap, ": ", 2) &&
        grow_append(h, &h->block, &h->len, &h->cap, value, vlen))
        grow_append(h, &h->block, &h->len, &h->cap, "\r\n", 2);
}

static void hop_reset(Hop *h, const char *url, bool preflight)
{
    h->url = url;
    h->preflight = preflight;
    h->len = h->olen = 0;
    if (h->block != NULL)
        h->block[0] = '\0';
    if (h->omitted != NULL)
        h->omitted[0] = '\0';
    h->too_long = h->no_memory = false;
}

static way_response_t hop_response(const Hop *h, int32_t status)
{
    way_response_t resp = {status, h->block != NULL ? h->block : "", h->len,
                           h->omitted != NULL ? h->omitted : "", h->olen};
    return resp;
}

// ── The request's own headers, as libfetch takes them ───────────────────

typedef struct {
    char accept[OS64_FETCH_ACCEPT_MAX];
    size_t accepts;                          // Accept lines the page wrote, empty ones included
    char body[OS64_FETCH_EXTRA_MAX];         // the representation headers
    char extra[OS64_FETCH_EXTRA_MAX];        // the rest, and this hop's Origin
    bool identity;                           // a Range: the representation's own bytes
} Fields;

static bool append_line(char *out, size_t cap, const char *name, size_t nlen, const char *value, size_t vlen)
{
    size_t at = os64_strlen(out);
    if (at + nlen + 2 + vlen + 3 > cap)
        return false;
    os64_memcpy(out + at, name, nlen);
    at += nlen;
    out[at++] = ':';
    out[at++] = ' ';
    os64_memcpy(out + at, value, vlen);
    at += vlen;
    out[at++] = '\r';
    out[at++] = '\n';
    out[at] = '\0';
    return true;
}

// A header's name keeps the case the page wrote it in (§ 2.2.2's header
// list does), so one is found without regard to case. `lower` is the name
// lowercased.
static bool is_named(const char *name, size_t len, const char *lower)
{
    if (len != os64_strlen(lower))
        return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i] >= 'A' && name[i] <= 'Z' ? (char)(name[i] - 'A' + 'a') : name[i];
        if (c != lower[i])
            return false;
    }
    return true;
}

static bool is_accept(const char *name, size_t len)
{
    return is_named(name, len, "accept");
}

// The browser's lines the page's headers already hold (WAY_GIVEN_): an
// Accept-Language, which Fetch appends the browser's only when there is
// none, and a Priority, which a second one would make a duplicate libfetch
// refuses.
static unsigned page_sets(const way_request_t *r)
{
    unsigned given = 0;
    size_t at = 0;
    way_header_t h;
    while (way_header_next(r->headers, r->headers_len, &at, &h)) {
        if (is_named(h.name, h.name_len, "accept-language"))
            given |= WAY_GIVEN_LANGUAGE;
        if (is_named(h.name, h.name_len, "priority"))
            given |= WAY_GIVEN_PRIORITY;
    }
    return given;
}

// The author headers sorted into libfetch's places: Accept (joined, as
// § 2.2.2's "get" joins), the four body headers, and the rest. A forbidden
// header never reached here from libdom; one that did is dropped again, as
// the transport trusts no caller. False when they do not fit.
static bool fields_of(const way_request_t *r, Fields *f)
{
    os64_memset(f, 0, sizeof(*f));
    size_t at = 0;
    way_header_t h;
    while (way_header_next(r->headers, r->headers_len, &at, &h)) {
        if (way_header_forbidden_request(h.name, h.name_len, h.value, h.value_len))
            continue;
        if (is_accept(h.name, h.name_len)) {
            size_t used = os64_strlen(f->accept);
            bool joined = f->accepts++ > 0;
            if (used + (joined ? 2 : 0) + h.value_len + 1 > sizeof(f->accept))
                return false;
            if (joined) {
                f->accept[used++] = ',';
                f->accept[used++] = ' ';
            }
            os64_memcpy(f->accept + used, h.value, h.value_len);
            f->accept[used + h.value_len] = '\0';
            continue;
        }
        if (way_header_privileged_no_cors(h.name, h.name_len))
            f->identity = true;
        char *into = way_header_request_body(h.name, h.name_len) ? f->body : f->extra;
        if (!append_line(into, sizeof(f->body), h.name, h.name_len, h.value, h.value_len))
            return false;
    }
    // Fetch installs `*/*` only when the list does not CONTAIN Accept: an
    // Accept the page set empty goes empty (libfetch sends an empty one).
    if (f->accepts == 0)
        os64_strcopy(f->accept, sizeof(f->accept), "*/*");
    return true;
}

// ── data: URLs (Fetch § 6) ──────────────────────────────────────────────

static int hex_value(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static bool ascii_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

static int base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Infra's forgiving-base64 decode of `in` into itself (it only shrinks).
static bool forgiving_base64(uint8_t *in, size_t *len)
{
    size_t n = 0;
    for (size_t i = 0; i < *len; i++)
        if (!ascii_space((char)in[i]))
            in[n++] = in[i];
    if (n % 4 == 0 && n >= 1 && in[n - 1] == '=') {
        n--;
        if (n >= 1 && in[n - 1] == '=')
            n--;
    }
    if (n % 4 == 1)
        return false;
    uint32_t buffer = 0;
    int bits = 0;
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        int v = base64_value((char)in[i]);
        if (v < 0)
            return false;
        buffer = (buffer << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            in[out++] = (uint8_t)(buffer >> bits);
            buffer &= (1u << bits) - 1;
        }
    }
    *len = out;
    return true;
}

bool yonder_fetch_data_url(const char *url, uint64_t max_body, uint8_t **body, size_t *len,
                           char *type, size_t type_cap)
{
    *body = NULL;
    *len = 0;
    size_t total = os64_strlen(url);
    if (total < 5 || total > YONDER_FETCH_DATA_URL_MAX)
        return false;
    for (size_t i = 0; i < 4; i++)
        if ((url[i] | 0x20) != "data"[i])
            return false;
    if (url[4] != ':')
        return false;
    const char *p = url + 5, *end = url + total;
    const char *hash = os64_memchr(p, '#', (size_t)(end - p));
    if (hash != NULL)
        end = hash;                                 // the URL without its fragment
    const char *comma = os64_memchr(p, ',', (size_t)(end - p));
    if (comma == NULL)
        return false;
    const char *mt = p, *mt_end = comma;
    while (mt < mt_end && ascii_space(*mt))
        mt++;
    while (mt_end > mt && ascii_space(mt_end[-1]))
        mt_end--;
    // The percent-decoded body is never longer than its text; past the cap
    // in text it may still decode under it, so the cap is judged after.
    size_t text = (size_t)(end - comma - 1);
    uint8_t *bytes = os64_malloc(text + 1);
    if (bytes == NULL)
        return false;
    size_t n = 0;
    for (const char *q = comma + 1; q < end; q++) {
        if (*q == '%' && end - q > 2 && hex_value(q[1]) >= 0 && hex_value(q[2]) >= 0) {
            bytes[n++] = (uint8_t)(hex_value(q[1]) * 16 + hex_value(q[2]));
            q += 2;
        } else {
            bytes[n++] = (uint8_t)*q;
        }
    }
    // "…;base64", its case not regarded, with zero or more U+0020 SPACE
    // between: SPACE only, not ASCII whitespace, as Fetch's data: URL
    // processor says (a tab there leaves the body as written).
    const char *b = mt_end;
    bool base64 = false;
    if (b - mt >= 6) {
        bool word = true;
        for (size_t i = 0; i < 6; i++)
            if ((b[-6 + (ptrdiff_t)i] | 0x20) != "base64"[i])
                word = false;
        if (word) {
            const char *s = b - 6;
            while (s > mt && s[-1] == ' ')
                s--;
            if (s > mt && s[-1] == ';') {
                base64 = true;
                mt_end = s - 1;
            }
        }
    }
    if (base64 && !forgiving_base64(bytes, &n)) {
        os64_free(bytes);
        return false;
    }
    if ((uint64_t)n > max_body) {
        os64_free(bytes);
        return false;
    }
    // Its type: what was written (`text/plain` before a bare parameter
    // list), parsed and serialized whole, every parameter that parses kept;
    // US-ASCII text when what was written is not a MIME type, or is longer
    // than WAY_MIME_SERIALIZE_IN_MAX.
    char written[WAY_MIME_SERIALIZE_IN_MAX + 1];
    size_t wlen = (size_t)(mt_end - mt);
    bool fit;
    if (wlen > 0 && mt[0] == ';')
        fit = os64_snprintf(written, sizeof(written), "text/plain%.*s", (int)wlen, mt) < (int32_t)sizeof(written);
    else
        fit = os64_snprintf(written, sizeof(written), "%.*s", (int)wlen, mt) < (int32_t)sizeof(written);
    if (!fit || !way_mime_serialize(written, os64_strlen(written), type, type_cap))
        os64_strcopy(type, type_cap, "text/plain;charset=US-ASCII");
    *body = bytes;
    *len = n;
    return true;
}

// ── The product ─────────────────────────────────────────────────────────

void yonder_fetched_free(yonder_fetched_t *got)
{
    if (got == NULL)
        return;
    os64_free(got->url);
    os64_free(got->headers);
    os64_free(got->body);
    yonder_bytecode_free(got->bytecode);
    os64_free(got);
}

void yonder_fetch_release(void *job, void *product)
{
    yonder_fetch_job_t *j = job;
    yonder_fetched_free(product);
    if (j != NULL) {
        os64_free(j->url);
        os64_free(j->headers);
        os64_free(j->body);
        os64_free(j);
    }
}

// A network error, and the page record's reason for it.
static int64_t network_error(yonder_fetched_t *got, const char *why)
{
    got->ok = false;
    got->type = WAY_TYPE_ERROR;
    got->status = 0;
    got->has_body = false;
    os64_strcopy(got->why, sizeof(got->why), why);
    return 0;
}

static char *copy_text(const char *s, size_t n)
{
    char *c = os64_malloc(n + 1);
    if (c != NULL) {
        os64_memcpy(c, s, n);
        c[n] = '\0';
    }
    return c;
}

// The address without its fragment, os64_malloc'd.
static char *without_fragment(const char *url)
{
    const char *hash = os64_strchr(url, '#');
    return copy_text(url, hash != NULL ? (size_t)(hash - url) : os64_strlen(url));
}

// The body, whole, up to `cap`. False when the read did not end whole, or
// passed the cap (`*limit`), or memory ran out.
static bool read_body(os64_fetch_t *f, uint64_t cap, uint8_t **out, size_t *len, bool *limit)
{
    size_t at = 0, size = 16 * 1024;
    uint8_t *buf = os64_malloc(size);
    *limit = false;
    if (buf == NULL)
        return false;
    for (;;) {
        if (at == size) {
            size_t want = size * 2;
            if ((uint64_t)want > cap + 1)
                want = (size_t)cap + 1;
            if (want <= size) {
                // Ask for the byte past the cap: libfetch refuses a body at
                // max_body on the read that would pass it.
                uint8_t probe;
                if (os64_fetch_read(f, &probe, 1) > 0)
                    *limit = true;
                break;
            }
            uint8_t *grown = os64_realloc(buf, want);
            if (grown == NULL) {
                os64_free(buf);
                return false;
            }
            buf = grown;
            size = want;
        }
        int64_t n = os64_fetch_read(f, buf + at, size - at);
        if (n <= 0)
            break;
        at += (size_t)n;
    }
    if (*limit || os64_fetch_status(f) != OS64_FETCH_OK) {
        *limit = *limit || os64_fetch_status(f) == OS64_FETCH_LIMIT;
        os64_free(buf);
        return false;
    }
    *out = buf;
    *len = at;
    return true;
}

// ── The hops ────────────────────────────────────────────────────────────

// The hops that took § 4.1's preflight branch: a network error anywhere
// after clears their cache entries ("clear cache entries using request"),
// every one. An entry's key is the page's site, the SERIALIZED REQUEST
// ORIGIN and the URL, and the request origin turns to "null" once a
// redirect taints the request, so each branch keeps the redirect-taint it
// had when its preflight was made, beside its URL, and is cleared under
// that: a later hop's taint must not make the clear miss an earlier grant.
typedef struct {
    char *urls[WAY_REDIRECTS_MAX + 1];
    way_rtaint_t rtaints[WAY_REDIRECTS_MAX + 1];
    int32_t n;
} Branches;

static void branches_clear(Branches *b, yonder_fetch_job_t *job, const way_request_t *r, bool failed)
{
    for (int32_t i = 0; i < b->n; i++) {
        if (failed) {
            way_request_t then = *r;        // the key reads the origin and the taint alone
            then.rtaint = b->rtaints[i];
            way_preflight_clear(job->preflight, &then, b->urls[i]);
        }
        os64_free(b->urls[i]);
    }
    b->n = 0;
}

static void opt_base(os64_fetch_options_t *opt, Hop *h, yonder_fetch_job_t *job, uint64_t max_body)
{
    os64_memset(opt, 0, sizeof(*opt));
    // The preset's order for the request, or for its preflight (hop_reset
    // has said which this is).
    way_metadata_t m = h->preflight ? way_metadata_preflight(&h->metadata) : h->metadata;
    opt->order = way_identity_order(job->identity, &m);
    opt->keep_alive = way_identity_keep_alive(job->identity);
    opt->user_agent = job->agent;
    opt->max_body = max_body;
    opt->cancelled = hop_cancelled;
    opt->headers_for = hop_fields;
    opt->on_set_cookie = hop_cookie_heard;
    opt->on_header = hop_header;
    opt->ctx = h;
}

// § 4.8: the preflight for this hop, judged, and its grants kept. False
// with the reason in `got` when the actual request may not go.
static bool preflight(Hop *h, yonder_fetch_job_t *job, way_request_t *r, const char *url, yonder_fetched_t *got)
{
    char lines[OS64_FETCH_EXTRA_MAX], origin[WAY_ORIGIN_TEXT_MAX];
    if (!way_preflight_lines(r, lines, sizeof(lines)) || !way_origin_header(r, url, origin, sizeof(origin)) ||
        !append_line(lines, sizeof(lines), "Origin", 6, origin, os64_strlen(origin))) {
        network_error(got, "the preflight's headers do not fit");
        return false;
    }
    hop_reset(h, url, true);
    os64_fetch_options_t opt;
    opt_base(&opt, h, job, 64 * 1024);
    opt.method_name = "OPTIONS";
    opt.accept = "*/*";
    opt.extra_headers = lines;
    os64_fetch_t *f = os64_fetch_open(url, &opt);
    if (f == NULL) {
        network_error(got, "out of memory");
        return false;
    }
    os64_fetch_status_t st = os64_fetch_status(f);
    const os64_fetch_head_t *head = os64_fetch_head(f);
    bool ok = false;
    if (h->too_long || h->no_memory) {
        network_error(got, h->too_long ? "the preflight's answer has more head than 64 KiB" : "out of memory");
    } else if (head == NULL || (st != OS64_FETCH_OK && st != OS64_FETCH_REDIRECT_STOPPED)) {
        char why[256];
        os64_snprintf(why, sizeof(why), "the preflight failed: %s", os64_fetch_reason(f));
        network_error(got, why);
    } else {
        way_response_t resp = hop_response(h, head->status);
        uint32_t age;
        if (way_preflight_judge(r, &resp, &age)) {
            way_preflight_store(job->preflight, r, url, &resp, age, yonder_now_ms());
            ok = true;
        } else {
            network_error(got, "CORS: the preflight's answer does not allow the request");
        }
    }
    os64_fetch_close(f);
    return ok;
}

// ── The final response ──────────────────────────────────────────────────

// The final response, filtered for the page: from the network (`f`, whose
// body is read here) or from the cache (`kept`, `kept_len`, `f` NULL) — the
// same filtering either way, so a page cannot tell them apart. False, with
// the reason in `got`, when it could not be had.
static bool finish(yonder_fetched_t *got, way_request_t *r, const char *current, int32_t status,
                   const char *reason, const way_response_t *resp, os64_fetch_t *f, const uint8_t *kept,
                   size_t kept_len, uint64_t max_body, bool image)
{
    way_type_t type = way_response_type(r);
    got->type = type;
    got->status = type == WAY_TYPE_OPAQUE && !image ? 0 : status;
    if (type != WAY_TYPE_OPAQUE)
        os64_strcopy(got->status_text, sizeof(got->status_text), reason);
    got->url = type == WAY_TYPE_OPAQUE ? copy_text("", 0) : without_fragment(current);
    got->redirected = type != WAY_TYPE_OPAQUE && r->urls > 1;
    got->hops = r->urls - 1;
    got->headers = os64_malloc(resp->headers_len + 1);
    if (got->url == NULL || got->headers == NULL)
        return network_error(got, "out of memory"), false;
    got->headers_len = way_response_headers(r, type, resp, got->headers, resp->headers_len + 1);
    if (got->headers_len == SIZE_MAX)
        return network_error(got, "the answer's headers did not fit"), false;
    // Script responses discard opaque bodies. Native images retain pixels
    // with the opaque verdict so Canvas can enforce origin tainting.
    got->has_body = way_response_has_body(r, image ? WAY_TYPE_BASIC : type, status);
    if (got->has_body && f != NULL) {
        bool limit;
        if (!read_body(f, max_body, &got->body, &got->body_len, &limit)) {
            char text[256];
            os64_snprintf(text, sizeof(text), "%s", limit ? "the body is past its cap" : os64_fetch_reason(f));
            return network_error(got, text), false;
        }
    } else if (got->has_body) {
        got->body = os64_malloc(kept_len > 0 ? kept_len : 1);
        if (got->body == NULL)
            return network_error(got, "out of memory"), false;
        os64_memcpy(got->body, kept, kept_len);
        got->body_len = kept_len;
    }
    got->ok = true;
    return true;
}

// ── The cache (SCRIPT_CACHE.md § 5-6) ───────────────────────────────────
//
// A request's first hop, a GET with no body, reads and keeps its answer in
// the browser's cache under the request's key: the page's partition, whether
// this hop sends credentials, the address. What it found is judged as an
// answer from the network is — the live CORS check over its kept head — and
// a stale one is asked about with its validator, added below the preflight,
// so it never makes a request worth one.

typedef struct {
    char partition[WAY_PARTITION_MAX];
    way_cache_key_t key;
    char *url;                      // the address the key names
    bool have;                      // an entry, the Vary'd lines the request's
    bool asked;                     // its validator went with the request
    bool held;                      // the entry holds the body the answer has
    way_entry_t e;
    uint8_t *body;                  // the entry's body, its blocks after it (way_cache_find)
    size_t len;
    way_blocks_t blocks;
    // What the request sends of its own, for Vary: the Accept, and the
    // lines the page and the hop added (Origin among them).
    char accept[OS64_FETCH_ACCEPT_MAX];
    char extra[OS64_FETCH_EXTRA_MAX];
    char representation[OS64_FETCH_EXTRA_MAX];
} Cached;

static void cached_free(Cached *c)
{
    if (c != NULL) {
        os64_free(c->body);
        os64_free(c->url);
        os64_free(c);
    }
}

// Every line `name` in `block`, appended to `out` at `*at`, joined by ", "
// to what is there: repeated lines are one list (RFC 9110 § 5.3), and a
// request that differs in any of them is another request. `*found` says
// whether one was; false when they do not fit.
static bool block_values(const char *block, const char *name, size_t nlen, char *out, size_t cap, size_t *at,
                         bool *found)
{
    size_t from = 0, len = os64_strlen(block);
    way_header_t h;
    while (way_header_next(block, len, &from, &h))
        if (h.name_len == nlen) {
            size_t i = 0;
            while (i < nlen && (h.name[i] | 0x20) == (name[i] | 0x20))
                i++;
            if (i != nlen)
                continue;
            size_t sep = *found ? 2 : 0;
            if (*at + sep + h.value_len >= cap)
                return false;
            if (sep != 0)
                os64_memcpy(out + *at, ", ", 2);
            os64_memcpy(out + *at + sep, h.value, h.value_len);
            *at += sep + h.value_len;
            out[*at] = '\0';
            *found = true;
        }
    return true;
}

// What the request sends for header `name` (way_request_value_t): its own
// lines', or NONE for one it does not send. The lines libfetch and the
// agent's preset add are not in the request's own, so for those it cannot
// say, and nothing that varies on them is kept.
static way_said_t request_value(void *ctx, const char *name, size_t nlen, char *out, size_t cap)
{
    const Cached *c = ctx;
    static const char *const kAdded[] = {"user-agent", "accept-language", "cookie", "referer", "priority",
                                         "host", "connection", "authorization"};
    if (is_named(name, nlen, "accept"))
        return os64_strcopy(out, cap, c->accept) < cap ? WAY_SAID_VALUE : WAY_SAID_UNKNOWN;
    size_t at = 0;
    bool found = false;
    out[0] = '\0';
    if (!block_values(c->extra, name, nlen, out, cap, &at, &found) ||
        !block_values(c->representation, name, nlen, out, cap, &at, &found))
        return WAY_SAID_UNKNOWN;
    if (found)
        return WAY_SAID_VALUE;
    for (size_t i = 0; i < sizeof(kAdded) / sizeof(kAdded[0]); i++)
        if (is_named(name, nlen, kAdded[i]))
            return WAY_SAID_UNKNOWN;
    if (nlen >= 4 && is_named(name, 4, "sec-"))
        return WAY_SAID_UNKNOWN;
    return WAY_SAID_NONE;
}

// Whether a Cache-Control or Pragma value holds `directive` (a token, or
// "max-age=0" spelled with any spacing the list allows), ignoring case.
static bool has_directive(const char *v, size_t len, const char *directive)
{
    size_t dlen = os64_strlen(directive);
    for (size_t at = 0; at < len;) {
        while (at < len && (v[at] == ' ' || v[at] == '\t' || v[at] == ','))
            at++;
        size_t end = at;
        while (end < len && v[end] != ',')
            end++;
        size_t last = end;
        while (last > at && (v[last - 1] == ' ' || v[last - 1] == '\t'))
            last--;
        if (last - at == dlen && is_named(v + at, dlen, directive))
            return true;
        at = end;
    }
    return false;
}

// The cache mode the request's own lines make of the page's (Fetch's
// HTTP-network-or-cache fetch; RFC 9111 § 5.2.1 for a request's own
// directives): a default-mode request carrying a precondition, or asking
// no-store, is no-store, since its condition is the server's to judge and
// a kept reply would answer without asking it; one asking no-cache or
// max-age=0 (or Pragma no-cache with no Cache-Control) is asked about
// every time, as no-cache mode is.
static way_cache_mode_t cache_mode_of(const yonder_fetch_job_t *job)
{
    static const char *const kConditions[] = {"if-match", "if-none-match", "if-modified-since",
                                              "if-unmodified-since", "if-range"};
    if (job->cache_mode != WAY_CACHE_DEFAULT)
        return job->cache_mode;
    bool conditional = false, no_store = false, no_cache = false, control = false, pragma = false;
    size_t at = 0, len = job->headers != NULL ? os64_strlen(job->headers) : 0;
    way_header_t h;
    while (way_header_next(job->headers, len, &at, &h)) {
        for (size_t i = 0; i < sizeof(kConditions) / sizeof(kConditions[0]); i++)
            conditional |= is_named(h.name, h.name_len, kConditions[i]);
        if (is_named(h.name, h.name_len, "cache-control")) {
            control = true;
            no_store |= has_directive(h.value, h.value_len, "no-store");
            no_cache |= has_directive(h.value, h.value_len, "no-cache") ||
                        has_directive(h.value, h.value_len, "max-age=0");
        } else if (is_named(h.name, h.name_len, "pragma")) {
            pragma |= has_directive(h.value, h.value_len, "no-cache");
        }
    }
    if (conditional || no_store)
        return WAY_CACHE_NO_STORE;
    return no_cache || (pragma && !control) ? WAY_CACHE_NO_CACHE : WAY_CACHE_DEFAULT;
}

// Whether this request may use the cache at all: a GET with no body, and
// no Range, whose answer is a part of a reply the cache would keep whole.
static bool cache_wanted(const yonder_fetch_job_t *job, const way_request_t *r)
{
    bool range = false;
    size_t at = 0, len = job->headers != NULL ? os64_strlen(job->headers) : 0;
    way_header_t h;
    while (!range && way_header_next(job->headers, len, &at, &h))
        range = is_named(h.name, h.name_len, "range");
    return !range && way_cache_enabled(job->cache) && job->cache_mode != WAY_CACHE_NO_STORE &&
           os64_streq(r->method, "GET") && !r->body;
}

// The request's key, what it sends, and what the cache holds under the key
// for a request that sends that — unless it is a reload, which reads
// nothing. NULL on no memory, which uses no cache.
static Cached *cache_open(const yonder_fetch_job_t *job, const way_request_t *r, const char *current,
                          const Fields *fields, const char *accept, uint64_t max_body)
{
    Cached *c = os64_calloc(1, sizeof(*c));
    if (c == NULL || (c->url = copy_text(current, os64_strlen(current))) == NULL) {
        os64_free(c);
        return NULL;
    }
    way_partition_of(&r->origin, c->partition, sizeof(c->partition));
    c->key = (way_cache_key_t){c->partition, way_credentials_included(r), c->url};
    os64_strcopy(c->accept, sizeof(c->accept), accept != NULL ? accept : "");
    os64_strcopy(c->extra, sizeof(c->extra), fields->extra);
    os64_strcopy(c->representation, sizeof(c->representation), fields->body);
    // An entry way_fetch_whole kept (a picture's, a sheet's, a classic
    // script's) has no head, which a script's request reads: it is a miss,
    // and the answer kept now replaces it.
    if (job->cache_mode != WAY_CACHE_RELOAD &&
        way_cache_find(job->cache, &c->key, &c->e, &c->body, &c->len, &c->blocks, (size_t)max_body)) {
        c->have = c->blocks.headers_len > 0 && way_vary_matches(c->blocks.vary, c->blocks.vary_len, request_value, c);
        if (!c->have) {
            os64_free(c->body);
            c->body = NULL;
        }
    }
    return c;
}

// What the cache's kept reply answers, judged as the network's would be.
// False when the live CORS check refuses it: a miss, not an error
// (SCRIPT_CACHE.md § 6, ruled), so the network is asked.
static bool serve_kept(Cached *c, way_request_t *r, const char *current, const way_blocks_t *b,
                       yonder_fetched_t *got, uint64_t max_body, way_served_t how)
{
    way_response_t resp = {c->e.status, b->headers, b->headers_len, b->omitted, b->omitted_len};
    if (r->tainting == WAY_TAINT_CORS && !way_cors_check(r, &resp))
        return false;
    finish(got, r, current, c->e.status, "OK", &resp, NULL, c->body, c->len, max_body, false);
    got->served = how;
    return true;
}

typedef enum {
    CACHE_MISS,                     // ask the network as if nothing were kept
    CACHE_ASKED,                    // ask the network with the kept reply's validator
    CACHE_ANSWERED,                 // the kept reply is the answer (in `got`)
    CACHE_REFUSED,                  // only-if-cached, with nothing it may serve
} CacheAnswer;

// The cache's answer for this hop, under the request's cache mode
// (SCRIPT_CACHE.md § 5's table). ASKED has added the validator to `fields`.
static CacheAnswer cache_answer(const yonder_fetch_job_t *job, way_request_t *r, const char *current,
                                Cached *c, Fields *fields, yonder_fetched_t *got, uint64_t max_body)
{
    way_cache_mode_t mode = job->cache_mode;
    if (!c->have)
        return mode == WAY_CACHE_ONLY_IF_CACHED ? CACHE_REFUSED : CACHE_MISS;
    bool any_age = mode == WAY_CACHE_FORCE_CACHE || mode == WAY_CACHE_ONLY_IF_CACHED;
    if (any_age || (mode != WAY_CACHE_NO_CACHE && way_keep_fresh(&c->e.keep, now_utc()))) {
        if (serve_kept(c, r, current, &c->blocks, got, max_body, WAY_SERVED_KEPT)) {
            c->held = got->ok;
            return CACHE_ANSWERED;
        }
        c->have = false;
        return mode == WAY_CACHE_ONLY_IF_CACHED ? CACHE_REFUSED : CACHE_MISS;
    }
    char conditions[OS64_FETCH_EXTRA_MAX];
    size_t n = way_keep_conditions(&c->e.keep, conditions, sizeof(conditions));
    if (n == 0 || os64_strlen(fields->extra) + n + 1 > sizeof(fields->extra))
        return CACHE_MISS;
    os64_memcpy(fields->extra + os64_strlen(fields->extra), conditions, n + 1);
    c->asked = true;
    return CACHE_ASKED;
}

// Whether two header names are one, any case.
static bool same_name(const char *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen)
        return false;
    for (size_t i = 0; i < alen; i++)
        if ((a[i] | 0x20) != (b[i] | 0x20))
            return false;
    return true;
}

// Whether the name list `names` ("name\r\n" lines) holds `name`, any case.
static bool names_hold(const char *names, size_t len, const char *name, size_t nlen)
{
    for (size_t at = 0; at < len;) {
        size_t end = at;
        while (end < len && names[end] != '\r' && names[end] != '\n')
            end++;
        if (same_name(names + at, end - at, name, nlen))
            return true;
        at = end;
        while (at < len && (names[at] == '\r' || names[at] == '\n'))
            at++;
    }
    return false;
}

// The kept reply's omitted names with a 304's laid over them, by name as
// its header lines are (way_headers_update): a kept name the 304 sends a
// line of, readable or omitted, is the 304's now, and the 304's own omitted
// names follow. Into `out`, which holds both lists and a line end more;
// its length.
static size_t omitted_update(const char *kept, size_t klen, const char *block, size_t blen, const char *update,
                             size_t ulen, char *out)
{
    size_t n = 0;
    for (size_t at = 0; at < klen;) {
        size_t end = at;
        while (end < klen && kept[end] != '\r' && kept[end] != '\n')
            end++;
        bool superseded = names_hold(update, ulen, kept + at, end - at);
        size_t from = 0;
        way_header_t h;
        while (!superseded && way_header_next(block, blen, &from, &h))
            superseded = same_name(h.name, h.name_len, kept + at, end - at);
        if (!superseded && end > at) {
            os64_memcpy(out + n, kept + at, end - at);
            n += end - at;
            os64_memcpy(out + n, "\r\n", 2);
            n += 2;
        }
        at = end;
        while (at < klen && (kept[at] == '\r' || kept[at] == '\n'))
            at++;
    }
    os64_memcpy(out + n, update, ulen);
    return n + ulen;
}

// A 304 to the question the cache asked: the kept reply with the 304's
// headers laid over its own (way_headers_update), judged by the live CORS
// check, served, and kept again with its freshness restarted — or let go,
// when the 304 forbids keeping it any longer. False, with the reason in
// `got`, when it cannot be the answer.
static bool cache_confirmed(const yonder_fetch_job_t *job, way_request_t *r, const char *current, Cached *c,
                            const Hop *h, const os64_fetch_keep_t *fields, yonder_fetched_t *got,
                            uint64_t max_body)
{
    size_t cap = c->blocks.headers_len + h->len + 1, ocap = c->blocks.omitted_len + h->olen + 3;
    char *merged = os64_malloc(cap), *omitted = os64_malloc(ocap);
    size_t mlen = merged != NULL ? way_headers_update(c->blocks.headers, c->blocks.headers_len,
                                                       h->block != NULL ? h->block : "", h->len, merged, cap)
                                 : SIZE_MAX;
    if (merged == NULL || omitted == NULL || mlen == SIZE_MAX) {
        os64_free(merged);
        os64_free(omitted);
        return network_error(got, "out of memory"), false;
    }
    size_t olen = omitted_update(c->blocks.omitted, c->blocks.omitted_len, h->block != NULL ? h->block : "", h->len,
                                 h->omitted != NULL ? h->omitted : "", h->olen, omitted);
    way_blocks_t b = {merged, mlen, omitted, olen, c->blocks.vary, c->blocks.vary_len};
    bool ok = serve_kept(c, r, current, &b, got, max_body, WAY_SERVED_CONFIRMED);
    if (!ok)
        network_error(got, "CORS: the answer does not allow this origin to read it");
    else if (way_keep_confirm_varied(&c->e.keep, fields, now_utc()))
        c->held = way_cache_put(job->cache, &c->e, &b, c->body, c->len);
    else
        way_cache_drop(job->cache, &c->key);
    os64_free(merged);
    os64_free(omitted);
    return ok && got->ok;
}

// A new answer from the network, at the key's address with no redirect on
// the way: kept, when it is a whole 200 that is not opaque and says it may
// be — its head without its cookies, and what the request said of each
// header it varies on. One the cache may not keep is still newer than
// whatever the cache holds under the key, read or not (a reload does not
// read), which must not be served again.
static void cache_store(const yonder_fetch_job_t *job, Cached *c, const way_request_t *r, const Hop *h,
                        const os64_fetch_head_t *head, const yonder_fetched_t *got)
{
    bool keep = job->cache_mode != WAY_CACHE_ONLY_IF_CACHED && r->urls == 1 && head->status == 200 &&
                got->type != WAY_TYPE_OPAQUE && got->has_body;
    way_entry_t *e = keep ? os64_calloc(1, sizeof(*e)) : NULL;
    char *headers = e != NULL ? os64_malloc(h->len + 1) : NULL;
    char *vary = headers != NULL ? os64_malloc(OS64_FETCH_EXTRA_MAX) : NULL;
    size_t hlen = 0, vlen = 0;
    keep = vary != NULL && way_keep_read_varied(&head->keep, now_utc(), &e->keep) &&
           (hlen = way_headers_storable(h->block != NULL ? h->block : "", h->len, headers, h->len + 1)) != SIZE_MAX &&
           (vlen = way_vary_lines(head->keep.vary, request_value, c, vary, OS64_FETCH_EXTRA_MAX)) != SIZE_MAX;
    if (keep) {
        os64_strcopy(e->partition, sizeof(e->partition), c->partition);
        e->credentials = c->key.credentials;
        os64_strcopy(e->url, sizeof(e->url), c->url);
        os64_strcopy(e->final, sizeof(e->final), c->url);
        e->status = head->status;
        os64_strcopy(e->content_type, sizeof(e->content_type), head->content_type);
        os64_strcopy(e->charset, sizeof(e->charset), head->charset);
        way_blocks_t b = {headers, hlen, h->omitted != NULL ? h->omitted : "", h->olen, vary, vlen};
        keep = way_cache_put(job->cache, e, &b, got->body, got->body_len);
        c->held = keep;
    }
    if (!keep)
        way_cache_drop(job->cache, &c->key);
    os64_free(vary);
    os64_free(headers);
    os64_free(e);
}

// A module's compiled form (BYTECODE_CACHE.md § 9), for an answer whose
// body the entry holds: where a compile of it is kept, and the blob kept
// for this very body, read beside the entry. Without memory for the
// record, or an address too long for it, the module is compiled as source.
static void module_bytecode(const yonder_fetch_job_t *job, const Cached *c, yonder_fetched_t *got)
{
    yonder_bytecode_t *bc = os64_calloc(1, sizeof(*bc));
    if (bc == NULL)
        return;
    os64_strcopy(bc->partition, sizeof(bc->partition), c->partition);
    bc->credentials = c->key.credentials;
    if (os64_strcopy(bc->url, sizeof(bc->url), c->url) >= sizeof(bc->url)) {
        os64_free(bc);
        return;
    }
    yonder_module_bytecode_tag(got->has_body ? got->body : NULL, got->has_body ? got->body_len : 0, bc->tag,
                               sizeof(bc->tag));
    (void)way_cache_companion_get(job->cache, &c->key, bc->tag, &bc->blob, &bc->length, SCRIPT_BYTECODE_MAX);
    got->bytecode = bc;
}

// A successful unsafe request invalidates what the cache keeps for its
// address (RFC 9111 § 4.4): a POST, PUT, PATCH or DELETE that worked has
// changed it. Under both credentials, since the page's GETs of it may be
// kept under either.
static void cache_invalidate(const yonder_fetch_job_t *job, const way_request_t *r, const yonder_fetched_t *got)
{
    static const char *const kSafe[] = {"GET", "HEAD", "OPTIONS", "TRACE"};
    for (size_t i = 0; i < sizeof(kSafe) / sizeof(kSafe[0]); i++)
        if (os64_streq(r->method, kSafe[i]))
            return;
    if (!got->ok || got->status < 200 || got->status >= 400 || !way_cache_enabled(job->cache))
        return;
    char *target = without_fragment(job->url);
    if (target == NULL)
        return;
    char partition[WAY_PARTITION_MAX];
    way_partition_of(&r->origin, partition, sizeof(partition));
    for (int credentials = 0; credentials < 2; credentials++)
        way_cache_drop(job->cache, &(way_cache_key_t){partition, credentials == 1, target});
    os64_free(target);
}

static int64_t fetch_run(void *job_ptr, bool (*cancelled)(void *ctx), void *ctx, void **out, bool image);

// The fetch, timed whole, whichever way it ended: what a request cost is
// the page record's to say (yonder.c request_record).
static int64_t fetch_timed(void *job_ptr, bool (*cancelled)(void *ctx), void *ctx, void **out, bool image)
{
    int64_t began = os64_micros();
    int64_t verdict = fetch_run(job_ptr, cancelled, ctx, out, image);
    int64_t ended = os64_micros();
    yonder_fetched_t *got = *out;
    if (got != NULL) {
        const yonder_fetch_job_t *job = job_ptr;
        got->elapsed_us = began >= 0 && ended >= began ? ended - began : -1;
        got->queued_us = job->submitted_us > 0 && began >= job->submitted_us ? began - job->submitted_us : -1;
        got->finished_us = ended >= 0 ? ended : -1;
    }
    return verdict;
}

static int64_t fetch_run(void *job_ptr, bool (*cancelled)(void *ctx), void *ctx, void **out, bool image)
{
    yonder_fetch_job_t *job = job_ptr;
    yonder_fetched_t *got = os64_calloc(1, sizeof(*got));
    if (got == NULL)
        return -1;
    *out = got;
    job->cache_mode = cache_mode_of(job);
    uint64_t max_body = job->max_body != 0 ? job->max_body : YONDER_FETCH_BODY_MAX;
    way_request_t *r = &job->request;
    r->headers = job->headers;
    r->headers_len = job->headers_len;
    r->body = job->body != NULL || job->body_len > 0;
    Hop h = {.job = job, .r = r, .cancelled = cancelled, .cancel_ctx = ctx, .cookies_short = &got->cookies_short};
    Branches branches = {0};
    // A fragment never leaves the browser, and no response's URL keeps one.
    char *current = without_fragment(job->url);
    if (current == NULL)
        return network_error(got, "out of memory");
    way_request_start(r, current);
    way_metadata_request(&h.metadata, &r->origin, image?WAY_DEST_IMAGE:WAY_DEST_EMPTY, r->mode);
    h.page_given = page_sets(r);
    int64_t verdict = 0;
    Cached *cached = NULL;          // the cache's, for the first hop (cache_open)
    for (;;) {
        if (cancelled != NULL && cancelled(ctx)) {
            network_error(got, "cancelled");
            break;
        }
        way_refusal_t why;
        way_hop_t kind = way_main_fetch(r, current, &why);
        if (kind == WAY_HOP_REFUSED) {
            got->mixed = why == WAY_REFUSE_MIXED;
            char text[256];
            os64_snprintf(text, sizeof(text), "refused: %s", way_refusal_name(why));
            network_error(got, text);
            break;
        }
        if (kind == WAY_HOP_DATA) {
            uint8_t *bytes;
            size_t len;
            char type[WAY_MIME_SERIALIZED_MAX];
            if (!yonder_fetch_data_url(current, max_body, &bytes, &len, type, sizeof(type))) {
                network_error(got, "a data: URL that is not one, or is past the body's cap");
                break;
            }
            got->headers_len = 14 + os64_strlen(type) + 2;
            got->headers = os64_malloc(got->headers_len + 1);
            got->url = without_fragment(current);
            if (got->headers == NULL || got->url == NULL) {
                os64_free(bytes);
                network_error(got, "out of memory");
                break;
            }
            os64_snprintf(got->headers, got->headers_len + 1, "Content-Type: %s\r\n", type);
            got->ok = true;
            got->type = WAY_TYPE_BASIC;
            got->status = 200;
            os64_strcopy(got->status_text, sizeof(got->status_text), "OK");
            got->has_body = way_response_has_body(r, WAY_TYPE_BASIC, 200);
            if (got->has_body) {
                got->body = bytes;
                got->body_len = len;
            } else {
                os64_free(bytes);
            }
            got->redirected = r->urls > 1;
            got->hops = r->urls - 1;
            verdict = 1;
            break;
        }
        bool branch = kind == WAY_HOP_CORS && way_preflight_capable(r);
        if (branch) {
            // The URL and the taint the key is made under are kept BEFORE
            // anything is cached under them, and a copy there is no memory
            // for ends the request: a branch the clear cannot name would
            // leave this hop's grants standing after a network error.
            char *kept = NULL;
            if (branches.n < (int32_t)(sizeof(branches.urls) / sizeof(branches.urls[0]))) {
                kept = copy_text(current, os64_strlen(current));
                if (kept == NULL) {
                    network_error(got, "out of memory");
                    break;
                }
                branches.rtaints[branches.n] = r->rtaint;
                branches.urls[branches.n++] = kept;
            }
            if (way_preflight_needed(job->preflight, r, current, yonder_now_ms()) &&
                !preflight(&h, job, r, current, got))
                break;
        }

        // The actual request.
        Fields *fields = os64_malloc(sizeof(*fields));
        char origin[WAY_ORIGIN_TEXT_MAX];
        if (fields == NULL || !fields_of(r, fields) ||
            (way_origin_header(r, current, origin, sizeof(origin)) &&
             !append_line(fields->extra, sizeof(fields->extra), "Origin", 6, origin, os64_strlen(origin)))) {
            os64_free(fields);
            network_error(got, fields == NULL ? "out of memory" : "the request's headers do not fit");
            break;
        }
        hop_reset(&h, current, false);
        os64_fetch_options_t opt;
        opt_base(&opt, &h, job, max_body);
        opt.method_name = r->method;
        opt.accept = fields->accept;
        // Fetch's own `*/*`, when the page set none, is the preset's list
        // for a fetch when it has one (the captured browsers send `*/*`).
        const char *preset = way_identity_accept(job->identity, WAY_DEST_EMPTY);
        if (fields->accepts == 0 && preset != NULL)
            opt.accept = preset;
        // The cache, on the request's first hop alone (SCRIPT_CACHE.md §
        // 5-6): what it keeps fresh is the answer; a stale reply is asked
        // about. A later hop is another address, which the kept reply's
        // validator must not reach. only-if-cached with no cache to ask
        // has nothing it may give.
        if (r->urls == 1 && cached == NULL && cache_wanted(job, r))
            cached = cache_open(job, r, current, fields, opt.accept, max_body);
        CacheAnswer answer = r->urls != 1 ? CACHE_MISS
                             : cached != NULL ? cache_answer(job, r, current, cached, fields, got, max_body)
                             : job->cache_mode == WAY_CACHE_ONLY_IF_CACHED ? CACHE_REFUSED
                                                                           : CACHE_MISS;
        if (answer == CACHE_ANSWERED || answer == CACHE_REFUSED) {
            os64_free(fields);
            if (answer == CACHE_REFUSED)
                network_error(got, "only-if-cached: the cache keeps no answer it may give");
            verdict = got->ok ? 1 : 0;
            break;
        }
        opt.extra_headers = fields->extra[0] ? fields->extra : NULL;
        opt.body_headers = fields->body[0] ? fields->body : NULL;
        opt.identity = fields->identity;
        bool sends = r->body && !os64_streq(r->method, "GET") && !os64_streq(r->method, "HEAD");
        opt.body = sends ? job->body : NULL;
        opt.body_len = sends ? job->body_len : 0;
        os64_fetch_t *f = os64_fetch_open(current, &opt);
        os64_free(fields);
        if (f == NULL) {
            network_error(got, "out of memory");
            break;
        }
        os64_fetch_status_t st = os64_fetch_status(f);
        const os64_fetch_head_t *head = os64_fetch_head(f);
        if (h.too_long || h.no_memory) {
            network_error(got, h.too_long ? "the answer has more head than 64 KiB" : "out of memory");
            os64_fetch_close(f);
            break;
        }
        if (head == NULL || (st != OS64_FETCH_OK && st != OS64_FETCH_REDIRECT_STOPPED)) {
            char text[256];
            os64_snprintf(text, sizeof(text), "%s", os64_fetch_reason(f));
            network_error(got, text);
            os64_fetch_close(f);
            break;
        }
        // A 304 to the question the cache asked: the kept reply, its headers
        // brought up to date, judged and served as one from the cache.
        if (cached != NULL && cached->asked && r->urls == 1 && head->status == 304) {
            if (cache_confirmed(job, r, current, cached, &h, &head->keep, got, max_body))
                verdict = 1;
            os64_fetch_close(f);
            break;
        }
        way_response_t resp = hop_response(&h, head->status);
        // § 4.4: the CORS check on every response of a cors hop, redirects
        // included. The hop's Set-Cookie was heard already (the order is
        // the rule; see the top of this file).
        if (r->tainting == WAY_TAINT_CORS && !way_cors_check(r, &resp)) {
            network_error(got, "CORS: the answer does not allow this origin to read it");
            os64_fetch_close(f);
            break;
        }
        if (way_redirect_status(head->status)) {
            if (r->redirect == WAY_REDIRECT_ERROR) {
                network_error(got, "a redirect, with redirect mode \"error\"");
                os64_fetch_close(f);
                break;
            }
            if (r->redirect == WAY_REDIRECT_MANUAL) {
                got->ok = true;
                got->type = WAY_TYPE_OPAQUEREDIRECT;
                got->url = without_fragment(current);
                got->hops = r->urls - 1;
                os64_fetch_close(f);
                if (got->url == NULL)
                    network_error(got, "out of memory");
                else
                    verdict = 1;
                break;
            }
            if (head->has_location) {
                os64_url_t *base = os64_malloc(sizeof(*base));
                char *target = os64_malloc(OS64_FETCH_URL_MAX);
                bool resolved = base != NULL && target != NULL && os64_url_parse(current, base) == OS64_URL_OK &&
                                os64_url_absolute(base, head->location, target, OS64_FETCH_URL_MAX);
                int32_t status = head->status;
                os64_free(base);
                os64_fetch_close(f);                // the head goes with it
                bool drop;
                if (!resolved || way_redirect_follow(r, status, current, target, &drop) != WAY_FOLLOW_GO) {
                    os64_free(target);
                    network_error(got, resolved ? "a redirect this request may not follow"
                                                : "a redirect to an address that does not resolve");
                    break;
                }
                os64_free(current);
                current = target;
                continue;
            }
        }
        if (finish(got, r, current, head->status, head->reason, &resp, f, NULL, 0, max_body, image)) {
            verdict = 1;
            // Kept, when the cache may keep it; a new answer it may not keep
            // is still newer than what it held, which must not be served.
            if (cached != NULL)
                cache_store(job, cached, r, &h, head, got);
        }
        os64_fetch_close(f);
        break;
    }
    got->hops = r->urls > 0 ? r->urls - 1 : 0;
    branches_clear(&branches, job, r, !got->ok);
    cache_invalidate(job, r, got);
    if (job->module && got->ok && cached != NULL && cached->held && !(cancelled != NULL && cancelled(ctx)))
        module_bytecode(job, cached, got);
    cached_free(cached);
    os64_free(current);
    os64_free(h.block);
    os64_free(h.omitted);
    return verdict;
}

int64_t yonder_fetch_run(void *job,bool (*cancelled)(void *),void *ctx,void **out)
{ return fetch_timed(job,cancelled,ctx,out,false); }
int64_t yonder_fetch_image_run(void *job,bool (*cancelled)(void *),void *ctx,void **out)
{ return fetch_timed(job,cancelled,ctx,out,true); }
