// The arithmetic of a sound's bytes (userland/apps/yonder/media_bytes.c),
// on the host under ASan and UBSan: every seek plan over a grid of windows,
// offsets and states against a model written as the plan's table, the
// front drop against its definition, the head's readers, and the words a
// failed element is recorded in.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "media_bytes.h"

static int s_checks, s_failed;

static void check(int ok, const char *what)
{
    s_checks++;
    if (!ok) {
        s_failed++;
        printf("FAIL: %s\n", what);
    }
}

// The plan's table (Y23 lifecycle), read row by row.
static yonder_media_seek_t model_plan(uint64_t base, uint64_t have, uint64_t off, int opening, int ranges,
                                      int fetching, int held, int ended)
{
    uint64_t end = base + have;
    int inside = off >= base && off <= end;
    int before = off < base;
    uint64_t past = off > end ? off - end : 0;
    if (inside)
        return YONDER_MEDIA_SEEK_HERE;
    if (opening)
        return YONDER_MEDIA_SEEK_REFUSE;
    if (before)
        return ranges ? YONDER_MEDIA_SEEK_JUMP : YONDER_MEDIA_SEEK_REFUSE;
    if (ended)
        return YONDER_MEDIA_SEEK_REFUSE;
    if (ranges && !fetching && !held)
        return YONDER_MEDIA_SEEK_JUMP;
    if (ranges && past > YONDER_MEDIA_JUMP_GAP)
        return YONDER_MEDIA_SEEK_JUMP;
    if (fetching || held)
        return YONDER_MEDIA_SEEK_WAIT;
    return YONDER_MEDIA_SEEK_REFUSE;
}

static void plan_cases(void)
{
    const uint64_t g = YONDER_MEDIA_JUMP_GAP;
    const uint64_t bases[] = {0, 1, 1000, g, 3 * g};
    const uint64_t haves[] = {0, 1, 999, g, 2 * g + 7};
    int bad = 0;
    for (size_t bi = 0; bi < sizeof(bases) / sizeof(bases[0]); bi++)
        for (size_t hi = 0; hi < sizeof(haves) / sizeof(haves[0]); hi++) {
            uint64_t base = bases[bi], have = haves[hi], end = base + have;
            const uint64_t offs[] = {0, base ? base - 1 : 0, base, base + have / 2, end, end + 1,
                                     end + g - 1, end + g, end + g + 1, end + 10 * g, UINT64_MAX - 1};
            for (size_t oi = 0; oi < sizeof(offs) / sizeof(offs[0]); oi++)
                for (int bits = 0; bits < 32; bits++) {
                    yonder_media_window_t w = {base, have};
                    yonder_media_seek_state_t s = {bits & 1, (bits >> 1) & 1, (bits >> 2) & 1, (bits >> 3) & 1,
                                                   (bits >> 4) & 1};
                    yonder_media_seek_t got = yonder_media_seek_plan(&w, offs[oi], &s);
                    yonder_media_seek_t want = model_plan(base, have, offs[oi], s.opening, s.ranges, s.fetching,
                                                          s.held, s.ended);
                    if (got != want && bad++ < 5)
                        printf("  plan base=%llu have=%llu off=%llu bits=%d: %d, the table says %d\n",
                               (unsigned long long)base, (unsigned long long)have, (unsigned long long)offs[oi],
                               bits, got, want);
                }
        }
    check(bad == 0, "plan: every window, offset and state answers as the table does");

    yonder_media_window_t w = {0, 1000};
    yonder_media_seek_state_t opening = {1, 1, 1, 0, 0}, live = {0, 1, 1, 0, 0}, plain = {0, 0, 1, 0, 0};
    yonder_media_seek_state_t held = {0, 1, 0, 1, 0};
    check(yonder_media_seek_plan(&w, 1100, &held) == YONDER_MEDIA_SEEK_WAIT,
          "plan: a little past a held fetch's window waits for its resume, keeping the window");
    check(yonder_media_seek_plan(&w, 1 << 30, &opening) == YONDER_MEDIA_SEEK_REFUSE,
          "plan: the probe of the end while opening makes no request");
    check(yonder_media_seek_plan(&w, 1 << 30, &live) == YONDER_MEDIA_SEEK_JUMP,
          "plan: far past what has come, a server with ranges is asked from there");
    check(yonder_media_seek_plan(&w, 1 << 30, &plain) == YONDER_MEDIA_SEEK_WAIT,
          "plan: a server without them is waited for");
}

static void drop_cases(void)
{
    const uint64_t max = 100, behind = 10;
    int bad = 0;
    for (uint64_t base = 0; base < 30; base += 7)
        for (uint64_t have = 0; have <= max; have++)
            for (uint64_t at = base; at <= base + have; at += 3)
                for (uint64_t n = 0; n <= 40; n += 5) {
                    yonder_media_window_t w = {base, have};
                    uint64_t d = yonder_media_drop_for(&w, n, at, max, behind);
                    int ok;
                    if (have + n <= max) {
                        ok = d == 0;
                    } else {
                        uint64_t can = at - base > behind ? at - base - behind : 0;
                        if (can > have)
                            can = have;
                        int possible = have + n - can <= max;
                        ok = possible ? d != UINT64_MAX && have - d + n <= max && at - (base + d) >= behind
                                      : d == UINT64_MAX;
                    }
                    if (!ok && bad++ < 5)
                        printf("  drop base=%llu have=%llu at=%llu n=%llu: %llu\n", (unsigned long long)base,
                               (unsigned long long)have, (unsigned long long)at, (unsigned long long)n,
                               (unsigned long long)d);
                }
    check(bad == 0, "drop: room is made only from behind the reader, keeping what is owed before it");
    yonder_media_window_t big = {0, UINT64_MAX - 5};
    check(yonder_media_drop_for(&big, 10, UINT64_MAX - 5, UINT64_MAX - 1, 0) == UINT64_MAX - 5,
          "drop: a sum past the counter's range is counted, not wrapped (all of it may go)");
    check(yonder_media_drop_for(&big, 10, 2, UINT64_MAX - 1, 0) == UINT64_MAX,
          "drop: and is no room when little may go");
    yonder_media_window_t w = {0, 50};
    check(yonder_media_take_more(&w, 10, 64) && !yonder_media_take_more(&w, 10, 40) &&
              yonder_media_take_more(&w, 50, 1),
          "take: more while less than the lead is kept past the reader");
}

static void head_cases(void)
{
    const char *h = "Content-Type: audio/mpeg\r\ncontent-length: 2994349\r\nAccept-Ranges: none, Bytes\r\n";
    check(yonder_media_content_length(h, strlen(h)) == 2994349, "head: Content-Length, either case, CRLF lines");
    check(yonder_media_accepts_ranges(h, strlen(h)), "head: Accept-Ranges naming bytes among others");
    const char *none = "Accept-Ranges: none\nContent-Length: 12x\n";
    check(!yonder_media_accepts_ranges(none, strlen(none)) && yonder_media_content_length(none, strlen(none)) == -1,
          "head: none is no ranges, and a length that is not a number is none");
    const char *coded = "Content-Length: 100\nContent-Encoding: gzip\n";
    check(yonder_media_content_length(coded, strlen(coded)) == -1, "head: a coded body has no length here");
    uint64_t first = 0;
    int64_t total = 0;
    const char *r = "Content-Range: bytes 1048576-2994348/2994349 \r\n";
    check(yonder_media_content_range(r, strlen(r), &first, &total) && first == 1048576 && total == 2994349,
          "head: Content-Range's first byte and total");
    const char *star = "content-range: bytes 0-9/*\n";
    check(yonder_media_content_range(star, strlen(star), &first, &total) && first == 0 && total == -1,
          "head: an unknown total");
    const char *bad[] = {"Content-Range: bytes 9-0/10\n", "Content-Range: bytes 0-9/9\n",
                         "Content-Range: items 0-9/10\n", "Content-Range: bytes */10\n",
                         "Content-Range: bytes 0-9/99999999999999999999\n", "Content-Length: 5\n"};
    int refused = 1;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        refused &= !yonder_media_content_range(bad[i], strlen(bad[i]), &first, &total);
    check(refused, "head: a range backwards, past its total, of another unit, unsatisfied, too big, or absent is none");
    char v[64], w[64];
    const char *both = "Last-Modified: Sat, 15 Nov 2026 10:00:00 GMT\r\nETag: \"abc\"\r\n";
    yonder_media_validator(both, strlen(both), v, sizeof(v));
    check(!strcmp(v, "etag \"abc\""), "validator: the ETag when there is one");
    const char *modified = "last-modified: Sat, 15 Nov 2026 10:00:00 GMT\n";
    yonder_media_validator(modified, strlen(modified), w, sizeof(w));
    check(!strcmp(w, "modified Sat, 15 Nov 2026 10:00:00 GMT"), "validator: else Last-Modified, never equal to an ETag");
    yonder_media_validator("Content-Length: 3\n", 18, v, sizeof(v));
    char tiny[8];
    yonder_media_validator(both, strlen(both), tiny, sizeof(tiny));
    check(v[0] == '\0' && tiny[0] == '\0', "validator: none, or one that does not fit, is empty");
}

static void failure_cases(void)
{
    char type[32], small[5];
    const char *h = "Content-Length: 9\r\ncontent-TYPE:  text/html; charset=utf-8\r\n";
    yonder_media_content_type(h, strlen(h), type, sizeof(type));
    check(!strcmp(type, "text/html"), "type: the media type, its parameters and blanks left off");
    yonder_media_content_type(h, strlen(h), small, sizeof(small));
    yonder_media_content_type("Content-Length: 9\n", 18, type, sizeof(type));
    check(small[0] == '\0' && type[0] == '\0', "type: none, or one that does not fit, is empty");

    char line[256];
    yonder_media_failure_t refused = {"video 2", 4, "The server answered 403", true, 403, "text/html", 0, 0,
                                      "", "", "", "", false, false};
    yonder_media_failure_text(&refused, line, sizeof(line));
    check(!strcmp(line, "video 2, not supported: The server answered 403 (HTTP 403, text/html, 0 bytes "
                        "received, the first request)"),
          "failure: a refused fetch, its status, type, bytes and request");
    yonder_media_failure_t broken = {"audio 1", 2, "The connection failed part way", true, 206, "audio/mpeg", 1,
                                     1048576, "", "", "", "", false, false};
    yonder_media_failure_text(&broken, line, sizeof(line));
    check(!strcmp(line, "audio 1, network: The connection failed part way (HTTP 206, audio/mpeg, 1 byte received, "
                        "a Range request from byte 1048576)"),
          "failure: a Range request broken part way");
    yonder_media_failure_t notmedia = {"video 1", 4, "not a media file this can read", true, 200, "text/html", 2048, 0,
                                       "not a media file this can read", "", "", "", false, false};
    yonder_media_failure_text(&notmedia, line, sizeof(line));
    check(!strcmp(line, "video 1, not supported: not a media file this can read (HTTP 200, text/html, 2048 bytes "
                        "received, the first request)"),
          "failure: libmedia's words left out when they are the reason");
    yonder_media_failure_t codec = {"video 3", 4, "media in a form this cannot decode", false, 0, "", 0, 0,
                                    "media in a form this cannot decode", "mov", "aac", "hevc", false, true};
    yonder_media_failure_text(&codec, line, sizeof(line));
    check(!strcmp(line, "video 3, not supported: media in a form this cannot decode (a file; libmedia read it as "
                        "mov, sound aac, picture hevc (not decoded here))"),
          "failure: a file, with the container and codecs libmedia named");
    yonder_media_failure_t decode = {"video 4", 3, "", true, 0, "", 0, 0, "damaged past decoding", "", "", "",
                                     false, false};
    yonder_media_failure_text(&decode, line, sizeof(line));
    check(!strcmp(line, "video 4, decode: no reason given (no answer, 0 bytes received, the first request; "
                        "libmedia: damaged past decoding)"),
          "failure: no reason and no answer said so, libmedia's words kept");
    yonder_media_failure_t named = {"audio 3", 77, "", true, 200, "", 9, 0, "", "", "opus", "", true, false};
    yonder_media_failure_text(&named, line, sizeof(line));
    check(!strcmp(line, "audio 3, error 77: no reason given (HTTP 200, 9 bytes received, the first request; "
                        "libmedia named sound opus (not decoded here))"),
          "failure: an unknown class by its number, a codec named with nothing else said");
    const char *twice = "Content-Type: audio/mpeg\r\nContent-Type: video/mp4\r\n";
    yonder_media_content_type(twice, strlen(twice), type, sizeof(type));
    const char *long_last = "Content-Type: audio/mpeg\r\nContent-Type: application/x-a-very-long-made-up-type\r\n";
    yonder_media_content_type(long_last, strlen(long_last), small, sizeof(small));
    check(!strcmp(type, "video/mp4") && small[0] == '\0', "type: the last one, empty when the last does not fit");
    char cut[24];
    yonder_media_failure_text(&refused, cut, sizeof(cut));
    check(strlen(cut) == sizeof(cut) - 1 && !strncmp(cut, "video 2, not supported:", sizeof(cut) - 1),
          "failure: cut short to fit, and ended");
}

int main(void)
{
    plan_cases();
    drop_cases();
    head_cases();
    failure_cases();
    printf("media bytes host: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed != 0;
}
