// test_way_origin_host.c — libway's origins and CORS (way/origin.h) on the
// host, under the sanitizers: one case per sentence of the Fetch Standard
// that origin.c applies (MODERN_WEB.md § Slice 2's origins and CORS; the
// numbers in the names are that design's host-case list), then every
// allocation failed in turn. origin.c is pure, so nothing here dials.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "way/origin.h"

// ── Allocation, and failing it on purpose ───────────────────────────────

static size_t allocations, fail_at, live;

void *os64_malloc(size_t size)
{
    allocations++;
    if (fail_at != 0 && allocations >= fail_at)
        return NULL;
    void *at = malloc(size != 0 ? size : 1);
    if (at != NULL)
        live++;
    return at;
}

void *os64_calloc(size_t count, size_t size)
{
    void *at = os64_malloc(count * size);
    if (at != NULL)
        memset(at, 0, count * size);
    return at;
}

void os64_free(void *ptr)
{
    if (ptr != NULL)
        live--;
    free(ptr);
}

// The cache's lock yields under contention; one thread never contends.
void os64_yield(void)
{
}

// libos64's printf family is linked for os64_snprintf; nothing here prints
// through it.
int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    (void)handle;
    (void)buf;
    return (int64_t)len;
}

static int checks, failures;

static void expect(const char *name, bool ok, const char *detail)
{
    checks++;
    if (!ok) {
        failures++;
        fprintf(stderr, "FAIL %s%s%s\n", name, detail != NULL ? ": " : "", detail != NULL ? detail : "");
    }
}

static void expect_text(const char *name, const char *got, const char *want)
{
    char detail[600];
    snprintf(detail, sizeof(detail), "got \"%s\", want \"%s\"", got, want);
    expect(name, strcmp(got, want) == 0, detail);
}

// ── Requests and responses ──────────────────────────────────────────────

// A request from the page at `page` (its origin an opaque one, identity 7,
// when it is not a tuple), for `url`, with author headers `headers`.
typedef struct {
    way_request_t r;
    char headers[2048];
} Req;

static void req(Req *q, const char *page, way_mode_t mode, way_credentials_t cred, const char *method,
                const char *headers, const char *url)
{
    memset(q, 0, sizeof(*q));
    way_origin_of(page, 7, &q->r.origin);
    q->r.mode = mode;
    q->r.credentials = cred;
    snprintf(q->r.method, sizeof(q->r.method), "%s", method);
    way_method_normalize(q->r.method);
    snprintf(q->headers, sizeof(q->headers), "%s", headers != NULL ? headers : "");
    q->r.headers = q->headers;
    q->r.headers_len = strlen(q->headers);
    way_request_start(&q->r, url);
}

static way_response_t resp(int32_t status, const char *headers)
{
    way_response_t r = {status, headers, strlen(headers), NULL, 0};
    return r;
}

static way_response_t resp_omitting(int32_t status, const char *headers, const char *omitted)
{
    way_response_t r = {status, headers, strlen(headers), omitted, strlen(omitted)};
    return r;
}

static const char *serialized(const char *url)
{
    static char out[WAY_ORIGIN_TEXT_MAX];
    way_origin_t o;
    way_origin_of(url, 0, &o);
    if (!way_origin_serialize(&o, out, sizeof(out)))
        strcpy(out, "<did not fit>");
    return out;
}

static bool same(const char *a, const char *b)
{
    way_origin_t x, y;
    way_origin_of(a, 0, &x);
    way_origin_of(b, 0, &y);
    return way_origin_same(&x, &y);
}

static bool same_site(const char *a, const char *b)
{
    way_origin_t x, y;
    way_origin_of(a, 0, &x);
    way_origin_of(b, 0, &y);
    return way_origin_same_site(&x, &y);
}

static bool safelisted(const char *name, const char *value)
{
    return way_header_cors_safelisted(name, strlen(name), value, strlen(value));
}

static bool forbidden(const char *name, const char *value)
{
    return way_header_forbidden_request(name, strlen(name), value, strlen(value));
}

static const char *unsafe_names(const char *block)
{
    static char out[512];
    if (way_cors_unsafe_names(block, strlen(block), out, sizeof(out)) < 0)
        strcpy(out, "<did not fit>");
    return out;
}

static way_hop_t hop(Req *q, const char *url, way_refusal_t *why)
{
    return way_main_fetch(&q->r, url, why);
}

// ── Origins (HTML § origin, Fetch § 3.2's serialization) ───────────────

static void origins(void)
{
    expect_text("1 tuple, lowercased, default port dropped", serialized("http://Example.COM/x"), "http://example.com");
    way_origin_t o;
    way_origin_of("http://Example.COM/x", 0, &o);
    expect("1 effective port", !o.opaque && o.port == 80 && strcmp(o.host, "example.com") == 0, NULL);
    expect_text("2 https default port", serialized("https://a.test:443/"), "https://a.test");
    expect_text("3 explicit port kept", serialized("https://a.test:8443/"), "https://a.test:8443");
    expect("4 :80 and none are one origin", same("http://a.test:80", "http://a.test/"), NULL);
    expect("5 scheme differs", !same("http://a.test", "https://a.test"), NULL);
    expect("6 port differs", !same("http://a.test", "http://a.test:8080"), NULL);
    expect("7 host differs", !same("http://a.test", "http://b.a.test"), NULL);
    expect_text("8 file is opaque", serialized("file:///etc/x"), "null");
    expect_text("9 about:blank opaque", serialized("about:blank"), "null");
    expect_text("9 data: opaque", serialized("data:,x"), "null");
    expect_text("9 javascript: opaque", serialized("javascript:1"), "null");
    expect_text("10 unparseable is opaque", serialized("http://a b/"), "null");
    way_origin_t p, q;
    way_origin_of("file:///x", 0, &p);
    way_origin_of("file:///x", 0, &q);
    expect("11 identity 0 is equal to nothing", !way_origin_same(&p, &q) && !way_origin_same(&p, &p), NULL);
    way_origin_of("file:///x", 9, &p);
    way_origin_of("file:///y", 9, &q);
    expect("11 an opaque origin is itself", way_origin_same(&p, &p) && way_origin_same(&p, &q), NULL);
    way_origin_of("file:///y", 10, &q);
    expect("11 two opaque origins differ", !way_origin_same(&p, &q), NULL);
    expect_text("12 blob: has its inner origin", serialized("blob:http://a.test/uuid"), "http://a.test");
    expect_text("12 blob: of file is opaque", serialized("blob:file:///x"), "null");
    expect_text("13 ws and ftp are tuples", serialized("WS://h.test:80/"), "ws://h.test");
    expect_text("13 ftp port", serialized("ftp://h.test:2121/"), "ftp://h.test:2121");
    expect_text("13 IP literal", serialized("http://10.0.0.1:8080/x"), "http://10.0.0.1:8080");
    expect("14 same site ignores ports", same_site("http://a.test", "http://a.test:81"), NULL);
    expect("14 same site keeps the scheme", !same_site("http://a.test", "https://a.test"), NULL);
    expect("14 subdomains are two sites until the PSL", !same_site("http://a.test", "http://b.a.test"), NULL);
    char small[8];
    way_origin_of("https://long.example.test", 0, &o);
    expect("serialize refuses a short buffer", !way_origin_serialize(&o, small, sizeof(small)), NULL);
}

// ── Methods and request headers (§ 2.2.1, § 2.2.2) ─────────────────────

static void methods_and_headers(void)
{
    expect("15 GET HEAD POST", way_method_safelisted("GET") && way_method_safelisted("HEAD") &&
                               way_method_safelisted("POST"), NULL);
    char m[WAY_METHOD_MAX] = "get";
    expect("15 get is not, before normalizing", !way_method_safelisted(m), NULL);
    way_method_normalize(m);
    expect_text("15 get normalizes", m, "GET");
    expect("16 PUT DELETE PATCH patch OPTIONS CHICKEN", !way_method_safelisted("PUT") &&
           !way_method_safelisted("DELETE") && !way_method_safelisted("PATCH") &&
           !way_method_safelisted("patch") && !way_method_safelisted("OPTIONS") &&
           !way_method_safelisted("CHICKEN"), NULL);
    const char *normal[][2] = {{"delete", "DELETE"}, {"Get", "GET"}, {"head", "HEAD"}, {"options", "OPTIONS"},
                               {"post", "POST"}, {"pUt", "PUT"}, {"patch", "patch"}, {"Egg", "Egg"}};
    for (size_t i = 0; i < sizeof(normal) / sizeof(normal[0]); i++) {
        snprintf(m, sizeof(m), "%s", normal[i][0]);
        way_method_normalize(m);
        expect_text("17 normalize", m, normal[i][1]);
    }
    expect("18 forbidden methods", way_method_forbidden("CONNECT", 7) && way_method_forbidden("trace", 5) &&
           way_method_forbidden("Track", 5) && !way_method_forbidden("PATCH", 5), NULL);
    expect("method is a token", way_method_ok("PATCH", 5) && !way_method_ok("GE T", 4) && !way_method_ok("", 0), NULL);
    const char *unsafe = "\"():<>?@[\\]{}\x7f\x01";
    for (const char *u = unsafe; *u; u++) {
        char v[16];
        snprintf(v, sizeof(v), "a%cb", *u);
        expect("19 an unsafe byte in Accept", !safelisted("Accept", v), v);
    }
    expect("19 a tab is safe", safelisted("Accept", "a\tb"), NULL);
    expect("20 Accept json", safelisted("Accept", "application/json"), NULL);
    char long_value[200];
    memset(long_value, 'a', 128);
    long_value[128] = '\0';
    expect("20 128 bytes", safelisted("Accept", long_value), NULL);
    long_value[128] = 'a';
    long_value[129] = '\0';
    expect("20 129 bytes", !safelisted("Accept", long_value), NULL);
    expect("21 Accept-Language", safelisted("Accept-Language", "en-US,en;q=0.9"), NULL);
    expect("21 underscore", !safelisted("accept-language", "en_US"), NULL);
    expect("22 Content-Language", safelisted("Content-Language", "de-DE"), NULL);
    expect("22 slash", !safelisted("Content-Language", "de/DE"), NULL);
    expect("23 text/plain", safelisted("Content-Type", "text/plain"), NULL);
    expect("23 urlencoded", safelisted("Content-Type", "application/x-www-form-urlencoded"), NULL);
    expect("23 multipart", safelisted("content-type", "multipart/form-data; boundary=x"), NULL);
    expect("24 case folded essence", safelisted("Content-Type", "TEXT/PLAIN; charset=UTF-8"), NULL);
    expect("25 json", !safelisted("Content-Type", "application/json"), NULL);
    expect("25 an unsafe byte in a parameter", !safelisted("Content-Type", "text/plain; a=\"x\""), NULL);
    expect("26 two types in one value", !safelisted("Content-Type", "text/plain, application/json"), NULL);
    expect("27 no subtype", !safelisted("Content-Type", "text"), NULL);
    expect("27 empty subtype", !safelisted("Content-Type", "text/"), NULL);
    expect("28 bytes=0-", safelisted("Range", "bytes=0-"), NULL);
    expect("28 bytes=5-10", safelisted("range", "bytes=5-10"), NULL);
    expect("28 bytes=-500", !safelisted("Range", "bytes=-500"), NULL);
    expect("28 two ranges", !safelisted("Range", "bytes=0-1,5-6"), NULL);
    expect("28 white space", !safelisted("Range", "bytes = 0-"), NULL);
    expect("28 start past end", !safelisted("Range", "bytes=10-5"), NULL);
    expect("28 long numbers compared as numbers", safelisted("Range", "bytes=0009-10") &&
           !safelisted("Range", "bytes=100000000000000000000-99999999999999999999"), NULL);
    expect("28 Bytes is not bytes", !safelisted("Range", "Bytes=0-"), NULL);
    expect("29 others", !safelisted("X-Requested-With", "x") && !safelisted("Authorization", "x") &&
           !safelisted("Cache-Control", "no-cache"), NULL);
    expect_text("30 sorted lowercase set", unsafe_names("X-B: 1\r\nx-a: 2\r\nX-A: 3\r\nAccept: */*\r\n"), "x-a,x-b");
    expect_text("31 judged per entry",
                unsafe_names("Content-Type: application/json\r\nContent-Type: text/plain\r\n"), "content-type");
    char block[4096];
    // Eight safelisted Accept-Language values of 128 bytes: 1024, then 1025.
    size_t used = 0;
    for (int i = 0; i < 8; i++) {
        used += (size_t)snprintf(block + used, sizeof(block) - used, "Accept-Language: ");
        memset(block + used, 'a', 128);
        used += 128;
        used += (size_t)snprintf(block + used, sizeof(block) - used, "\r\n");
    }
    expect_text("32 1024 bytes of safelisted values", unsafe_names(block), "");
    snprintf(block + used, sizeof(block) - used, "Accept: a\r\n");
    expect_text("32 1025 bytes make them all unsafe", unsafe_names(block), "accept,accept-language");
    char tiny[4];
    expect("unsafe names refuse a short buffer",
           way_cors_unsafe_names("X-Long-Name: 1\r\n", 16, tiny, sizeof(tiny)) == -1, NULL);
    expect("unsafe names count with no buffer", way_cors_unsafe_names("X-A: 1\r\nX-B: 2\r\n", 16, NULL, 0) == 2, NULL);
    expect("33 Authorization", way_header_non_wildcard("Authorization", 13) &&
           way_header_non_wildcard("authorization", 13) && !way_header_non_wildcard("X-Auth", 6), NULL);
    expect("34 no-cors safelisted", way_header_no_cors_safelisted("Accept", 6, "*/*", 3) &&
           !way_header_no_cors_safelisted("Content-Type", 12, "application/json", 16) &&
           !way_header_no_cors_safelisted("X-Foo", 5, "1", 1) &&
           !way_header_no_cors_safelisted("Range", 5, "bytes=0-", 8) &&
           way_header_privileged_no_cors("Range", 5), NULL);
    const char *names[] = {"Accept-Charset", "Accept-Encoding", "Access-Control-Request-Headers",
                           "Access-Control-Request-Method", "Connection", "Content-Length", "Cookie",
                           "Cookie2", "Date", "DNT", "Expect", "Host", "Keep-Alive", "Origin", "Referer",
                           "Set-Cookie", "TE", "Trailer", "Transfer-Encoding", "Upgrade", "Via"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char up[64];
        snprintf(up, sizeof(up), "%s", names[i]);
        for (char *c = up; *c; c++)
            *c = (char)((*c >= 'a' && *c <= 'z') ? *c - 32 : *c);
        expect("35 forbidden name", forbidden(names[i], "x") && forbidden(up, "x"), names[i]);
    }
    expect("35 proxy- and sec-", forbidden("Proxy-Foo", "x") && forbidden("sec-foo", "x"), NULL);
    expect("35 not prefixes alone", !forbidden("Proxy", "x") && !forbidden("Sector", "x"), NULL);
    expect("36 override GET", !forbidden("X-HTTP-Method-Override", "GET"), NULL);
    expect("36 override GET, TRACE", forbidden("X-HTTP-Method-Override", "GET, TRACE"), NULL);
    expect("36 X-HTTP-Method connect", forbidden("x-http-method", "connect"), NULL);
    expect("36 a quoted method is its quotes too", !forbidden("X-Method-Override", "\"CONNECT\""), NULL);
    expect("36 a comma inside quotes", !forbidden("X-Method-Override", "\"a,TRACE\""), NULL);
    expect("37 User-Agent (stated divergence)", forbidden("User-Agent", "x"), NULL);
    expect("38 forbidden response names", way_header_forbidden_response("Set-Cookie", 10) &&
           way_header_forbidden_response("set-cookie2", 11) && !way_header_forbidden_response("Set-Cookie3", 11), NULL);
    expect("request-body names", way_header_request_body("Content-Type", 12) &&
           way_header_request_body("content-encoding", 16) && way_header_request_body("Content-Location", 16) &&
           way_header_request_body("Content-Language", 16) && !way_header_request_body("Content-Length", 14), NULL);
    expect("names are tokens", way_header_name_ok("X-Foo", 5) && !way_header_name_ok("X Foo", 5) &&
           !way_header_name_ok("", 0) && !way_header_name_ok("X:", 2), NULL);
    expect("values", way_header_value_ok("a b", 3) && way_header_value_ok("", 0) &&
           !way_header_value_ok(" a", 2) && !way_header_value_ok("a\t", 2) && !way_header_value_ok("a\rb", 3) &&
           !way_header_value_ok("a\nb", 3), NULL);
    const char *v = " \t\r\nx y\n ";
    size_t vl = strlen(v);
    way_header_value_normalize(&v, &vl);
    expect("normalize takes HTTP white space off both ends", vl == 3 && memcmp(v, "x y", 3) == 0, NULL);
    size_t at = 0;
    way_header_t h;
    const char *bad[] = {"X-Foo:1\r\n", "X-Foo: 1\n", "X-Foo: 1", ": 1\r\n", "X-Foo: 1\r"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        at = 0;
        expect("a malformed block line stops the walk", !way_header_next(bad[i], strlen(bad[i]), &at, &h), bad[i]);
    }
    at = 0;
    expect("an empty value", way_header_next("A: \r\n", 5, &at, &h) && h.value_len == 0 && at == 5, NULL);
}

static void mime(void)
{
    char e[WAY_MIME_ESSENCE_MAX], c[WAY_MIME_CHARSET_MAX];
    expect("mime: type and charset", way_mime_parse(" Text/HTML ; Charset=\"UTF-8\" ", 30, e, sizeof(e), c, sizeof(c)) &&
           strcmp(e, "text/html") == 0 && strcmp(c, "UTF-8") == 0, e);
    expect("mime: the first charset wins", way_mime_parse("a/b;charset=x;charset=y", 23, e, sizeof(e), c, sizeof(c)) &&
           strcmp(c, "x") == 0, c);
    const char *twice = "a/b;charset=\"\x01\";charset=y";
    expect("mime: an invalid charset is skipped", way_mime_parse(twice, strlen(twice), e, sizeof(e), c, sizeof(c)) &&
           strcmp(c, "y") == 0, c);
    expect("mime: no slash", !way_mime_parse("text", 4, e, sizeof(e), c, sizeof(c)), NULL);
    expect("mime: not a token", !way_mime_parse("te xt/plain", 11, e, sizeof(e), NULL, 0), NULL);
    expect("mime: an escape in a quoted value", way_mime_parse("a/b;charset=\"u\\tf\"", 18, e, sizeof(e), c, sizeof(c)) &&
           strcmp(c, "utf") == 0, c);
    char tiny[4];
    expect("mime: an essence too long for the buffer", !way_mime_parse("text/plain", 10, tiny, sizeof(tiny), NULL, 0), NULL);
}

static void response_names(void)
{
    // 39: the CORS-safelisted response-header names, through the filter.
    Req q;
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", "http://b.test/");
    way_refusal_t why;
    hop(&q, "http://b.test/", &why);
    char out[1024];
    way_response_t r = resp(200, "Cache-Control: a\r\nContent-Language: b\r\nContent-Length: 1\r\n"
                                 "Content-Type: c\r\nExpires: d\r\nLast-Modified: e\r\nPragma: f\r\nX-Foo: g\r\n");
    way_response_headers(&q.r, WAY_TYPE_CORS, &r, out, sizeof(out));
    expect_text("39 the seven, not X-Foo", out, "Cache-Control: a\r\nContent-Language: b\r\nContent-Length: 1\r\n"
                                               "Content-Type: c\r\nExpires: d\r\nLast-Modified: e\r\nPragma: f\r\n");
}

// ── Main fetch (§ 4.1, § 2.9) ───────────────────────────────────────────

static void main_fetch(void)
{
    Req q;
    way_refusal_t why;
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "PUT", "X-Foo: 1\r\n", "http://a.test/x");
    expect("40 same origin is basic", hop(&q, "http://a.test/x", &why) == WAY_HOP_BASIC &&
           !way_preflight_capable(&q.r), NULL);
    q.r.tainting = WAY_TAINT_CORS;
    expect("41 never back to basic", hop(&q, "http://a.test/x", &why) == WAY_HOP_CORS, NULL);
    req(&q, "http://a.test/", WAY_MODE_SAME_ORIGIN, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", "data:,x");
    expect("42 data: is basic, any mode", hop(&q, "data:,x", &why) == WAY_HOP_DATA && q.r.tainting == WAY_TAINT_BASIC, NULL);
    req(&q, "http://a.test/", WAY_MODE_SAME_ORIGIN, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", "http://b.test/");
    expect("43 same-origin mode refuses", hop(&q, "http://b.test/", &why) == WAY_HOP_REFUSED &&
           why == WAY_REFUSE_SAME_ORIGIN, NULL);
    req(&q, "http://a.test/", WAY_MODE_NO_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", "http://b.test/");
    expect("44 no-cors is opaque", hop(&q, "http://b.test/", &why) == WAY_HOP_OPAQUE && q.r.tainting == WAY_TAINT_OPAQUE, NULL);
    for (int i = 0; i < 2; i++) {
        req(&q, "http://a.test/", WAY_MODE_NO_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", "http://b.test/");
        q.r.redirect = i == 0 ? WAY_REDIRECT_MANUAL : WAY_REDIRECT_ERROR;
        expect("45 no-cors with manual or error", hop(&q, "http://b.test/", &why) == WAY_HOP_REFUSED &&
               why == WAY_REFUSE_NO_CORS, NULL);
    }
    const char *schemes[] = {"file:///x", "about:blank", "blob:http://a.test/u", "ftp://b.test/"};
    for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++) {
        req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", schemes[i]);
        expect("46 other schemes refuse", hop(&q, schemes[i], &why) == WAY_HOP_REFUSED && why == WAY_REFUSE_SCHEME, schemes[i]);
    }
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", "http://b.test/");
    expect("47 a simple GET", hop(&q, "http://b.test/", &why) == WAY_HOP_CORS && !way_preflight_capable(&q.r), NULL);
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "PUT", "", "http://b.test/");
    hop(&q, "http://b.test/", &why);
    expect("48 PUT preflights", way_preflight_capable(&q.r), NULL);
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "X-Foo: 1\r\n", "http://b.test/");
    hop(&q, "http://b.test/", &why);
    expect("49 X-Foo preflights", way_preflight_capable(&q.r), NULL);
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "Accept: text/html\r\n", "http://b.test/");
    hop(&q, "http://b.test/", &why);
    expect("49 Accept does not", !way_preflight_capable(&q.r), NULL);
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "POST", "Content-Type: application/json\r\n", "http://b.test/");
    hop(&q, "http://b.test/", &why);
    expect("50 a JSON POST preflights", way_preflight_capable(&q.r), NULL);
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "POST", "Content-Type: text/plain\r\n", "http://b.test/");
    hop(&q, "http://b.test/", &why);
    expect("50 a text POST does not", !way_preflight_capable(&q.r), NULL);
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", "http://b.test/");
    q.r.preflight_flag = true;
    hop(&q, "http://b.test/", &why);
    expect("51 the flag preflights", way_preflight_capable(&q.r), NULL);
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", "http://a.test/");
    q.r.preflight_flag = true;
    hop(&q, "http://a.test/", &why);
    expect("51 not same-origin", !way_preflight_capable(&q.r), NULL);
    const uint16_t bad[] = {0, 1, 22, 25, 6000, 10080, 6667};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char url[64];
        snprintf(url, sizeof(url), "http://b.test:%u/", bad[i]);
        req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", url);
        way_hop_t got = hop(&q, url, &why);
        // Port 0 does not parse: it is refused as an unreadable address.
        expect("52 a bad port", got == WAY_HOP_REFUSED && (why == WAY_REFUSE_PORT || (bad[i] == 0 && why == WAY_REFUSE_URL)), url);
    }
    expect("52 good ports", !way_port_bad("http://b.test:8080/") && !way_port_bad("https://b.test/") &&
           !way_port_bad("http://b.test/") && way_port_bad("https://b.test:25/") && !way_port_bad("data:,x") &&
           way_port_bad("http://b.test:99999/"), NULL);
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", "http://user@b.test/");
    expect("an address os64 cannot read", hop(&q, "http://user@b.test/", &why) == WAY_HOP_REFUSED &&
           why == WAY_REFUSE_URL, NULL);
    // 53: a same-origin PUT redirected cross-origin preflights on hop 2.
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "PUT", "", "http://a.test/x");
    hop(&q, "http://a.test/x", &why);
    bool drop;
    way_redirect_follow(&q.r, 307, "http://a.test/x", "http://b.test/y", &drop);
    expect("53 hop 2 preflights", hop(&q, "http://b.test/y", &why) == WAY_HOP_CORS && way_preflight_capable(&q.r), NULL);
    // 54: a cross-origin PUT turned into a GET by a 303 needs none.
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "PUT", "Content-Type: application/json\r\n", "http://b.test/x");
    q.r.body = true;
    hop(&q, "http://b.test/x", &why);
    way_redirect_follow(&q.r, 303, "http://b.test/x", "http://b.test/y", &drop);
    hop(&q, "http://b.test/y", &why);
    expect("54 after 303, none", strcmp(q.r.method, "GET") == 0 && drop && !way_preflight_capable(&q.r), NULL);
    expect("refusal names", strcmp(way_refusal_name(WAY_REFUSE_MIXED), "mixed content") == 0, NULL);
}

// ── The preflight (§ 4.8) ───────────────────────────────────────────────

static Req cors_request(const char *method, const char *headers, way_credentials_t cred)
{
    Req q;
    way_refusal_t why;
    req(&q, "https://a.test/", WAY_MODE_CORS, cred, method, headers, "https://b.test/r");
    hop(&q, "https://b.test/r", &why);
    q.r.headers = q.headers;            // the copy returned moves the buffer
    return q;
}

static bool judge(Req *q, int32_t status, const char *headers)
{
    q->r.headers = q->headers;
    way_response_t r = resp(status, headers);
    uint32_t age;
    return way_preflight_judge(&q->r, &r, &age);
}

static void preflight(void)
{
    Req q = cors_request("PUT", "", WAY_CREDENTIALS_SAME_ORIGIN);
    char lines[512];
    expect("55 method line", way_preflight_lines(&q.r, lines, sizeof(lines)) &&
           strcmp(lines, "Access-Control-Request-Method: PUT\r\n") == 0, lines);
    q = cors_request("PUT", "X-B: 1\r\nx-a: 2\r\nAccept: */*\r\n", WAY_CREDENTIALS_SAME_ORIGIN);
    q.r.headers = q.headers;
    expect("56 headers line", way_preflight_lines(&q.r, lines, sizeof(lines)) &&
           strcmp(lines, "Access-Control-Request-Method: PUT\r\nAccess-Control-Request-Headers: x-a,x-b\r\n") == 0, lines);
    char tiny[40];
    expect("preflight lines refuse a short buffer", !way_preflight_lines(&q.r, tiny, sizeof(tiny)), NULL);
    q = cors_request("PUT", "Accept: */*\r\n", WAY_CREDENTIALS_SAME_ORIGIN);
    q.r.headers = q.headers;
    expect("57 no headers line", way_preflight_lines(&q.r, lines, sizeof(lines)) &&
           strstr(lines, "Request-Headers") == NULL, lines);
    // 58: the preflight's Origin; its credentials are the job's (§ 4.6 says
    // never), proved in the scripted-page host.
    char origin[WAY_ORIGIN_TEXT_MAX];
    expect("58 Origin", way_origin_header(&q.r, "https://b.test/r", origin, sizeof(origin)) &&
           strcmp(origin, "https://a.test") == 0, origin);
    q = cors_request("PUT", "", WAY_CREDENTIALS_SAME_ORIGIN);
    const char *good = "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n";
    expect("59 200", judge(&q, 200, good), NULL);
    expect("59 204", judge(&q, 204, good), NULL);
    expect("59 299", judge(&q, 299, good), NULL);
    expect("60 300 301 404 500", !judge(&q, 300, good) && !judge(&q, 301, good) && !judge(&q, 404, good) &&
           !judge(&q, 500, good), NULL);
    expect("61 no ACAO", !judge(&q, 200, "Access-Control-Allow-Methods: PUT\r\n"), NULL);
    const char *star = "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: PUT\r\n";
    expect("61 * without credentials", judge(&q, 200, star), NULL);
    Req qi = cors_request("PUT", "", WAY_CREDENTIALS_INCLUDE);
    expect("61 * with include", !judge(&qi, 200, "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Credentials: true\r\n"
                                                 "Access-Control-Allow-Methods: PUT\r\n"), NULL);
    expect("62 include needs ACAC", !judge(&qi, 200, good), NULL);
    expect("62 True is not true", !judge(&qi, 200, "Access-Control-Allow-Origin: https://a.test\r\n"
                                                   "Access-Control-Allow-Credentials: True\r\nAccess-Control-Allow-Methods: PUT\r\n"), NULL);
    expect("62 true", judge(&qi, 200, "Access-Control-Allow-Origin: https://a.test\r\n"
                                      "Access-Control-Allow-Credentials: true\r\nAccess-Control-Allow-Methods: PUT\r\n"), NULL);
    expect("63 no ACAM, PUT", !judge(&q, 200, "Access-Control-Allow-Origin: https://a.test\r\n"), NULL);
    Req qp = cors_request("POST", "X-Foo: 1\r\n", WAY_CREDENTIALS_SAME_ORIGIN);
    expect("63 no ACAM, POST passes the method", judge(&qp, 200, "Access-Control-Allow-Origin: https://a.test\r\n"
                                                                 "Access-Control-Allow-Headers: x-foo\r\n"), NULL);
    expect("64 methods are case-sensitive", !judge(&q, 200, "Access-Control-Allow-Origin: https://a.test\r\n"
                                                            "Access-Control-Allow-Methods: put\r\n"), NULL);
    Req qd = cors_request("DELETE", "", WAY_CREDENTIALS_SAME_ORIGIN);
    expect("65 * methods", judge(&qd, 200, "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: *\r\n"), NULL);
    Req qdi = cors_request("DELETE", "", WAY_CREDENTIALS_INCLUDE);
    expect("65 * methods not under include", !judge(&qdi, 200, "Access-Control-Allow-Origin: https://a.test\r\n"
                                                               "Access-Control-Allow-Credentials: true\r\nAccess-Control-Allow-Methods: *\r\n"), NULL);
    expect("66 list with OWS and empties", judge(&q, 200, "Access-Control-Allow-Origin: https://a.test\r\n"
                                                          "Access-Control-Allow-Methods: GET,, PUT ,\r\n"), NULL);
    expect("66 across two lines", judge(&q, 200, "Access-Control-Allow-Origin: https://a.test\r\n"
                                                 "Access-Control-Allow-Methods: GET\r\nAccess-Control-Allow-Methods: PUT\r\n"), NULL);
    expect("67 not a token", !judge(&q, 200, "Access-Control-Allow-Origin: https://a.test\r\n"
                                             "Access-Control-Allow-Methods: PU T\r\n"), NULL);
    Req qf = cors_request("GET", "", WAY_CREDENTIALS_SAME_ORIGIN);
    qf.r.preflight_flag = true;
    expect("68 flag with no ACAM", judge(&qf, 200, "Access-Control-Allow-Origin: https://a.test\r\n"), NULL);
    Req qh = cors_request("GET", "X-Foo: 1\r\n", WAY_CREDENTIALS_SAME_ORIGIN);
    expect("69 named", judge(&qh, 200, "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Headers: x-foo\r\n"), NULL);
    expect("69 absent", !judge(&qh, 200, "Access-Control-Allow-Origin: https://a.test\r\n"), NULL);
    expect("70 *", judge(&qh, 200, "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Headers: *\r\n"), NULL);
    Req qhi = cors_request("GET", "X-Foo: 1\r\n", WAY_CREDENTIALS_INCLUDE);
    expect("70 * under include", !judge(&qhi, 200, "Access-Control-Allow-Origin: https://a.test\r\n"
                                                   "Access-Control-Allow-Credentials: true\r\nAccess-Control-Allow-Headers: *\r\n"), NULL);
    Req qa = cors_request("GET", "Authorization: x\r\n", WAY_CREDENTIALS_SAME_ORIGIN);
    expect("71 * is not Authorization", !judge(&qa, 200, "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Headers: *\r\n"), NULL);
    expect("71 named beside *", judge(&qa, 200, "Access-Control-Allow-Origin: https://a.test\r\n"
                                                "Access-Control-Allow-Headers: *, authorization\r\n"), NULL);
    expect("71 case-insensitive", judge(&qa, 200, "Access-Control-Allow-Origin: https://a.test\r\n"
                                                  "Access-Control-Allow-Headers: AUTHORIZATION\r\n"), NULL);
    Req qb = cors_request("GET", "X-Foo: 1\r\nX-Baz: 2\r\n", WAY_CREDENTIALS_SAME_ORIGIN);
    expect("72 one unnamed", !judge(&qb, 200, "Access-Control-Allow-Origin: https://a.test\r\n"
                                              "Access-Control-Allow-Headers: x-foo, x-bar\r\n"), NULL);
    expect("73 not a field-name", !judge(&qh, 200, "Access-Control-Allow-Origin: https://a.test\r\n"
                                                   "Access-Control-Allow-Headers: x foo\r\n"), NULL);
    const char *ok = "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n";
    q.r.headers = q.headers;
    way_response_t r = resp_omitting(200, ok, "access-control-allow-origin\r\n");
    uint32_t age;
    expect("75 ACAO unreadable", !way_preflight_judge(&q.r, &r, &age), NULL);
    r = resp_omitting(200, ok, "Access-Control-Allow-Methods\r\n");
    expect("75 ACAM unreadable", !way_preflight_judge(&q.r, &r, &age), NULL);
    qh.r.headers = qh.headers;
    r = resp_omitting(200, "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Headers: x-foo\r\n",
                      "access-control-allow-headers\r\n");
    expect("75 ACAH unreadable", !way_preflight_judge(&qh.r, &r, &age), NULL);
}

// ── The preflight cache (§ 4.9) ─────────────────────────────────────────

static bool needed(way_preflight_t *c, Req *q, const char *url, uint64_t now)
{
    q->r.headers = q->headers;
    return way_preflight_needed(c, &q->r, url, now);
}

static void store(way_preflight_t *c, Req *q, const char *url, const char *headers, uint64_t now)
{
    q->r.headers = q->headers;
    way_response_t r = resp(200, headers);
    uint32_t age;
    if (way_preflight_judge(&q->r, &r, &age))
        way_preflight_store(c, &q->r, url, &r, age, now);
}

static void cache(void)
{
    const char *url = "https://b.test/r";
    way_preflight_t *c = way_preflight_new();
    Req q = cors_request("PUT", "", WAY_CREDENTIALS_SAME_ORIGIN);
    expect("a NULL cache always wants one", way_preflight_needed(NULL, &q.r, url, 0), NULL);
    store(c, &q, url, "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n", 1000);
    expect("76 5 s by default: hit at 4.999", !needed(c, &q, url, 5999), NULL);
    expect("76 miss at 5", needed(c, &q, url, 6000), NULL);
    expect("76 expired entries go", way_preflight_count(c) == 0, NULL);
    store(c, &q, url, "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n"
                      "Access-Control-Max-Age: 600\r\n", 0);
    expect("77 600 s", !needed(c, &q, url, 599999) && needed(c, &q, url, 600000), NULL);
    store(c, &q, url, "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n"
                      "Access-Control-Max-Age: 100000\r\n", 0);
    expect("78 capped at 7200 s", !needed(c, &q, url, 7199999) && needed(c, &q, url, 7200000), NULL);
    store(c, &q, url, "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n"
                      "Access-Control-Max-Age: 0\r\n", 0);
    expect("79 0 keeps nothing", way_preflight_count(c) == 0, NULL);
    const char *ages[] = {"Access-Control-Max-Age: -1\r\n", "Access-Control-Max-Age: 1.5\r\n",
                          "Access-Control-Max-Age: abc\r\n", "Access-Control-Max-Age: 600\r\nAccess-Control-Max-Age: 600\r\n"};
    for (size_t i = 0; i < sizeof(ages) / sizeof(ages[0]); i++) {
        char h[512];
        snprintf(h, sizeof(h), "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n%s", ages[i]);
        store(c, &q, url, h, 1000000);
        expect("80 failure is 5 s", !needed(c, &q, url, 1004999) && needed(c, &q, url, 1005000), ages[i]);
    }
    q.r.headers = q.headers;
    way_response_t r = resp_omitting(200, "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n"
                                          "Access-Control-Max-Age: 600\r\n", "access-control-max-age\r\n");
    uint32_t age;
    expect("an unreadable Max-Age keeps nothing", way_preflight_judge(&q.r, &r, &age) && age == 0, NULL);
    // 81: the key.
    store(c, &q, url, "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n"
                      "Access-Control-Max-Age: 600\r\n", 2000000);
    Req other;
    way_refusal_t why;
    req(&other, "https://c.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "PUT", "", url);
    hop(&other, url, &why);
    expect("81 another origin", needed(c, &other, url, 2000001), NULL);
    expect("81 another URL", needed(c, &q, "https://b.test/s", 2000001), NULL);
    expect("81 another query", needed(c, &q, "https://b.test/r?b", 2000001), NULL);
    expect("81 the same", !needed(c, &q, url, 2000001), NULL);
    Req qi = cors_request("PUT", "", WAY_CREDENTIALS_INCLUDE);
    expect("82 uncredentialed does not answer include", needed(c, &qi, url, 2000001), NULL);
    Req qp = cors_request("PATCH", "", WAY_CREDENTIALS_SAME_ORIGIN);
    Req qpi = cors_request("PATCH", "", WAY_CREDENTIALS_INCLUDE);
    store(c, &qpi, url, "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Credentials: true\r\n"
                        "Access-Control-Allow-Methods: PATCH\r\nAccess-Control-Max-Age: 600\r\n", 2000000);
    expect("82 credentialed answers both", !needed(c, &qp, url, 2000001) && !needed(c, &qpi, url, 2000001), NULL);
    // 83: a redirect-tainted request's key is "null".
    Req qt = cors_request("PUT", "", WAY_CREDENTIALS_SAME_ORIGIN);
    qt.r.rtaint = WAY_RTAINT_CROSS_SITE;
    expect("83 null-origin does not take the untainted one", needed(c, &qt, url, 2000001), NULL);
    store(c, &qt, url, "Access-Control-Allow-Origin: null\r\nAccess-Control-Allow-Methods: DELETE\r\n"
                       "Access-Control-Max-Age: 600\r\n", 2000000);
    Req qdel = cors_request("DELETE", "", WAY_CREDENTIALS_SAME_ORIGIN);
    expect("83 nor the reverse", needed(c, &qdel, url, 2000001), NULL);
    way_preflight_free(c);

    c = way_preflight_new();
    Req qs = cors_request("PUT", "", WAY_CREDENTIALS_INCLUDE);
    store(c, &qs, url, "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Credentials: true\r\n"
                       "Access-Control-Allow-Methods: PUT, *\r\nAccess-Control-Max-Age: 600\r\n", 0);
    Req qx = cors_request("DELETE", "", WAY_CREDENTIALS_INCLUDE);
    Req qy = cors_request("DELETE", "", WAY_CREDENTIALS_SAME_ORIGIN);
    expect("84 a * never answers include (stated tightening)", needed(c, &qx, url, 1), NULL);
    expect("84 a * answers the uncredentialed", !needed(c, &qy, url, 1), NULL);
    Req qh = cors_request("GET", "X-Foo: 1\r\n", WAY_CREDENTIALS_SAME_ORIGIN);
    store(c, &qh, url, "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Headers: *\r\n"
                       "Access-Control-Max-Age: 600\r\n", 0);
    Req qa = cors_request("GET", "Authorization: x\r\n", WAY_CREDENTIALS_SAME_ORIGIN);
    expect("85 header * answers X-Foo", !needed(c, &qh, url, 1), NULL);
    expect("85 not Authorization", needed(c, &qa, url, 1), NULL);
    Req ql = cors_request("put", "", WAY_CREDENTIALS_SAME_ORIGIN);
    snprintf(ql.r.method, sizeof(ql.r.method), "Put");
    way_preflight_free(c);
    c = way_preflight_new();
    store(c, &q, url, "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n"
                      "Access-Control-Max-Age: 600\r\n", 0);
    expect("86 methods byte-exact", needed(c, &ql, url, 1), NULL);
    Req q2 = cors_request("PUT", "X-A: 1\r\nX-B: 2\r\n", WAY_CREDENTIALS_SAME_ORIGIN);
    store(c, &q2, url, "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n"
                       "Access-Control-Allow-Headers: x-a, x-b\r\nAccess-Control-Max-Age: 600\r\n", 0);
    expect("87 all cached", !needed(c, &q2, url, 1), NULL);
    Req q3 = cors_request("PUT", "X-A: 1\r\nX-C: 2\r\n", WAY_CREDENTIALS_SAME_ORIGIN);
    expect("87 one uncached name", needed(c, &q3, url, 1), NULL);
    int32_t before = way_preflight_count(c);
    store(c, &q2, url, "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n"
                       "Access-Control-Allow-Headers: X-A, x-b\r\nAccess-Control-Max-Age: 900\r\n", 0);
    expect("88 updated, not duplicated", way_preflight_count(c) == before && !needed(c, &q2, url, 899000), NULL);
    q2.r.headers = q2.headers;
    way_preflight_clear(c, &q2.r, url);
    expect("74 clear", way_preflight_count(c) == 0, NULL);
    // 89: the cap.
    for (int i = 0; i < WAY_PREFLIGHT_ENTRIES_MAX; i++) {
        char u[64];
        snprintf(u, sizeof(u), "https://b.test/%d", i);
        char h[256];
        snprintf(h, sizeof(h), "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n"
                               "Access-Control-Max-Age: %d\r\n", i == 5 ? 10 : 1000);
        store(c, &q, u, h, 0);
    }
    expect("89 full", way_preflight_count(c) == WAY_PREFLIGHT_ENTRIES_MAX, NULL);
    store(c, &q, "https://b.test/new", "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n"
                                       "Access-Control-Max-Age: 1000\r\n", 1);
    expect("89 the soonest went", way_preflight_count(c) == WAY_PREFLIGHT_ENTRIES_MAX &&
           needed(c, &q, "https://b.test/5", 2) && !needed(c, &q, "https://b.test/new", 2) &&
           !needed(c, &q, "https://b.test/6", 2), NULL);
    store(c, &q, "https://b.test/newer", "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n"
                                         "Access-Control-Max-Age: 1000\r\n", 20000);
    expect("89 expired first", way_preflight_count(c) <= WAY_PREFLIGHT_ENTRIES_MAX &&
           !needed(c, &q, "https://b.test/newer", 20001), NULL);
    way_preflight_free(c);
    // 90: partitions.
    c = way_preflight_new();
    Req pa, pb;
    req(&pa, "https://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "PUT", "", url);
    hop(&pa, url, &why);
    req(&pb, "https://a.test:8443/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "PUT", "", url);
    hop(&pb, url, &why);
    store(c, &pa, url, "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n"
                       "Access-Control-Max-Age: 600\r\n", 0);
    expect("90 the other origin of one site misses on origin", needed(c, &pb, url, 1), NULL);
    // 91: the clock is the caller's; nothing here reads a wall clock.
    expect("91 an earlier clock is not an expiry", !needed(c, &pa, url, 0), NULL);
    way_preflight_free(c);
}

// ── Redirect-taint (§ 2.2.5) ────────────────────────────────────────────

static const char *chain(const char *const *urls, size_t n)
{
    static char out[WAY_ORIGIN_TEXT_MAX];
    Req q;
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", urls[0]);
    for (size_t i = 1; i < n; i++) {
        bool drop;
        way_redirect_follow(&q.r, 302, urls[i - 1], urls[i], &drop);
    }
    way_request_origin(&q.r, out, sizeof(out));
    return out;
}

static void redirect_taint(void)
{
    const char *a[] = {"http://a.test/"};
    expect_text("92 the page's own", chain(a, 1), "http://a.test");
    const char *b[] = {"http://b.test/"};
    expect_text("93 a plain cross-origin request", chain(b, 1), "http://a.test");
    const char *ab[] = {"http://a.test/", "http://b.test/"};
    expect_text("94 from the page's own origin", chain(ab, 2), "http://a.test");
    const char *bc[] = {"http://b.test/", "http://c.test/"};
    expect_text("95 cross-site", chain(bc, 2), "null");
    const char *ba[] = {"http://b.test/", "http://a.test/"};
    expect_text("96 back through another site", chain(ba, 2), "null");
    const char *bb[] = {"http://b.test/", "http://b.test:81/"};
    expect_text("97 same-site taint", chain(bb, 2), "null");
    const char *bca[] = {"http://b.test/", "http://c.test/", "http://a.test/", "http://a.test/x"};
    expect_text("98 never lowered", chain(bca, 4), "null");
}

// ── Credentials, Set-Cookie and the redirect rewrite (§ 4.5–4.7) ───────

static void credentials(void)
{
    Req q;
    way_refusal_t why;
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_INCLUDE, "GET", "", "http://b.test/");
    hop(&q, "http://b.test/", &why);
    expect("99 include, cors", way_credentials_included(&q.r), NULL);
    q.r.tainting = WAY_TAINT_OPAQUE;
    expect("99 include, opaque", way_credentials_included(&q.r), NULL);
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", "http://a.test/");
    hop(&q, "http://a.test/", &why);
    expect("100 same-origin, basic", way_credentials_included(&q.r), NULL);
    q.r.tainting = WAY_TAINT_CORS;
    expect("100 same-origin, cors", !way_credentials_included(&q.r), NULL);
    q.r.tainting = WAY_TAINT_OPAQUE;
    expect("100 same-origin, opaque", !way_credentials_included(&q.r), NULL);
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_OMIT, "GET", "", "http://a.test/");
    hop(&q, "http://a.test/", &why);
    expect("101 omit", !way_credentials_included(&q.r), NULL);
    // 102 and 103 are the job's (a preflight's request is never the
    // credentialed one; Set-Cookie follows way_credentials_included per hop):
    // the scripted-page host proves them on the wire.
    bool drop;
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "POST",
        "Content-Type: text/plain\r\nX-Keep: 1\r\nContent-Language: de\r\nContent-Encoding: gzip\r\n"
        "Content-Location: /x\r\n", "http://a.test/p");
    q.r.body = true;
    way_redirect_follow(&q.r, 301, "http://a.test/p", "http://a.test/q", &drop);
    expect("104 301 POST becomes GET", strcmp(q.r.method, "GET") == 0 && drop && !q.r.body, NULL);
    expect_text("104 the body headers go", q.headers, "X-Keep: 1\r\n");
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "POST", "Content-Type: text/plain\r\n", "http://a.test/p");
    q.r.body = true;
    way_redirect_follow(&q.r, 302, "http://a.test/p", "http://a.test/q", &drop);
    expect("104 302 POST becomes GET", strcmp(q.r.method, "GET") == 0 && drop && q.headers[0] == '\0', NULL);
    for (int s = 301; s <= 302; s++) {
        req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "PUT", "Content-Type: text/plain\r\n", "http://a.test/p");
        q.r.body = true;
        way_redirect_follow(&q.r, s, "http://a.test/p", "http://a.test/q", &drop);
        expect("105 301/302 PUT kept", strcmp(q.r.method, "PUT") == 0 && !drop && q.r.body &&
               strcmp(q.headers, "Content-Type: text/plain\r\n") == 0, NULL);
    }
    const char *m303[] = {"POST", "PUT", "DELETE"};
    for (size_t i = 0; i < 3; i++) {
        req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, m303[i], "", "http://a.test/p");
        q.r.body = true;
        way_redirect_follow(&q.r, 303, "http://a.test/p", "http://a.test/q", &drop);
        expect("106 303 makes GET", strcmp(q.r.method, "GET") == 0 && drop, m303[i]);
    }
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "HEAD", "", "http://a.test/p");
    way_redirect_follow(&q.r, 303, "http://a.test/p", "http://a.test/q", &drop);
    expect("106 303 keeps HEAD", strcmp(q.r.method, "HEAD") == 0, NULL);
    for (int s = 307; s <= 308; s++) {
        req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "POST", "Content-Type: text/plain\r\n", "http://a.test/p");
        q.r.body = true;
        way_redirect_follow(&q.r, s, "http://a.test/p", "http://a.test/q", &drop);
        expect("107 307/308 keep all", strcmp(q.r.method, "POST") == 0 && !drop && q.r.body &&
               strcmp(q.headers, "Content-Type: text/plain\r\n") == 0, NULL);
    }
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "Authorization: x\r\nX-A: 1\r\n", "http://a.test/p");
    way_redirect_follow(&q.r, 302, "http://a.test/p", "http://a.test/q", &drop);
    expect_text("108 same origin keeps Authorization", q.headers, "Authorization: x\r\nX-A: 1\r\n");
    way_redirect_follow(&q.r, 302, "http://a.test/q", "http://b.test/q", &drop);
    expect_text("108 another origin strips it", q.headers, "X-A: 1\r\n");
    way_redirect_follow(&q.r, 302, "http://b.test/q", "http://a.test/q", &drop);
    expect_text("109 it stays stripped", q.headers, "X-A: 1\r\n");
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", "http://a.test/0");
    bool all = true;
    for (int i = 1; i <= 20; i++) {
        char from[64], to[64];
        snprintf(from, sizeof(from), "http://a.test/%d", i - 1);
        snprintf(to, sizeof(to), "http://a.test/%d", i);
        all = all && way_redirect_follow(&q.r, 302, from, to, &drop) == WAY_FOLLOW_GO;
    }
    expect("110 twenty redirects", all, NULL);
    expect("110 the 21st", way_redirect_follow(&q.r, 302, "http://a.test/20", "http://a.test/21", &drop) == WAY_FOLLOW_REFUSED, NULL);
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", "http://a.test/");
    expect("111 not http", way_redirect_follow(&q.r, 302, "http://a.test/", "mailto:x", &drop) == WAY_FOLLOW_REFUSED, NULL);
    expect("111 unparseable", way_redirect_follow(&q.r, 302, "http://a.test/", "http://a b/", &drop) == WAY_FOLLOW_REFUSED, NULL);
    expect("111 userinfo", way_redirect_follow(&q.r, 302, "http://a.test/", "http://u:p@b.test/", &drop) == WAY_FOLLOW_REFUSED, NULL);
    expect("111 redirect statuses", way_redirect_status(301) && way_redirect_status(308) && !way_redirect_status(300) &&
           !way_redirect_status(304) && !way_redirect_status(305), NULL);
    expect("112 https to http (stated tightening)", way_redirect_follow(&q.r, 302, "https://a.test/", "http://a.test/", &drop) ==
           WAY_FOLLOW_REFUSED, NULL);
    expect("112 http to https is fine", way_redirect_follow(&q.r, 302, "http://a.test/", "https://a.test/", &drop) ==
           WAY_FOLLOW_GO, NULL);
}

// ── The CORS check (§ 4.10, § 3.3.5's table) ───────────────────────────

static bool check(way_credentials_t cred, const char *headers)
{
    Req q;
    way_refusal_t why;
    req(&q, "https://rabbit.invalid/", WAY_MODE_CORS, cred, "GET", "", "https://other.invalid/");
    hop(&q, "https://other.invalid/", &why);
    way_response_t r = resp(200, headers);
    return way_cors_check(&q.r, &r);
}

static void cors_check(void)
{
    expect("113 absent", !check(WAY_CREDENTIALS_OMIT, ""), NULL);
    Req q;
    way_refusal_t why;
    req(&q, "file:///page", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", "https://other.invalid/");
    hop(&q, "https://other.invalid/", &why);
    way_response_t r = resp(200, "Access-Control-Allow-Origin: null\r\n");
    expect("114 null for a null origin", way_cors_check(&q.r, &r), NULL);
    expect("114 null for a tuple origin", !check(WAY_CREDENTIALS_OMIT, "Access-Control-Allow-Origin: null\r\n"), NULL);
    expect("115 omit *", check(WAY_CREDENTIALS_OMIT, "Access-Control-Allow-Origin: *\r\n"), NULL);
    expect("115 omit * true", check(WAY_CREDENTIALS_OMIT, "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Credentials: true\r\n"), NULL);
    expect("116 a trailing slash", !check(WAY_CREDENTIALS_OMIT, "Access-Control-Allow-Origin: https://rabbit.invalid/\r\n"), NULL);
    expect("116 exact", check(WAY_CREDENTIALS_OMIT, "Access-Control-Allow-Origin: https://rabbit.invalid\r\n"), NULL);
    expect("117 include *", !check(WAY_CREDENTIALS_INCLUDE, "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Credentials: true\r\n"), NULL);
    expect("118 include exact true", check(WAY_CREDENTIALS_INCLUDE, "Access-Control-Allow-Origin: https://rabbit.invalid\r\n"
                                                                    "Access-Control-Allow-Credentials: true\r\n"), NULL);
    expect("118 True", !check(WAY_CREDENTIALS_INCLUDE, "Access-Control-Allow-Origin: https://rabbit.invalid\r\n"
                                                       "Access-Control-Allow-Credentials: True\r\n"), NULL);
    expect("118 no ACAC", !check(WAY_CREDENTIALS_INCLUDE, "Access-Control-Allow-Origin: https://rabbit.invalid\r\n"), NULL);
    expect("119 byte compare", !check(WAY_CREDENTIALS_OMIT, "Access-Control-Allow-Origin: HTTPS://rabbit.invalid\r\n"), NULL);
    expect("120 two lines", !check(WAY_CREDENTIALS_OMIT, "Access-Control-Allow-Origin: https://rabbit.invalid\r\n"
                                                         "Access-Control-Allow-Origin: https://rabbit.invalid\r\n"), NULL);
    expect("121 two ACAC", !check(WAY_CREDENTIALS_INCLUDE, "Access-Control-Allow-Origin: https://rabbit.invalid\r\n"
                                                           "Access-Control-Allow-Credentials: true\r\nAccess-Control-Allow-Credentials: true\r\n"), NULL);
    expect("same-origin mode credentials act as omit", check(WAY_CREDENTIALS_SAME_ORIGIN, "Access-Control-Allow-Origin: *\r\n"), NULL);
    // 122: the check runs on a redirect response too; the job applies it to
    // every response of a cors hop, and the scripted-page host proves that.
    req(&q, "https://rabbit.invalid/", WAY_MODE_CORS, WAY_CREDENTIALS_INCLUDE, "GET", "", "https://other.invalid/");
    hop(&q, "https://other.invalid/", &why);
    r = resp_omitting(200, "Access-Control-Allow-Origin: https://rabbit.invalid\r\nAccess-Control-Allow-Credentials: true\r\n",
                      "Access-Control-Allow-Origin\r\n");
    expect("123 unreadable ACAO", !way_cors_check(&q.r, &r), NULL);
    r = resp_omitting(200, "Access-Control-Allow-Origin: https://rabbit.invalid\r\nAccess-Control-Allow-Credentials: true\r\n",
                      "x-other\r\naccess-control-allow-credentials\r\n");
    expect("123 unreadable ACAC", !way_cors_check(&q.r, &r), NULL);
    char longer[400];
    snprintf(longer, sizeof(longer), "Access-Control-Allow-Origin: https://rabbit.invalid%0300d\r\n", 0);
    expect("a value longer than any origin", !check(WAY_CREDENTIALS_OMIT, longer), NULL);
}

// ── Response types and exposed headers (§ 2.2.6, § 4.1) ────────────────

static const char *filtered(way_credentials_t cred, way_type_t type, const char *headers, const char *omitted)
{
    static char out[2048];
    Req q;
    way_refusal_t why;
    req(&q, "http://a.test/", WAY_MODE_CORS, cred, "GET", "", "http://b.test/");
    hop(&q, "http://b.test/", &why);
    way_response_t r = resp_omitting(200, headers, omitted);
    if (way_response_headers(&q.r, type, &r, out, sizeof(out)) == SIZE_MAX)
        strcpy(out, "<did not fit>");
    return out;
}

static void response_types(void)
{
    const char *h = "Set-Cookie: a=1\r\nX-Foo: 1\r\nset-cookie2: b\r\nContent-Type: text/plain\r\nX-Bar: 2\r\n";
    expect_text("124 basic keeps all but Set-Cookie", filtered(WAY_CREDENTIALS_SAME_ORIGIN, WAY_TYPE_BASIC, h, ""),
                "X-Foo: 1\r\nContent-Type: text/plain\r\nX-Bar: 2\r\n");
    expect_text("125 cors keeps the safelisted", filtered(WAY_CREDENTIALS_SAME_ORIGIN, WAY_TYPE_CORS, h, ""),
                "Content-Type: text/plain\r\n");
    char with[512];
    snprintf(with, sizeof(with), "%sAccess-Control-Expose-Headers: X-Foo, x-bar\r\n", h);
    expect_text("126 exposed", filtered(WAY_CREDENTIALS_SAME_ORIGIN, WAY_TYPE_CORS, with, ""),
                "X-Foo: 1\r\nContent-Type: text/plain\r\nX-Bar: 2\r\n");
    snprintf(with, sizeof(with), "%sAccess-Control-Expose-Headers: *\r\n", h);
    expect_text("127 * without include", filtered(WAY_CREDENTIALS_SAME_ORIGIN, WAY_TYPE_CORS, with, ""),
                "X-Foo: 1\r\nContent-Type: text/plain\r\nX-Bar: 2\r\nAccess-Control-Expose-Headers: *\r\n");
    expect_text("128 * under include", filtered(WAY_CREDENTIALS_INCLUDE, WAY_TYPE_CORS, with, ""),
                "Content-Type: text/plain\r\n");
    expect_text("128 a header named *", filtered(WAY_CREDENTIALS_INCLUDE, WAY_TYPE_CORS,
                "*: star\r\nAccess-Control-Expose-Headers: *\r\n", ""), "*: star\r\n");
    snprintf(with, sizeof(with), "%sAccess-Control-Expose-Headers: Set-Cookie\r\n", h);
    expect_text("129 Set-Cookie stays hidden", filtered(WAY_CREDENTIALS_SAME_ORIGIN, WAY_TYPE_CORS, with, ""),
                "Content-Type: text/plain\r\n");
    snprintf(with, sizeof(with), "%sAccess-Control-Expose-Headers: X Foo\r\n", h);
    expect_text("130 malformed", filtered(WAY_CREDENTIALS_SAME_ORIGIN, WAY_TYPE_CORS, with, ""), "Content-Type: text/plain\r\n");
    snprintf(with, sizeof(with), "%sAccess-Control-Expose-Headers: X-Foo\r\n", h);
    expect_text("130 unreadable", filtered(WAY_CREDENTIALS_SAME_ORIGIN, WAY_TYPE_CORS, with, "access-control-expose-headers\r\n"),
                "Content-Type: text/plain\r\n");
    expect_text("131 opaque", filtered(WAY_CREDENTIALS_SAME_ORIGIN, WAY_TYPE_OPAQUE, h, ""), "");
    expect_text("132 opaqueredirect", filtered(WAY_CREDENTIALS_SAME_ORIGIN, WAY_TYPE_OPAQUEREDIRECT, h, ""), "");
    expect_text("133 error", filtered(WAY_CREDENTIALS_SAME_ORIGIN, WAY_TYPE_ERROR, h, ""), "");
    Req q;
    way_refusal_t why;
    req(&q, "http://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", "http://b.test/");
    hop(&q, "http://b.test/", &why);
    way_response_t r = resp(200, h);
    char tiny[8];
    expect("the filter refuses a short buffer", way_response_headers(&q.r, WAY_TYPE_BASIC, &r, tiny, sizeof(tiny)) == SIZE_MAX, NULL);
    expect("types", way_response_type(&q.r) == WAY_TYPE_CORS && strcmp(way_type_name(WAY_TYPE_OPAQUEREDIRECT), "opaqueredirect") == 0, NULL);
    // 134/135 are the job's sequencing of these answers; the scripted-page
    // host proves them.
    expect("136 bodies", way_response_has_body(&q.r, WAY_TYPE_CORS, 200) && !way_response_has_body(&q.r, WAY_TYPE_CORS, 204) &&
           !way_response_has_body(&q.r, WAY_TYPE_CORS, 205) && !way_response_has_body(&q.r, WAY_TYPE_CORS, 304) &&
           !way_response_has_body(&q.r, WAY_TYPE_CORS, 101) && !way_response_has_body(&q.r, WAY_TYPE_CORS, 103) &&
           !way_response_has_body(&q.r, WAY_TYPE_OPAQUE, 200) && !way_response_has_body(&q.r, WAY_TYPE_OPAQUEREDIRECT, 200) &&
           !way_response_has_body(&q.r, WAY_TYPE_ERROR, 200), NULL);
    snprintf(q.r.method, sizeof(q.r.method), "HEAD");
    expect("136 HEAD", !way_response_has_body(&q.r, WAY_TYPE_BASIC, 200), NULL);
}

// ── The Origin header (§ 3.2) ───────────────────────────────────────────

static const char *origin_header(const char *page, way_mode_t mode, way_policy_t policy, const char *method,
                                 const char *url)
{
    static char out[WAY_ORIGIN_TEXT_MAX];
    Req q;
    way_refusal_t why;
    req(&q, page, mode, WAY_CREDENTIALS_SAME_ORIGIN, method, "", url);
    q.r.policy = policy;
    hop(&q, url, &why);
    if (!way_origin_header(&q.r, url, out, sizeof(out)))
        strcpy(out, "<none>");
    return out;
}

static void origin_headers(void)
{
    expect_text("138 cors GET", origin_header("https://a.test/", WAY_MODE_CORS, 0, "GET", "https://b.test/"), "https://a.test");
    expect_text("138 opaque page", origin_header("file:///x", WAY_MODE_CORS, 0, "GET", "https://b.test/"), "null");
    const char *bc[] = {"https://b.test/", "https://c.test/"};
    (void)bc;
    Req q;
    way_refusal_t why;
    bool drop;
    req(&q, "https://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", "https://b.test/");
    hop(&q, "https://b.test/", &why);
    way_redirect_follow(&q.r, 302, "https://b.test/", "https://c.test/", &drop);
    hop(&q, "https://c.test/", &why);
    char out[WAY_ORIGIN_TEXT_MAX];
    expect("138 redirect-tainted", way_origin_header(&q.r, "https://c.test/", out, sizeof(out)) && strcmp(out, "null") == 0, out);
    expect_text("139 basic GET", origin_header("https://a.test/", WAY_MODE_CORS, 0, "GET", "https://a.test/x"), "<none>");
    expect_text("139 basic HEAD", origin_header("https://a.test/", WAY_MODE_CORS, 0, "HEAD", "https://a.test/x"), "<none>");
    expect_text("140 basic POST", origin_header("https://a.test/", WAY_MODE_CORS, 0, "POST", "https://a.test/x"), "https://a.test");
    expect_text("140 basic PUT", origin_header("https://a.test/", WAY_MODE_CORS, 0, "PUT", "https://a.test/x"), "https://a.test");
    expect_text("140 basic DELETE", origin_header("https://a.test/", WAY_MODE_CORS, 0, "DELETE", "https://a.test/x"), "https://a.test");
    expect_text("141 no-cors POST https", origin_header("https://a.test/", WAY_MODE_NO_CORS, WAY_POLICY_STRICT_ORIGIN_WHEN_CROSS_ORIGIN,
                                                        "POST", "https://b.test/"), "https://a.test");
    way_policy_t down[] = {WAY_POLICY_NO_REFERRER_WHEN_DOWNGRADE, WAY_POLICY_STRICT_ORIGIN, WAY_POLICY_STRICT_ORIGIN_WHEN_CROSS_ORIGIN};
    for (size_t i = 0; i < 3; i++)
        expect_text("141 downgrade to loopback", origin_header("https://a.test/", WAY_MODE_NO_CORS, down[i], "POST",
                                                               "http://127.0.0.1/"), "null");
    expect_text("142 no-referrer", origin_header("https://a.test/", WAY_MODE_NO_CORS, WAY_POLICY_NO_REFERRER, "POST",
                                                 "https://b.test/"), "null");
    expect_text("143 same-origin policy, another origin", origin_header("https://a.test/", WAY_MODE_NO_CORS, WAY_POLICY_SAME_ORIGIN,
                                                                        "POST", "https://b.test/"), "null");
    expect_text("143 same-origin policy, same origin", origin_header("https://a.test/", WAY_MODE_NO_CORS, WAY_POLICY_SAME_ORIGIN,
                                                                     "POST", "https://a.test/x"), "https://a.test");
    expect_text("144 unsafe-url", origin_header("https://a.test/", WAY_MODE_NO_CORS, WAY_POLICY_UNSAFE_URL, "POST",
                                                "http://127.0.0.1/"), "https://a.test");
    expect_text("144 origin", origin_header("https://a.test/", WAY_MODE_NO_CORS, WAY_POLICY_ORIGIN, "POST",
                                            "http://127.0.0.1/"), "https://a.test");
    expect_text("145 cors mode ignores the policy", origin_header("https://a.test/", WAY_MODE_CORS, WAY_POLICY_NO_REFERRER,
                                                                  "POST", "https://a.test/x"), "https://a.test");
    // 146: the preflight sends way_origin_header for its cors request, the
    // same call; proved on the wire by the scripted-page host.
}

static void referrers(void)
{
    struct { way_policy_t p; const char *from, *to, *want; } cases[] = {
        {WAY_POLICY_STRICT_ORIGIN_WHEN_CROSS_ORIGIN, "https://a.test/p?q#f", "https://a.test/x", "https://a.test/p?q"},
        {WAY_POLICY_STRICT_ORIGIN_WHEN_CROSS_ORIGIN, "https://a.test/p?q", "https://b.test/x", "https://a.test/"},
        {WAY_POLICY_STRICT_ORIGIN_WHEN_CROSS_ORIGIN, "https://a.test/p", "http://b.test/x", NULL},
        {WAY_POLICY_NO_REFERRER, "https://a.test/p", "https://a.test/x", NULL},
        {WAY_POLICY_NO_REFERRER_WHEN_DOWNGRADE, "https://a.test/p", "https://b.test/x", "https://a.test/p"},
        {WAY_POLICY_NO_REFERRER_WHEN_DOWNGRADE, "https://a.test/p", "http://b.test/x", NULL},
        {WAY_POLICY_SAME_ORIGIN, "https://a.test/p", "https://a.test/x", "https://a.test/p"},
        {WAY_POLICY_SAME_ORIGIN, "https://a.test/p", "https://b.test/x", NULL},
        {WAY_POLICY_ORIGIN, "https://a.test/p", "http://b.test/x", "https://a.test/"},
        {WAY_POLICY_STRICT_ORIGIN, "https://a.test/p", "https://a.test/x", "https://a.test/"},
        {WAY_POLICY_STRICT_ORIGIN, "https://a.test/p", "http://a.test/x", NULL},
        {WAY_POLICY_ORIGIN_WHEN_CROSS_ORIGIN, "https://a.test/p", "https://a.test/x", "https://a.test/p"},
        {WAY_POLICY_ORIGIN_WHEN_CROSS_ORIGIN, "https://a.test/p", "http://b.test/x", "https://a.test/"},
        {WAY_POLICY_UNSAFE_URL, "https://a.test/p", "http://b.test/x", "https://a.test/p"},
        {WAY_POLICY_UNSAFE_URL, "file:///p", "http://b.test/x", NULL},
        {WAY_POLICY_UNSAFE_URL, "", "http://b.test/x", NULL},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Req q;
        req(&q, "https://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", cases[i].to);
        q.r.policy = cases[i].p;
        char out[512];
        bool got = way_referrer_for(&q.r, cases[i].from, cases[i].to, out, sizeof(out));
        char name[96];
        snprintf(name, sizeof(name), "referrer %zu", i);
        if (cases[i].want == NULL)
            expect(name, !got, out);
        else
            expect_text(name, got ? out : "<none>", cases[i].want);
    }
    char from[5000];
    int n = snprintf(from, sizeof(from), "https://a.test/");
    memset(from + n, 'p', 4200);
    from[n + 4200] = '\0';
    Req q;
    req(&q, "https://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", "https://a.test/x");
    q.r.policy = WAY_POLICY_UNSAFE_URL;
    char out[8300];
    expect("referrer past 4096 bytes is its origin", way_referrer_for(&q.r, from, "https://a.test/x", out, sizeof(out)) &&
           strcmp(out, "https://a.test/") == 0, NULL);
}

// ── Mixed content ───────────────────────────────────────────────────────

static bool mixed(const char *page, const char *url)
{
    way_origin_t o;
    way_origin_of(page, 3, &o);
    return way_mixed_blocked(&o, url);
}

static void mixed_content(void)
{
    expect("147 https page, http URL", mixed("https://a.test/", "http://a.test/"), NULL);
    expect("147 https page, https URL", !mixed("https://a.test/", "https://a.test/"), NULL);
    expect("148 loopback literals", !mixed("https://a.test/", "http://127.0.0.1/") && !mixed("https://a.test/", "http://127.9.9.9:8080/"), NULL);
    expect("148 localhost is not (stated choice)", mixed("https://a.test/", "http://localhost/"), NULL);
    expect("148 other loopback spellings are not", mixed("https://a.test/", "http://127.1/") &&
           mixed("https://a.test/", "http://0127.0.0.1/") && mixed("https://a.test/", "http://127.0.0.1.5/") &&
           mixed("https://a.test/", "http://127.0.0.256/") && mixed("https://a.test/", "http://128.0.0.1/"), NULL);
    expect("149 data:", !mixed("https://a.test/", "data:,x"), NULL);
    expect("150 http page", !mixed("http://a.test/", "http://b.test/"), NULL);
    expect("150 file page", !mixed("file:///x", "http://b.test/"), NULL);
    Req q;
    way_refusal_t why;
    bool drop;
    req(&q, "https://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", "https://a.test/r");
    hop(&q, "https://a.test/r", &why);
    expect("151 refused as a downgrade", way_redirect_follow(&q.r, 302, "https://a.test/r", "http://b.test/", &drop) ==
           WAY_FOLLOW_REFUSED, NULL);
    req(&q, "https://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "GET", "", "http://b.test/");
    expect("151 and blocked at the hop", hop(&q, "http://b.test/", &why) == WAY_HOP_REFUSED && why == WAY_REFUSE_MIXED, NULL);
}

// ── Every allocation failed in turn ─────────────────────────────────────

// What allocates: an origin (the parsed URL), a referrer, the cache's keys
// and grants. Each answer under a failed allocation must be the one that
// grants nothing, and nothing may leak.
static void sweep(void)
{
    for (size_t n = 1; n < 64; n++) {
        size_t base = live;
        allocations = 0;
        fail_at = n;
        way_origin_t o;
        way_origin_of("https://a.test/", 0, &o);
        bool origin_ok = o.opaque || strcmp(o.host, "a.test") == 0;
        bool port = way_port_bad("http://b.test:8080/");     // unreadable reads as bad
        Req q;
        way_refusal_t why;
        req(&q, "https://a.test/", WAY_MODE_CORS, WAY_CREDENTIALS_SAME_ORIGIN, "PUT", "", "https://b.test/r");
        way_hop_t h = hop(&q, "https://b.test/r", &why);
        char out[256];
        bool ref = way_referrer_for(&q.r, "https://a.test/p", "https://b.test/r", out, sizeof(out));
        way_preflight_t *c = way_preflight_new();
        if (c != NULL) {
            way_response_t r = resp(200, "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT, PATCH\r\n");
            way_preflight_store(c, &q.r, "https://b.test/r", &r, 60, 0);
            int32_t kept = way_preflight_count(c);
            bool need = way_preflight_needed(c, &q.r, "https://b.test/r", 1);
            way_preflight_clear(c, &q.r, "https://b.test/r");
            // A grant that was not kept is asked for again; one kept for an
            // origin that read as opaque answers a different key, and asks too.
            expect("sweep: a cache that kept nothing asks again", kept > 0 || need, NULL);
            expect("sweep: clear empties it", way_preflight_count(c) == 0 || h != WAY_HOP_CORS, NULL);
            way_preflight_free(c);
        }
        fail_at = 0;
        (void)port;
        (void)ref;
        char name[64];
        snprintf(name, sizeof(name), "sweep %zu: origin", n);
        expect(name, origin_ok, NULL);
        snprintf(name, sizeof(name), "sweep %zu: a hop refused or judged", n);
        expect(name, h == WAY_HOP_CORS || h == WAY_HOP_REFUSED, NULL);
        snprintf(name, sizeof(name), "sweep %zu: nothing leaked", n);
        expect(name, live == base, NULL);
        if (allocations < n)
            break;
    }
}

int main(void)
{
    origins();
    methods_and_headers();
    mime();
    response_names();
    main_fetch();
    preflight();
    cache();
    redirect_taint();
    credentials();
    cors_check();
    response_types();
    origin_headers();
    referrers();
    mixed_content();
    sweep();
    expect("nothing leaked", live == 0, NULL);
    printf("way origin: %d checks, %d failed\n", checks, failures);
    return failures != 0 ? 1 : 0;
}
