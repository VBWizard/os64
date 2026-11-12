// test_http_host.c — host-side harness for os64get's HTTP parser.
//
// The companion shell script generates replies with Python (http.client is
// the reference implementation for the well-formed ones, urllib.parse for
// the URLs) and drives this across every input chunk size, under ASan and
// UBSan. The point of the chunk sweep is that a stream parser's bugs live
// exactly where a token straddles two reads — a status line split between
// the '2' and the '00', a CRLF whose halves arrive a second apart.

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fetch/http.h"
#include "os64/str.h"

// fmt.c's printf half writes through this; nothing under test calls it.
int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    return (int64_t)fwrite(buf, 1, len, handle == 2 ? stderr : stdout);
}

typedef struct {
    const uint8_t *bytes;
    size_t         len;
    size_t         pos;
    size_t         chunk;    // most bytes handed over per call
    size_t         breakAt;  // answer -1 once this many have been handed over
    int            broken;
} mem_source_t;

static int64_t mem_read(void *ctx, void *buf, size_t cap)
{
    mem_source_t *m = (mem_source_t *)ctx;

    if (m->breakAt != 0 && m->pos >= m->breakAt) {
        m->broken = 1;
        return -1;
    }
    if (m->pos >= m->len)
        return 0;

    size_t n = m->len - m->pos;
    if (n > cap)
        n = cap;
    if (n > m->chunk)
        n = m->chunk;
    if (m->breakAt != 0 && m->pos + n > m->breakAt)
        n = m->breakAt - m->pos;

    memcpy(buf, m->bytes + m->pos, n);
    m->pos += n;
    return (int64_t)n;
}

static uint8_t *read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long end = ftell(file);
    if (end < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }

    size_t size = (size_t)end;
    uint8_t *bytes = malloc(size == 0 ? 1 : size);
    if (bytes == NULL || fread(bytes, 1, size, file) != size) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *length = size;
    return bytes;
}

static const char *url_result_name(http_url_result_t rc)
{
    switch (rc) {
        case HTTP_URL_OK:         return "ok";
        case HTTP_URL_NOT_A_URL:  return "not_a_url";
        case HTTP_URL_SCHEME:     return "scheme";
        case HTTP_URL_NO_HOST:    return "no_host";
        case HTTP_URL_HOST_CHARS: return "host_chars";
        case HTTP_URL_PATH_CHARS: return "path_chars";
        case HTTP_URL_PORT:       return "port";
        case HTTP_URL_TOO_LONG:   return "too_long";
    }
    return "?";
}

static const char *head_result_name(http_head_result_t rc)
{
    switch (rc) {
        case HTTP_HEAD_OK:       return "ok";
        case HTTP_HEAD_SOURCE:   return "source";
        case HTTP_HEAD_STATUS:   return "status";
        case HTTP_HEAD_SYNTAX:   return "syntax";
        case HTTP_HEAD_TOO_MUCH: return "too_much";
        case HTTP_HEAD_CONFLICT: return "conflict";
        case HTTP_HEAD_FRAMING:  return "framing";
        case HTTP_HEAD_SWITCHED: return "switched";
    }
    return "?";
}

static const char *body_result_name(http_body_result_t rc)
{
    switch (rc) {
        case HTTP_BODY_OPEN:     return "open";
        case HTTP_BODY_DONE:     return "done";
        case HTTP_BODY_CUT:      return "cut";
        case HTTP_BODY_BROKE:    return "broke";
        case HTTP_BODY_SYNTAX:   return "syntax";
        case HTTP_BODY_TOO_MUCH: return "too_much";
    }
    return "?";
}

static int do_url(const char *text)
{
    http_url_t url;
    http_url_result_t rc = http_url_parse(text, &url);

    printf("%s\n", url_result_name(rc));
    printf("scheme=%s\n", url.scheme);
    if (rc == HTTP_URL_OK) {
        printf("host=%s\n", url.host);
        printf("port=%u\n", (unsigned)url.port);
        printf("path=%s\n", url.path);

        char rendered[HTTP_URL_TEXT_MAX];
        if (!http_url_render(&url, rendered, sizeof(rendered))) {
            fprintf(stderr, "render does not fit\n");
            return 3;
        }
        printf("render=%s\n", rendered);

        char request[HTTP_TARGET_MAX + 1024];
        if (!http_request(request, sizeof(request), &url, false, NULL)) {
            fprintf(stderr, "request does not fit\n");
            return 3;
        }
        fputs("request<<\n", stdout);
        fputs(request, stdout);
        fputs(">>\n", stdout);

        // The same ask, addressed to a proxy: whole URL in the request line,
        // Host still naming the origin.
        if (!http_request(request, sizeof(request), &url, true, NULL)) {
            fprintf(stderr, "proxy request does not fit\n");
            return 3;
        }
        fputs("proxyrequest<<\n", stdout);
        fputs(request, stdout);
        fputs(">>\n", stdout);
    }
    return 0;
}

static bool emit_headers;
static void print_header(void *ctx, const char *name, size_t name_len, const char *value, size_t len)
{
    (void)ctx;
    printf("header=%.*s\t%.*s\n", (int)name_len, name, (int)len, value);
}

static int do_head(const char *path, size_t chunk, size_t sip, size_t breakAt,
                   const char *bodyPath)
{
    size_t length = 0;
    uint8_t *bytes = read_file(path, &length);
    if (bytes == NULL) {
        fprintf(stderr, "cannot read %s\n", path);
        return 2;
    }

    mem_source_t source = {
        .bytes = bytes, .len = length, .pos = 0,
        .chunk = chunk == 0 ? length + 1 : chunk,
        .breakAt = breakAt, .broken = 0,
    };

    http_stream_t stream;
    http_stream_init(&stream, mem_read, &source);

    http_response_t reply;
    http_head_result_t rc = http_head_read_with_headers(&stream, &reply,
                                                         emit_headers ? print_header : NULL, NULL);
    printf("%s\n", head_result_name(rc));
    if (rc != HTTP_HEAD_OK) {
        free(bytes);
        return 0;
    }

    printf("status=%d\n", (int)reply.status);
    printf("reason=%s\n", reply.reason);
    printf("haslength=%d\n", reply.hasLength ? 1 : 0);
    printf("length=%llu\n", (unsigned long long)(reply.hasLength ? reply.length : 0));
    printf("transfer=%s\n", reply.transferEncoding);
    printf("content=%s\n", reply.contentEncoding);
    printf("location=%s\n", reply.location);

    // The body, read exactly the way os64get reads it: through the body
    // reader, which takes the framing off — a length, chunks, or the close.
    // A transfer coding it cannot undo is reported as os64get would refuse
    // it, and nothing is read.
    http_body_t body;
    if (!http_body_open(&body, &stream, &reply)) {
        printf("framing=unsupported\n");
        free(bytes);
        return 0;
    }
    printf("framing=%s\n", body.framing == HTTP_FRAMING_CHUNKED ? "chunked"
                         : body.framing == HTTP_FRAMING_LENGTH  ? "length" : "close");

    FILE *out = fopen(bodyPath, "wb");
    if (out == NULL) {
        fprintf(stderr, "cannot write %s\n", bodyPath);
        free(bytes);
        return 2;
    }

    uint8_t *sipBuf = malloc(sip);
    if (sipBuf == NULL) { fclose(out); free(bytes); return 2; }

    uint64_t got = 0;
    for (;;) {
        int64_t n = http_body_read(&body, sipBuf, sip);
        if (n <= 0) break;
        fwrite(sipBuf, 1, (size_t)n, out);
        got += (uint64_t)n;
    }
    // Once over, the reader must keep answering the same verdict: a caller
    // that asks again after 0 or -1 gets no fresh byte and no new story.
    http_body_result_t verdict = body.result;
    uint8_t again[8];
    int64_t more = http_body_read(&body, again, sizeof(again));
    if (body.result != verdict || more > 0) {
        fprintf(stderr, "the body reader changed its mind after finishing\n");
        return 3;
    }
    fclose(out);
    free(sipBuf);
    free(bytes);

    printf("bodylen=%llu\n", (unsigned long long)got);
    printf("verdict=%s\n", body_result_name(verdict));
    // The two words os64get's exit codes are keyed on. `short` is a body the
    // framing says is incomplete; `broke` is a source that failed.
    printf("broke=%d\n", verdict == HTTP_BODY_BROKE ? 1 : 0);
    printf("short=%d\n", verdict == HTTP_BODY_CUT ? 1 : 0);
    return 0;
}

// "absolute": given a base URL and a Location, print the whole address the
// redirect resolves to — or "declined" when there is none to spell (an empty
// reference, or an answer too long to hold). Checked against
// urllib.parse.urljoin, which implements the same RFC 3986 §5.2.
static int do_absolute(const char *baseText, const char *location)
{
    http_url_t base;
    if (http_url_parse(baseText, &base) != HTTP_URL_OK) {
        fprintf(stderr, "base is not a URL: %s\n", baseText);
        return 2;
    }

    // The resolver lives in libos64 and takes the library's URL, where a
    // default port is spelled as 0 — http_url_to_os64 is the one conversion.
    os64_url_t baseUrl;
    http_url_to_os64(&base, &baseUrl);
    char whole[HTTP_URL_TEXT_MAX];
    if (!os64_url_absolute(&baseUrl, location, whole, sizeof(whole))) {
        printf("declined\n");
        return 0;
    }
    printf("absolute\n");
    printf("url=%s\n", whole);
    return 0;
}

// ── The request as HTTP/2's field list, against the same request as text ──

static int field_failures;
#define FIELD_CHECK(x) \
    do { \
        if (!(x)) { \
            fprintf(stderr, "fields: %s:%d: %s\n", __FILE__, __LINE__, #x); \
            field_failures++; \
        } \
    } while (0)

static bool field_is(const http_field_t *f, const char *name, const char *value)
{
    return f->name_len == strlen(name) && memcmp(f->name, name, f->name_len) == 0 &&
           f->value_len == strlen(value) && memcmp(f->value, value, f->value_len) == 0;
}

static bool of_the_connection(const char *name, size_t len)
{
    static const char *const kNames[] = {"connection", "keep-alive", "proxy-connection", "transfer-encoding",
                                         "upgrade", "te", "host"};
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++)
        if (strlen(kNames[i]) == len && strncasecmp(name, kNames[i], len) == 0)
            return true;
    return false;
}

// THE TWO RENDERINGS AGREE: the list is the four pseudo-headers, then the
// text's lines after Host, in the text's order, each name in lower case,
// less the fields that speak of the connection, a Cookie line as its pairs.
static void fields_against_text(const char *what, const char *address, const http_request_extras_t *x)
{
    http_url_t url;
    static char text[16384], storage[16384];
    http_field_t fields[64];
    size_t count = 0;
    if (http_url_parse(address, &url) != HTTP_URL_OK || !http_request(text, sizeof(text), &url, false, x) ||
        !http_request_fields(&url, x, fields, 64, storage, sizeof(storage), &count) || count < 4) {
        fprintf(stderr, "fields: %s: a request that should render did not\n", what);
        field_failures++;
        return;
    }
    const char *method = x != NULL && x->method_name != NULL ? x->method_name
                       : x != NULL && x->method == HTTP_METHOD_POST ? "POST" : "GET";
    FIELD_CHECK(field_is(&fields[0], ":method", method));
    FIELD_CHECK(fields[1].name_len == 10 && memcmp(fields[1].name, ":authority", 10) == 0);
    FIELD_CHECK(field_is(&fields[2], ":scheme", url.scheme) && field_is(&fields[3], ":path", url.path));
    // Host's value is :authority's.
    const char *host = strstr(text, "\r\nHost: ");
    FIELD_CHECK(host != NULL);
    if (host == NULL)
        return;
    host += 8;
    const char *host_end = strstr(host, "\r\n");
    FIELD_CHECK((size_t)(host_end - host) == fields[1].value_len && memcmp(host, fields[1].value, fields[1].value_len) == 0);

    size_t at = 4;
    for (const char *line = host_end + 2; line[0] != '\r'; line = strstr(line, "\r\n") + 2) {
        const char *colon = strchr(line, ':'), *end = strstr(line, "\r\n");
        size_t name_len = (size_t)(colon - line);
        const char *value = colon + 1;
        while (value < end && (*value == ' ' || *value == '\t'))
            value++;
        while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        if (of_the_connection(line, name_len))
            continue;
        if (name_len == 6 && strncasecmp(line, "cookie", 6) == 0) {
            for (const char *p = value; p < end;) {
                const char *semi = memchr(p, ';', (size_t)(end - p));
                const char *stop = semi != NULL ? semi : end;
                if (stop > p) {
                    FIELD_CHECK(at < count && fields[at].name_len == 6 && memcmp(fields[at].name, "cookie", 6) == 0 &&
                                fields[at].value_len == (size_t)(stop - p) && memcmp(fields[at].value, p, (size_t)(stop - p)) == 0);
                    at++;
                }
                p = stop + 1;
                while (p < end && *p == ' ')
                    p++;
            }
            continue;
        }
        bool same = at < count && fields[at].name_len == name_len && fields[at].value_len == (size_t)(end - value) &&
                    memcmp(fields[at].value, value, fields[at].value_len) == 0;
        for (size_t i = 0; same && i < name_len; i++)
            same = fields[at].name[i] == (char)tolower((unsigned char)line[i]);
        if (!same) {
            fprintf(stderr, "fields: %s: field %zu is not the text's line \"%.*s\"\n", what, at, (int)(end - line), line);
            field_failures++;
        }
        bool credential = (name_len == 13 && strncasecmp(line, "authorization", 13) == 0) ||
                          (name_len == 19 && strncasecmp(line, "proxy-authorization", 19) == 0);
        FIELD_CHECK(at >= count || fields[at].sensitive == credential);
        at++;
    }
    if (at != count) {
        fprintf(stderr, "fields: %s: %zu fields, and the text has %zu\n", what, count, at);
        field_failures++;
    }
    for (size_t i = 0; i < count; i++)
        FIELD_CHECK(!of_the_connection(fields[i].name, fields[i].name_len));
}

static int do_fields(void)
{
    static const http_request_extras_t kPlain = {0};
    static const http_request_extras_t kBrowser = {
        .user_agent = "Mozilla/5.0 (X11) Thing/1.0", .accept = "text/html,*/*;q=0.8", .keep_alive = true,
        .extra_headers = "Cookie: a=1; b=two; c=3\r\nReferer: https://example.test/from\r\n"
                         "Authorization: Basic c2VjcmV0\r\nX-Mixed-CASE:   padded value  \r\n",
        .order = "Accept, Cookie, User-Agent, Referer"};
    static const http_request_extras_t kPost = {.method = HTTP_METHOD_POST, .body_len = 12,
                                                .content_type = "application/json", .user_agent = "ua"};
    static const http_request_extras_t kNamed = {.method_name = "PUT", .body_len = 0, .body_present = true,
                                                 .body_headers = "Content-Type: text/plain\r\nContent-Language: en\r\n",
                                                 .identity = true};
    static const http_request_extras_t kOneCookie = {.extra_headers = "Cookie: only=one\r\n"};
    static const http_request_extras_t kOddCookie = {.extra_headers = "Cookie: a=1;b=2;  c=3;\r\n"};
    static const http_request_extras_t kGapCookie = {.extra_headers = "Cookie: a=1;; b=2; ;c=3\r\n"};
    fields_against_text("no extras", "https://example.test/", &kPlain);
    fields_against_text("a NULL extras", "https://example.test/path?q=1", NULL);
    fields_against_text("a browser's", "https://example.test/page", &kBrowser);
    fields_against_text("a POST", "https://example.test/api", &kPost);
    fields_against_text("a named method", "https://example.test/thing", &kNamed);
    fields_against_text("one cookie", "https://example.test/", &kOneCookie);
    fields_against_text("cookies oddly spaced", "https://example.test/", &kOddCookie);
    fields_against_text("cookies with nothing between two semicolons", "https://example.test/", &kGapCookie);
    fields_against_text("a port in the authority", "https://example.test:8443/", &kBrowser);

    // Spelled out once, so the comparison above is not the only witness.
    http_url_t url;
    static char storage[4096];
    http_field_t f[32];
    size_t n = 0;
    FIELD_CHECK(http_url_parse("https://example.test:8443/a?b", &url) == HTTP_URL_OK);
    FIELD_CHECK(http_request_fields(&url, &kBrowser, f, 32, storage, sizeof(storage), &n) && n == 13);
    if (n == 13) {
        FIELD_CHECK(field_is(&f[0], ":method", "GET") && field_is(&f[1], ":authority", "example.test:8443") &&
                    field_is(&f[2], ":scheme", "https") && field_is(&f[3], ":path", "/a?b"));
        FIELD_CHECK(field_is(&f[4], "accept", "text/html,*/*;q=0.8"));
        FIELD_CHECK(field_is(&f[5], "cookie", "a=1") && field_is(&f[6], "cookie", "b=two") && field_is(&f[7], "cookie", "c=3"));
        FIELD_CHECK(field_is(&f[8], "user-agent", "Mozilla/5.0 (X11) Thing/1.0"));
        FIELD_CHECK(field_is(&f[9], "referer", "https://example.test/from"));
        FIELD_CHECK(field_is(&f[10], "accept-encoding", "gzip, identity"));
        FIELD_CHECK(field_is(&f[11], "authorization", "Basic c2VjcmV0") && f[11].sensitive && !f[10].sensitive);
        FIELD_CHECK(field_is(&f[12], "x-mixed-case", "padded value"));
    }
    // What does not fit is refused and not cut: the table, then the bytes.
    FIELD_CHECK(!http_request_fields(&url, &kBrowser, f, 12, storage, sizeof(storage), &n));
    FIELD_CHECK(!http_request_fields(&url, &kBrowser, f, 32, storage, 100, &n));
    // And what http_request refuses, this refuses.
    static const http_request_extras_t kBad = {.extra_headers = "Host: elsewhere\r\n"};
    FIELD_CHECK(!http_request_fields(&url, &kBad, f, 32, storage, sizeof(storage), &n));

    // WHAT HTTP/2 ALONE CARRIES (HTTP2.md § 4 H2.5). The pseudo-headers in
    // the order asked for; a Priority and `te: trailers` after every other
    // line; and none of the three in the text.
    static const http_request_extras_t kFirefoxLike = {.user_agent = "Agent/1", .extra_headers = "X-One: 1\r\n",
                                                       .keep_alive = true, .h2_priority = "u=4",
                                                       .h2_te_trailers = true, .h2_pseudo = "mpas"};
    FIELD_CHECK(http_request_fields(&url, &kFirefoxLike, f, 32, storage, sizeof(storage), &n) && n == 9);
    if (n == 9) {
        FIELD_CHECK(field_is(&f[0], ":method", "GET") && field_is(&f[1], ":path", "/a?b") &&
                    field_is(&f[2], ":authority", "example.test:8443") && field_is(&f[3], ":scheme", "https"));
        FIELD_CHECK(field_is(&f[4], "user-agent", "Agent/1") && field_is(&f[6], "x-one", "1"));
        FIELD_CHECK(field_is(&f[7], "priority", "u=4") && field_is(&f[8], "te", "trailers"));
    }
    static char text[2048];
    FIELD_CHECK(http_request(text, sizeof(text), &url, false, &kFirefoxLike) &&
                strstr(text, "riority") == NULL && strstr(text, "\nTE:") == NULL && strstr(text, "\nte:") == NULL &&
                strstr(text, "Connection: keep-alive\r\n") != NULL);
    // The order may name them, and then they go where it puts them.
    static const http_request_extras_t kOrdered = {.user_agent = "Agent/1", .h2_priority = "u=0, i",
                                                   .h2_te_trailers = true, .order = "TE, Priority, User-Agent"};
    FIELD_CHECK(http_request_fields(&url, &kOrdered, f, 32, storage, sizeof(storage), &n) && n == 8);
    if (n == 8)
        FIELD_CHECK(field_is(&f[4], "te", "trailers") && field_is(&f[5], "priority", "u=0, i") &&
                    field_is(&f[6], "user-agent", "Agent/1"));
    // With neither asked for, neither is sent; and the default order is
    // method, authority, scheme, path (checked against the text, above).
    static const http_request_extras_t kNeither = {.user_agent = "Agent/1"};
    FIELD_CHECK(http_request_fields(&url, &kNeither, f, 32, storage, sizeof(storage), &n) && n == 6);
    // A pseudo-header order that is not the four letters once each, and a
    // Priority that holds a byte a header cannot, are refused.
    static const char *const kBadOrders[] = {"mas", "masps", "mmas", "masx", ""};
    for (size_t i = 0; i < sizeof(kBadOrders) / sizeof(kBadOrders[0]); i++) {
        http_request_extras_t x = {.h2_pseudo = kBadOrders[i]};
        FIELD_CHECK(!http_request_fields(&url, &x, f, 32, storage, sizeof(storage), &n));
    }
    static const http_request_extras_t kBadPriority = {.h2_priority = "u=1\r\nX-Smuggled: 1"};
    FIELD_CHECK(!http_request_fields(&url, &kBadPriority, f, 32, storage, sizeof(storage), &n));
    // A caller's own TE line is still refused: the one above is the walk's.
    static const http_request_extras_t kOwnTe = {.extra_headers = "TE: trailers\r\n"};
    FIELD_CHECK(!http_request_fields(&url, &kOwnTe, f, 32, storage, sizeof(storage), &n));
    return field_failures == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "fields") == 0)
        return do_fields();
    if (argc == 4 && strcmp(argv[1], "post") == 0) {
        http_url_t url;
        char request[8192];
        http_request_extras_t opt = { .method = HTTP_METHOD_POST,
            .body_len = (size_t)strtoull(argv[2], NULL, 10), .content_type = argv[3] };
        if (http_url_parse("http://example.test/form", &url) != HTTP_URL_OK ||
            !http_request(request, sizeof(request), &url, false, &opt)) return 1;
        fputs(request, stdout);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "url") == 0)
        return do_url(argv[2]);

    if (argc >= 4 && strcmp(argv[1], "absolute") == 0)
        return do_absolute(argv[2], argv[3]);

    emit_headers = argc >= 2 && strcmp(argv[1], "headers") == 0;
    if (argc >= 7 && (strcmp(argv[1], "head") == 0 || emit_headers))
        return do_head(argv[2], (size_t)strtoul(argv[3], NULL, 0),
                       (size_t)strtoul(argv[4], NULL, 0),
                       (size_t)strtoul(argv[5], NULL, 0), argv[6]);

    fprintf(stderr,
            "usage: test_http url <URL>\n"
            "       test_http head <file> <chunk> <sip> <breakat> <bodyout>\n"
            "       test_http absolute <base-url> <location>\n");
    return 2;
}
