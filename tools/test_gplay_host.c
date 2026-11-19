// test_gplay_host.c — gplay's arithmetic that needs no machine
// (userland/apps/gplay/view.c), under ASan and UBSan: the letterbox, the
// first window size, the time text, the keys, and where a seek lands.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "view.h"

static int s_failures, s_checks;
#define CHECK(cond, ...) do { s_checks++; if (!(cond)) { s_failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); } } while (0)

static void letterbox(void)
{
    gplay_rect_t r = gplay_letterbox(1920, 1080, 1920, 1080);
    CHECK(r.x == 0 && r.y == 0 && r.w == 1920 && r.h == 1080, "the same shape fills the box");
    r = gplay_letterbox(1920, 1080, 1000, 1000);
    CHECK(r.w == 1000 && r.h == 563 && r.x == 0 && r.y == 218, "wider than the box: bars above and below (%d,%d %ux%u)",
          r.x, r.y, r.w, r.h);
    r = gplay_letterbox(1080, 1920, 1000, 1000);
    CHECK(r.h == 1000 && r.w == 563 && r.y == 0 && r.x == 218, "taller: bars beside");
    r = gplay_letterbox(640, 360, 2560, 1440);
    CHECK(r.w == 2560 && r.h == 1440, "enlarged to fill");
    r = gplay_letterbox(0, 360, 100, 100);
    CHECK(r.w == 0 && r.h == 0, "an empty picture: nothing");
    r = gplay_letterbox(640, 360, 0, 100);
    CHECK(r.w == 0 && r.h == 0, "an empty box: nothing");
    r = gplay_letterbox(100000, 1, 50, 50);
    CHECK(r.w == 50 && r.h == 1 && r.y == 24, "a sliver keeps one row");
    r = gplay_letterbox(4096, 4096, 3, 2);
    CHECK(r.w == 2 && r.h == 2 && r.x == 0, "a square in a tiny box");
    // Every result fits the box and keeps the picture's shape to within a pixel.
    for (uint32_t i = 0; i < 2000; i++) {
        uint32_t sw = 1 + (uint32_t)rand() % 4000, sh = 1 + (uint32_t)rand() % 4000;
        uint32_t bw = 1 + (uint32_t)rand() % 3000, bh = 1 + (uint32_t)rand() % 3000;
        r = gplay_letterbox(sw, sh, bw, bh);
        CHECK(r.w >= 1 && r.h >= 1 && r.w <= bw && r.h <= bh && r.x >= 0 && r.y >= 0 &&
              (uint32_t)r.x + r.w <= bw && (uint32_t)r.y + r.h <= bh, "fits the box");
        CHECK(r.w == bw || r.h == bh, "touches two sides");
        // The side that does not fill the box is the exact one rounded.
        double exact = r.w == bw ? (double)bw * sh / sw : (double)bh * sw / sh;
        double side = r.w == bw ? r.h : r.w;
        CHECK((side - exact <= 0.5 + 1e-9 && exact - side <= 0.5 + 1e-9) || side == 1,
              "keeps its shape (%ux%u in %ux%u gave %ux%u)", sw, sh, bw, bh, r.w, r.h);
        CHECK((uint32_t)r.x == (bw - r.w) / 2 && (uint32_t)r.y == (bh - r.h) / 2, "centred");
    }
}

static void first_size(void)
{
    uint32_t w, h;
    gplay_first_size(1280, 720, 2560, 1440, 48, &w, &h);
    CHECK(w == 1280 && h == 720, "a picture that fits keeps its size");
    gplay_first_size(2560, 1440, 2560, 1440, 48, &w, &h);
    CHECK(w <= 2560 - 96 && h <= 1440 - 96 && (w == 2464 || h == 1344), "a picture the screen's size shrinks (%ux%u)",
          w, h);
    gplay_first_size(3840, 2160, 1024, 768, 48, &w, &h);
    CHECK(w == 928 && h == 522, "a 4K picture on a small screen (%ux%u)", w, h);
    gplay_first_size(640, 360, 50, 50, 48, &w, &h);
    CHECK(w >= 1 && h >= 1, "a screen smaller than its margins still gives a window");
}

static void time_text(void)
{
    char t[16];
    CHECK(gplay_time_text(0, t, sizeof(t)) == 4 && strcmp(t, "0:00") == 0, "zero");
    gplay_time_text(48000 * 59 + 47999, t, sizeof(t));
    CHECK(strcmp(t, "0:59") == 0, "a second is whole until it ends (%s)", t);
    gplay_time_text(48000 * 61, t, sizeof(t));
    CHECK(strcmp(t, "1:01") == 0, "%s", t);
    gplay_time_text(48000LL * 3599, t, sizeof(t));
    CHECK(strcmp(t, "59:59") == 0, "%s", t);
    gplay_time_text(48000LL * 3600, t, sizeof(t));
    CHECK(strcmp(t, "1:00:00") == 0, "an hour (%s)", t);
    gplay_time_text(48000LL * (36000 + 61), t, sizeof(t));
    CHECK(strcmp(t, "10:01:01") == 0, "%s", t);
    gplay_time_text(-5, t, sizeof(t));
    CHECK(strcmp(t, "0:00") == 0, "below 0 reads as 0");
    char small[4];
    CHECK(gplay_time_text(48000 * 61, small, sizeof(small)) == 3 && strcmp(small, "1:0") == 0, "cut to the room");
    CHECK(gplay_time_text(48000, small, 0) == 0, "no room at all");
}

static void keys(void)
{
    CHECK(gplay_key(' ', 0x39, false) == GPLAY_KEY_PAUSE, "space");
    CHECK(gplay_key(0, 0x4B, false) == GPLAY_KEY_BACK && gplay_key(0, 0x4B, true) == GPLAY_KEY_BACK_FAR, "left");
    CHECK(gplay_key(0, 0x4D, false) == GPLAY_KEY_FORWARD && gplay_key(0, 0x4D, true) == GPLAY_KEY_FORWARD_FAR,
          "right");
    CHECK(gplay_key(0, 0x48, false) == GPLAY_KEY_LOUDER && gplay_key(0, 0x50, false) == GPLAY_KEY_SOFTER, "up, down");
    CHECK(gplay_key('m', 0x32, false) == GPLAY_KEY_MUTE && gplay_key('M', 0x32, true) == GPLAY_KEY_MUTE, "m");
    CHECK(gplay_key('i', 0x17, false) == GPLAY_KEY_READOUT, "i");
    CHECK(gplay_key('[', 0x1A, false) == GPLAY_KEY_SOUND_EARLIER && gplay_key(']', 0x1B, false) == GPLAY_KEY_SOUND_LATER,
          "the sync dial");
    CHECK(gplay_key('q', 0x10, false) == GPLAY_KEY_QUIT && gplay_key(27, 0x01, false) == GPLAY_KEY_QUIT &&
          gplay_key(0, 0x01, false) == GPLAY_KEY_QUIT, "q and Escape");
    CHECK(gplay_key('x', 0x2D, false) == GPLAY_KEY_NONE && gplay_key(0, 0x3B, false) == GPLAY_KEY_NONE, "others");
}

static void seeks(void)
{
    CHECK(gplay_seek_target(48000 * 10, 48000 * 5, 48000 * 60) == 48000 * 15, "forward");
    CHECK(gplay_seek_target(48000 * 3, -48000 * 5, 48000 * 60) == 0, "back past the start: the start");
    CHECK(gplay_seek_target(48000 * 58, 48000 * 5, 48000 * 60) == 48000 * 60 - 1, "past the end: its last frame");
    CHECK(gplay_seek_target(48000 * 58, 48000 * 5, -1) == 48000 * 63, "length unknown: not held");
    int64_t t = 0;
    for (int i = 0; i < 5; i++)
        t = gplay_seek_target(t, 48000 * 5, 48000 * 600);
    CHECK(t == 48000 * 25, "five presses from a pending target move 25 s");
}

int main(void)
{
    letterbox();
    first_size();
    time_text();
    keys();
    seeks();
    if (s_failures) {
        fprintf(stderr, "FAIL gplay view: %d of %d checks\n", s_failures, s_checks);
        return 1;
    }
    printf("PASS gplay view: %d checks\n", s_checks);
    return 0;
}
