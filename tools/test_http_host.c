// test_http_host.c — host-side harness for os64get's HTTP parser.
//
// The companion shell script generates replies with Python (http.client is
// the reference implementation for the well-formed ones, urllib.parse for
// the URLs) and drives this across every input chunk size, under ASan and
// UBSan. The point of the chunk sweep is that a stream parser's bugs live
// exactly where a token straddles two reads — a status line split between
// the '2' and the '00', a CRLF whose halves arrive a second apart.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
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

        char rendered[4096];
        if (!http_url_render(&url, rendered, sizeof(rendered))) {
            fprintf(stderr, "render does not fit\n");
            return 3;
        }
        printf("render=%s\n", rendered);

        char request[4096];
        if (!http_request(request, sizeof(request), &url, false)) {
            fprintf(stderr, "request does not fit\n");
            return 3;
        }
        fputs("request<<\n", stdout);
        fputs(request, stdout);
        fputs(">>\n", stdout);

        // The same ask, addressed to a proxy: whole URL in the request line,
        // Host still naming the origin.
        if (!http_request(request, sizeof(request), &url, true)) {
            fprintf(stderr, "proxy request does not fit\n");
            return 3;
        }
        fputs("proxyrequest<<\n", stdout);
        fputs(request, stdout);
        fputs(">>\n", stdout);
    }
    return 0;
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
    http_head_result_t rc = http_head_read(&stream, &reply);
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

    // The body, read exactly the way os64get reads it: bounded by
    // Content-Length when there is one, by the close when there is not.
    FILE *out = fopen(bodyPath, "wb");
    if (out == NULL) {
        fprintf(stderr, "cannot write %s\n", bodyPath);
        free(bytes);
        return 2;
    }

    uint8_t *sipBuf = malloc(sip);
    if (sipBuf == NULL) { fclose(out); free(bytes); return 2; }

    uint64_t got = 0;
    int broke = 0;
    for (;;) {
        if (reply.hasLength && got >= reply.length)
            break;
        size_t want = sip;
        if (reply.hasLength && (reply.length - got) < (uint64_t)want)
            want = (size_t)(reply.length - got);

        int64_t n = http_stream_read(&stream, sipBuf, want);
        if (n < 0) { broke = 1; break; }
        if (n == 0) break;
        fwrite(sipBuf, 1, (size_t)n, out);
        got += (uint64_t)n;
    }
    fclose(out);
    free(sipBuf);
    free(bytes);

    printf("bodylen=%llu\n", (unsigned long long)got);
    printf("broke=%d\n", broke);
    printf("short=%d\n", (reply.hasLength && got != reply.length) ? 1 : 0);
    return 0;
}

// "absolute": given a base URL and a Location, print the whole address the
// redirect advice would offer — or "relative" when this deliberately does not
// resolve one. Checked against urllib.parse.urljoin for the forms it claims.
static int do_absolute(const char *baseText, const char *location)
{
    http_url_t base;
    if (http_url_parse(baseText, &base) != HTTP_URL_OK) {
        fprintf(stderr, "base is not a URL: %s\n", baseText);
        return 2;
    }

    char whole[HTTP_LINE_MAX + HTTP_HOST_MAX + 16];
    if (!http_url_absolute(&base, location, whole, sizeof(whole))) {
        printf("relative\n");
        return 0;
    }
    printf("absolute\n");
    printf("url=%s\n", whole);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "url") == 0)
        return do_url(argv[2]);

    if (argc >= 4 && strcmp(argv[1], "absolute") == 0)
        return do_absolute(argv[2], argv[3]);

    if (argc >= 7 && strcmp(argv[1], "head") == 0)
        return do_head(argv[2], (size_t)strtoul(argv[3], NULL, 0),
                       (size_t)strtoul(argv[4], NULL, 0),
                       (size_t)strtoul(argv[5], NULL, 0), argv[6]);

    fprintf(stderr,
            "usage: test_http url <URL>\n"
            "       test_http head <file> <chunk> <sip> <breakat> <bodyout>\n"
            "       test_http absolute <base-url> <location>\n");
    return 2;
}
