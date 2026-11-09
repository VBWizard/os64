// test_ftp_host.c — drive /bin/ftp's wire parser from the host.
//
// The harness binary. tools/test_ftp_host.sh is the suite that runs it; this
// file only turns argv plus stdin into one call and prints what came back in
// `key=value` fields, so the expectations live in one place (the script) and
// the plumbing lives in another.
//
// INPUT ARRIVES ON STDIN, not in argv, because every case that matters here
// is a byte a shell cannot pass: a reply is made of CRLFs, a banner under
// test carries a line that looks like a reply, and the consume ceiling needs
// 64KB of them.

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

// Print a string with the bytes a terminal obeys spelled out, so the suite's
// diagnostics cannot repaint the screen of whoever is reading them — and so a
// reply's embedded newlines survive a line-oriented field format.
static void print_visible(const char *label, const char *s)
{
    printf("%s=", label);
    for (; *s != '\0'; s++) {
        unsigned char b = (unsigned char)*s;
        if (b == '\\')
            printf("\\\\");
        else if (b < 0x20 || b == 0x7F)
            printf("\\x%02x", b);
        else
            putchar(*s);
    }
}

// ── A source that hands out `chunk` bytes at a time ─────────────────────
// The whole point of the source function: a parser's bugs live where a token
// straddles two reads, and a multiline reply is a token that can straddle a
// hundred of them.

typedef struct {
    const char *data;
    size_t      len;
    size_t      pos;
    size_t      chunk;      // 0 = hand over whatever is left
    // What the feeder says when the data runs out. False is end of input; true
    // is a deadline expiring, which is a quiet peer rather than a closed one —
    // the difference the drain after an interrupted transfer depends on.
    bool        stallAtEnd;
} feeder_t;

static int64_t feed(void *ctx, void *buf, size_t cap)
{
    feeder_t *f = ctx;
    size_t left = f->len - f->pos;
    if (left == 0)
        return f->stallAtEnd ? FTP_SOURCE_STALLED : 0;
    size_t take = left < cap ? left : cap;
    if (f->chunk != 0 && take > f->chunk)
        take = f->chunk;
    memcpy(buf, f->data + f->pos, take);
    f->pos += take;
    return (int64_t)take;
}

// The whole of stdin, with one spare byte past the end so a caller that wants
// a C string can plant a NUL there without a second allocation.
static char *slurp_stdin(size_t *outLen)
{
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf) { perror("malloc"); exit(2); }

    for (;;) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *grown = realloc(buf, cap);
            if (!grown) { perror("realloc"); exit(2); }
            buf = grown;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, stdin);
        len += n;
        if (n == 0)
            break;
    }
    *outLen = len;
    return buf;
}

// The text modes take ONE line, so the newline the shell adds is not part of
// the case. `reply` is the opposite — its terminators ARE the case — which is
// why the trim happens here and not in slurp_stdin.
static char *as_line(char *data, size_t len)
{
    while (len > 0 && (data[len - 1] == '\n' || data[len - 1] == '\r'))
        len--;
    data[len] = '\0';
    return data;
}

static const char *reply_result_name(ftp_reply_result_t rc)
{
    switch (rc) {
        case FTP_REPLY_OK:        return "OK";
        case FTP_REPLY_END:       return "END";
        case FTP_REPLY_FAILED:    return "FAILED";
        case FTP_REPLY_MALFORMED: return "MALFORMED";
        case FTP_REPLY_TOO_LONG:  return "TOO_LONG";
        case FTP_REPLY_STALLED:   return "STALLED";
    }
    return "?";
}

static const char *pasv_result_name(ftp_pasv_result_t rc)
{
    switch (rc) {
        case FTP_PASV_OK:        return "OK";
        case FTP_PASV_NO_TUPLE:  return "NO_TUPLE";
        case FTP_PASV_RANGE:     return "RANGE";
        case FTP_PASV_PORT_ZERO: return "PORT_ZERO";
    }
    return "?";
}

static const char *command_result_name(ftp_command_result_t rc)
{
    switch (rc) {
        case FTP_COMMAND_OK:       return "OK";
        case FTP_COMMAND_TOO_LONG: return "TOO_LONG";
        case FTP_COMMAND_NEWLINE:  return "NEWLINE";
    }
    return "?";
}

// Read every reply the input holds, one field line each, then the verdict that
// ended the stream. Reading them ALL is the point: a desync shows up as the
// SECOND reply, never the first.
static int do_replies(size_t chunk, const char *data, size_t len, bool stall)
{
    feeder_t f = { .data = data, .len = len, .pos = 0, .chunk = chunk,
                   .stallAtEnd = stall };
    ftp_control_t c;
    ftp_control_init(&c, feed, &f);

    for (int n = 1;; n++) {
        ftp_reply_t reply;
        ftp_reply_result_t rc = ftp_reply_read(&c, &reply);
        if (rc != FTP_REPLY_OK) {
            // Ask the SAME question twice. A stall must leave the channel
            // usable, so the second answer has to match the first; a channel
            // poisoned by its own deadline answers FAILED the second time,
            // which is the bug this line exists to catch.
            ftp_reply_t again;
            ftp_reply_result_t twice = ftp_reply_read(&c, &again);
            printf("end=%s again=%s drained=%d\n", reply_result_name(rc),
                   reply_result_name(twice), ftp_control_drained(&c) ? 1 : 0);
            return 0;
        }
        printf("n=%d code=%d lines=%zu truncated=%d ",
               n, reply.code, reply.lines, reply.truncated ? 1 : 0);
        print_visible("text", reply.text);
        putchar('\n');
    }
}

static int do_pasv(const char *text)
{
    uint32_t ip = 0;
    uint16_t port = 0;
    ftp_pasv_result_t rc = ftp_pasv_parse(text, &ip, &port);

    printf("result=%s ip=%u.%u.%u.%u port=%u\n", pasv_result_name(rc),
           (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
           port);
    return 0;
}

static int do_path257(const char *text)
{
    char path[FTP_PATH_MAX];
    bool ok = ftp_path_from_257(text, path, sizeof(path));

    printf("result=%s ", ok ? "OK" : "NONE");
    print_visible("path", ok ? path : "");
    putchar('\n');
    return 0;
}

static int do_command(const char *verb, const char *arg)
{
    char out[FTP_COMMAND_MAX];
    size_t written = 0;
    ftp_command_result_t rc = ftp_command(verb, arg, out, sizeof(out), &written);

    printf("result=%s written=%zu ", command_result_name(rc), written);
    print_visible("wire", rc == FTP_COMMAND_OK ? out : "");
    putchar('\n');
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s reply|replystall <chunk> | pasv | path257 "
                        "| command <verb>\n", argv[0]);
        return 2;
    }

    size_t len = 0;
    char *data = slurp_stdin(&len);

    int rc;
    if (strcmp(argv[1], "reply") == 0 && argc >= 3)
        rc = do_replies((size_t)strtoul(argv[2], NULL, 10), data, len, false);
    else if (strcmp(argv[1], "replystall") == 0 && argc >= 3)
        rc = do_replies((size_t)strtoul(argv[2], NULL, 10), data, len, true);
    else if (strcmp(argv[1], "pasv") == 0)
        rc = do_pasv(as_line(data, len));
    else if (strcmp(argv[1], "path257") == 0)
        rc = do_path257(as_line(data, len));
    else if (strcmp(argv[1], "command") == 0 && argc >= 3)
        rc = do_command(argv[2], as_line(data, len));
    else {
        fprintf(stderr, "%s: unknown mode\n", argv[0]);
        rc = 2;
    }

    free(data);
    return rc;
}
