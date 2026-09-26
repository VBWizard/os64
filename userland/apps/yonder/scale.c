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

// One source pixel onto one destination pixel, by its alpha.
static void put(uint32_t *out, uint32_t p)
{
    uint32_t a = p >> 24;
    if (a == 0)
        return;
    if (a == 255) {
        *out = p;
        return;
    }
    uint32_t d = *out;
    *out = 0xff000000u | over((p >> 16) & 0xff, (d >> 16) & 0xff, a) << 16 |
           over((p >> 8) & 0xff, (d >> 8) & 0xff, a) << 8 | over(p & 0xff, d & 0xff, a);
}

// `v` modulo `m` for m > 0, never negative.
static uint32_t wrap(int64_t v, uint32_t m)
{
    int64_t r = v % (int64_t)m;
    return (uint32_t)(r < 0 ? r + m : r);
}

void yonder_tile_picture(uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip, os64_gui_rect_t area,
                         int32_t ox, int32_t oy, const uint32_t *src, uint32_t sw, uint32_t sh)
{
    if (area.w <= 0 || area.h <= 0 || sw == 0 || sh == 0)
        return;
    int32_t x0 = max32(area.x, clip.x), y0 = max32(area.y, clip.y);
    int32_t x1 = min32(area.x + area.w, clip.x + clip.w);
    int32_t y1 = min32(area.y + area.h, clip.y + clip.h);
    for (int32_t y = y0; y < y1; y++) {
        const uint32_t *row = src + (uint64_t)wrap((int64_t)y - oy, sh) * sw;
        uint32_t *out = dst + (uint64_t)y * pitch;
        uint32_t sx = x0 < x1 ? wrap((int64_t)x0 - ox, sw) : 0;
        for (int32_t x = x0; x < x1; x++) {
            put(&out[x], row[sx]);
            if (++sx == sw)
                sx = 0;
        }
    }
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
            put(&out[x], row[sx]);
        }
    }
}
