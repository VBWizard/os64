// scale.c — a picture into its box (scale.h).

#include "scale.h"

static int64_t max64(int64_t a, int64_t b)
{
    return a > b ? a : b;
}

static int64_t min64(int64_t a, int64_t b)
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

// Which of `n` source pixels lands at `u` of a copy `size` across: the one
// whose span covers u's middle, floor((u + 1/2) * n / size).
static uint32_t source_at(uint32_t u, uint32_t n, int32_t size)
{
    return (uint32_t)(((int64_t)u * 2 + 1) * n / ((int64_t)size * 2));
}

// Where `u` of a copy `size` across falls among `n` source pixels, between
// pixel *i and the next at *f of 256: (u + 1/2) * n / size - 1/2, each end
// held to the picture's edge pixel.
static void between(uint32_t u, uint32_t n, int32_t size, uint32_t *i, uint32_t *f)
{
    double s = ((double)u + 0.5) * n / size - 0.5;
    if (s <= 0) {
        *i = 0;
        *f = 0;
    } else if (s >= n - 1) {
        *i = n - 1;
        *f = 0;
    } else {
        *i = (uint32_t)s;
        *f = (uint32_t)((s - *i) * 256 + 0.5);
        if (*f == 256) {
            (*i)++;
            *f = 0;
        }
    }
}

// The picture at (u, v) of a copy `tw` x `th`, mixed from the four source
// pixels round it by how near each is — their colours weighed by their
// alphas, so a transparent neighbour lends no colour — as 0xAARRGGBB.
static uint32_t smooth_at(const uint32_t *src, uint32_t sw, uint32_t sh, uint32_t u, int32_t tw,
                          uint32_t v, int32_t th)
{
    uint32_t x, fx, y, fy;
    between(u, sw, tw, &x, &fx);
    between(v, sh, th, &y, &fy);
    uint32_t x1 = x + 1 < sw ? x + 1 : x, y1 = y + 1 < sh ? y + 1 : y;
    const uint32_t p[4] = {src[(uint64_t)y * sw + x], src[(uint64_t)y * sw + x1],
                           src[(uint64_t)y1 * sw + x], src[(uint64_t)y1 * sw + x1]};
    const uint32_t w[4] = {(256 - fx) * (256 - fy), fx * (256 - fy), (256 - fx) * fy, fx * fy};
    uint64_t a = 0, r = 0, g = 0, b = 0;
    for (int k = 0; k < 4; k++) {
        uint64_t wa = (uint64_t)w[k] * (p[k] >> 24);
        a += wa;
        r += wa * ((p[k] >> 16) & 0xff);
        g += wa * ((p[k] >> 8) & 0xff);
        b += wa * (p[k] & 0xff);
    }
    if (a == 0)
        return 0;
    return (uint32_t)((a + 32768) >> 16) << 24 | (uint32_t)((r + a / 2) / a) << 16 |
           (uint32_t)((g + a / 2) / a) << 8 | (uint32_t)((b + a / 2) / a);
}

void yonder_tile_picture(uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip, os64_gui_rect_t area,
                         os64_gui_rect_t tile, bool repeat_x, bool repeat_y, bool smooth,
                         const uint32_t *src, uint32_t sw, uint32_t sh)
{
    if (area.w <= 0 || area.h <= 0 || tile.w <= 0 || tile.h <= 0 || sw == 0 || sh == 0)
        return;
    // Edges add in 64 bits, as yonder_draw_picture's do: a box's area may
    // reach as far as an int32_t says.
    int32_t x0 = (int32_t)max64(area.x, clip.x), y0 = (int32_t)max64(area.y, clip.y);
    int32_t x1 = (int32_t)min64((int64_t)area.x + area.w, (int64_t)clip.x + clip.w);
    int32_t y1 = (int32_t)min64((int64_t)area.y + area.h, (int64_t)clip.y + clip.h);
    // An axis that does not repeat has its one copy, where the tile is.
    if (!repeat_x) {
        x0 = (int32_t)max64(x0, tile.x);
        x1 = (int32_t)min64(x1, (int64_t)tile.x + tile.w);
    }
    if (!repeat_y) {
        y0 = (int32_t)max64(y0, tile.y);
        y1 = (int32_t)min64(y1, (int64_t)tile.y + tile.h);
    }
    // A copy at the picture's own size steps through its row; a scaled one
    // asks which source pixel each of its columns shows, or, smoothed, what
    // the four round it make.
    bool own = (uint32_t)tile.w == sw;
    smooth = smooth && !(own && (uint32_t)tile.h == sh);
    for (int32_t y = y0; y < y1; y++) {
        uint32_t v = wrap((int64_t)y - tile.y, (uint32_t)tile.h);
        const uint32_t *row = src + (uint64_t)source_at(v, sh, tile.h) * sw;
        uint32_t *out = dst + (uint64_t)y * pitch;
        uint32_t u = x0 < x1 ? wrap((int64_t)x0 - tile.x, (uint32_t)tile.w) : 0;
        for (int32_t x = x0; x < x1; x++) {
            put(&out[x], smooth ? smooth_at(src, sw, sh, u, tile.w, v, tile.h)
                                : row[own ? u : source_at(u, sw, tile.w)]);
            if (++u == (uint32_t)tile.w)
                u = 0;
        }
    }
}

void yonder_draw_picture(uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip, os64_gui_rect_t box,
                         bool smooth, const uint32_t *src, uint32_t sw, uint32_t sh)
{
    if (box.w <= 0 || box.h <= 0 || sw == 0 || sh == 0)
        return;
    smooth = smooth && !((uint32_t)box.w == sw && (uint32_t)box.h == sh);
    // Edges are added in 64 bits: a picture may be as wide as an int32_t
    // says (a width of 1000000% three tables deep), and its box need not
    // start at zero. What is walked is cut to `clip`, a real surface's.
    int32_t x0 = (int32_t)max64(box.x, clip.x), y0 = (int32_t)max64(box.y, clip.y);
    int32_t x1 = (int32_t)min64((int64_t)box.x + box.w, (int64_t)clip.x + clip.w);
    int32_t y1 = (int32_t)min64((int64_t)box.y + box.h, (int64_t)clip.y + clip.h);
    for (int32_t y = y0; y < y1; y++) {
        // The source row whose span covers the middle of this one:
        // floor((y - box.y + 1/2) * sh / box.h), in integers.
        uint32_t sy = (uint32_t)(((int64_t)(y - box.y) * 2 + 1) * sh / ((int64_t)box.h * 2));
        const uint32_t *row = src + (uint64_t)sy * sw;
        uint32_t *out = dst + (uint64_t)y * pitch;
        for (int32_t x = x0; x < x1; x++) {
            if (smooth) {
                put(&out[x], smooth_at(src, sw, sh, (uint32_t)((int64_t)x - box.x), box.w,
                                       (uint32_t)((int64_t)y - box.y), box.h));
                continue;
            }
            uint32_t sx = (uint32_t)(((int64_t)(x - box.x) * 2 + 1) * sw / ((int64_t)box.w * 2));
            put(&out[x], row[sx]);
        }
    }
}
