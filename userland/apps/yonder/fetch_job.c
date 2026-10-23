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
#include "os64/str.h"
#include "ticker.h"

// ── A hop's conversation with libfetch ──────────────────────────────────

typedef struct {
    yonder_fetch_job_t *job;
    way_request_t *r;
    const char *url;                // this hop's address
    bool preflight;                 // a preflight: no cookies go, none are heard
    bool (*cancelled)(void *ctx);
    void *cancel_ctx;
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
    if (h->preflight || h->job->jar == NULL || !way_credentials_included(h->r) ||
        cap <= sizeof(kCookie) + 2)
        return 0;
    size_t room = cap - (sizeof(kCookie) - 1) - 2;
    size_t n = way_jar_cookies(h->job->jar, to, encrypted, now_utc(), out + sizeof(kCookie) - 1, room, NULL);
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

// libfetch's per-hop fields: the cookies above, then the Referer the
// request's policy gives this hop, when it fits after them.
static bool hop_fields(void *ctx, const os64_url_t *to, bool encrypted, char *out, size_t cap)
{
    Hop *h = ctx;
    out[0] = '\0';
    size_t used = hop_cookies(h, to, encrypted, out, cap);
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
// list does), so Accept is found without regard to case.
static bool is_accept(const char *name, size_t len)
{
    static const char kAccept[] = "accept";
    if (len != 6)
        return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i] >= 'A' && name[i] <= 'Z' ? (char)(name[i] - 'A' + 'a') : name[i];
        if (c != kAccept[i])
            return false;
    }
    return true;
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
            if (used + (used ? 2 : 0) + h.value_len + 1 > sizeof(f->accept))
                return false;
            if (used) {
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
    if (f->accept[0] == '\0')
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
    // Its type: what was written, `text/plain` before a bare parameter list,
    // and US-ASCII text when what was written is not a MIME type.
    char essence[WAY_MIME_ESSENCE_MAX], charset[WAY_MIME_CHARSET_MAX];
    char written[512];
    size_t wlen = (size_t)(mt_end - mt);
    if (wlen > 0 && mt[0] == ';')
        os64_snprintf(written, sizeof(written), "text/plain%.*s", (int)wlen, mt);
    else
        os64_snprintf(written, sizeof(written), "%.*s", (int)(wlen < sizeof(written) ? wlen : 0), mt);
    if (way_mime_parse(written, os64_strlen(written), essence, sizeof(essence), charset, sizeof(charset)))
        os64_snprintf(type, type_cap, charset[0] ? "%s;charset=%s" : "%s", essence, charset);
    else
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

// The URLs whose hop took § 4.1's preflight branch: a network error anywhere
// after clears their cache entries ("clear cache entries using request").
typedef struct {
    char *urls[WAY_REDIRECTS_MAX + 1];
    int32_t n;
} Branches;

static void branches_clear(Branches *b, yonder_fetch_job_t *job, way_request_t *r, bool failed)
{
    for (int32_t i = 0; i < b->n; i++) {
        if (failed)
            way_preflight_clear(job->preflight, r, b->urls[i]);
        os64_free(b->urls[i]);
    }
    b->n = 0;
}

static void opt_base(os64_fetch_options_t *opt, Hop *h, yonder_fetch_job_t *job, uint64_t max_body)
{
    os64_memset(opt, 0, sizeof(*opt));
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

int64_t yonder_fetch_run(void *job_ptr, bool (*cancelled)(void *ctx), void *ctx, void **out)
{
    yonder_fetch_job_t *job = job_ptr;
    yonder_fetched_t *got = os64_calloc(1, sizeof(*got));
    if (got == NULL)
        return -1;
    *out = got;
    uint64_t max_body = job->max_body != 0 ? job->max_body : YONDER_FETCH_BODY_MAX;
    way_request_t *r = &job->request;
    r->headers = job->headers;
    r->headers_len = job->headers_len;
    r->body = job->body != NULL || job->body_len > 0;
    Hop h = {.job = job, .r = r, .cancelled = cancelled, .cancel_ctx = ctx};
    Branches branches = {0};
    // A fragment never leaves the browser, and no response's URL keeps one.
    char *current = without_fragment(job->url);
    if (current == NULL)
        return network_error(got, "out of memory");
    way_request_start(r, current);
    int64_t verdict = 0;
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
            char type[WAY_MIME_ESSENCE_MAX + WAY_MIME_CHARSET_MAX + 16];
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
            if (branches.n < (int32_t)(sizeof(branches.urls) / sizeof(branches.urls[0])))
                branches.urls[branches.n++] = copy_text(current, os64_strlen(current));
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
        // The final response, filtered for the page.
        way_type_t type = way_response_type(r);
        got->type = type;
        got->status = type == WAY_TYPE_OPAQUE ? 0 : head->status;
        if (type != WAY_TYPE_OPAQUE)
            os64_strcopy(got->status_text, sizeof(got->status_text), head->reason);
        got->url = type == WAY_TYPE_OPAQUE ? copy_text("", 0) : without_fragment(current);
        got->redirected = type != WAY_TYPE_OPAQUE && r->urls > 1;
        got->hops = r->urls - 1;
        got->headers = os64_malloc(resp.headers_len + 1);
        if (got->url == NULL || got->headers == NULL) {
            os64_fetch_close(f);
            network_error(got, "out of memory");
            break;
        }
        got->headers_len = way_response_headers(r, type, &resp, got->headers, resp.headers_len + 1);
        if (got->headers_len == SIZE_MAX) {
            os64_fetch_close(f);
            network_error(got, "the answer's headers did not fit");
            break;
        }
        // An opaque body is never read: what was never read cannot leak.
        got->has_body = way_response_has_body(r, type, head->status);
        if (got->has_body) {
            bool limit;
            if (!read_body(f, max_body, &got->body, &got->body_len, &limit)) {
                char text[256];
                os64_snprintf(text, sizeof(text), "%s", limit ? "the body is past its cap" : os64_fetch_reason(f));
                os64_fetch_close(f);
                network_error(got, text);
                break;
            }
        }
        os64_fetch_close(f);
        got->ok = true;
        verdict = 1;
        break;
    }
    got->hops = r->urls > 0 ? r->urls - 1 : 0;
    branches_clear(&branches, job, r, !got->ok);
    os64_free(current);
    os64_free(h.block);
    os64_free(h.omitted);
    return verdict;
}
