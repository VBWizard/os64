// warp.c — a group drawn through its matrix, laid onto the page (warp.h).
//
// Every frame of a turning element comes through here, once a pixel of the
// box it sweeps, so the arithmetic is whole numbers: a position is a count
// of 1/2^32 pixels, stepped by adding along a row; a colour mixed from four
// pixels is a count of 1/65536 of a level; and laying it is one exact
// division. A pixel whose four sheet pixels are all nothing painted is
// skipped, and a row is walked only over the stretch that can reach the
// sheets — and most of a turning wedge's box is one or the other.

#include "warp.h"

#include <math.h>

// Whether `m` is copied by index: each of x' and y' reads exactly one of
// x and y, by exactly +1 or -1, moved by a whole pixel. Exact, not within
// a tolerance: a tolerance chooses what is drawn, and libflow makes every
// whole quarter turn exact at its source.
bool yonder_warp_exact(const double m[6])
{
    for (int i = 0; i < 4; i++)
        if (m[i] != 0 && m[i] != 1 && m[i] != -1)
            return false;
    if (m[4] != floor(m[4]) || m[5] != floor(m[5]))
        return false;
    return (m[0] != 0 && m[3] != 0 && m[1] == 0 && m[2] == 0) ||
           (m[1] != 0 && m[2] != 0 && m[0] == 0 && m[3] == 0);
}

// One pixel in fixed point: a sheet coordinate is held as a count of
// 1/2^32 pixels.
#define ONE ((int64_t)1 << 32)

// The bound on a coordinate held in fixed point, and on a sheet's side:
// 2^30 pixels, which is 2^62 in 1/2^32 pixels, half of what an int64 holds.
#define FAR 1073741824.0

// Whether `c` is inside FAR; false for a NaN.
static inline bool near(long double c)
{
    return c > -FAR && c < FAR;
}

// `c` in 1/2^32 pixels, to the nearest. |c| below FAR.
static int64_t fixed(long double c)
{
    long double t = c * (long double)ONE;
    return (int64_t)(t < 0 ? t - 0.5L : t + 0.5L);
}

// The whole pixel at or below a fixed coordinate: a floor, negatives too.
static inline int64_t whole(int64_t c)
{
    return (c - (int64_t)((uint64_t)c & 0xffffffffu)) / ONE;
}

// One channel of a 0x00RRGGBB pixel: 0 red, 1 green, 2 blue.
static inline uint32_t chan(uint32_t p, int c)
{
    return (p >> (16 - 8 * c)) & 0xffu;
}

// One channel of the backdrop d under B and W — each in 1/65536 of a level
// — at opacity a of 255: a B + (1 - a + a T) D with T = (W - B) / 255 held
// to 0..1, worked exactly over 255 x 255 x 65536 and rounded half up. W is
// at most 255 levels, so T cannot pass 1 and the result cannot pass 255.
static inline uint32_t lay(uint64_t b, uint64_t w, uint64_t d, uint64_t a)
{
    uint64_t t = w > b ? w - b : 0;
    uint64_t num = a * (255 * b + t * d) + (255 - a) * (255u * 65536u) * d;
    return (uint32_t)((num + 65025ull * 32768) / (65025ull * 65536));
}

// The backdrop pixel d with B and W laid on it at opacity a, its top byte
// the surface's own and kept.
static inline uint32_t lay_px(const uint64_t B[3], const uint64_t W[3], uint32_t d, uint32_t a)
{
    uint32_t out = 0;
    for (int c = 0; c < 3; c++)
        out |= lay(B[c], W[c], chan(d, c), a) << (16 - 8 * c);
    return (d & 0xff000000u) | out;
}

// Nothing painted: black on the black sheet and white on the white. Laid at
// any opacity it gives back the backdrop exactly, so it is skipped.
static inline bool blank(uint32_t b, uint32_t w)
{
    return (b & 0xffffffu) == 0 && (w & 0xffffffu) == 0xffffffu;
}

// The sheet pixel holding (U, V), copied; false off the sheets or where
// nothing was painted.
static inline bool take(const yonder_sheets_t *s, int64_t U, int64_t V, uint64_t B[3],
                        uint64_t W[3])
{
    int64_t i = whole(U), j = whole(V);
    if (i < 0 || j < 0 || i >= s->w || j >= s->h)
        return false;
    size_t at = (size_t)j * (size_t)s->w + (size_t)i;
    uint32_t pb = s->black[at], pw = s->white[at];
    if (blank(pb, pw))
        return false;
    for (int c = 0; c < 3; c++) {
        B[c] = (uint64_t)chan(pb, c) << 16;
        W[c] = (uint64_t)chan(pw, c) << 16;
    }
    return true;
}

// The four sheet pixels whose centres are round (U, V) + half a pixel — U
// and V the point less half a pixel, so their wholes name the top left one
// — B and W mixed apart, by 16-bit weights; off the sheets, nothing painted.
// False where all four are nothing painted, or none is on the sheets.
static inline bool mix(const yonder_sheets_t *s, int64_t U, int64_t V, uint64_t B[3],
                       uint64_t W[3])
{
    int64_t fx = whole(U), fy = whole(V);
    if (fx < -1 || fy < -1 || fx >= s->w || fy >= s->h)
        return false;
    uint64_t ax = ((uint64_t)U >> 16) & 0xffffu, ay = ((uint64_t)V >> 16) & 0xffffu;
    uint32_t pb[4], pw[4];
    if (fx >= 0 && fy >= 0 && fx + 1 < s->w && fy + 1 < s->h) {
        // All four on the sheets: two pairs side by side.
        size_t at = (size_t)fy * (size_t)s->w + (size_t)fx, next = at + (size_t)s->w;
        pb[0] = s->black[at];
        pb[1] = s->black[at + 1];
        pb[2] = s->black[next];
        pb[3] = s->black[next + 1];
        pw[0] = s->white[at];
        pw[1] = s->white[at + 1];
        pw[2] = s->white[next];
        pw[3] = s->white[next + 1];
    } else {
        for (int k = 0; k < 4; k++) {
            int64_t i = fx + (k & 1), j = fy + (k >> 1);
            bool in = i >= 0 && j >= 0 && i < s->w && j < s->h;
            size_t at = in ? (size_t)j * (size_t)s->w + (size_t)i : 0;
            pb[k] = in ? s->black[at] : 0;
            pw[k] = in ? s->white[at] : 0xffffffu;
        }
    }
    if (blank(pb[0] | pb[1] | pb[2] | pb[3], pw[0] & pw[1] & pw[2] & pw[3]))
        return false;
    // Each pixel's share of 65536: three rounded down, so the first, which
    // takes what they leave, is never short of its own and the four make
    // the whole. A channel's mix then fits 32 bits.
    uint32_t k1 = (uint32_t)((ax * (65536 - ay)) >> 16);
    uint32_t k2 = (uint32_t)(((65536 - ax) * ay) >> 16);
    uint32_t k3 = (uint32_t)((ax * ay) >> 16);
    uint32_t k0 = 65536 - k1 - k2 - k3;
    for (int c = 0; c < 3; c++) {
        B[c] = chan(pb[0], c) * k0 + chan(pb[1], c) * k1 + chan(pb[2], c) * k2 + chan(pb[3], c) * k3;
        W[c] = chan(pw[0], c) * k0 + chan(pw[1], c) * k1 + chan(pw[2], c) * k2 + chan(pw[3], c) * k3;
    }
    return true;
}

// [*from, *to) cut to the pixels x whose coordinate c0 + dc (x - base) may
// lie in [lo, hi), a pixel and more to spare each side: the sampler checks
// every pixel it is given, so this only spares it the ones that cannot hit.
static void narrow(int32_t *from, int32_t *to, int32_t base, double c0, double dc, double lo,
                   double hi)
{
    if (dc == 0) {
        if (!(c0 >= lo && c0 < hi))
            *to = *from;
        return;
    }
    double k0 = (lo - c0) / dc, k1 = (hi - c0) / dc;
    if (k0 > k1) {
        double k = k0;
        k0 = k1;
        k1 = k;
    }
    double a = base + floor(k0) - 1, b = base + ceil(k1) + 2;
    if (a >= *to || b <= *from) {
        *to = *from;
        return;
    }
    if (a > *from)
        *from = (int32_t)a;
    if (b < *to)
        *to = (int32_t)b;
}

// The sheet coordinate a destination centre (cx, cy) goes back to, less
// `off`: in long double, as every sum of a matrix's products here — a
// finite matrix's terms may each pass a double and cancel.
static long double back_u(const double inv[6], long double cx, long double cy, long double off)
{
    return inv[0] * cx + inv[2] * cy + inv[4] - off;
}

static long double back_v(const double inv[6], long double cx, long double cy, long double off)
{
    return inv[1] * cx + inv[3] * cy + inv[5] - off;
}

// Whether a row of [x0, x1) x [y0, y1) can be stepped by adding: a step
// small enough to hold in fixed point, and the coordinate inside FAR at
// every corner — it is affine, so then everywhere between.
static bool steppable(const double inv[6], int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      long double off)
{
    if (!(fabs(inv[0]) < 1048576.0 && fabs(inv[1]) < 1048576.0))
        return false;
    for (int k = 0; k < 4; k++) {
        long double cx = (k & 1 ? x1 - 1 : x0) + 0.5L, cy = (k & 2 ? y1 - 1 : y0) + 0.5L;
        long double u = back_u(inv, cx, cy, off), v = back_v(inv, cx, cy, off);
        if (!near(u) || !near(v))
            return false;
    }
    return true;
}

bool yonder_warp(uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip, const yonder_sheets_t *s,
                 const double m[6], const double inv[6], uint8_t alpha)
{
    if (inv == NULL || s->w <= 0 || s->h <= 0 || s->w > FAR || s->h > FAR)
        return false;
    // By index a centre names the pixel it is in; mixed, the four round it
    // are named from the point half a pixel up and left. What may reach the
    // sheets is a whole coordinate in [lo, side).
    bool exact = yonder_warp_exact(m);
    long double off = exact ? 0 : 0.5L;
    double lo = exact ? 0 : -1;
    // Where the sheets land, its corners' bounds, met with the clip. Not
    // the half sheet pixel round it where a mixed centre still takes a
    // share of an edge pixel: what the page marks for a group drawn
    // through its matrix is where it lands (flow_affine_rect), so a pixel
    // laid past that could be painted once and never cleared.
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (int k = 0; k < 4; k++) {
        long double x = k & 1 ? s->w : 0, y = k & 2 ? s->h : 0;
        double tx = (double)(m[0] * x + m[2] * y + m[4]);
        double ty = (double)(m[1] * x + m[3] * y + m[5]);
        x0 = fmin(x0, tx);
        y0 = fmin(y0, ty);
        x1 = fmax(x1, tx);
        y1 = fmax(y1, ty);
    }
    x0 = fmax(floor(x0 + 1e-6), clip.x);
    y0 = fmax(floor(y0 + 1e-6), clip.y);
    x1 = fmin(ceil(x1 - 1e-6), (double)clip.x + clip.w);
    y1 = fmin(ceil(y1 - 1e-6), (double)clip.y + clip.h);
    if (!(x0 < x1 && y0 < y1))
        return true;
    int32_t ax0 = (int32_t)x0, ay0 = (int32_t)y0, ax1 = (int32_t)x1, ay1 = (int32_t)y1;
    bool stepped = steppable(inv, ax0, ay0, ax1, ay1, off);
    int64_t du = stepped ? fixed(inv[0]) : 0, dv = stepped ? fixed(inv[1]) : 0;
    uint64_t B[3], W[3];
    for (int32_t y = ay0; y < ay1; y++) {
        uint32_t *row = dst + (size_t)y * pitch;
        long double cy = y + 0.5L;
        if (stepped) {
            int32_t from = ax0, to = ax1;
            long double cx = ax0 + 0.5L;
            narrow(&from, &to, ax0, (double)back_u(inv, cx, cy, off), inv[0], lo, s->w);
            narrow(&from, &to, ax0, (double)back_v(inv, cx, cy, off), inv[1], lo, s->h);
            // Each row starts from its own centre, so a step's rounding
            // never carries past the row it was made in.
            cx = from + 0.5L;
            int64_t U = fixed(back_u(inv, cx, cy, off)), V = fixed(back_v(inv, cx, cy, off));
            for (int32_t x = from; x < to; x++, U += du, V += dv)
                if (exact ? take(s, U, V, B, W) : mix(s, U, V, B, W))
                    row[x] = lay_px(B, W, row[x], alpha);
            continue;
        }
        for (int32_t x = ax0; x < ax1; x++) {
            long double cx = x + 0.5L;
            long double u = back_u(inv, cx, cy, off), v = back_v(inv, cx, cy, off);
            // Inside the sheets' landing a centre goes back inside the
            // sheets; this holds the fixed point to what it can say should
            // an inverse that does not undo `m` ever be handed in.
            if (!near(u) || !near(v))
                continue;
            int64_t U = fixed(u), V = fixed(v);
            if (exact ? take(s, U, V, B, W) : mix(s, U, V, B, W))
                row[x] = lay_px(B, W, row[x], alpha);
        }
    }
    return true;
}

bool yonder_warp_masked(uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip,
                        const yonder_sheets_t *s, const uint32_t *mask, int32_t dx, int32_t dy)
{
    if (s->w <= 0 || s->h <= 0 || mask == NULL)
        return false;
    // Where the sheets land, met with the clip, in 64 bits: an offset and a
    // side each fit 32, their sum may not.
    int64_t x0 = dx > clip.x ? dx : clip.x, y0 = dy > clip.y ? dy : clip.y;
    int64_t x1 = (int64_t)dx + s->w, y1 = (int64_t)dy + s->h;
    if ((int64_t)clip.x + clip.w < x1)
        x1 = (int64_t)clip.x + clip.w;
    if ((int64_t)clip.y + clip.h < y1)
        y1 = (int64_t)clip.y + clip.h;
    uint64_t B[3], W[3];
    for (int64_t y = y0; y < y1; y++) {
        uint32_t *row = dst + (size_t)y * pitch;
        size_t j = (size_t)(y - dy);
        for (int64_t x = x0; x < x1; x++) {
            size_t at = j * (size_t)s->w + (size_t)(x - dx);
            uint32_t c = (mask[at] >> 8) & 0xffu, pb = s->black[at], pw = s->white[at];
            if (c == 0 || blank(pb, pw))
                continue;
            for (int k = 0; k < 3; k++) {
                B[k] = (uint64_t)chan(pb, k) << 16;
                W[k] = (uint64_t)chan(pw, k) << 16;
            }
            row[x] = lay_px(B, W, row[x], c);
        }
    }
    return true;
}

void yonder_warp_rebase(const double m[6], int32_t bx, int32_t by, int32_t dx, int32_t dy,
                        double out[6])
{
    long double x = bx, y = by;
    out[0] = m[0];
    out[1] = m[1];
    out[2] = m[2];
    out[3] = m[3];
    out[4] = (double)(m[0] * x + m[2] * y + m[4] + dx);
    out[5] = (double)(m[1] * x + m[3] * y + m[5] + dy);
}
