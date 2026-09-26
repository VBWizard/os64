// scale.c — a picture into its box (scale.h).

#include "scale.h"

static int32_t max32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

static int32_t min32(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

// One channel of `s` over `d` at alpha `a` (0..255), rounded.
static uint32_t over(uint32_t s, uint32_t d, uint32_t a)
{
    return (s * a + d * (255 - a) + 127) / 255;
}

void yonder_draw_picture(uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip, os64_gui_rect_t box,
                         const uint32_t *src, uint32_t sw, uint32_t sh)
{
    if (box.w <= 0 || box.h <= 0 || sw == 0 || sh == 0)
        return;
    int32_t x0 = max32(box.x, clip.x), y0 = max32(box.y, clip.y);
    int32_t x1 = min32(box.x + box.w, clip.x + clip.w);
    int32_t y1 = min32(box.y + box.h, clip.y + clip.h);
    for (int32_t y = y0; y < y1; y++) {
        // The source row whose span covers the middle of this one:
        // floor((y - box.y + 1/2) * sh / box.h), in integers.
        uint32_t sy = (uint32_t)(((int64_t)(y - box.y) * 2 + 1) * sh / ((int64_t)box.h * 2));
        const uint32_t *row = src + (uint64_t)sy * sw;
        uint32_t *out = dst + (uint64_t)y * pitch;
        for (int32_t x = x0; x < x1; x++) {
            uint32_t sx = (uint32_t)(((int64_t)(x - box.x) * 2 + 1) * sw / ((int64_t)box.w * 2));
            uint32_t p = row[sx], a = p >> 24;
            if (a == 0)
                continue;
            if (a == 255) {
                out[x] = p;
                continue;
            }
            uint32_t d = out[x];
            out[x] = 0xff000000u | over((p >> 16) & 0xff, (d >> 16) & 0xff, a) << 16 |
                     over((p >> 8) & 0xff, (d >> 8) & 0xff, a) << 8 | over(p & 0xff, d & 0xff, a);
        }
    }
}
