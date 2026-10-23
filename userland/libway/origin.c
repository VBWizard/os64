// origin.c — origins and the CORS protocol (way/origin.h).
//
// The Fetch Standard's sections are cited where their rule is applied. Each
// rule is one function; the job and libdom only ever ask these.

#include "way/origin.h"
#include "os64/fmt.h"
#include "os64/lock.h"
#include "os64/mem.h"
#include "os64/str.h"

// ── Text ────────────────────────────────────────────────────────────────

// A page sizes a request's headers and a server a response's, and no script
// budget can interrupt C, so the work over either is counted where it is
// done: tools/test_way_origin_host.c defines WAY_ORIGIN_STEPS and holds the
// count to n log n at sizes past every cap. Nothing else counts.
#ifdef WAY_ORIGIN_STEPS
extern uint64_t way_origin_steps;
#endif
static inline void step(void)
{
#ifdef WAY_ORIGIN_STEPS
    way_origin_steps++;
#endif
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static char upper(char c)
{
    return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}

static bool same_nocase(const char *a, size_t alen, const char *b, size_t blen)
{
    step();
    if (alen != blen)
        return false;
    for (size_t i = 0; i < alen; i++)
        if (lower(a[i]) != lower(b[i]))
            return false;
    return true;
}

static bool is_nocase(const char *a, size_t alen, const char *word)
{
    return same_nocase(a, alen, word, os64_strlen(word));
}

static bool same_bytes(const char *a, size_t alen, const char *b, size_t blen)
{
    step();
    return alen == blen && os64_memcmp(a, b, alen) == 0;
}

// Bytewise order of two names, case folded: <0, 0, >0.
static int32_t order_nocase(const char *a, size_t alen, const char *b, size_t blen)
{
    step();
    for (size_t i = 0; i < alen && i < blen; i++) {
        unsigned char x = (unsigned char)lower(a[i]), y = (unsigned char)lower(b[i]);
        if (x != y)
            return x < y ? -1 : 1;
    }
    return alen == blen ? 0 : alen < blen ? -1 : 1;
}

static bool http_tab_or_space(char c)
{
    return c == ' ' || c == '\t';
}

static bool http_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

// RFC 9110 § 5.6.2's tchar.
static bool is_token_byte(char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c))
        return true;
    return c != '\0' && os64_strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static bool is_token(const char *s, size_t len)
{
    if (len == 0)
        return false;
    for (size_t i = 0; i < len; i++)
        if (!is_token_byte(s[i]))
            return false;
    return true;
}

// ── Origins ─────────────────────────────────────────────────────────────

// The URL Standard's special schemes with a host, and their ports. file is
// special too, and its origin is opaque: os64 treats every file as its own.
static uint16_t special_port(const char *scheme)
{
    static const struct { const char *scheme; uint16_t port; } kSpecial[] = {
        {"http", 80}, {"https", 443}, {"ws", 80}, {"wss", 443}, {"ftp", 21},
    };
    for (size_t i = 0; i < sizeof(kSpecial) / sizeof(kSpecial[0]); i++)
        if (os64_streq(kSpecial[i].scheme, scheme))
            return kSpecial[i].port;
    return 0;
}

static void opaque(way_origin_t *out, uint64_t id)
{
    os64_memset(out, 0, sizeof(*out));
    out->opaque = true;
    out->id = id;
}

void way_origin_of(const char *url, uint64_t opaque_id, way_origin_t *out)
{
    if (url == NULL) {
        opaque(out, opaque_id);
        return;
    }
    // URL § origin: a blob: URL has its inner URL's origin when that is
    // http or https, and an opaque one otherwise.
    char scheme[OS64_URL_SCHEME_MAX];
    if (os64_url_scheme_of(url, scheme, sizeof(scheme)) && os64_streq(scheme, "blob")) {
        const char *inner = url + 5;
        char inner_scheme[OS64_URL_SCHEME_MAX];
        if (os64_url_scheme_of(inner, inner_scheme, sizeof(inner_scheme)) &&
            (os64_streq(inner_scheme, "http") || os64_streq(inner_scheme, "https"))) {
            way_origin_of(inner, opaque_id, out);
            return;
        }
        opaque(out, opaque_id);
        return;
    }
    os64_url_t *parsed = os64_malloc(sizeof(*parsed));
    if (parsed == NULL || os64_url_parse(url, parsed) != OS64_URL_OK || special_port(parsed->scheme) == 0) {
        os64_free(parsed);
        opaque(out, opaque_id);
        return;
    }
    os64_memset(out, 0, sizeof(*out));
    os64_strcopy(out->scheme, sizeof(out->scheme), parsed->scheme);
    os64_strcopy(out->host, sizeof(out->host), parsed->host);
    out->port = parsed->port != 0 ? parsed->port : special_port(parsed->scheme);
    os64_free(parsed);
}

bool way_origin_same(const way_origin_t *a, const way_origin_t *b)
{
    if (a->opaque || b->opaque)
        return a->opaque && b->opaque && a->id != 0 && a->id == b->id;
    return a->port == b->port && os64_streq(a->scheme, b->scheme) && os64_streq(a->host, b->host);
}

bool way_origin_same_site(const way_origin_t *a, const way_origin_t *b)
{
    // HTML § same site: two opaque origins are same site when they are the
    // same origin. A tuple's site is its scheme and its host's registrable
    // domain; without the public-suffix list (DEBTS) the host itself stands
    // in for that, which only ever splits one site into several.
    if (a->opaque || b->opaque)
        return way_origin_same(a, b);
    return os64_streq(a->scheme, b->scheme) && os64_streq(a->host, b->host);
}

bool way_origin_serialize(const way_origin_t *o, char *out, size_t cap)
{
    if (o->opaque)
        return os64_strcopy(out, cap, "null") < cap;
    int32_t n = o->port != special_port(o->scheme)
        ? os64_snprintf(out, cap, "%s://%s:%u", o->scheme, o->host, (unsigned)o->port)
        : os64_snprintf(out, cap, "%s://%s", o->scheme, o->host);
    return n > 0 && (size_t)n < cap;
}

bool way_origin_prohibits_mixed(const way_origin_t *page)
{
    return !page->opaque && os64_streq(page->scheme, "https");
}

// Four decimal parts, each 0..255 written without a leading zero, the first
// 127. Any other spelling of a loopback address (127.1, 0177.0.0.1) is not
// trusted: what it means depends on whose parser reads it.
static bool loopback_literal(const char *host)
{
    const char *p = host;
    for (int32_t part = 0; part < 4; part++) {
        if (!is_digit(*p))
            return false;
        uint32_t v = 0;
        const char *start = p;
        while (is_digit(*p) && p - start < 4)
            v = v * 10 + (uint32_t)(*p++ - '0');
        if (v > 255 || (p - start > 1 && *start == '0') || is_digit(*p))
            return false;
        if (part == 0 && v != 127)
            return false;
        if (part < 3 && *p++ != '.')
            return false;
    }
    return *p == '\0';
}

// Secure Contexts § 3.1, for a URL: https, wss and data: are; an http URL
// is when its host is loopback.
static bool trustworthy(const char *url)
{
    char scheme[OS64_URL_SCHEME_MAX];
    if (url == NULL || !os64_url_scheme_of(url, scheme, sizeof(scheme)))
        return false;
    if (os64_streq(scheme, "https") || os64_streq(scheme, "wss") || os64_streq(scheme, "data"))
        return true;
    way_origin_t o;
    way_origin_of(url, 0, &o);
    return !o.opaque && loopback_literal(o.host);
}

bool way_mixed_blocked(const way_origin_t *page, const char *url)
{
    return way_origin_prohibits_mixed(page) && !trustworthy(url);
}

bool way_port_bad(const char *url)
{
    static const uint16_t kBad[] = {
        0, 1, 7, 9, 11, 13, 15, 17, 19, 20, 21, 22, 23, 25, 37, 42, 43, 53, 69, 77, 79, 87, 95,
        101, 102, 103, 104, 109, 110, 111, 113, 115, 117, 119, 123, 135, 137, 139, 143, 161,
        179, 389, 427, 465, 512, 513, 514, 515, 526, 530, 531, 532, 540, 548, 554, 556, 563,
        587, 601, 636, 989, 990, 993, 995, 1719, 1720, 1723, 2049, 3659, 4045, 4190, 5060, 5061,
        6000, 6566, 6665, 6666, 6667, 6668, 6669, 6679, 6697, 10080,
    };
    char scheme[OS64_URL_SCHEME_MAX];
    if (url == NULL || !os64_url_scheme_of(url, scheme, sizeof(scheme)) ||
        (!os64_streq(scheme, "http") && !os64_streq(scheme, "https")))
        return false;
    way_origin_t o;
    way_origin_of(url, 0, &o);
    if (o.opaque)
        return true;                            // a port nobody could read is not one to allow
    for (size_t i = 0; i < sizeof(kBad) / sizeof(kBad[0]); i++)
        if (o.port == kBad[i])
            return true;
    return false;
}

// ── Methods (§ 2.2.1) ───────────────────────────────────────────────────

bool way_method_ok(const char *method, size_t len)
{
    return len < WAY_METHOD_MAX && is_token(method, len);
}

bool way_method_forbidden(const char *method, size_t len)
{
    return is_nocase(method, len, "CONNECT") || is_nocase(method, len, "TRACE") ||
           is_nocase(method, len, "TRACK");
}

bool way_method_safelisted(const char *method)
{
    return os64_streq(method, "GET") || os64_streq(method, "HEAD") || os64_streq(method, "POST");
}

void way_method_normalize(char *method)
{
    static const char *const kNormal[] = {"DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT"};
    size_t len = os64_strlen(method);
    for (size_t i = 0; i < sizeof(kNormal) / sizeof(kNormal[0]); i++)
        if (is_nocase(method, len, kNormal[i])) {
            for (size_t j = 0; j < len; j++)
                method[j] = upper(method[j]);
            return;
        }
}

// ── Headers (§ 2.2.2) ───────────────────────────────────────────────────

bool way_header_name_ok(const char *name, size_t len)
{
    return is_token(name, len);
}

bool way_header_value_ok(const char *value, size_t len)
{
    if (len > 0 && (http_tab_or_space(value[0]) || http_tab_or_space(value[len - 1])))
        return false;
    for (size_t i = 0; i < len; i++)
        if (value[i] == '\0' || value[i] == '\r' || value[i] == '\n')
            return false;
    return true;
}

void way_header_value_normalize(const char **value, size_t *len)
{
    while (*len > 0 && http_whitespace((*value)[0])) {
        (*value)++;
        (*len)--;
    }
    while (*len > 0 && http_whitespace((*value)[*len - 1]))
        (*len)--;
}

// § 2.2 "collect an HTTP quoted string" from `s` at `*at` (a '"'), without
// extracting: the span it covers, quotes included, and `*at` moved past it.
static void quoted_string(const char *s, size_t len, size_t *at)
{
    (*at)++;
    while (*at < len) {
        char c = s[(*at)++];
        if (c == '"')
            return;
        if (c == '\\' && *at < len)
            (*at)++;
    }
}

// § 2.2.2 "get, decode, and split", one value at a time: the next value at
// `*at` (white space taken off its ends), and `*at` moved past its comma.
// False when there is none left.
static bool split_next(const char *s, size_t len, size_t *at, bool *started, const char **v, size_t *vlen)
{
    if (*at > len || (*at == len && *started))
        return false;
    *started = true;
    size_t from = *at;
    while (*at < len && s[*at] != ',') {
        if (s[*at] == '"')
            quoted_string(s, len, at);
        else
            (*at)++;
    }
    size_t to = *at;
    while (from < to && http_tab_or_space(s[from]))
        from++;
    while (to > from && http_tab_or_space(s[to - 1]))
        to--;
    *v = s + from;
    *vlen = to - from;
    if (*at < len)
        (*at)++;            // the comma
    else
        *at = len + 1;      // that was the last value
    return true;
}

bool way_header_forbidden_request(const char *name, size_t nlen, const char *value, size_t vlen)
{
    static const char *const kForbidden[] = {
        "accept-charset", "accept-encoding", "access-control-request-headers",
        "access-control-request-method", "connection", "content-length", "cookie", "cookie2",
        "date", "dnt", "expect", "host", "keep-alive", "origin", "referer", "set-cookie", "te",
        "trailer", "transfer-encoding", "upgrade", "via",
        // Not on the standard's list since it was removed from it; Chrome
        // still drops it, and a page's User-Agent on the wire would be a
        // CORS-unsafe header forcing a preflight nobody expected.
        "user-agent",
    };
    for (size_t i = 0; i < sizeof(kForbidden) / sizeof(kForbidden[0]); i++)
        if (is_nocase(name, nlen, kForbidden[i]))
            return true;
    if ((nlen >= 6 && is_nocase(name, 6, "proxy-")) || (nlen >= 4 && is_nocase(name, 4, "sec-")))
        return true;
    if (is_nocase(name, nlen, "x-http-method") || is_nocase(name, nlen, "x-http-method-override") ||
        is_nocase(name, nlen, "x-method-override")) {
        size_t at = 0;
        bool started = false;
        const char *v;
        size_t len;
        while (split_next(value, vlen, &at, &started, &v, &len))
            if (way_method_forbidden(v, len))
                return true;
    }
    return false;
}

bool way_header_forbidden_response(const char *name, size_t nlen)
{
    return is_nocase(name, nlen, "set-cookie") || is_nocase(name, nlen, "set-cookie2");
}

static bool cors_unsafe_byte(unsigned char c)
{
    return (c < 0x20 && c != 0x09) || c == 0x7F || (c != '\0' && os64_strchr("\"():<>?@[\\]{}", (char)c) != NULL);
}

static bool any_cors_unsafe_byte(const char *v, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (cors_unsafe_byte((unsigned char)v[i]))
            return true;
    return false;
}

// § 2.2.2 "parse a single range header value", allowWhitespace false: the
// form `bytes=N-`, `bytes=N-M` or `bytes=-M`. `*has_start` says N was there.
static bool single_range(const char *v, size_t len, bool *has_start)
{
    if (len < 5 || os64_memcmp(v, "bytes", 5) != 0)
        return false;
    size_t at = 5;
    if (at >= len || v[at] != '=')
        return false;
    at++;
    size_t start = at;
    while (at < len && is_digit(v[at]))
        at++;
    size_t start_end = at;
    if (at >= len || v[at] != '-')
        return false;
    at++;
    size_t end = at;
    while (at < len && is_digit(v[at]))
        at++;
    size_t end_end = at;
    if (at != len || (start == start_end && end == end_end))
        return false;
    *has_start = start != start_end;
    if (start != start_end && end != end_end) {
        // Compare as numbers of any length: leading zeros off, then the
        // longer is larger, then the bytes decide.
        while (start + 1 < start_end && v[start] == '0')
            start++;
        while (end + 1 < end_end && v[end] == '0')
            end++;
        size_t a = start_end - start, b = end_end - end;
        if (a > b || (a == b && os64_memcmp(v + start, v + end, a) > 0))
            return false;
    }
    return true;
}

bool way_header_cors_safelisted(const char *name, size_t nlen, const char *value, size_t vlen)
{
    if (vlen > 128)
        return false;
    if (is_nocase(name, nlen, "accept"))
        return !any_cors_unsafe_byte(value, vlen);
    if (is_nocase(name, nlen, "accept-language") || is_nocase(name, nlen, "content-language")) {
        for (size_t i = 0; i < vlen; i++) {
            char c = value[i];
            if (!is_digit(c) && !(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') &&
                os64_strchr(" *,-.;=", c) == NULL)
                return false;
            if (c == '\0')
                return false;
        }
        return true;
    }
    if (is_nocase(name, nlen, "content-type")) {
        if (any_cors_unsafe_byte(value, vlen))
            return false;
        char essence[WAY_MIME_ESSENCE_MAX];
        if (!way_mime_parse(value, vlen, essence, sizeof(essence), NULL, 0))
            return false;
        return os64_streq(essence, "application/x-www-form-urlencoded") ||
               os64_streq(essence, "multipart/form-data") || os64_streq(essence, "text/plain");
    }
    if (is_nocase(name, nlen, "range")) {
        bool has_start;
        return single_range(value, vlen, &has_start) && has_start;
    }
    return false;
}

bool way_header_no_cors_safelisted_name(const char *name, size_t nlen)
{
    return is_nocase(name, nlen, "accept") || is_nocase(name, nlen, "accept-language") ||
           is_nocase(name, nlen, "content-language") || is_nocase(name, nlen, "content-type");
}

bool way_header_no_cors_safelisted(const char *name, size_t nlen, const char *value, size_t vlen)
{
    return way_header_no_cors_safelisted_name(name, nlen) &&
           way_header_cors_safelisted(name, nlen, value, vlen);
}

bool way_header_privileged_no_cors(const char *name, size_t nlen)
{
    return is_nocase(name, nlen, "range");
}

bool way_header_request_body(const char *name, size_t nlen)
{
    return is_nocase(name, nlen, "content-encoding") || is_nocase(name, nlen, "content-language") ||
           is_nocase(name, nlen, "content-location") || is_nocase(name, nlen, "content-type");
}

bool way_header_non_wildcard(const char *name, size_t nlen)
{
    return is_nocase(name, nlen, "authorization");
}

bool way_header_next(const char *block, size_t len, size_t *at, way_header_t *h)
{
    step();
    if (block == NULL || *at >= len)
        return false;
    const char *p = block + *at, *end = block + len;
    const char *name = p;
    while (p < end && *p != ':' && *p != '\r' && *p != '\n')
        p++;
    if (p + 1 >= end || p == name || p[0] != ':' || p[1] != ' ')
        return false;
    size_t name_len = (size_t)(p - name);
    p += 2;
    const char *value = p;
    while (p < end && *p != '\r' && *p != '\n')
        p++;
    if (p + 1 >= end || p[0] != '\r' || p[1] != '\n')
        return false;
    h->name = name;
    h->name_len = name_len;
    h->value = value;
    h->value_len = (size_t)(p - value);
    *at = (size_t)(p + 2 - block);
    return true;
}

// § 2.2.2 "CORS-unsafe request-header names": whether the header at `h` is
// one, given whether the safelisted values together pass 1024 bytes.
static bool unsafe_name(const way_header_t *h, bool overflow)
{
    return overflow || !way_header_cors_safelisted(h->name, h->name_len, h->value, h->value_len);
}

static bool safelist_overflows(const char *block, size_t len)
{
    size_t total = 0, at = 0;
    way_header_t h;
    while (way_header_next(block, len, &at, &h))
        if (way_header_cors_safelisted(h.name, h.name_len, h.value, h.value_len))
            total += h.value_len;
    return total > 1024;
}

// ── Names, sorted ───────────────────────────────────────────────────────
//
// A page sizes a request's header list and a server a response's, and no
// script budget can interrupt C, so every walk over either is n log n at
// worst: names are gathered once, sorted once, and searched.

typedef struct {
    const char *p;
    size_t n;
} Span;

// Bottom-up merge sort of `n` spans by case-folded bytes, through `scratch`
// (room for `n`): n log n whatever order they arrive in.
static void spans_sort(Span *s, Span *scratch, size_t n)
{
    for (size_t width = 1; width < n; width *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * width) {
            size_t mid = lo + width < n ? lo + width : n;
            size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                scratch[k++] = order_nocase(s[j].p, s[j].n, s[i].p, s[i].n) < 0 ? s[j++] : s[i++];
            while (i < mid)
                scratch[k++] = s[i++];
            while (j < hi)
                scratch[k++] = s[j++];
        }
        os64_memcpy(s, scratch, n * sizeof(*s));
    }
}

// The first of the sorted `s` not ordered before `want`, case folded.
static size_t spans_lower_bound(const Span *s, size_t n, const char *want, size_t len)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (order_nocase(s[mid].p, s[mid].n, want, len) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

int32_t way_cors_unsafe_names(const char *block, size_t len, char *out, size_t cap)
{
    if (out != NULL && cap > 0)
        out[0] = '\0';
    bool overflow = safelist_overflows(block, len);
    size_t n = 0, at = 0;
    way_header_t h;
    while (way_header_next(block, len, &at, &h))
        if (unsafe_name(&h, overflow))
            n++;
    if (n == 0)
        return 0;
    Span *s = os64_malloc(2 * n * sizeof(Span));
    if (s == NULL)
        return -1;
    size_t k = 0;
    at = 0;
    while (way_header_next(block, len, &at, &h))
        if (unsafe_name(&h, overflow))
            s[k++] = (Span){h.name, h.name_len};
    spans_sort(s, s + n, n);
    // The sorted-lowercase set: each name once, smallest first.
    int32_t count = 0;
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        if (i > 0 && order_nocase(s[i].p, s[i].n, s[i - 1].p, s[i - 1].n) == 0)
            continue;
        if (out != NULL) {
            if (used + (count > 0) + s[i].n + 1 > cap) {
                os64_free(s);
                return -1;
            }
            if (count > 0)
                out[used++] = ',';
            for (size_t j = 0; j < s[i].n; j++)
                out[used++] = lower(s[i].p[j]);
            out[used] = '\0';
        }
        count++;
    }
    os64_free(s);
    return count;
}

// ── MIME types ──────────────────────────────────────────────────────────

// MIME Sniffing's "HTTP quoted-string token code point": tab, the printable
// ASCII range and the bytes a header's isomorphic decoding makes U+0080..FF.
static bool quoted_token_byte(unsigned char c)
{
    return c == 0x09 || (c >= 0x20 && c <= 0x7E) || c >= 0x80;
}

// MIME Sniffing § 4.4 "parse a MIME type": the essence ("type/subtype",
// lowercased) into `essence`, and each parameter that is valid there (a
// token name, a value of quoted-string token code points, with a quoted
// value's quotes and escapes off) handed to `each` in order, its value
// unfit (`fits` false) past WAY_MIME_VALUE_MAX bytes. Which of a repeated
// name stands is the caller's: the standard keeps the first valid one.
// One walk for both the charset and the serializer, so the two cannot
// disagree about what a parameter is.
typedef void (*mime_param_fn)(void *ctx, const char *name, size_t name_len, const char *value, size_t value_len,
                              bool fits);

#define WAY_MIME_VALUE_MAX 512

static bool mime_walk(const char *value, size_t len, char *essence, size_t ecap, mime_param_fn each, void *ctx)
{
    way_header_value_normalize(&value, &len);
    size_t at = 0;
    while (at < len && value[at] != '/')
        at++;
    if (at == 0 || at >= len || !is_token(value, at))
        return false;
    size_t type_len = at++;
    size_t sub = at;
    while (at < len && value[at] != ';')
        at++;
    size_t sub_end = at;
    while (sub_end > sub && http_whitespace(value[sub_end - 1]))
        sub_end--;
    if (!is_token(value + sub, sub_end - sub))
        return false;
    if (type_len + 1 + (sub_end - sub) + 1 > ecap)
        return false;
    size_t e = 0;
    for (size_t i = 0; i < type_len; i++)
        essence[e++] = lower(value[i]);
    essence[e++] = '/';
    for (size_t i = sub; i < sub_end; i++)
        essence[e++] = lower(value[i]);
    essence[e] = '\0';
    while (at < len) {
        at++;                                   // the ';'
        while (at < len && http_whitespace(value[at]))
            at++;
        size_t name = at;
        while (at < len && value[at] != ';' && value[at] != '=')
            at++;
        size_t name_len = at - name;
        if (at < len && value[at] == ';')
            continue;
        if (at >= len)
            break;
        at++;                                   // the '='
        if (at >= len)
            break;
        char got[WAY_MIME_VALUE_MAX];
        size_t got_len = 0;
        bool fits = true, valid = true;
        if (value[at] == '"') {
            // Collected with its value extracted: the quotes and escapes off.
            at++;
            while (at < len) {
                char c = value[at++];
                if (c == '"')
                    break;
                if (c == '\\') {
                    if (at >= len) {
                        c = '\\';
                    } else {
                        c = value[at++];
                    }
                }
                if (!quoted_token_byte((unsigned char)c))
                    valid = false;
                if (got_len + 1 < sizeof(got))
                    got[got_len++] = c;
                else
                    fits = false;
            }
            while (at < len && value[at] != ';')
                at++;
        } else {
            size_t from = at;
            while (at < len && value[at] != ';')
                at++;
            size_t to = at;
            while (to > from && http_whitespace(value[to - 1]))
                to--;
            if (to == from)
                continue;
            for (size_t i = from; i < to; i++) {
                if (!quoted_token_byte((unsigned char)value[i]))
                    valid = false;
                if (got_len + 1 < sizeof(got))
                    got[got_len++] = value[i];
                else
                    fits = false;
            }
        }
        got[got_len] = '\0';
        if (name_len > 0 && is_token(value + name, name_len) && valid)
            each(ctx, value + name, name_len, got, got_len, fits);
    }
    return true;
}

typedef struct {
    char *charset;
    size_t ccap;
    bool have;
} CharsetWalk;

static void charset_param(void *ctx, const char *name, size_t name_len, const char *value, size_t value_len,
                          bool fits)
{
    CharsetWalk *w = ctx;
    (void)value_len;
    if (w->have || !is_nocase(name, name_len, "charset"))
        return;
    w->have = true;
    if (fits && w->charset != NULL && value_len < WAY_MIME_CHARSET_MAX)
        os64_strcopy(w->charset, w->ccap, value);
}

bool way_mime_parse(const char *value, size_t len, char *essence, size_t ecap, char *charset,
                    size_t ccap)
{
    if (charset != NULL && ccap > 0)
        charset[0] = '\0';
    CharsetWalk w = {charset, ccap, false};
    return mime_walk(value, len, essence, ecap, charset_param, &w);
}

bool way_mime_extract(const char *value, size_t len, char *essence, size_t ecap, char *charset, size_t ccap)
{
    char e[WAY_MIME_ESSENCE_MAX], c[WAY_MIME_CHARSET_MAX], remembered[WAY_MIME_CHARSET_MAX] = "";
    bool found = false;
    size_t at = 0;
    bool started = false;
    const char *v;
    size_t vlen;
    if (charset != NULL && ccap > 0)
        charset[0] = '\0';
    // Split as "get, decode, and split" does, so a comma inside a quoted
    // parameter value is the value's.
    while (split_next(value, len, &at, &started, &v, &vlen)) {
        if (!way_mime_parse(v, vlen, e, sizeof(e), c, sizeof(c)) || os64_streq(e, "*/*"))
            continue;
        if (!found || !os64_streq(e, essence)) {
            if (os64_strcopy(essence, ecap, e) >= ecap)
                return false;
            os64_strcopy(remembered, sizeof(remembered), c);
        }
        found = true;
        // The type's own charset, else the one its essence's first type
        // carried ("set mimeType's parameters[charset] to charset").
        if (charset != NULL)
            os64_strcopy(charset, ccap, c[0] != '\0' ? c : remembered);
    }
    return found;
}

// The serializer's walk: each parameter whose (lowercased) name is not
// already written goes on as ";name=value", the value quoted (each quote and
// backslash escaped) unless it is a non-empty token (MIME Sniffing § 4.6). Names are
// compared against the ones written before, a walk per parameter, which the
// input's cap (WAY_MIME_SERIALIZE_IN_MAX) keeps small.
typedef struct {
    char *out;
    size_t cap, used;
    bool overflow;
    const char *names[WAY_MIME_SERIALIZE_IN_MAX / 4];
    size_t lens[WAY_MIME_SERIALIZE_IN_MAX / 4];
    size_t count;
} SerializeWalk;

static void serial_put(SerializeWalk *w, char c)
{
    if (w->used + 1 >= w->cap) {
        w->overflow = true;
        return;
    }
    w->out[w->used++] = c;
    w->out[w->used] = '\0';
}

static void serialize_param(void *ctx, const char *name, size_t name_len, const char *value, size_t value_len,
                            bool fits)
{
    SerializeWalk *w = ctx;
    for (size_t i = 0; i < w->count; i++)
        if (w->lens[i] == name_len) {
            bool same = true;
            for (size_t j = 0; same && j < name_len; j++)
                same = lower(w->names[i][j]) == lower(name[j]);
            if (same)
                return;
        }
    if (!fits || w->count == sizeof(w->names) / sizeof(w->names[0])) {
        w->overflow = true;
        return;
    }
    w->names[w->count] = name;
    w->lens[w->count++] = name_len;
    serial_put(w, ';');
    for (size_t i = 0; i < name_len; i++)
        serial_put(w, lower(name[i]));
    serial_put(w, '=');
    bool token = value_len > 0 && is_token(value, value_len);
    if (!token)
        serial_put(w, '"');
    for (size_t i = 0; i < value_len; i++) {
        if (!token && (value[i] == '"' || value[i] == '\\'))
            serial_put(w, '\\');
        serial_put(w, value[i]);
    }
    if (!token)
        serial_put(w, '"');
}

bool way_mime_serialize(const char *value, size_t len, char *out, size_t cap)
{
    if (cap == 0 || len > WAY_MIME_SERIALIZE_IN_MAX)
        return false;
    out[0] = '\0';
    SerializeWalk w;
    w.out = out;
    w.cap = cap;
    w.used = 0;
    w.overflow = false;
    w.count = 0;
    char essence[WAY_MIME_ESSENCE_MAX];
    if (!mime_walk(value, len, essence, sizeof(essence), serialize_param, &w) || w.overflow)
        return false;
    // The essence goes first: moved in front of the parameters written.
    size_t elen = os64_strlen(essence);
    if (elen + w.used + 1 > cap)
        return false;
    os64_memmove(out + elen, out, w.used + 1);
    os64_memcpy(out, essence, elen);
    return true;
}

// ── A request, hop by hop ───────────────────────────────────────────────

static bool scheme_is(const char *url, const char *want)
{
    char scheme[OS64_URL_SCHEME_MAX];
    return url != NULL && os64_url_scheme_of(url, scheme, sizeof(scheme)) && os64_streq(scheme, want);
}

static bool http_scheme(const char *url)
{
    return scheme_is(url, "http") || scheme_is(url, "https");
}

void way_request_start(way_request_t *r, const char *url)
{
    r->tainting = WAY_TAINT_BASIC;
    r->rtaint = WAY_RTAINT_SAME_ORIGIN;
    way_origin_of(url, 0, &r->current);
    r->urls = 1;
}

const char *way_refusal_name(way_refusal_t why)
{
    switch (why) {
    case WAY_REFUSE_NONE: return "none";
    case WAY_REFUSE_PORT: return "a blocked port";
    case WAY_REFUSE_MIXED: return "mixed content";
    case WAY_REFUSE_SAME_ORIGIN: return "mode same-origin met another origin";
    case WAY_REFUSE_NO_CORS: return "mode no-cors cannot stop at a redirect";
    case WAY_REFUSE_SCHEME: return "not an http, https or data address";
    case WAY_REFUSE_URL: return "an address this browser cannot fetch";
    }
    return "?";
}

way_hop_t way_main_fetch(way_request_t *r, const char *url, way_refusal_t *why)
{
    *why = WAY_REFUSE_NONE;
    if (http_scheme(url) && r->current.opaque) {
        *why = WAY_REFUSE_URL;
        return WAY_HOP_REFUSED;
    }
    if (way_port_bad(url)) {
        *why = WAY_REFUSE_PORT;
        return WAY_HOP_REFUSED;
    }
    if (way_mixed_blocked(&r->origin, url)) {
        *why = WAY_REFUSE_MIXED;
        return WAY_HOP_REFUSED;
    }
    // § 4.1's first matching statement. Same origin keeps a request basic
    // only while it IS basic: a chain that left the origin never returns.
    // Its scheme fetch serves http(s) alone: a same-origin blob: URL has no
    // Blob URL store to come from.
    if (way_origin_same(&r->current, &r->origin) && r->tainting == WAY_TAINT_BASIC) {
        if (!http_scheme(url)) {
            *why = WAY_REFUSE_SCHEME;
            return WAY_HOP_REFUSED;
        }
        return WAY_HOP_BASIC;
    }
    if (scheme_is(url, "data")) {
        r->tainting = WAY_TAINT_BASIC;
        return WAY_HOP_DATA;
    }
    if (r->mode == WAY_MODE_SAME_ORIGIN) {
        *why = WAY_REFUSE_SAME_ORIGIN;
        return WAY_HOP_REFUSED;
    }
    if (!http_scheme(url)) {
        // no-cors goes to scheme fetch, which fetches nothing but http(s)
        // and data: here: the same refusal either way.
        *why = WAY_REFUSE_SCHEME;
        return WAY_HOP_REFUSED;
    }
    if (r->mode == WAY_MODE_NO_CORS) {
        if (r->redirect != WAY_REDIRECT_FOLLOW) {
            *why = WAY_REFUSE_NO_CORS;
            return WAY_HOP_REFUSED;
        }
        r->tainting = WAY_TAINT_OPAQUE;
        return WAY_HOP_OPAQUE;
    }
    r->tainting = WAY_TAINT_CORS;
    return WAY_HOP_CORS;
}

bool way_preflight_capable(const way_request_t *r)
{
    return r->tainting == WAY_TAINT_CORS &&
           (r->preflight_flag || !way_method_safelisted(r->method) ||
            way_cors_unsafe_names(r->headers, r->headers_len, NULL, 0) != 0);
}

bool way_credentials_included(const way_request_t *r)
{
    return r->credentials == WAY_CREDENTIALS_INCLUDE ||
           (r->credentials == WAY_CREDENTIALS_SAME_ORIGIN && r->tainting == WAY_TAINT_BASIC);
}

bool way_request_origin(const way_request_t *r, char *out, size_t cap)
{
    if (r->rtaint != WAY_RTAINT_SAME_ORIGIN)
        return os64_strcopy(out, cap, "null") < cap;
    return way_origin_serialize(&r->origin, out, cap);
}

bool way_origin_header(const way_request_t *r, const char *url, char *out, size_t cap)
{
    if (r->tainting == WAY_TAINT_CORS)
        return way_request_origin(r, out, cap);
    if (os64_streq(r->method, "GET") || os64_streq(r->method, "HEAD"))
        return false;
    if (!way_request_origin(r, out, cap))
        return false;
    if (r->mode == WAY_MODE_CORS)
        return true;
    way_origin_t to;
    way_origin_of(url, 0, &to);
    bool null = false;
    switch (r->policy) {
    case WAY_POLICY_NO_REFERRER:
        null = true;
        break;
    case WAY_POLICY_NO_REFERRER_WHEN_DOWNGRADE:
    case WAY_POLICY_STRICT_ORIGIN:
    case WAY_POLICY_STRICT_ORIGIN_WHEN_CROSS_ORIGIN:
        null = !r->origin.opaque && os64_streq(r->origin.scheme, "https") && !scheme_is(url, "https");
        break;
    case WAY_POLICY_SAME_ORIGIN:
        null = !way_origin_same(&r->origin, &to);
        break;
    default:
        break;
    }
    if (null)
        return os64_strcopy(out, cap, "null") < cap;
    return true;
}

// Referrer Policy § 8.4 "strip url for use as a referrer": the address
// without its fragment (os64's URL never holds userinfo), or its origin
// alone as a URL ("https://a.test/"). Only http(s) pages are referrers
// here; the standard lets file: be one, Chrome does not, and neither does
// way_referrer.
static bool strip_referrer(const char *referrer, bool origin_only, char *out, size_t cap)
{
    if (!http_scheme(referrer))
        return false;
    os64_url_t *u = os64_malloc(sizeof(*u));
    if (u == NULL)
        return false;
    bool ok = os64_url_parse(referrer, u) == OS64_URL_OK;
    if (ok && origin_only)
        os64_strcopy(u->path, sizeof(u->path), "/");
    ok = ok && os64_url_spell(u, out, cap);
    os64_free(u);
    return ok;
}

bool way_referrer_for(const way_request_t *r, const char *referrer, const char *url, char *out,
                      size_t cap)
{
    if (referrer == NULL || referrer[0] == '\0' || r->policy == WAY_POLICY_NO_REFERRER)
        return false;
    way_origin_t from, to;
    way_origin_of(referrer, 0, &from);
    way_origin_of(url, 0, &to);
    bool same = way_origin_same(&from, &to);
    bool downgrade = trustworthy(referrer) && !trustworthy(url);
    bool whole;
    switch (r->policy) {
    case WAY_POLICY_ORIGIN: whole = false; break;
    case WAY_POLICY_UNSAFE_URL: whole = true; break;
    case WAY_POLICY_STRICT_ORIGIN:
        if (downgrade)
            return false;
        whole = false;
        break;
    case WAY_POLICY_SAME_ORIGIN:
        if (!same)
            return false;
        whole = true;
        break;
    case WAY_POLICY_ORIGIN_WHEN_CROSS_ORIGIN: whole = same; break;
    case WAY_POLICY_NO_REFERRER_WHEN_DOWNGRADE:
        if (downgrade)
            return false;
        whole = true;
        break;
    default:                                    // strict-origin-when-cross-origin
        if (same) {
            whole = true;
            break;
        }
        if (downgrade)
            return false;
        whole = false;
        break;
    }
    if (!strip_referrer(referrer, !whole, out, cap))
        return false;
    // § 8.3 step 7: an address longer than 4096 bytes goes as its origin.
    if (whole && os64_strlen(out) > 4096)
        return strip_referrer(referrer, true, out, cap);
    return true;
}

// ── A response's headers ────────────────────────────────────────────────

typedef enum { GET_ABSENT, GET_OK, GET_UNREADABLE, GET_LONG } GetResult;

static bool omitted(const way_response_t *resp, const char *name)
{
    size_t at = 0, nlen = os64_strlen(name);
    const char *o = resp->omitted;
    while (o != NULL && at < resp->omitted_len) {
        size_t from = at;
        while (at < resp->omitted_len && o[at] != '\r' && o[at] != '\n')
            at++;
        if (same_nocase(o + from, at - from, name, nlen))
            return true;
        while (at < resp->omitted_len && (o[at] == '\r' || o[at] == '\n'))
            at++;
    }
    return false;
}

// § 2.2.2 "get": every line named `name`, values joined by ", ", into `out`.
static GetResult header_get(const way_response_t *resp, const char *name, char *out, size_t cap)
{
    if (omitted(resp, name))
        return GET_UNREADABLE;
    size_t at = 0, used = 0, nlen = os64_strlen(name);
    bool found = false;
    way_header_t h;
    out[0] = '\0';
    while (way_header_next(resp->headers, resp->headers_len, &at, &h)) {
        if (!same_nocase(h.name, h.name_len, name, nlen))
            continue;
        size_t need = (found ? 2 : 0) + h.value_len;
        if (used + need + 1 > cap)
            return GET_LONG;
        if (found) {
            out[used++] = ',';
            out[used++] = ' ';
        }
        os64_memcpy(out + used, h.value, h.value_len);
        used += h.value_len;
        out[used] = '\0';
        found = true;
    }
    return found ? GET_OK : GET_ABSENT;
}

// § 2.2.2 "extract header list values" for a `#token` header (#method,
// #field-name), one element at a time across every line of that name.
typedef struct {
    const way_response_t *resp;
    const char *name;
    size_t name_len;
    size_t line_at;             // where the next line is read from
    const char *value;          // the line being split
    size_t value_len, at;
    bool in_line;
} ListWalk;

typedef enum { LIST_ABSENT, LIST_OK, LIST_FAILURE } ListResult;

static void list_start(ListWalk *w, const way_response_t *resp, const char *name)
{
    os64_memset(w, 0, sizeof(*w));
    w->resp = resp;
    w->name = name;
    w->name_len = os64_strlen(name);
}

// The next non-empty element, or false at the end. `*bad` says one was not
// a token.
static bool list_next(ListWalk *w, const char **item, size_t *len, bool *bad)
{
    for (;;) {
        step();
        if (!w->in_line) {
            way_header_t h;
            bool found = false;
            while (way_header_next(w->resp->headers, w->resp->headers_len, &w->line_at, &h))
                if (same_nocase(h.name, h.name_len, w->name, w->name_len)) {
                    found = true;
                    break;
                }
            if (!found)
                return false;
            w->value = h.value;
            w->value_len = h.value_len;
            w->at = 0;
            w->in_line = true;
        }
        if (w->at >= w->value_len) {
            w->in_line = false;
            continue;
        }
        size_t from = w->at;
        while (w->at < w->value_len && w->value[w->at] != ',')
            w->at++;
        size_t to = w->at;
        if (w->at < w->value_len)
            w->at++;
        while (from < to && http_tab_or_space(w->value[from]))
            from++;
        while (to > from && http_tab_or_space(w->value[to - 1]))
            to--;
        if (to == from)
            continue;                           // an empty element is ignored
        *item = w->value + from;
        *len = to - from;
        if (!is_token(*item, *len))
            *bad = true;
        return true;
    }
}

// A response's list, read once: its verdict, and its elements sorted (case
// folded) for searching. ABSENT, OK, or FAILURE: an element that is not a
// token, a line that could not be read, more than WAY_CORS_LIST_MAX
// elements, or no memory to hold them, each read the restrictive way.
typedef struct {
    ListResult result;
    Span *items;
    size_t n;
} Names;

static void names_free(Names *l)
{
    os64_free(l->items);
    l->items = NULL;
    l->n = 0;
}

static void names_read(const way_response_t *resp, const char *name, Names *out)
{
    os64_memset(out, 0, sizeof(*out));
    out->result = LIST_FAILURE;
    if (omitted(resp, name))
        return;
    size_t lines = 0, at = 0;
    way_header_t h;
    while (way_header_next(resp->headers, resp->headers_len, &at, &h))
        if (is_nocase(h.name, h.name_len, name))
            lines++;
    if (lines == 0) {
        out->result = LIST_ABSENT;
        return;
    }
    ListWalk w;
    list_start(&w, resp, name);
    const char *item;
    size_t len, n = 0;
    bool bad = false;
    while (list_next(&w, &item, &len, &bad) && !bad && n <= WAY_CORS_LIST_MAX)
        n++;
    if (bad || n > WAY_CORS_LIST_MAX)
        return;
    out->result = LIST_OK;
    if (n == 0)
        return;
    Span *s = os64_malloc(2 * n * sizeof(Span));
    if (s == NULL) {
        out->result = LIST_FAILURE;
        return;
    }
    list_start(&w, resp, name);
    for (size_t k = 0; k < n && list_next(&w, &item, &len, &bad); k++)
        s[k] = (Span){item, len};
    spans_sort(s, s + n, n);
    out->items = s;
    out->n = n;
}

// Whether the list names `want`: case folded, or (for methods, which are
// case-sensitive) byte for byte.
static bool names_has(const Names *l, const char *want, size_t len, bool nocase)
{
    if (l->result != LIST_OK)
        return false;
    for (size_t i = spans_lower_bound(l->items, l->n, want, len);
         i < l->n && same_nocase(l->items[i].p, l->items[i].n, want, len); i++)
        if (nocase || same_bytes(l->items[i].p, l->items[i].n, want, len))
            return true;
    return false;
}

bool way_cors_check(const way_request_t *r, const way_response_t *resp)
{
    char origin[WAY_ORIGIN_TEXT_MAX + 8];
    // Longer than any serialized origin is a mismatch, so GET_LONG fails too.
    if (header_get(resp, "access-control-allow-origin", origin, sizeof(origin)) != GET_OK)
        return false;
    bool include = r->credentials == WAY_CREDENTIALS_INCLUDE;
    if (!include && os64_streq(origin, "*"))
        return true;
    char mine[WAY_ORIGIN_TEXT_MAX];
    if (!way_request_origin(r, mine, sizeof(mine)) || !os64_streq(origin, mine))
        return false;
    if (!include)
        return true;
    char credentials[8];
    return header_get(resp, "access-control-allow-credentials", credentials, sizeof(credentials)) == GET_OK &&
           os64_streq(credentials, "true");
}

// ── The preflight ───────────────────────────────────────────────────────

bool way_preflight_lines(const way_request_t *r, char *out, size_t cap)
{
    int32_t n = os64_snprintf(out, cap, "Access-Control-Request-Method: %s\r\n", r->method);
    if (n <= 0 || (size_t)n >= cap)
        return false;
    size_t used = (size_t)n;
    // The names go straight after their header's name; with none, the
    // header is taken back off.
    static const char kHeaders[] = "Access-Control-Request-Headers: ";
    if (used + sizeof(kHeaders) - 1 >= cap)
        return false;
    os64_memcpy(out + used, kHeaders, sizeof(kHeaders) - 1);
    int32_t names = way_cors_unsafe_names(r->headers, r->headers_len, out + used + sizeof(kHeaders) - 1,
                                          cap - used - (sizeof(kHeaders) - 1));
    if (names < 0)
        return false;
    if (names == 0) {
        out[used] = '\0';
        return true;
    }
    used += sizeof(kHeaders) - 1;
    used += os64_strlen(out + used);
    if (used + 3 > cap)
        return false;
    out[used++] = '\r';
    out[used++] = '\n';
    out[used] = '\0';
    return true;
}

// Access-Control-Max-Age (§ 3.3.4, delta-seconds, a single header) as the
// seconds to keep the grants: 5 when absent or not a number, capped, and 0
// when a line of it could not be read (nothing kept on a guess).
static uint32_t max_age_of(const way_response_t *resp)
{
    if (omitted(resp, "access-control-max-age"))
        return 0;
    size_t at = 0, lines = 0;
    way_header_t h, one = {0};
    while (way_header_next(resp->headers, resp->headers_len, &at, &h))
        if (is_nocase(h.name, h.name_len, "access-control-max-age")) {
            one = h;
            lines++;
        }
    if (lines != 1 || one.value_len == 0)
        return 5;
    uint64_t v = 0;
    for (size_t i = 0; i < one.value_len; i++) {
        if (!is_digit(one.value[i]))
            return 5;
        if (v < WAY_PREFLIGHT_AGE_MAX)
            v = v * 10 + (uint64_t)(one.value[i] - '0');
    }
    return v > WAY_PREFLIGHT_AGE_MAX ? WAY_PREFLIGHT_AGE_MAX : (uint32_t)v;
}

// § 4.8's judgement over lists already read.
static bool preflight_allows(const way_request_t *r, const Names *methods, const Names *names)
{
    if (methods->result == LIST_FAILURE || names->result == LIST_FAILURE)
        return false;
    bool include = r->credentials == WAY_CREDENTIALS_INCLUDE;
    // A null method list with the use-CORS-preflight flag is « method ».
    bool in_methods = methods->result == LIST_OK ? names_has(methods, r->method, os64_strlen(r->method), false)
                                                 : r->preflight_flag;
    bool method_star = names_has(methods, "*", 1, false);
    if (!in_methods && !way_method_safelisted(r->method) && (include || !method_star))
        return false;
    bool name_star = names_has(names, "*", 1, false);
    size_t at = 0;
    way_header_t h;
    bool overflow = safelist_overflows(r->headers, r->headers_len);
    while (way_header_next(r->headers, r->headers_len, &at, &h)) {
        bool named = names_has(names, h.name, h.name_len, true);
        if (way_header_non_wildcard(h.name, h.name_len) && !named)
            return false;
        if (unsafe_name(&h, overflow) && !named && (include || !name_star))
            return false;
    }
    return true;
}

bool way_preflight_judge(const way_request_t *r, const way_response_t *resp, uint32_t *max_age)
{
    *max_age = 0;
    if (!way_cors_check(r, resp) || resp->status < 200 || resp->status > 299)
        return false;
    Names methods, names;
    names_read(resp, "access-control-allow-methods", &methods);
    names_read(resp, "access-control-allow-headers", &names);
    bool allowed = preflight_allows(r, &methods, &names);
    names_free(&methods);
    names_free(&names);
    if (allowed)
        *max_age = max_age_of(resp);
    return allowed;
}

// ── The preflight cache ─────────────────────────────────────────────────
//
// A grant is one method or header name for one key. Grants of one key share
// a number, found once per call by comparing whole keys, so a call compares
// at most the cap's worth of keys, and then only numbers and short names,
// however many names an answer lists (WAY_CORS_LIST_MAX bounds those).

typedef struct {
    char *key;                  // partition, '\n', request origin, '\n', URL; one allocation with `item`
    size_t key_len;
    uint64_t key_hash;
    uint32_t key_id;            // the same for every grant of one key
    char *item;                 // the method, or the header name
    size_t item_len;
    bool method;
    bool credentials;
    uint64_t expires_ms;
} Grant;

struct way_preflight {
    os64_lock_t lock;
    Grant grants[WAY_PREFLIGHT_ENTRIES_MAX];
    int32_t n;
    uint32_t next_id;
};

// An entry's key, in three pieces, so that looking one up allocates nothing:
// § 4.9's network partition key (the page's site, by the same-host stand-in),
// the serialized request origin, and the URL.
typedef struct {
    char partition[WAY_ORIGIN_TEXT_MAX + 24];
    char origin[WAY_ORIGIN_TEXT_MAX];
    const char *url;
    size_t partition_len, origin_len, url_len, len;
    uint64_t hash;
} Key;

// FNV-1a, over the key's bytes as one string.
static uint64_t hash_bytes(uint64_t h, const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static bool key_of(const way_request_t *r, const char *url, Key *k)
{
    if (r->origin.opaque)
        os64_snprintf(k->partition, sizeof(k->partition), "opaque %lu", (unsigned long)r->origin.id);
    else
        os64_snprintf(k->partition, sizeof(k->partition), "%s %s", r->origin.scheme, r->origin.host);
    if (!way_request_origin(r, k->origin, sizeof(k->origin)))
        return false;
    k->url = url;
    k->partition_len = os64_strlen(k->partition);
    k->origin_len = os64_strlen(k->origin);
    k->url_len = os64_strlen(url);
    k->len = k->partition_len + 1 + k->origin_len + 1 + k->url_len;
    uint64_t h = hash_bytes(14695981039346656037ull, k->partition, k->partition_len);
    h = hash_bytes(h, "\n", 1);
    h = hash_bytes(h, k->origin, k->origin_len);
    h = hash_bytes(h, "\n", 1);
    k->hash = hash_bytes(h, url, k->url_len);
    return true;
}

static bool key_is(const Grant *g, const Key *k)
{
    step();
    if (g->key_hash != k->hash || g->key_len != k->len)
        return false;
    const char *p = g->key;
    return os64_memcmp(p, k->partition, k->partition_len) == 0 && p[k->partition_len] == '\n' &&
           os64_memcmp(p + k->partition_len + 1, k->origin, k->origin_len) == 0 &&
           p[k->partition_len + 1 + k->origin_len] == '\n' &&
           os64_memcmp(p + k->partition_len + 1 + k->origin_len + 1, k->url, k->url_len) == 0;
}

// The number the key's grants share, 0 when it has none.
static uint32_t key_find(const way_preflight_t *c, const Key *k)
{
    for (int32_t i = 0; i < c->n; i++)
        if (key_is(&c->grants[i], k))
            return c->grants[i].key_id;
    return 0;
}

way_preflight_t *way_preflight_new(void)
{
    return os64_calloc(1, sizeof(way_preflight_t));
}

static void grant_drop(way_preflight_t *c, int32_t i)
{
    os64_free(c->grants[i].key);
    c->grants[i] = c->grants[--c->n];
}

void way_preflight_free(way_preflight_t *cache)
{
    if (cache == NULL)
        return;
    while (cache->n > 0)
        grant_drop(cache, 0);
    os64_free(cache);
}

static void expire(way_preflight_t *c, uint64_t now_ms)
{
    for (int32_t i = 0; i < c->n;)
        if (c->grants[i].expires_ms <= now_ms)
            grant_drop(c, i);
        else
            i++;
}

// § 4.9 "cache entry match", for grants of the key numbered `id`.
static bool grant_matches(const Grant *g, uint32_t id, bool include)
{
    step();
    return id != 0 && g->key_id == id && (g->credentials || !include);
}

static bool method_cached(const way_preflight_t *c, uint32_t id, bool include, const char *method)
{
    size_t len = os64_strlen(method);
    for (int32_t i = 0; i < c->n; i++) {
        const Grant *g = &c->grants[i];
        if (g->method && grant_matches(g, id, include) &&
            (same_bytes(g->item, g->item_len, method, len) || same_bytes(g->item, g->item_len, "*", 1)))
            return true;
    }
    return false;
}

static bool name_cached(const way_preflight_t *c, uint32_t id, bool include, const char *name, size_t len)
{
    for (int32_t i = 0; i < c->n; i++) {
        const Grant *g = &c->grants[i];
        if (g->method || !grant_matches(g, id, include))
            continue;
        if (same_nocase(g->item, g->item_len, name, len) ||
            (same_bytes(g->item, g->item_len, "*", 1) && !way_header_non_wildcard(name, len)))
            return true;
    }
    return false;
}

bool way_preflight_needed(way_preflight_t *cache, const way_request_t *r, const char *url,
                          uint64_t now_ms)
{
    Key k;
    if (cache == NULL || !key_of(r, url, &k))
        return true;
    bool include = r->credentials == WAY_CREDENTIALS_INCLUDE;
    os64_lock_acquire(&cache->lock);
    expire(cache, now_ms);
    uint32_t id = key_find(cache, &k);
    bool needed = !method_cached(cache, id, include, r->method) &&
                  (!way_method_safelisted(r->method) || r->preflight_flag);
    bool overflow = safelist_overflows(r->headers, r->headers_len);
    size_t at = 0;
    way_header_t h;
    while (!needed && way_header_next(r->headers, r->headers_len, &at, &h))
        if (unsafe_name(&h, overflow) && !name_cached(cache, id, include, h.name, h.name_len))
            needed = true;
    os64_lock_release(&cache->lock);
    return needed;
}

// A number no live grant has: 0 means "none", and after the counter wraps a
// number still in use would make two keys one. At most the cap's worth of
// numbers are in use, so the search ends within that many steps.
static uint32_t id_new(way_preflight_t *c)
{
    for (;;) {
        if (++c->next_id == 0)
            c->next_id = 1;
        bool used = false;
        for (int32_t i = 0; i < c->n && !used; i++)
            used = c->grants[i].key_id == c->next_id;
        if (!used)
            return c->next_id;
    }
}

// One grant kept for the key numbered `*id` (0: the key has none yet, and
// gets its number here): an existing one of the same kind and item has its
// lifetime set; else a new one, past the cap evicting the soonest to
// expire. A grant there is no memory for is not kept, and is asked for again.
static void grant_put(way_preflight_t *c, const Key *k, uint32_t *id, bool method, bool credentials,
                      const char *item, size_t item_len, uint64_t expires_ms)
{
    for (int32_t i = 0; *id != 0 && i < c->n; i++) {
        Grant *g = &c->grants[i];
        step();
        if (g->key_id == *id && g->method == method && g->credentials == credentials &&
            (method ? same_bytes(g->item, g->item_len, item, item_len)
                    : same_nocase(g->item, g->item_len, item, item_len))) {
            g->expires_ms = expires_ms;
            return;
        }
    }
    char *block = os64_malloc(k->len + 1 + item_len + 1);
    if (block == NULL)
        return;
    if (c->n == WAY_PREFLIGHT_ENTRIES_MAX) {
        int32_t soonest = 0;
        for (int32_t i = 1; i < c->n; i++)
            if (c->grants[i].expires_ms < c->grants[soonest].expires_ms)
                soonest = i;
        grant_drop(c, soonest);
    }
    if (*id == 0)
        *id = id_new(c);
    char *p = block;
    os64_memcpy(p, k->partition, k->partition_len);
    p += k->partition_len;
    *p++ = '\n';
    os64_memcpy(p, k->origin, k->origin_len);
    p += k->origin_len;
    *p++ = '\n';
    os64_memcpy(p, k->url, k->url_len);
    p += k->url_len;
    *p++ = '\0';
    os64_memcpy(p, item, item_len);
    p[item_len] = '\0';
    Grant *g = &c->grants[c->n++];
    g->key = block;
    g->key_len = k->len;
    g->key_hash = k->hash;
    g->key_id = *id;
    g->item = p;
    g->item_len = item_len;
    g->method = method;
    g->credentials = credentials;
    g->expires_ms = expires_ms;
}

void way_preflight_store(way_preflight_t *cache, const way_request_t *r, const char *url,
                         const way_response_t *resp, uint32_t max_age, uint64_t now_ms)
{
    Key k;
    if (cache == NULL || max_age == 0 || !key_of(r, url, &k))
        return;
    bool include = r->credentials == WAY_CREDENTIALS_INCLUDE;
    uint64_t expires = now_ms + (uint64_t)max_age * 1000u;
    // The lists are read and sorted outside the lock, which is spun on. What
    // the lock holds is the larger part: an expiry pass and a key lookup over
    // the grants, a number for a new key, then for each of up to 2 *
    // WAY_CORS_LIST_MAX items the server named, grant_put's scan of up to
    // WAY_PREFLIGHT_ENTRIES_MAX grants for one to renew, and, for a grant
    // kept, an allocation (its copy of the key and the item), plus, past the
    // cap, an eviction scan of the same grants and a free. O(items *
    // WAY_PREFLIGHT_ENTRIES_MAX), bounded by the two constants whatever the
    // server sends.
    Names lists[2];
    names_read(resp, "access-control-allow-methods", &lists[0]);
    names_read(resp, "access-control-allow-headers", &lists[1]);
    os64_lock_acquire(&cache->lock);
    expire(cache, now_ms);
    uint32_t id = key_find(cache, &k);
    // A `*` is kept as an uncredentialed grant whatever this request was: the
    // standard's match would otherwise let a `*` from a credentialed answer
    // that also named one method stand in for a method it never named.
    for (int32_t which = 0; which < 2; which++) {
        bool method = which == 0;
        if (method && lists[0].result == LIST_ABSENT && r->preflight_flag)
            grant_put(cache, &k, &id, true, include, r->method, os64_strlen(r->method), expires);
        for (size_t i = 0; lists[which].result == LIST_OK && i < lists[which].n; i++) {
            const Span *item = &lists[which].items[i];
            bool star = item->n == 1 && item->p[0] == '*';
            grant_put(cache, &k, &id, method, include && !star, item->p, item->n, expires);
        }
    }
    os64_lock_release(&cache->lock);
    names_free(&lists[0]);
    names_free(&lists[1]);
}

void way_preflight_clear(way_preflight_t *cache, const way_request_t *r, const char *url)
{
    Key k;
    if (cache == NULL || !key_of(r, url, &k))
        return;
    os64_lock_acquire(&cache->lock);
    uint32_t id = key_find(cache, &k);
    for (int32_t i = 0; id != 0 && i < cache->n;)
        if (cache->grants[i].key_id == id)
            grant_drop(cache, i);
        else
            i++;
    os64_lock_release(&cache->lock);
}

int32_t way_preflight_count(way_preflight_t *cache)
{
    if (cache == NULL)
        return 0;
    os64_lock_acquire(&cache->lock);
    int32_t n = cache->n;
    os64_lock_release(&cache->lock);
    return n;
}

// ── Redirects ───────────────────────────────────────────────────────────

bool way_redirect_status(int32_t status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Deletes every author header the predicate names, in place.
static void delete_headers(way_request_t *r, bool (*doomed)(const char *name, size_t len))
{
    size_t at = 0, keep = 0;
    way_header_t h;
    while (r->headers != NULL && at < r->headers_len) {
        size_t from = at;
        if (!way_header_next(r->headers, r->headers_len, &at, &h))
            break;
        if (doomed(h.name, h.name_len))
            continue;
        if (keep != from)
            os64_memmove(r->headers + keep, r->headers + from, at - from);
        keep += at - from;
    }
    if (r->headers != NULL) {
        r->headers_len = keep;
        r->headers[keep] = '\0';
    }
}

// § 2.2.5 "compute the redirect-taint", one URL at a time: what appending
// `next` after `last` does to it. It only ever rises.
static void taint_step(way_request_t *r, const way_origin_t *last, const way_origin_t *next)
{
    if (r->rtaint == WAY_RTAINT_CROSS_SITE)
        return;
    if (!way_origin_same_site(next, last) && !way_origin_same_site(&r->origin, last))
        r->rtaint = WAY_RTAINT_CROSS_SITE;
    else if (!way_origin_same(next, last) && !way_origin_same(&r->origin, last))
        r->rtaint = WAY_RTAINT_SAME_SITE;
}

way_follow_t way_redirect_follow(way_request_t *r, int32_t status, const char *from, const char *to,
                                 bool *drop_body)
{
    *drop_body = false;
    if (r->urls > WAY_REDIRECTS_MAX || !http_scheme(to))
        return WAY_FOLLOW_REFUSED;
    way_origin_t next;
    way_origin_of(to, 0, &next);
    if (next.opaque)
        return WAY_FOLLOW_REFUSED;              // a URL os64's grammar refuses (userinfo among them)
    // House policy (fetch.h): what a script asks for never steps down from
    // https to plain http on a server's say-so, whatever the page's scheme.
    if (scheme_is(from, "https") && scheme_is(to, "http"))
        return WAY_FOLLOW_REFUSED;
    if (((status == 301 || status == 302) && os64_streq(r->method, "POST")) ||
        (status == 303 && !os64_streq(r->method, "GET") && !os64_streq(r->method, "HEAD"))) {
        os64_strcopy(r->method, sizeof(r->method), "GET");
        *drop_body = r->body;
        r->body = false;
        delete_headers(r, way_header_request_body);
    }
    way_origin_t was;
    way_origin_of(from, 0, &was);
    if (!way_origin_same(&was, &next))
        delete_headers(r, way_header_non_wildcard);
    taint_step(r, &r->current, &next);
    r->current = next;
    r->urls++;
    return WAY_FOLLOW_GO;
}

// ── The response a page sees ────────────────────────────────────────────

const char *way_type_name(way_type_t type)
{
    switch (type) {
    case WAY_TYPE_BASIC: return "basic";
    case WAY_TYPE_CORS: return "cors";
    case WAY_TYPE_OPAQUE: return "opaque";
    case WAY_TYPE_OPAQUEREDIRECT: return "opaqueredirect";
    case WAY_TYPE_ERROR: return "error";
    case WAY_TYPE_DEFAULT: return "default";
    }
    return "error";
}

way_type_t way_response_type(const way_request_t *r)
{
    return r->tainting == WAY_TAINT_CORS ? WAY_TYPE_CORS
         : r->tainting == WAY_TAINT_OPAQUE ? WAY_TYPE_OPAQUE : WAY_TYPE_BASIC;
}

static bool always_safelisted_response(const char *name, size_t len)
{
    static const char *const kSafe[] = {
        "cache-control", "content-language", "content-length", "content-type", "expires",
        "last-modified", "pragma",
    };
    for (size_t i = 0; i < sizeof(kSafe) / sizeof(kSafe[0]); i++)
        if (is_nocase(name, len, kSafe[i]))
            return true;
    return false;
}

size_t way_response_headers(const way_request_t *r, way_type_t type, const way_response_t *resp,
                            char *out, size_t cap)
{
    if (cap == 0)
        return SIZE_MAX;
    out[0] = '\0';
    if (type != WAY_TYPE_BASIC && type != WAY_TYPE_CORS)
        return 0;
    // § 4.1: the CORS-exposed header-name list. A list that cannot be read
    // or does not parse (Names' FAILURE) exposes nothing beyond the
    // safelisted names.
    Names exposed = {.result = LIST_ABSENT};
    bool expose_all = false;
    if (type == WAY_TYPE_CORS) {
        names_read(resp, "access-control-expose-headers", &exposed);
        expose_all = r->credentials != WAY_CREDENTIALS_INCLUDE && names_has(&exposed, "*", 1, false);
    }
    size_t at = 0, used = 0;
    way_header_t h;
    while (way_header_next(resp->headers, resp->headers_len, &at, &h)) {
        bool keep;
        if (way_header_forbidden_response(h.name, h.name_len))
            keep = false;
        else if (type == WAY_TYPE_BASIC || always_safelisted_response(h.name, h.name_len) || expose_all)
            keep = true;
        else
            keep = names_has(&exposed, h.name, h.name_len, true);
        if (!keep)
            continue;
        size_t line = h.name_len + 2 + h.value_len + 2;
        if (used + line + 1 > cap) {
            names_free(&exposed);
            return SIZE_MAX;
        }
        os64_memcpy(out + used, h.name, h.name_len);
        used += h.name_len;
        out[used++] = ':';
        out[used++] = ' ';
        os64_memcpy(out + used, h.value, h.value_len);
        used += h.value_len;
        out[used++] = '\r';
        out[used++] = '\n';
        out[used] = '\0';
    }
    names_free(&exposed);
    return used;
}

bool way_response_has_body(const way_request_t *r, way_type_t type, int32_t status)
{
    if (type != WAY_TYPE_BASIC && type != WAY_TYPE_CORS && type != WAY_TYPE_DEFAULT)
        return false;
    if (r != NULL && (os64_streq(r->method, "HEAD") || os64_streq(r->method, "CONNECT")))
        return false;
    return status != 101 && status != 103 && status != 204 && status != 205 && status != 304;
}
