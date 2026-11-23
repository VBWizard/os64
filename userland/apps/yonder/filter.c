// filter.c — a filtered or blended group's pixels, and a backdrop's
// (filter.h; FILTER.md, MIX_BLEND.md).

#include "filter.h"

#include <math.h>

static uint32_t chan(uint32_t p, int shift)
{
    return (p >> shift) & 0xffu;
}

static uint32_t pack(uint32_t a, uint32_t r, uint32_t g, uint32_t b)
{
    return a << 24 | r << 16 | g << 8 | b;
}

void yonder_layer_recover(const yonder_sheets_t *s, uint32_t *out)
{
    size_t n = (size_t)s->w * (size_t)s->h;
    for (size_t i = 0; i < n; i++) {
        uint32_t b = s->black[i], w = s->white[i];
        int32_t through = (int32_t)chan(w, 8) - (int32_t)chan(b, 8);
        uint32_t a = through <= 0 ? 255 : through >= 255 ? 0 : (uint32_t)(255 - through);
        uint32_t r = chan(b, 16), g = chan(b, 8), bl = chan(b, 0);
        out[i] = pack(a, r < a ? r : a, g < a ? g : a, bl < a ? bl : a);
    }
}

// ── The colour functions (Filter Effects 1 § 13) ────────────────────────

// A 3x3 matrix on the colour, then an affine transfer of each channel —
// slope and intercept on the colour, a slope on the alpha — each result
// held to [0, 1].
typedef struct {
    double m[9];
    double slope, intercept, alpha;
} Colour;

static void identity(Colour *c)
{
    static const double one[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int k = 0; k < 9; k++)
        c->m[k] = one[k];
    c->slope = 1;
    c->intercept = 0;
    c->alpha = 1;
}

static void colour_of(const flow_filter_t *f, Colour *c)
{
    identity(c);
    double a = f->amount, u = 1 - a;
    switch (f->fn) {
    case FLOW_FILTER_GRAYSCALE: {
        const double m[9] = {0.2126 + 0.7874 * u, 0.7152 - 0.7152 * u, 0.0722 - 0.0722 * u,
                             0.2126 - 0.2126 * u, 0.7152 + 0.2848 * u, 0.0722 - 0.0722 * u,
                             0.2126 - 0.2126 * u, 0.7152 - 0.7152 * u, 0.0722 + 0.9278 * u};
        for (int k = 0; k < 9; k++)
            c->m[k] = m[k];
        return;
    }
    case FLOW_FILTER_SEPIA: {
        const double m[9] = {0.393 + 0.607 * u, 0.769 - 0.769 * u, 0.189 - 0.189 * u,
                             0.349 - 0.349 * u, 0.686 + 0.314 * u, 0.168 - 0.168 * u,
                             0.272 - 0.272 * u, 0.534 - 0.534 * u, 0.131 + 0.869 * u};
        for (int k = 0; k < 9; k++)
            c->m[k] = m[k];
        return;
    }
    case FLOW_FILTER_SATURATE: {
        const double m[9] = {0.213 + 0.787 * a, 0.715 - 0.715 * a, 0.072 - 0.072 * a,
                             0.213 - 0.213 * a, 0.715 + 0.285 * a, 0.072 - 0.072 * a,
                             0.213 - 0.213 * a, 0.715 - 0.715 * a, 0.072 + 0.928 * a};
        for (int k = 0; k < 9; k++)
            c->m[k] = m[k];
        return;
    }
    case FLOW_FILTER_HUE_ROTATE: {
        double r = a * 3.14159265358979323846 / 180, co = cos(r), si = sin(r);
        const double m[9] = {
            0.213 + co * 0.787 - si * 0.213, 0.715 - co * 0.715 - si * 0.715, 0.072 - co * 0.072 + si * 0.928,
            0.213 - co * 0.213 + si * 0.143, 0.715 + co * 0.285 + si * 0.140, 0.072 - co * 0.072 - si * 0.283,
            0.213 - co * 0.213 - si * 0.787, 0.715 - co * 0.715 + si * 0.715, 0.072 + co * 0.928 + si * 0.072};
        for (int k = 0; k < 9; k++)
            c->m[k] = m[k];
        return;
    }
    case FLOW_FILTER_BRIGHTNESS: c->slope = a; return;
    case FLOW_FILTER_CONTRAST:
        c->slope = a;
        c->intercept = 0.5 - 0.5 * a;
        return;
    case FLOW_FILTER_INVERT:
        c->slope = 1 - 2 * a;
        c->intercept = a;
        return;
    case FLOW_FILTER_OPACITY: c->alpha = a; return;
    default: return;
    }
}

// Held to [0, 1], a NaN to 0: an amount the page wrote as large as a
// double holds (contrast(1e500) is infinite) makes inf - inf, and a NaN
// must not reach the cast to a byte.
static double held(double v)
{
    return v > 0 ? (v < 1 ? v : 1) : 0;
}

static uint32_t byte(double v)
{
    return (uint32_t)(held(v) * 255 + 0.5);
}

static void colour_run(uint32_t *px, size_t n, const Colour *c)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t a = chan(px[i], 24);
        if (a == 0)
            continue;
        double in[3] = {chan(px[i], 16) / (double)a, chan(px[i], 8) / (double)a, chan(px[i], 0) / (double)a};
        double out[3];
        for (int k = 0; k < 3; k++) {
            double v = held(c->m[3 * k] * in[0] + c->m[3 * k + 1] * in[1] + c->m[3 * k + 2] * in[2]);
            out[k] = held(v * c->slope + c->intercept);
        }
        double na = held(a / 255.0 * c->alpha);
        uint32_t ab = byte(na);
        px[i] = pack(ab, byte(out[0] * na), byte(out[1] * na), byte(out[2] * na));
    }
}

// ── The blurs ───────────────────────────────────────────────────────────

// A box's sum of one channel over `span` pixels, averaged to the nearest:
// (sum + span / 2) / span, as a multiply by the reciprocal `inv`
// (2^32 / span, rounded up), which is exact for every sum a box of at most
// 2 * FLOW_FILTER_BLUR_MAX + 1 pixels can hold: the error, sum times
// (inv * span - 2^32) / 2^32, stays under 1 / span while the sum is below
// 2^32 / span.
static inline uint32_t box_mean(uint32_t sum, uint32_t span, uint64_t inv)
{
    return (uint32_t)(((uint64_t)(sum + span / 2) * inv) >> 32);
}

// The columns a column pass carries at once: their pixels share cache
// lines, where one column at a time reads a whole row's stride apart at
// every step.
#define BOX_STRIP 16

// One box blur of radius `r` along every row, each of a pixel's four 8-bit
// channels apart, reading nothing (0) past the edges. `tmp` holds a row.
static void box_rows(uint32_t *px, int32_t w, int32_t h, int32_t r, uint32_t *tmp)
{
    uint32_t span = 2 * (uint32_t)r + 1;
    uint64_t inv = ((1ull << 32) + span - 1) / span;
    for (int32_t l = 0; l < h; l++) {
        uint32_t *v = px + (size_t)l * (size_t)w;
        uint32_t sum[4] = {0, 0, 0, 0};
        for (int32_t i = 0; i <= r && i < w; i++)
            for (int c = 0; c < 4; c++)
                sum[c] += chan(v[i], 8 * c);
        for (int32_t i = 0; i < w; i++) {
            uint32_t o = 0;
            for (int c = 0; c < 4; c++)
                o |= box_mean(sum[c], span, inv) << (8 * c);
            tmp[i] = o;
            int32_t out = i - r, in = i + r + 1;
            for (int c = 0; c < 4; c++) {
                if (in < w)
                    sum[c] += chan(v[in], 8 * c);
                if (out >= 0)
                    sum[c] -= chan(v[out], 8 * c);
            }
        }
        for (int32_t i = 0; i < w; i++)
            v[i] = tmp[i];
    }
}

// The same down every column, BOX_STRIP columns at a time, row by row
// within the strip; each column's result is the one a column alone would
// have. `tmp` holds BOX_STRIP columns of `h`.
static void box_columns(uint32_t *px, int32_t w, int32_t h, int32_t r, uint32_t *tmp)
{
    uint32_t span = 2 * (uint32_t)r + 1;
    uint64_t inv = ((1ull << 32) + span - 1) / span;
    for (int32_t x0 = 0; x0 < w; x0 += BOX_STRIP) {
        int32_t n = w - x0 < BOX_STRIP ? w - x0 : BOX_STRIP;
        uint32_t sum[BOX_STRIP][4] = {{0}};
        for (int32_t i = 0; i <= r && i < h; i++) {
            const uint32_t *row = px + (size_t)i * (size_t)w + (size_t)x0;
            for (int32_t k = 0; k < n; k++)
                for (int c = 0; c < 4; c++)
                    sum[k][c] += chan(row[k], 8 * c);
        }
        for (int32_t i = 0; i < h; i++) {
            uint32_t *out_row = tmp + (size_t)i * BOX_STRIP;
            for (int32_t k = 0; k < n; k++) {
                uint32_t o = 0;
                for (int c = 0; c < 4; c++)
                    o |= box_mean(sum[k][c], span, inv) << (8 * c);
                out_row[k] = o;
            }
            int32_t out = i - r, in = i + r + 1;
            const uint32_t *adding = in < h ? px + (size_t)in * (size_t)w + (size_t)x0 : NULL;
            const uint32_t *leaving = out >= 0 ? px + (size_t)out * (size_t)w + (size_t)x0 : NULL;
            for (int32_t k = 0; k < n; k++)
                for (int c = 0; c < 4; c++) {
                    if (adding != NULL)
                        sum[k][c] += chan(adding[k], 8 * c);
                    if (leaving != NULL)
                        sum[k][c] -= chan(leaving[k], 8 * c);
                }
        }
        for (int32_t i = 0; i < h; i++)
            for (int32_t k = 0; k < n; k++)
                px[(size_t)i * (size_t)w + (size_t)x0 + (size_t)k] = tmp[(size_t)i * BOX_STRIP + (size_t)k];
    }
}

// ── A large blur, on a reduced copy ─────────────────────────────────────
//
// A blur of box radius r keeps nothing finer than about r pixels, so a
// large one is made on a copy reduced s times each way: each s by s square
// of the layer averaged into one pixel, blurred there, and enlarged back
// bilinearly. The reduced blur is three EXTENDED boxes (Gwosdek, Grewenig,
// Bruhn and Weickert, 2011): a box of radius k with a weight of a / 256 on
// the pixel just past each end, k and a chosen so that the three passes,
// the reduction and the enlargement together spread a pixel exactly as far
// (the same variance) as three boxes of radius r do. Three plain boxes
// cannot: their radius is a whole number of reduced pixels, so their
// spread missed by up to 7% and the result by up to 11 of 255.
//
// s is the largest power of two up to BLUR_REDUCED_MOST whose k is at
// least BLUR_REDUCED_FLOOR; a blur with none takes three boxes at full
// size, as it always did. THE CONTRACT, measured against those over a hard
// edge, a gradient, noise, a photograph and alpha edges at every r up to
// FLOW_FILTER_BLUR_MAX (tools/test_yonder_filter_host.c holds it at the
// radii either side of each threshold and the largest, a picture's shading
// standing in for the photograph):
// within 2 of 255 in any channel wherever the content stays a reach inside
// its layer, with a ramp never stepping back or jumping past 2; where the
// content touches an edge the layer was cut at — an ancestor's clip — within
// 6, near the corners, which is that band's measured bound at this floor
// (floor 6 held 2 there too, at half the saving on the blurs that cost
// most; ruled for 3). The two constants are that table's — k = 2 gave 3
// of 255 inside, and s = 16 gave 3 at r = 59 — so move either only with
// the table measured again. A radius crossing a threshold (r 13 to 14, 27
// to 28) changes s between two frames of an animated blur; the step is
// inside the same contract.
//
// The squares are counted from the GROUP's origin (`grid`, filter.h), not
// the layer's, which is cut to what is being painted: a part painted alone
// and the whole painted at once agree to the pixel, and a group moved by a
// pixel comes out as the same pixels, moved. A reduced blur reads farther
// than three boxes of r do (reduced_reads), so the painter cuts the layer
// that much wider (yonder_filter_reads_more), and every pixel shown reads
// only pixels painted.
#define BLUR_REDUCED_FLOOR 3
// At most 8: the enlargement adds two reduced pixels' 8-bit channels
// weighted by 2s, then two of those weighted by 2s, in 16-bit halves of a
// word — 255 (2s)^2 plus the rounding's 2s^2 fits under 65536 only up to 8.
#define BLUR_REDUCED_MOST 8

typedef struct {
    int32_t s;                  // the reduction; 1 for three boxes at full size
    int32_t k;                  // each reduced pass's box radius
    uint32_t a;                 // and its end weight, of 256
} Reduction;

// How far the bilinear enlargement by `s` spreads a reduced pixel: the
// variance, in full pixels, of the weights it gives the pixels around it.
static double enlarge_spread(int32_t s)
{
    double weights = 0, spread = 0;
    for (int32_t m = 0; m < s; m++) {
        double d = m + 0.5, w = 1 - d / s;
        weights += 2 * w;
        spread += 2 * d * d * w;
    }
    return spread / weights;
}

// The box radius for a shadow blur radius `blur` (flow.h's): blur / 2,
// held at FLOW_FILTER_BLUR_MAX.
static int32_t blur_radius(int32_t blur)
{
    int32_t most = 2 * FLOW_FILTER_BLUR_MAX;
    return (blur < most ? blur : most) / 2;
}

// The reduction for a shadow blur radius `blur`.
static Reduction blur_reduction(int32_t blur)
{
    Reduction plain = {1, 0, 0};
    int32_t r = blur_radius(blur);
    if (r <= 0)
        return plain;
    // Three boxes of r spread a pixel by 3 r (r + 1) / 3; the reduction
    // (a box of s) by (s^2 - 1) / 12 and the enlargement by its own; what
    // is left is the three reduced passes', in reduced pixels, a third
    // each.
    double whole = (double)r * (r + 1);
    for (int32_t s = BLUR_REDUCED_MOST; s >= 2; s /= 2) {
        double t = (whole - (s * s - 1) / 12.0 - enlarge_spread(s)) / ((double)s * s) / 3;
        if (t <= 0)
            continue;
        // A box of k spreads by k (k + 1) / 3: the largest k within t, then
        // the end weight that makes up the rest, (2k + 1 + 2a) t =
        // k (k + 1)(2k + 1) / 3 + 2a (k + 1)^2.
        int32_t k = (int32_t)floor(sqrt(3 * t + 0.25) - 0.5);
        if (k < BLUR_REDUCED_FLOOR)
            continue;
        double a = (t * (2 * k + 1) - k * (k + 1.0) * (2 * k + 1) / 3) / (2.0 * (k + 1) * (k + 1) - 2 * t);
        long w = (long)floor(a * 256 + 0.5);
        return (Reduction){s, k, (uint32_t)(w < 0 ? 0 : w > 255 ? 255 : w)};
    }
    return plain;
}

// How far a reduced blur reads past a pixel: two reduced pixels for the
// enlargement, 3 (k + 1) for the passes, and each a square of s, within
// 3 (k + 1) s + 3s / 2 of it; three boxes of r read 3r.
static int32_t reduced_reads(Reduction d)
{
    return d.s > 1 ? 3 * (d.k + 1) * d.s + 3 * d.s / 2 : 0;
}

// Where `at` falls in a square of `s`, counted from 0.
static int32_t phase(int32_t at, int32_t s)
{
    return (at % s + s) % s;
}

// The two 8-bit halves of a pixel's channels apart, each in a 16-bit half
// of a word: blue and red, then green and alpha.
#define HALVES 0x00ff00ffu

// The layer reduced into `sm` (rw by rh): each square of the grid, whose
// first square begins `p0` pixels before the layer's first column and
// `q0` before its first row, averaged to the nearest, what lies past the
// layer counted 0. The sums of s^2 channels of 255 fit their halves.
static void reduce(const uint32_t *px, int32_t w, int32_t h, int32_t s, int32_t p0, int32_t q0,
                   uint32_t *sm, int32_t rw, int32_t rh)
{
    int32_t shift = 2 * __builtin_ctz((unsigned)s);
    uint32_t half = (uint32_t)(s * s / 2) * 0x00010001u;
    for (int32_t j = 0; j < rh; j++) {
        int32_t y0 = j * s - q0, y1 = y0 + s;
        y0 = y0 < 0 ? 0 : y0;
        y1 = y1 > h ? h : y1;
        for (int32_t i = 0; i < rw; i++) {
            int32_t x0 = i * s - p0, x1 = x0 + s;
            x0 = x0 < 0 ? 0 : x0;
            x1 = x1 > w ? w : x1;
            uint32_t rb = 0, ag = 0;
            for (int32_t y = y0; y < y1; y++) {
                const uint32_t *row = px + (size_t)y * (size_t)w;
                for (int32_t x = x0; x < x1; x++) {
                    rb += row[x] & HALVES;
                    ag += (row[x] >> 8) & HALVES;
                }
            }
            sm[(size_t)j * (size_t)rw + (size_t)i] =
                (((rb + half) >> shift) & HALVES) | (((ag + half) >> shift) & HALVES) << 8;
        }
    }
}

// One extended box of radius k and end weight a along `n` pixels `step`
// apart, each channel apart, reading nothing (0) past the ends. The mean
// is (x + den / 2) / den to the nearest, as box_mean's, by a multiply by
// 2^48 / den rounded up, exact while x den < 2^48: x is below 256 den,
// and den, 256 (2k + 3) at most, below 2^20. `tmp` holds n.
static void extended_line(uint32_t *v, int32_t n, size_t step, int32_t k, uint32_t a, uint32_t *tmp)
{
    uint64_t den = 256u * (2u * (uint64_t)k + 1) + 2u * a;
    uint64_t inv = ((1ull << 48) + den - 1) / den;
    uint32_t in[4] = {0, 0, 0, 0};          // the sum within k, at the pixel
    for (int32_t i = 0; i <= k && i < n; i++)
        for (int c = 0; c < 4; c++)
            in[c] += chan(v[(size_t)i * step], 8 * c);
    for (int32_t i = 0; i < n; i++) {
        int32_t lo = i - k - 1, hi = i + k + 1;
        uint32_t below = lo >= 0 ? v[(size_t)lo * step] : 0, above = hi < n ? v[(size_t)hi * step] : 0;
        uint32_t o = 0;
        for (int c = 0; c < 4; c++) {
            uint64_t x = (uint64_t)in[c] * 256u + (uint64_t)(chan(below, 8 * c) + chan(above, 8 * c)) * a;
            o |= (uint32_t)(((x + den / 2) * inv) >> 48) << (8 * c);
        }
        tmp[i] = o;
        for (int c = 0; c < 4; c++) {
            in[c] += chan(above, 8 * c);
            if (i - k >= 0)
                in[c] -= chan(v[(size_t)(i - k) * step], 8 * c);
        }
    }
    for (int32_t i = 0; i < n; i++)
        v[(size_t)i * step] = tmp[i];
}

// The same down every column of the copy (rw by rh), BOX_STRIP columns at a
// time as box_columns goes; each column's result is extended_line's.
// `tmp` holds BOX_STRIP columns of rh.
static void extended_columns(uint32_t *sm, int32_t rw, int32_t rh, int32_t k, uint32_t a, uint32_t *tmp)
{
    uint64_t den = 256u * (2u * (uint64_t)k + 1) + 2u * a;
    uint64_t inv = ((1ull << 48) + den - 1) / den;
    for (int32_t x0 = 0; x0 < rw; x0 += BOX_STRIP) {
        int32_t n = rw - x0 < BOX_STRIP ? rw - x0 : BOX_STRIP;
        uint32_t in[BOX_STRIP][4] = {{0}};
        for (int32_t i = 0; i <= k && i < rh; i++) {
            const uint32_t *row = sm + (size_t)i * (size_t)rw + (size_t)x0;
            for (int32_t q = 0; q < n; q++)
                for (int c = 0; c < 4; c++)
                    in[q][c] += chan(row[q], 8 * c);
        }
        for (int32_t i = 0; i < rh; i++) {
            int32_t lo = i - k - 1, hi = i + k + 1, out = i - k;
            const uint32_t *below = lo >= 0 ? sm + (size_t)lo * (size_t)rw + (size_t)x0 : NULL;
            const uint32_t *above = hi < rh ? sm + (size_t)hi * (size_t)rw + (size_t)x0 : NULL;
            const uint32_t *leaving = out >= 0 ? sm + (size_t)out * (size_t)rw + (size_t)x0 : NULL;
            uint32_t *put = tmp + (size_t)i * BOX_STRIP;
            for (int32_t q = 0; q < n; q++) {
                uint32_t pb = below != NULL ? below[q] : 0, pa = above != NULL ? above[q] : 0, o = 0;
                for (int c = 0; c < 4; c++) {
                    uint64_t x = (uint64_t)in[q][c] * 256u + (uint64_t)(chan(pb, 8 * c) + chan(pa, 8 * c)) * a;
                    o |= (uint32_t)(((x + den / 2) * inv) >> 48) << (8 * c);
                }
                put[q] = o;
                for (int c = 0; c < 4; c++) {
                    in[q][c] += chan(pa, 8 * c);
                    if (leaving != NULL)
                        in[q][c] -= chan(leaving[q], 8 * c);
                }
            }
        }
        for (int32_t i = 0; i < rh; i++)
            for (int32_t q = 0; q < n; q++)
                sm[(size_t)i * (size_t)rw + (size_t)x0 + (size_t)q] = tmp[(size_t)i * BOX_STRIP + (size_t)q];
    }
}

// Reduced row `src` (rw) widened to the layer's `w` columns, each pixel's
// halves weighted by 2s between the two reduced pixels about it, into
// `line` (two words a pixel); past the reduced row's ends, its end pixel.
static void widen(const uint32_t *src, int32_t rw, int32_t s, int32_t p0, uint32_t *line, int32_t w)
{
    int32_t two = 2 * s, shift = __builtin_ctz((unsigned)two);
    for (int32_t x = 0; x < w; x++) {
        // The pixel's centre in reduced pixels, (2 (x + p0) + 1 - s) / 2s,
        // offset by one so it is never below 0: its whole part less one is
        // the reduced pixel before it, its fraction the weight of the next.
        int32_t at = 2 * (x + p0) + 1 + s;
        int32_t i0 = (at >> shift) - 1, f = at & (two - 1);
        int32_t ia = i0 < 0 ? 0 : i0 >= rw ? rw - 1 : i0;
        int32_t ib = i0 + 1 < 0 ? 0 : i0 + 1 >= rw ? rw - 1 : i0 + 1;
        uint32_t pa = src[ia], pb = src[ib];
        line[2 * x] = (pa & HALVES) * (uint32_t)(two - f) + (pb & HALVES) * (uint32_t)f;
        line[2 * x + 1] = ((pa >> 8) & HALVES) * (uint32_t)(two - f) + ((pb >> 8) & HALVES) * (uint32_t)f;
    }
}

// The reduced copy enlarged back over the layer, bilinearly: each row from
// the two widened reduced rows about it, weighted by 2s, the sum of
// (2s)^2 rounded to the nearest. `rows` holds two widened rows.
static void enlarge(const uint32_t *sm, int32_t rw, int32_t rh, int32_t s, int32_t p0, int32_t q0,
                    uint32_t *px, int32_t w, int32_t h, uint32_t *rows)
{
    int32_t two = 2 * s, shift = __builtin_ctz((unsigned)two);
    int32_t out = 2 * shift;
    uint32_t half = (uint32_t)(2 * s * s) * 0x00010001u;
    uint32_t *line[2] = {rows, rows + 2 * (size_t)w};
    int32_t held[2] = {-1, -1};
    for (int32_t y = 0; y < h; y++) {
        int32_t at = 2 * (y + q0) + 1 + s;
        int32_t j0 = (at >> shift) - 1, g = at & (two - 1);
        int32_t want[2] = {j0 < 0 ? 0 : j0 >= rh ? rh - 1 : j0,
                           j0 + 1 < 0 ? 0 : j0 + 1 >= rh ? rh - 1 : j0 + 1};
        if (want[0] != held[0] && want[0] == held[1]) {
            uint32_t *swap = line[0];
            line[0] = line[1];
            line[1] = swap;
            held[0] = held[1];
            held[1] = -1;
        }
        for (int q = 0; q < 2; q++)
            if (want[q] != held[q]) {
                widen(sm + (size_t)want[q] * (size_t)rw, rw, s, p0, line[q], w);
                held[q] = want[q];
            }
        uint32_t *v = px + (size_t)y * (size_t)w;
        for (int32_t x = 0; x < w; x++) {
            uint32_t rb = line[0][2 * x] * (uint32_t)(two - g) + line[1][2 * x] * (uint32_t)g;
            uint32_t ag = line[0][2 * x + 1] * (uint32_t)(two - g) + line[1][2 * x + 1] * (uint32_t)g;
            v[x] = (((rb + half) >> out) & HALVES) | (((ag + half) >> out) & HALVES) << 8;
        }
    }
}

// The reduced copy's side for a layer's `n`: the squares over it, one more
// where the first begins up to s - 1 pixels before it, and one wholly past
// each end (blur_reduced).
static int32_t reduced_side(int32_t n, int32_t s)
{
    return (n + 2 * s - 2) / s + 2;
}

// The words a reduced blur by `d` needs beside the line: the copy and two
// widened rows.
static size_t reduced_words(int32_t w, int32_t h, Reduction d)
{
    if (d.s <= 1)
        return 0;
    return (size_t)reduced_side(w, d.s) * (size_t)reduced_side(h, d.s) + 4 * (size_t)w;
}

// Three boxes at full size keep each pass inside the layer: a pass along
// the rows writes only the layer's columns, so the next pass reads nothing
// where it spread past them. The reduced copy does the same after a pass
// along `n` squares (`step` apart, `lines` of them `stride` apart): a
// square wholly past the layer's `len` pixels is emptied, one partly past
// it keeps the part inside, `inside` of s, each channel scaled alike.
static void keep_inside(uint32_t *sm, int32_t n, size_t step, int32_t lines, size_t stride, int32_t s,
                        int32_t p0, int32_t len)
{
    for (int32_t i = 0; i < n; i++) {
        int32_t lo = i * s - p0, hi = lo + s;
        lo = lo < 0 ? 0 : lo;
        hi = hi > len ? len : hi;
        uint32_t inside = hi > lo ? (uint32_t)(hi - lo) : 0;
        if (inside == (uint32_t)s)
            continue;
        for (int32_t l = 0; l < lines; l++) {
            uint32_t *p = &sm[(size_t)l * stride + (size_t)i * step], o = 0;
            for (int c = 0; c < 32; c += 8)
                o |= ((chan(*p, c) * inside + (uint32_t)s / 2) / (uint32_t)s) << c;
            *p = o;
        }
    }
}

// The copy also holds a square wholly past each edge of the layer, which
// the LAST pass each way leaves as it spread there: a pixel at the edge is
// enlarged between two squares the blur made, as every other pixel is,
// and not held at the square inside, which lost the slope a blur has where
// the content touches the edge.
static void blur_reduced(uint32_t *px, int32_t w, int32_t h, Reduction d, flow_point_t grid,
                         uint32_t *tmp, uint32_t *more)
{
    int32_t s = d.s, p0 = phase(grid.x, s) + s, q0 = phase(grid.y, s) + s;
    int32_t rw = (w + p0 + s - 1) / s + 1, rh = (h + q0 + s - 1) / s + 1;
    uint32_t *sm = more, *rows = more + (size_t)rw * (size_t)rh;
    reduce(px, w, h, s, p0, q0, sm, rw, rh);
    for (int k = 0; k < 3; k++) {
        for (int32_t j = 0; j < rh; j++)
            extended_line(sm + (size_t)j * (size_t)rw, rw, 1, d.k, d.a, tmp);
        if (k < 2)
            keep_inside(sm, rw, 1, rh, (size_t)rw, s, p0, w);
        extended_columns(sm, rw, rh, d.k, d.a, tmp);
        if (k < 2)
            keep_inside(sm, rh, (size_t)rw, rw, 1, s, q0, h);
    }
    enlarge(sm, rw, rh, s, p0, q0, px, w, h, rows);
}

// Three box blurs each way, for a shadow blur radius `blur` (flow.h's) —
// on a reduced copy when it is large enough (above), its squares counted
// from `grid`. `tmp` holds blur_line's; `more` reduced_words'.
static void blur_run(uint32_t *px, int32_t w, int32_t h, int32_t blur, flow_point_t grid, uint32_t *tmp,
                     uint32_t *more)
{
    Reduction d = blur_reduction(blur);
    if (d.s > 1) {
        blur_reduced(px, w, h, d, grid, tmp, more);
        return;
    }
    int32_t r = blur_radius(blur);
    if (r <= 0)
        return;
    for (int k = 0; k < 3; k++) {
        box_rows(px, w, h, r, tmp);
        box_columns(px, w, h, r, tmp);
    }
}

// The layer's coverage, blurred, moved and tinted, laid under it.
static void shadow_run(uint32_t *px, int32_t w, int32_t h, const flow_filter_t *f, uint32_t current,
                       flow_point_t grid, uint32_t *mask, uint32_t *tmp, uint32_t *more)
{
    size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++)
        mask[i] = chan(px[i], 24) << 24;
    blur_run(mask, w, h, f->shadow.blur, grid, tmp, more);
    uint32_t colour = f->shadow.current ? current : f->shadow.colour;
    uint32_t ca = flow_alpha(colour);
    uint32_t cr = chan(colour, 16), cg = chan(colour, 8), cb = chan(colour, 0);
    int32_t ox = f->shadow.x, oy = f->shadow.y;
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) {
            int64_t sx = (int64_t)x - ox, sy = (int64_t)y - oy;
            if (sx < 0 || sy < 0 || sx >= w || sy >= h)
                continue;
            uint32_t s = chan(mask[(size_t)sy * (size_t)w + (size_t)sx], 24) * ca / 255;
            if (s == 0)
                continue;
            uint32_t *p = &px[(size_t)y * (size_t)w + (size_t)x];
            uint32_t keep = 255 - chan(*p, 24);
            uint32_t a = chan(*p, 24) + (s * keep + 127) / 255;
            uint32_t r = chan(*p, 16) + (cr * s * keep + 255 * 255 / 2) / (255 * 255);
            uint32_t g = chan(*p, 8) + (cg * s * keep + 255 * 255 / 2) / (255 * 255);
            uint32_t b = chan(*p, 0) + (cb * s * keep + 255 * 255 / 2) / (255 * 255);
            *p = pack(a, r < a ? r : a, g < a ? g : a, b < a ? b : a);
        }
}

// The blurs' line: a row (box_rows) or BOX_STRIP columns (box_columns) of
// the layer, or of the reduced copy (extended_line, extended_columns),
// which is at most two squares longer than the layer either way
// (reduced_side).
static size_t blur_line(int32_t w, int32_t h)
{
    size_t strip = (size_t)BOX_STRIP * ((size_t)h + 2), row = (size_t)w + 2;
    return row > strip ? row : strip;
}

static bool blurs(const flow_filter_t *f)
{
    return f->fn == FLOW_FILTER_BLUR || f->fn == FLOW_FILTER_DROP_SHADOW;
}

// The words beside the line and a drop shadow's mask: the most any of the
// list's reduced blurs needs.
static size_t more_words(int32_t w, int32_t h, const flow_filter_t *f, int32_t n)
{
    size_t most = 0;
    for (int32_t i = 0; i < n; i++)
        if (blurs(&f[i])) {
            size_t need = reduced_words(w, h, blur_reduction(f[i].shadow.blur));
            most = need > most ? need : most;
        }
    return most;
}

static bool shadowed(const flow_filter_t *f, int32_t n)
{
    for (int32_t i = 0; i < n; i++)
        if (f[i].fn == FLOW_FILTER_DROP_SHADOW)
            return true;
    return false;
}

size_t yonder_filter_work_bytes(int32_t w, int32_t h, const flow_filter_t *f, int32_t n)
{
    size_t words = blur_line(w, h) + more_words(w, h, f, n);
    if (shadowed(f, n))
        words += (size_t)w * (size_t)h;
    return words * sizeof(uint32_t);
}

int32_t yonder_filter_reads_more(const flow_filter_t *f, int32_t n)
{
    for (int32_t i = 0; i < n; i++)
        if (f[i].fn == FLOW_FILTER_URL)
            return 0;
    int64_t more = 0;
    for (int32_t i = 0; i < n; i++)
        if (blurs(&f[i])) {
            // Three boxes of r reach 3r (flow_shadow_blur_reach's rule).
            int32_t past = reduced_reads(blur_reduction(f[i].shadow.blur)) - 3 * blur_radius(f[i].shadow.blur);
            more += past > 0 ? past : 0;
        }
    return (int32_t)(more < INT32_MAX ? more : INT32_MAX);
}

int32_t yonder_filter_reduction(int32_t blur)
{
    return blur_reduction(blur).s;
}

void yonder_filter_run(uint32_t *px, int32_t w, int32_t h, const flow_filter_t *f, int32_t n,
                       uint32_t current, flow_point_t grid, void *work)
{
    if (w <= 0 || h <= 0)
        return;
    uint32_t *tmp = work;
    uint32_t *more = tmp + blur_line(w, h);
    uint32_t *mask = more + more_words(w, h, f, n);
    size_t count = (size_t)w * (size_t)h;
    for (int32_t i = 0; i < n; i++) {
        Colour c;
        switch (f[i].fn) {
        case FLOW_FILTER_URL: break;
        case FLOW_FILTER_BLUR: blur_run(px, w, h, f[i].shadow.blur, grid, tmp, more); break;
        case FLOW_FILTER_DROP_SHADOW: shadow_run(px, w, h, &f[i], current, grid, mask, tmp, more); break;
        default:
            colour_of(&f[i], &c);
            colour_run(px, count, &c);
            break;
        }
    }
}

// ── Blending (Compositing and Blending 1 § 10) ──────────────────────────

static double lum(const double c[3])
{
    return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
}

static void clip_colour(double c[3])
{
    double l = lum(c), n = c[0], x = c[0];
    for (int k = 1; k < 3; k++) {
        n = c[k] < n ? c[k] : n;
        x = c[k] > x ? c[k] : x;
    }
    for (int k = 0; k < 3; k++) {
        if (n < 0)
            c[k] = l + (c[k] - l) * l / (l - n);
        if (x > 1)
            c[k] = l + (c[k] - l) * (1 - l) / (x - l);
    }
}

static void set_lum(double c[3], double l)
{
    double d = l - lum(c);
    for (int k = 0; k < 3; k++)
        c[k] += d;
    clip_colour(c);
}

static double sat(const double c[3])
{
    double n = c[0], x = c[0];
    for (int k = 1; k < 3; k++) {
        n = c[k] < n ? c[k] : n;
        x = c[k] > x ? c[k] : x;
    }
    return x - n;
}

// The largest channel to `s`, the smallest to 0, the middle in proportion.
static void set_sat(double c[3], double s)
{
    int lo = 0, hi = 0;
    for (int k = 1; k < 3; k++) {
        lo = c[k] < c[lo] ? k : lo;
        hi = c[k] > c[hi] ? k : hi;
    }
    if (lo == hi) {
        c[0] = c[1] = c[2] = 0;
        return;
    }
    int mid = 3 - lo - hi;
    c[mid] = (c[mid] - c[lo]) * s / (c[hi] - c[lo]);
    c[hi] = s;
    c[lo] = 0;
}

static double hard_light(double b, double s)
{
    return s <= 0.5 ? b * 2 * s : b + (2 * s - 1) - b * (2 * s - 1);
}

static double soft_light(double b, double s)
{
    if (s <= 0.5)
        return b - (1 - 2 * s) * b * (1 - b);
    double d = b <= 0.25 ? ((16 * b - 12) * b + 4) * b : sqrt(b);
    return b + (2 * s - 1) * (d - b);
}

static double separable(flow_blend_t mode, double b, double s)
{
    switch (mode) {
    case FLOW_BLEND_MULTIPLY: return b * s;
    case FLOW_BLEND_SCREEN: return b + s - b * s;
    case FLOW_BLEND_OVERLAY: return hard_light(s, b);
    case FLOW_BLEND_DARKEN: return b < s ? b : s;
    case FLOW_BLEND_LIGHTEN: return b > s ? b : s;
    case FLOW_BLEND_COLOR_DODGE:
        return b == 0 ? 0 : s >= 1 ? 1 : (b / (1 - s) < 1 ? b / (1 - s) : 1);
    case FLOW_BLEND_COLOR_BURN:
        return b >= 1 ? 1 : s <= 0 ? 0 : 1 - ((1 - b) / s < 1 ? (1 - b) / s : 1);
    case FLOW_BLEND_HARD_LIGHT: return hard_light(b, s);
    case FLOW_BLEND_SOFT_LIGHT: return soft_light(b, s);
    case FLOW_BLEND_DIFFERENCE: return b > s ? b - s : s - b;
    case FLOW_BLEND_EXCLUSION: return b + s - 2 * b * s;
    default: return s;
    }
}

// B(Cb, Cs) for every mode but NORMAL and the plus- ones, into `out`.
static void blend_fn(flow_blend_t mode, const double b[3], const double s[3], double out[3])
{
    for (int k = 0; k < 3; k++)
        out[k] = s[k];
    switch (mode) {
    case FLOW_BLEND_HUE:
        set_sat(out, sat(b));
        set_lum(out, lum(b));
        return;
    case FLOW_BLEND_SATURATION:
        for (int k = 0; k < 3; k++)
            out[k] = b[k];
        set_sat(out, sat(s));
        set_lum(out, lum(b));
        return;
    case FLOW_BLEND_COLOR: set_lum(out, lum(b)); return;
    case FLOW_BLEND_LUMINOSITY:
        for (int k = 0; k < 3; k++)
            out[k] = b[k];
        set_lum(out, lum(s));
        return;
    default:
        for (int k = 0; k < 3; k++)
            out[k] = separable(mode, b[k], s[k]);
        return;
    }
}

void yonder_layer_lay(uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip, const uint32_t *px,
                      int32_t w, int32_t h, int32_t dx, int32_t dy, uint8_t alpha,
                      flow_blend_t mode)
{
    if (mode == FLOW_BLEND_PLUS_DARKER)
        mode = FLOW_BLEND_NORMAL;
    int64_t x0 = dx > clip.x ? dx : clip.x, y0 = dy > clip.y ? dy : clip.y;
    int64_t x1 = (int64_t)dx + w, y1 = (int64_t)dy + h;
    if (x1 > (int64_t)clip.x + clip.w)
        x1 = (int64_t)clip.x + clip.w;
    if (y1 > (int64_t)clip.y + clip.h)
        y1 = (int64_t)clip.y + clip.h;
    for (int64_t y = y0; y < y1; y++)
        for (int64_t x = x0; x < x1; x++) {
            uint32_t s = px[(size_t)(y - dy) * (size_t)w + (size_t)(x - dx)];
            uint32_t sa = chan(s, 24) * alpha;          // of 255 * 255
            if (sa == 0)
                continue;
            uint32_t *d = &dst[(size_t)y * pitch + (size_t)x];
            uint32_t keep = 255 * 255 - sa, out = *d & 0xff000000u;
            if (mode == FLOW_BLEND_NORMAL) {
                for (int c = 0; c < 3; c++) {
                    uint32_t v = (chan(s, 8 * c) * alpha * 255 + chan(*d, 8 * c) * keep + 255 * 255 / 2) /
                                 (255 * 255);
                    out |= (v > 255 ? 255 : v) << (8 * c);
                }
                *d = out;
                continue;
            }
            // Blue, green, red: a channel's place is 8 * c, as above.
            double a = sa / (255.0 * 255.0), cs[3], cb[3], bl[3];
            for (int c = 0; c < 3; c++) {
                cs[c] = chan(s, 8 * c) / (double)chan(s, 24);
                cs[c] = cs[c] > 1 ? 1 : cs[c];
                cb[c] = chan(*d, 8 * c) / 255.0;
            }
            if (mode == FLOW_BLEND_PLUS_LIGHTER) {
                for (int c = 0; c < 3; c++)
                    bl[c] = cb[c] + cs[c] * a;
            } else {
                // Lum's weights are red's, green's and blue's: hand them over
                // in that order and back.
                const double rb[3] = {cb[2], cb[1], cb[0]}, rs[3] = {cs[2], cs[1], cs[0]};
                double mixed[3];
                blend_fn(mode, rb, rs, mixed);
                const double m[3] = {mixed[2], mixed[1], mixed[0]};
                for (int c = 0; c < 3; c++)
                    bl[c] = (1 - a) * cb[c] + a * m[c];
            }
            for (int c = 0; c < 3; c++)
                out |= byte(bl[c]) << (8 * c);
            *d = out;
        }
}

bool yonder_backdrop_take(const uint32_t *src, uint32_t pitch, os64_gui_rect_t whole, int32_t x0,
                          int32_t y0, int32_t w, int32_t h, uint32_t *out)
{
    if (whole.w <= 0 || whole.h <= 0)
        return false;
    int64_t last_x = (int64_t)whole.x + whole.w - 1, last_y = (int64_t)whole.y + whole.h - 1;
    for (int32_t j = 0; j < h; j++) {
        int64_t y = (int64_t)y0 + j;
        y = y < whole.y ? whole.y : y > last_y ? last_y : y;
        for (int32_t i = 0; i < w; i++) {
            int64_t x = (int64_t)x0 + i;
            x = x < whole.x ? whole.x : x > last_x ? last_x : x;
            out[(size_t)j * (size_t)w + (size_t)i] = 0xff000000u | (src[(size_t)y * pitch + (size_t)x] & 0xffffffu);
        }
    }
    return true;
}

void yonder_backdrop_put(uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip, const uint32_t *layer,
                         int32_t w, int32_t h, int32_t x0, int32_t y0, os64_gui_rect_t box,
                         const uint8_t *cover)
{
    int64_t ax = box.x > clip.x ? box.x : clip.x, ay = box.y > clip.y ? box.y : clip.y;
    int64_t bx = (int64_t)box.x + box.w, by = (int64_t)box.y + box.h;
    if (bx > (int64_t)clip.x + clip.w)
        bx = (int64_t)clip.x + clip.w;
    if (by > (int64_t)clip.y + clip.h)
        by = (int64_t)clip.y + clip.h;
    if (ax < x0)
        ax = x0;
    if (ay < y0)
        ay = y0;
    if (bx > (int64_t)x0 + w)
        bx = (int64_t)x0 + w;
    if (by > (int64_t)y0 + h)
        by = (int64_t)y0 + h;
    for (int64_t y = ay; y < by; y++)
        for (int64_t x = ax; x < bx; x++) {
            uint32_t c = cover != NULL ? cover[(size_t)(y - box.y) * (size_t)box.w + (size_t)(x - box.x)] : 255;
            if (c == 0)
                continue;
            uint32_t s = layer[(size_t)(y - y0) * (size_t)w + (size_t)(x - x0)];
            uint32_t *d = &dst[(size_t)y * pitch + (size_t)x];
            uint32_t keep = 255 - chan(s, 24), out = *d & 0xff000000u;
            for (int k = 0; k < 3; k++) {
                uint32_t was = chan(*d, 8 * k);
                uint32_t over = chan(s, 8 * k) + (was * keep + 127) / 255;
                over = over > 255 ? 255 : over;
                out |= ((over * c + was * (255 - c) + 127) / 255) << (8 * k);
            }
            *d = out;
        }
}

// ── Masks (MASKS.md § 3) ─────────────────────────────────────────────────

// x / 255, rounded to the nearest, for x up to 255 * 255 * 2.
static uint32_t over255(uint32_t x)
{
    return (x + 127) / 255;
}

void yonder_mask_take(const uint32_t *layer, size_t n, bool luminance, uint8_t *out)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t p = layer[i];
        if (!luminance) {
            out[i] = (uint8_t)chan(p, 24);
            continue;
        }
        // The colour premultiplied, so its luminance is already times its
        // alpha: feColorMatrix's luminance-to-alpha weights (Masking 1
        // § 7.10.1), in 10000ths.
        uint32_t y = (2125 * chan(p, 16) + 7154 * chan(p, 8) + 721 * chan(p, 0) + 5000) / 10000;
        out[i] = (uint8_t)(y > 255 ? 255 : y);
    }
}

void yonder_mask_combine(uint8_t *run, const uint8_t *c, size_t n, flow_composite_t op)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t s = c[i], d = run[i], v;
        switch (op) {
        case FLOW_COMPOSITE_SUBTRACT: v = over255(s * (255 - d)); break;
        case FLOW_COMPOSITE_INTERSECT: v = over255(s * d); break;
        case FLOW_COMPOSITE_EXCLUDE: v = over255(s * (255 - d) + d * (255 - s)); break;
        default: v = s + over255(d * (255 - s)); break;
        }
        run[i] = (uint8_t)(v > 255 ? 255 : v);
    }
}

void yonder_layer_cover(uint32_t *layer, size_t n, const uint8_t *cover)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t c = cover[i], p = layer[i];
        if (c == 255)
            continue;
        layer[i] = pack(over255(chan(p, 24) * c), over255(chan(p, 16) * c),
                        over255(chan(p, 8) * c), over255(chan(p, 0) * c));
    }
}
