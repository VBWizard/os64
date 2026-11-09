// The occlusion geometry (kernel/src/gui/visible.c) against a pixel oracle
// (OCCLUSION.md § Proof): for random rectangles and random frame stacks,
// every pixel's owner is worked out by brute force (the first frame,
// top-down, that contains it) and read back through the pieces, and the
// two must agree everywhere. Plus the named scenes the design reasons
// about. Run by tools/test_visible_host.sh under ASan and UBSan.

#include "gui/visible.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;

static void expect(const char *name, bool ok)
{
    checks++;
    if (!ok) {
        failures++;
        fprintf(stderr, "FAIL %s\n", name);
    }
}

static uint64_t s_seed = 0x9e3779b97f4a7c15ull;
static int32_t rnd(int32_t lo, int32_t hi)
{
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 7;
    s_seed ^= s_seed << 17;
    return lo + (int32_t)(s_seed % (uint64_t)(hi - lo + 1));
}

static bool inside(visible_rect_t r, int32_t x, int32_t y)
{
    return r.w > 0 && r.h > 0 && x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

// The grid the oracle paints: one cell per pixel of D, at most this side.
#define SIDE 96

// Checks one call against brute force. `name` is the case's, for a
// failure; answers whether every check passed.
static bool verify(const char *name, visible_rect_t d, const visible_rect_t *frames, int32_t n,
                   int32_t cap)
{
    // Exactly the room promised, on the heap, so a write past it is ASan's.
    int32_t limit = cap < VISIBLE_PIECES_MAX ? cap : VISIBLE_PIECES_MAX;
    visible_piece_t *pieces = malloc(sizeof(*pieces) * (size_t)limit);
    int32_t consumed = -1;
    memset(pieces, 0xa5, sizeof(*pieces) * (size_t)limit);
    int32_t np = visible_claim(d, frames, n, pieces, cap, &consumed);
    bool ok = np >= 0 && np <= limit && consumed >= 0 && consumed <= n;
    // A first frame always fits when there is room for a claim and its four
    // cuts, so a walk with that room never stops before it.
    if (n > 0 && limit >= 5 && d.w > 0 && d.h > 0)
        ok = ok && consumed >= 1;
    // Every piece inside D and not empty; every claimed piece inside the
    // frame that owns it, and that frame one of the consumed.
    for (int32_t i = 0; ok && i < np; i++) {
        visible_rect_t r = pieces[i].r;
        ok = r.w > 0 && r.h > 0 && r.x >= d.x && r.y >= d.y && r.x + r.w <= d.x + d.w &&
             r.y + r.h <= d.y + d.h;
        if (ok && pieces[i].owner != VISIBLE_UNCLAIMED) {
            visible_rect_t f = frames[pieces[i].owner];
            ok = pieces[i].owner >= 0 && pieces[i].owner < consumed && r.x >= f.x && r.y >= f.y &&
                 r.x + r.w <= f.x + f.w && r.y + r.h <= f.y + f.h;
        }
    }
    // Each pixel of D in exactly one piece, and owned as brute force says:
    // the first consumed frame containing it, or unclaimed.
    static int16_t hits[SIDE][SIDE], owner[SIDE][SIDE];
    if (ok && d.w > 0 && d.h > 0) {
        memset(hits, 0, sizeof(hits));
        for (int32_t i = 0; i < np; i++)
            for (int32_t y = pieces[i].r.y; y < pieces[i].r.y + pieces[i].r.h; y++)
                for (int32_t x = pieces[i].r.x; x < pieces[i].r.x + pieces[i].r.w; x++) {
                    hits[y - d.y][x - d.x]++;
                    owner[y - d.y][x - d.x] = (int16_t)pieces[i].owner;
                }
        for (int32_t y = d.y; ok && y < d.y + d.h; y++)
            for (int32_t x = d.x; ok && x < d.x + d.w; x++) {
                int32_t want = VISIBLE_UNCLAIMED;
                for (int32_t f = 0; f < consumed; f++)
                    if (inside(frames[f], x, y)) {
                        want = f;
                        break;
                    }
                ok = hits[y - d.y][x - d.x] == 1 && owner[y - d.y][x - d.x] == want;
            }
    } else if (ok) {
        ok = np == 0;   // an empty rectangle has no pieces
    }
    free(pieces);
    expect(name, ok);
    return ok;
}

static visible_rect_t R(int32_t x, int32_t y, int32_t w, int32_t h)
{
    return (visible_rect_t){x, y, w, h};
}

static int32_t count_owned(const visible_piece_t *p, int32_t n, int32_t owner)
{
    int32_t c = 0;
    for (int32_t i = 0; i < n; i++)
        c += p[i].owner == owner;
    return c;
}

static void scenes(void)
{
    visible_piece_t p[VISIBLE_PIECES_MAX];
    int32_t consumed;
    // A: yonder on top of a window behind it and the fullscreen desktop.
    visible_rect_t a[] = {R(10, 10, 60, 40), R(30, 20, 60, 60), R(0, 0, 96, 96)};
    verify("scene A: three layers, each pixel to the top one", R(0, 0, 96, 96), a, 3, VISIBLE_PIECES_MAX);
    int32_t n = visible_claim(R(20, 20, 20, 20), a, 3, p, VISIBLE_PIECES_MAX, &consumed);
    expect("scene A: a rectangle inside the top window is that window's, whole",
           n == 1 && p[0].owner == 0 && consumed == 3);
    // B: a cover over most of a window, a sliver of it showing.
    visible_rect_t b[] = {R(0, 0, 80, 96), R(0, 0, 96, 96)};
    n = visible_claim(R(0, 0, 96, 96), b, 2, p, VISIBLE_PIECES_MAX, &consumed);
    expect("scene B: the cover claims its part, the window the sliver",
           n == 2 && count_owned(p, n, 0) == 1 && count_owned(p, n, 1) == 1);
    verify("scene B", R(0, 0, 96, 96), b, 2, VISIBLE_PIECES_MAX);
    // The ring: a cover in the middle of the rectangle leaves four pieces.
    visible_rect_t ring[] = {R(30, 30, 20, 20)};
    n = visible_claim(R(10, 10, 60, 60), ring, 1, p, VISIBLE_PIECES_MAX, &consumed);
    expect("the ring: one claim, four unclaimed around it",
           n == 5 && count_owned(p, n, 0) == 1 && count_owned(p, n, VISIBLE_UNCLAIMED) == 4);
    // Identical frames: the lower one claims nothing.
    visible_rect_t twins[] = {R(5, 5, 40, 40), R(5, 5, 40, 40)};
    n = visible_claim(R(0, 0, 50, 50), twins, 2, p, VISIBLE_PIECES_MAX, &consumed);
    expect("identical frames: only the top one claims", count_owned(p, n, 1) == 0);
    verify("identical frames", R(0, 0, 50, 50), twins, 2, VISIBLE_PIECES_MAX);
    // Nested, off-screen, one pixel wide, and nothing at all.
    visible_rect_t nest[] = {R(20, 20, 5, 5), R(10, 10, 30, 30), R(-1000000, -1000000, 1000010, 1000010),
                             R(40, 0, 1, 96)};
    verify("nested, off-screen and one pixel wide", R(0, 0, 96, 96), nest, 4, VISIBLE_PIECES_MAX);
    verify("no frames: all unclaimed", R(3, 4, 50, 60), NULL, 0, VISIBLE_PIECES_MAX);
    verify("an empty rectangle has no pieces", R(10, 10, 0, 40), a, 3, VISIBLE_PIECES_MAX);
    // A frame far past int32's edge arithmetic: edges are summed in 64 bits.
    visible_rect_t far[] = {R(2147483000, 2147483000, 2000, 2000), R(-2147483000, 0, 100, 100)};
    verify("frames at the edge of int32", R(0, 0, 96, 96), far, 2, VISIBLE_PIECES_MAX);
    // The cap: a stack that needs more pieces than the cap stops before the
    // frame that would overflow it, and what it leaves is what is unclaimed.
    visible_rect_t dots[40];
    for (int32_t i = 0; i < 40; i++)
        dots[i] = R(4 + (i % 8) * 11, 4 + (i / 8) * 18, 3, 3);
    n = visible_claim(R(0, 0, 96, 96), dots, 40, p, 16, &consumed);
    expect("the cap: the walk stops before overflowing", n <= 16 && consumed < 40);
    verify("the cap trips: consumed frames own, the rest is unclaimed", R(0, 0, 96, 96), dots, 40, 16);
    verify("the cap at its smallest useful size", R(0, 0, 96, 96), dots, 40, 5);
    // A cap past the piece arrays is held to them.
    visible_rect_t grid[100];
    for (int32_t i = 0; i < 100; i++)
        grid[i] = R(2 + (i % 10) * 9, 2 + (i / 10) * 9, 3, 3);
    verify("a cap larger than the arrays: at most VISIBLE_PIECES_MAX", R(0, 0, 96, 96), grid, 100, 1000);
}

// Small holes and bands across them: each hole leaves up to four pieces for
// one claim, and a band cuts every piece it crosses, so the next set
// outgrows the arrays partway through a frame. Plain random stacks almost
// never get there. Holes first is what fills the arrays; mixed is what
// makes a hole's four-piece cut arrive when they are nearly full.
static void fill_dots_and_bands(visible_rect_t *frames, int32_t *n)
{
    int32_t dots = rnd(8, 21), bands = rnd(1, 4);
    *n = 0;
    for (int32_t i = 0; i < dots; i++)
        frames[(*n)++] = R(rnd(0, 89), rnd(0, 89), rnd(1, 3), rnd(1, 3));
    for (int32_t i = 0; i < bands; i++)
        frames[(*n)++] = rnd(0, 1) ? R(0, rnd(0, 89), 96, rnd(1, 3)) : R(rnd(0, 89), 0, rnd(1, 3), 96);
    if (rnd(0, 1))
        for (int32_t i = *n - 1; i > 0; i--) {
            int32_t j = rnd(0, i);
            visible_rect_t t = frames[i];
            frames[i] = frames[j];
            frames[j] = t;
        }
}

static void fuzz(int32_t rounds)
{
    visible_rect_t frames[25];
    for (int32_t round = 0; round < rounds; round++) {
        visible_rect_t d = R(rnd(-20, 40), rnd(-20, 40), rnd(0, SIDE), rnd(0, SIDE));
        int32_t n = rnd(0, 24);
        if (round % 4 == 0) {
            d = R(0, 0, SIDE, SIDE);
            fill_dots_and_bands(frames, &n);
        } else for (int32_t i = 0; i < n; i++) {
            int32_t kind = rnd(0, 9);
            if (kind == 0)
                frames[i] = d;                                    // exactly D
            else if (kind == 1 && i > 0)
                frames[i] = frames[rnd(0, i - 1)];                // a twin above
            else if (kind == 2)
                frames[i] = R(rnd(-40, 140), rnd(-40, 140), 1, rnd(1, 120));   // one pixel wide
            else if (kind == 3)
                frames[i] = R(rnd(-40, 140), rnd(-40, 140), rnd(-5, 0), rnd(1, 50));   // empty
            else
                frames[i] = R(rnd(-40, 140), rnd(-40, 140), rnd(1, 80), rnd(1, 80));
        }
        int32_t cap = rnd(0, 3) == 0 ? rnd(1, VISIBLE_PIECES_MAX) : VISIBLE_PIECES_MAX;
        char name[64];
        snprintf(name, sizeof(name), "fuzz round %d", round);
        if (!verify(name, d, frames, n, cap))
            return;   // one is enough to read
    }
}

int main(int argc, char **argv)
{
    int32_t rounds = argc > 1 ? atoi(argv[1]) : 20000;
    scenes();
    fuzz(rounds);
    printf("visible: %d checks, %d failed (%d random stacks)\n", checks, failures, rounds);
    return failures ? 1 : 0;
}
