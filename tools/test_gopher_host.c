// test_gopher_host.c — drive gopher's wire parser from the host.
//
// The harness binary. tools/test_gopher_host.sh is the suite that runs it;
// this file only turns argv into one call and prints what came back in
// `key=value` lines, so the expectations live in one place (the script) and
// the plumbing lives in another.
//
// Bytes that a shell cannot pass are given as HEX and decoded here — a menu
// line carrying ESC or NUL is precisely what needs testing, and precisely
// what argv cannot carry.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wire.h"

// fmt.c's printf half writes through this; nothing under test calls it.
int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    return (int64_t)fwrite(buf, 1, len, handle == 2 ? stderr : stdout);
}

static size_t unhex(const char *hex, char *out, size_t cap)
{
    size_t n = 0;
    for (; hex[0] != '\0' && hex[1] != '\0' && n + 1 < cap; hex += 2) {
        unsigned value;
        if (sscanf(hex, "%2x", &value) != 1)
            break;
        out[n++] = (char)value;
    }
    out[n] = '\0';
    return n;
}

// Print a string with the bytes a terminal obeys spelled out, so the suite's
// diagnostics cannot repaint the screen of whoever is reading them.
static void print_visible(const char *label, const char *s)
{
    printf("%s=", label);
    for (; *s != '\0'; s++) {
        unsigned char b = (unsigned char)*s;
        if (b < 0x20 || b == 0x7F)
            printf("\\x%02x", b);
        else
            putchar(*s);
    }
    putchar('\n');
}

// ── A source that hands out `chunk` bytes at a time ─────────────────────
// The whole point of the source function: a parser's bugs live where a token
// straddles two reads, so the suite drives every case at one byte, then two,
// then seventeen.
typedef struct {
    const char *data;
    size_t      len;
    size_t      pos;
    size_t      chunk;
} feeder_t;

static int64_t feed(void *ctx, void *buf, size_t cap)
{
    feeder_t *f = ctx;
    size_t left = f->len - f->pos;
    if (left == 0)
        return 0;
    size_t take = left < cap ? left : cap;
    if (f->chunk != 0 && take > f->chunk)
        take = f->chunk;
    memcpy(buf, f->data + f->pos, take);
    f->pos += take;
    return (int64_t)take;
}

static int do_url(const char *text)
{
    gopher_addr_t addr;
    gopher_url_result_t rc = gopher_url_parse(text, &addr);
    printf("%s\n", rc == GOPHER_URL_OK ? "ok" : "refused");
    printf("code=%d\n", (int)rc);
    printf("reason=%s\n", gopher_url_reason(rc));
    if (rc != GOPHER_URL_OK)
        return 0;
    printf("host=%s\n", addr.host);
    printf("port=%u\n", (unsigned)addr.port);
    printf("type=%c\n", addr.type);
    print_visible("selector", addr.selector);
    print_visible("query", addr.query);

    char text2[2048];
    size_t wanted = gopher_url_text(&addr, text2, sizeof(text2));
    printf("rendered=%s\n", text2);
    printf("rendered_wanted=%zu\n", wanted);

    // The round trip: what we render must parse back to the same address, or
    // the location shown to a person is one they cannot retype.
    gopher_addr_t again;
    if (gopher_url_parse(text2, &again) != GOPHER_URL_OK) {
        printf("roundtrip=unparseable\n");
    } else {
        bool same = strcmp(addr.host, again.host) == 0 && addr.port == again.port
                 && addr.type == again.type
                 && strcmp(addr.selector, again.selector) == 0
                 && strcmp(addr.query, again.query) == 0;
        printf("roundtrip=%s\n", same ? "same" : "DIFFERENT");
    }

    char request[2048];
    size_t n = gopher_request(&addr, request, sizeof(request));
    printf("request_len=%zu\n", n);
    print_visible("request", request);
    return 0;
}

static int do_item(const char *hex)
{
    char line[8192];
    unhex(hex, line, sizeof(line));

    gopher_item_t item;
    gopher_item_result_t rc = gopher_item_parse(line, &item);
    static const char *names[] = { "ok", "info", "malformed", "refused" };
    printf("%s\n", names[rc]);
    printf("type=%c\n", item.type != '\0' ? item.type : '?');
    print_visible("display", item.display);
    printf("followable=%d\n", item.followable ? 1 : 0);
    printf("typename=%s\n", gopher_type_name(item.type));
    if (rc == GOPHER_ITEM_OK) {
        printf("host=%s\n", item.addr.host);
        printf("port=%u\n", (unsigned)item.addr.port);
        print_visible("selector", item.addr.selector);
        static const char *framings[] = { "menu", "text", "binary" };
        printf("framing=%s\n", framings[gopher_framing_for(item.type)]);
    }
    return 0;
}

static int do_lines(const char *chunkText, const char *unstuffText, const char *hex)
{
    static char data[1 << 20];
    size_t len = unhex(hex, data, sizeof(data));

    feeder_t f = { data, len, 0, (size_t)atoi(chunkText) };
    gopher_stream_t s;
    gopher_stream_init(&s, feed, &f);

    bool unstuff = atoi(unstuffText) != 0;
    char line[GOPHER_LINE_MAX];
    int count = 0;
    for (;;) {
        int rc = gopher_stream_line(&s, line, sizeof(line), unstuff);
        if (rc < 0) { printf("error\n"); break; }
        if (rc == 0) break;
        char label[32];
        snprintf(label, sizeof(label), "line%d", count++);
        print_visible(label, line);
    }
    printf("count=%d\n", count);
    printf("terminated=%d\n", s.terminated ? 1 : 0);
    printf("truncated=%d\n", s.truncated ? 1 : 0);
    printf("failed=%d\n", s.failed ? 1 : 0);
    return 0;
}

static int do_raw(const char *chunkText, const char *hex)
{
    static char data[1 << 20];
    size_t len = unhex(hex, data, sizeof(data));

    feeder_t f = { data, len, 0, (size_t)atoi(chunkText) };
    gopher_stream_t s;
    gopher_stream_init(&s, feed, &f);

    unsigned long total = 0;
    unsigned long sum = 0;
    char buf[4096];
    for (;;) {
        int64_t n = gopher_stream_raw(&s, buf, sizeof(buf));
        if (n < 0) { printf("error\n"); return 0; }
        if (n == 0) break;
        for (int64_t i = 0; i < n; i++)
            sum = (sum * 31 + (unsigned char)buf[i]) & 0xFFFFFFFF;
        total += (unsigned long)n;
    }
    printf("bytes=%lu\n", total);
    printf("hash=%lu\n", sum);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "url") == 0)
        return do_url(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "item") == 0)
        return do_item(argv[2]);
    if (argc >= 5 && strcmp(argv[1], "lines") == 0)
        return do_lines(argv[2], argv[3], argv[4]);
    if (argc >= 4 && strcmp(argv[1], "raw") == 0)
        return do_raw(argv[2], argv[3]);

    fprintf(stderr, "usage: %s url <text> | item <hex> |"
                    " lines <chunk> <unstuff> <hex> | raw <chunk> <hex>\n", argv[0]);
    return 2;
}
