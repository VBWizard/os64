#include "gui/surface.h"
#include "printd.h"
#include "CONFIG.h"

// surface_blit against its contract, pixel by pixel: a destination pixel
// takes the source pixel it lands on when that pixel is inside both
// src_rect and the source surface, and keeps what it had otherwise. The
// cases put rows on both sides of memcpy's string-copy floor, at odd
// offsets, hanging off every edge of both surfaces, on surfaces whose pitch
// is wider than their width.

#define BLIT_SENTINEL 0x00FE01FDu
#define BLIT_SRC_W 300
#define BLIT_SRC_H 40
#define BLIT_DST_W 333
#define BLIT_DST_H 50

static uint32_t blit_source_pixel(int32_t x, int32_t y)
{
    return 0x01000000u | ((uint32_t)y << 12) | (uint32_t)x;
}

bool test_surface_blit(void)
{
    static const struct {
        rect_t src_rect;
        int32_t dx, dy;
    } cases[] = {
        {{0, 0, BLIT_SRC_W, BLIT_SRC_H}, 0, 0},       // whole, wide rows
        {{3, 1, 7, 5}, 11, 13},                        // narrow, odd everywhere
        {{1, 2, 200, 9}, 5, 3},                        // wide, unaligned
        {{0, 0, 100, 10}, -7, -3},                     // hangs off the top left
        {{10, 5, 290, 30}, 300, 40},                   // hangs off the bottom right
        {{-20, -4, 60, 12}, 2, 2},                     // src_rect starts outside src
        {{250, 30, 100, 30}, 1, 1},                    // src_rect ends outside src
        {{0, 0, 129, 2}, 64, 0},                       // just over the floor
        {{0, 0, 127, 2}, 65, 0},                       // just under it
        {{5, 5, 10, 10}, 400, 10},                     // lands wholly off dst
        {{0, 0, 0, 5}, 3, 3},                          // empty
    };
    surface_t src, dst;
    if (surface_init_capacity(&src, BLIT_SRC_W, BLIT_SRC_H, BLIT_SRC_W + 13, BLIT_SRC_H) != 0) {
        printd(DEBUG_TESTS, "\tFAIL: test_surface_blit - no memory\n");
        return false;
    }
    if (surface_init_capacity(&dst, BLIT_DST_W, BLIT_DST_H, BLIT_DST_W + 7, BLIT_DST_H) != 0) {
        surface_free(&src);
        printd(DEBUG_TESTS, "\tFAIL: test_surface_blit - no memory\n");
        return false;
    }
    for (int32_t y = 0; y < BLIT_SRC_H; y++)
        for (int32_t x = 0; x < BLIT_SRC_W; x++)
            src.pixels[(size_t)y * src.pitch_px + x] = blit_source_pixel(x, y);

    bool ok = true;
    for (size_t k = 0; ok && k < sizeof(cases) / sizeof(cases[0]); k++) {
        rect_t r = cases[k].src_rect;
        int32_t dx = cases[k].dx, dy = cases[k].dy;
        for (int32_t y = 0; y < BLIT_DST_H; y++)
            for (int32_t x = 0; x < (int32_t)dst.pitch_px; x++)
                dst.pixels[(size_t)y * dst.pitch_px + x] = BLIT_SENTINEL;
        surface_blit(&dst, dx, dy, &src, r);
        for (int32_t y = 0; ok && y < BLIT_DST_H; y++)
            for (int32_t x = 0; ok && x < (int32_t)dst.pitch_px; x++) {
                int32_t sx = x - dx + r.x, sy = y - dy + r.y;
                bool lands = x < BLIT_DST_W
                    && sx >= r.x && sx < r.x + r.w && sy >= r.y && sy < r.y + r.h
                    && sx >= 0 && sx < BLIT_SRC_W && sy >= 0 && sy < BLIT_SRC_H;
                uint32_t want = lands ? blit_source_pixel(sx, sy) : BLIT_SENTINEL;
                uint32_t got = dst.pixels[(size_t)y * dst.pitch_px + x];
                if (got != want) {
                    printd(DEBUG_TESTS, "\tFAIL: test_surface_blit - case %u: (%d,%d) is 0x%08x, want 0x%08x\n",
                           (unsigned)k, x, y, got, want);
                    ok = false;
                }
            }
    }
    surface_free(&dst);
    surface_free(&src);
    if (ok)
        printd(DEBUG_TESTS, "\tPASS: test_surface_blit (%u placements)\n",
               (unsigned)(sizeof(cases) / sizeof(cases[0])));
    return ok;
}
