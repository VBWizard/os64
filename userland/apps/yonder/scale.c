// scale.c — a picture into its box (scale.h).

#include "scale.h"

#include "os64/mem.h"
#include "os64/str.h"

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
static void put(uint32_t *out, uint32_t p, bool premultiplied)
{
    uint32_t a = p >> 24;
    if (a == 0)
        return;
    if (a == 255) {
        *out = p;
        return;
    }
    uint32_t d = *out;
    if (premultiplied) {
        *out = 0xff000000u |
            ((((p >> 16) & 255) + (((d >> 16) & 255) * (255-a) + 127) / 255) << 16) |
            ((((p >> 8) & 255) + (((d >> 8) & 255) * (255-a) + 127) / 255) << 8) |
            ((p & 255) + ((d & 255) * (255-a) + 127) / 255);
        return;
    }
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

// THE COLUMNS AND THE ROWS. Where a copy's column falls in the source
// depends on the column alone, and a row's on the row alone, so a walk
// works each out once — a column for every row, a row for every column of
// a run — and mixes from them at every pixel: the same pixels as asking at
// each, without two divisions a pixel. The columns are worked SCALE_COLUMNS
// at a time, every row taken across them before the next run; each pixel
// is its own, so the order changes none.
#define SCALE_COLUMNS 256

// A smoothed copy's column or row: the source pixel before it, the one after
// (the edge pixel again past the last), and the weight of the one after,
// of 256 (between).
typedef struct {
    uint32_t i, next, f;
} Tap;

static Tap tap_at(uint32_t u, uint32_t n, int32_t size)
{
    Tap t;
    between(u, n, size, &t.i, &t.f);
    t.next = t.i + 1 < n ? t.i + 1 : t.i;
    return t;
}

// The picture at column `c` and row `r`, mixed from the four source pixels
// round it by how near each is — their colours weighed by their alphas for
// a straight source, or directly for a premultiplied source. The result
// retains the source alpha convention.
static uint32_t mix(const uint32_t *src, size_t stride, Tap c, Tap r, bool premultiplied)
{
    const uint32_t p[4] = {src[(uint64_t)r.i * stride + c.i], src[(uint64_t)r.i * stride + c.next],
                           src[(uint64_t)r.next * stride + c.i], src[(uint64_t)r.next * stride + c.next]};
    const uint32_t w[4] = {(256 - c.f) * (256 - r.f), c.f * (256 - r.f), (256 - c.f) * r.f, c.f * r.f};
    if (premultiplied) {
        // Two channels to a word, each in a 32-bit half: a weight is at
        // most 65536 and a channel 255, so four weighted channels sum
        // below 2^26 and neither half carries into the other — the same
        // sums as one channel at a time, in half the multiplies.
        uint64_t ar = 0, gb = 0;
        for (int k = 0; k < 4; k++) {
            uint64_t x = p[k];
            ar += ((x >> 24) << 32 | ((x >> 16) & 0xff)) * w[k];
            gb += (((x >> 8) & 0xff) << 32 | (x & 0xff)) * w[k];
        }
        uint32_t a = (uint32_t)(ar >> 32);
        if (a == 0)
            return 0;
        return ((a + 32768) >> 16) << 24 | (((uint32_t)ar + 32768) >> 16) << 16 |
               (((uint32_t)(gb >> 32) + 32768) >> 16) << 8 | (((uint32_t)gb + 32768) >> 16);
    }
    // A straight source's colours are weighed by their alphas, and the sum
    // divided by the alphas' again.
    uint64_t a = 0, red = 0, g = 0, b = 0;
    for (int k = 0; k < 4; k++) {
        uint64_t wa = (uint64_t)w[k] * (p[k] >> 24);
        a += wa;
        red += wa * ((p[k] >> 16) & 0xff);
        g += wa * ((p[k] >> 8) & 0xff);
        b += wa * (p[k] & 0xff);
    }
    if (a == 0)
        return 0;
    return (uint32_t)((a + 32768) >> 16) << 24 | (uint32_t)((red + a / 2) / a) << 16 |
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
    // shows the source pixel each column lands on, or, smoothed, what the
    // four round it make — each column worked out once for all the rows.
    bool own = (uint32_t)tile.w == sw;
    smooth = smooth && !(own && (uint32_t)tile.h == sh);
    Tap taps[SCALE_COLUMNS];
    uint32_t at[SCALE_COLUMNS];
    for (int32_t cx = x0; cx < x1; cx += SCALE_COLUMNS) {
        int32_t n = x1 - cx < SCALE_COLUMNS ? x1 - cx : SCALE_COLUMNS;
        uint32_t u = wrap((int64_t)cx - tile.x, (uint32_t)tile.w);
        for (int32_t k = 0; k < n; k++) {
            if (smooth)
                taps[k] = tap_at(u, sw, tile.w);
            else
                at[k] = own ? u : source_at(u, sw, tile.w);
            if (++u == (uint32_t)tile.w)
                u = 0;
        }
        for (int32_t y = y0; y < y1; y++) {
            uint32_t v = wrap((int64_t)y - tile.y, (uint32_t)tile.h);
            uint32_t *out = dst + (uint64_t)y * pitch + cx;
            if (smooth) {
                Tap r = tap_at(v, sh, tile.h);
                for (int32_t k = 0; k < n; k++)
                    put(&out[k], mix(src, sw, taps[k], r, false), false);
            } else {
                const uint32_t *row = src + (uint64_t)source_at(v, sh, tile.h) * sw;
                for (int32_t k = 0; k < n; k++)
                    put(&out[k], row[at[k]], false);
            }
        }
    }
}

bool yonder_draw_surface(uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip, os64_gui_rect_t box,
                          bool smooth, const draw2d_surface_t *source)
{
    if (!source || (source->format != DRAW2D_ARGB8_PREMULTIPLIED && source->format != DRAW2D_ARGB8_STRAIGHT) ||
        draw2d_surface_validate(source) != DRAW2D_OK) return false;
    const uint32_t *src = source->pixels;
    uint32_t sw = source->width, sh = source->height;
    size_t stride = source->stride / sizeof(uint32_t);
    bool premultiplied = source->format == DRAW2D_ARGB8_PREMULTIPLIED;
    if (box.w <= 0 || box.h <= 0 || sw == 0 || sh == 0)
        return true;
    bool own = (uint32_t)box.w == sw && (uint32_t)box.h == sh;
    smooth = smooth && !own;
    // Edges are added in 64 bits: a picture may be as wide as an int32_t
    // says (a width of 1000000% three tables deep), and its box need not
    // start at zero. What is walked is cut to `clip`, a real surface's.
    int32_t x0 = (int32_t)max64(box.x, clip.x), y0 = (int32_t)max64(box.y, clip.y);
    int32_t x1 = (int32_t)min64((int64_t)box.x + box.w, (int64_t)clip.x + clip.w);
    int32_t y1 = (int32_t)min64((int64_t)box.y + box.h, (int64_t)clip.y + clip.h);
    // At native size the pixel centers coincide: each row is walked
    // directly, with nothing to work out.
    if (own) {
        for (int32_t y = y0; y < y1; y++) {
            const uint32_t *row = src + (uint64_t)((int64_t)y - box.y) * stride;
            uint32_t *out = dst + (uint64_t)y * pitch;
            for (int32_t x = x0; x < x1; x++)
                put(&out[x], row[(size_t)((int64_t)x - box.x)], premultiplied);
        }
        return true;
    }
    Tap taps[SCALE_COLUMNS];
    uint32_t at[SCALE_COLUMNS];
    for (int32_t cx = x0; cx < x1; cx += SCALE_COLUMNS) {
        int32_t n = x1 - cx < SCALE_COLUMNS ? x1 - cx : SCALE_COLUMNS;
        for (int32_t k = 0; k < n; k++) {
            uint32_t u = (uint32_t)((int64_t)cx + k - box.x);
            if (smooth)
                taps[k] = tap_at(u, sw, box.w);
            else    // the source column whose span covers this one's middle
                at[k] = (uint32_t)(((int64_t)u * 2 + 1) * sw / ((int64_t)box.w * 2));
        }
        for (int32_t y = y0; y < y1; y++) {
            uint32_t v = (uint32_t)((int64_t)y - box.y);
            uint32_t *out = dst + (uint64_t)y * pitch + cx;
            if (smooth) {
                Tap r = tap_at(v, sh, box.h);
                for (int32_t k = 0; k < n; k++)
                    put(&out[k], mix(src, stride, taps[k], r, premultiplied), premultiplied);
            } else {
                // The source row whose span covers the middle of this one:
                // floor((v + 1/2) * sh / box.h), in integers.
                const uint32_t *row = src + (uint64_t)(((int64_t)v * 2 + 1) * sh / ((int64_t)box.h * 2)) * stride;
                for (int32_t k = 0; k < n; k++)
                    put(&out[k], row[at[k]], premultiplied);
            }
        }
    }
    return true;
}

void yonder_draw_picture(uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip, os64_gui_rect_t box,
                         bool smooth, const uint32_t *src, uint32_t sw, uint32_t sh)
{
    if ((uint64_t)sw * sh > SIZE_MAX / sizeof(uint32_t)) return;
    draw2d_surface_t source = {(void *)src, sw, sh, (size_t)sw * 4, (size_t)sw * sh * 4,
                                DRAW2D_ARGB8_STRAIGHT};
    (void)yonder_draw_surface(dst, pitch, clip, box, smooth, &source);
}

void yonder_picture_kept_size(uint32_t nw, uint32_t nh, uint32_t dw, uint32_t dh, uint32_t *kw,
                              uint32_t *kh)
{
    if (nw == 0 || nh == 0 || dw == 0 || dh == 0) {
        *kw = nw;
        *kh = nh;
        return;
    }
    uint64_t w, h;
    if ((uint64_t)dw * nh >= (uint64_t)dh * nw) {
        w = dw;
        h = ((uint64_t)nh * dw + nw - 1) / nw;
    } else {
        h = dh;
        w = ((uint64_t)nw * dh + nh - 1) / nh;
    }
    *kw = w < nw ? (uint32_t)w : nw;
    *kh = h < nh ? (uint32_t)h : nh;
}

bool yonder_reduce_picture(const uint32_t *src, uint32_t sw, uint32_t sh, uint32_t *dst,
                           uint32_t dw, uint32_t dh)
{
    if (sw == 0 || sh == 0 || dw == 0 || dh == 0 || dw > sw || dh > sh)
        return false;
    // Measured in 1/dw of a source column and 1/dh of a source row, source
    // pixel i spans [i * dw, (i + 1) * dw) and output pixel o spans
    // [o * sw, (o + 1) * sw): every overlap is a whole number, and every
    // output pixel's weights add to sw * sh. A row's sums: weight times
    // alpha, then that times each colour, which stay within 64 bits for
    // any picture of fewer than 2^28 pixels.
    uint64_t *sum = os64_calloc((size_t)dw * 4, sizeof(*sum));
    if (sum == NULL)
        return false;
    uint64_t total = (uint64_t)sw * sh;
    for (uint32_t oy = 0; oy < dh; oy++) {
        os64_memset(sum, 0, (size_t)dw * 4 * sizeof(*sum));
        uint64_t y0 = (uint64_t)oy * sh, y1 = y0 + sh;
        for (uint64_t sy = y0 / dh; sy * dh < y1; sy++) {
            uint64_t top = sy * dh > y0 ? sy * dh : y0;
            uint64_t bottom = (sy + 1) * dh < y1 ? (sy + 1) * dh : y1;
            uint64_t wy = bottom - top;
            const uint32_t *row = src + sy * sw;
            for (uint32_t ox = 0; ox < dw; ox++) {
                uint64_t x0 = (uint64_t)ox * sw, x1 = x0 + sw;
                uint64_t *s = sum + (size_t)ox * 4;
                for (uint64_t sx = x0 / dw; sx * dw < x1; sx++) {
                    uint64_t left = sx * dw > x0 ? sx * dw : x0;
                    uint64_t right = (sx + 1) * dw < x1 ? (sx + 1) * dw : x1;
                    uint32_t p = row[sx];
                    uint64_t wa = (right - left) * wy * (p >> 24);
                    s[0] += wa;
                    s[1] += wa * ((p >> 16) & 0xff);
                    s[2] += wa * ((p >> 8) & 0xff);
                    s[3] += wa * (p & 0xff);
                }
            }
        }
        uint32_t *out = dst + (uint64_t)oy * dw;
        for (uint32_t ox = 0; ox < dw; ox++) {
            const uint64_t *s = sum + (size_t)ox * 4;
            uint64_t a = s[0];
            out[ox] = a == 0 ? 0
                             : (uint32_t)((a + total / 2) / total) << 24 |
                                   (uint32_t)((s[1] + a / 2) / a) << 16 |
                                   (uint32_t)((s[2] + a / 2) / a) << 8 | (uint32_t)((s[3] + a / 2) / a);
        }
    }
    os64_free(sum);
    return true;
}
