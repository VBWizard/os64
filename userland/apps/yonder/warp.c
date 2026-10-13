// warp.c — a group drawn through its matrix, laid onto the page (warp.h).

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

// One channel of a sheet pixel.
static double part(uint32_t p, int shift)
{
    return (double)((p >> shift) & 0xffu);
}

// One channel of the backdrop under B and T, at opacity `a` of 1.
static uint32_t lay(double b, double w, double d, double a)
{
    double t = (w - b) / 255;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    double v = a * b + (1 - a + a * t) * d;
    v = v < 0 ? 0 : v > 255 ? 255 : v;
    return (uint32_t)(v + 0.5);
}

bool yonder_warp(uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip, const yonder_sheets_t *s,
                 const double m[6], const double inv[6], uint8_t alpha)
{
    if (inv == NULL || s->w <= 0 || s->h <= 0)
        return false;
    // Where the sheets land, its corners' bounds, met with the clip.
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (int k = 0; k < 4; k++) {
        // In long double, as every sum of a matrix's products here: a
        // finite matrix's terms may each pass a double and cancel.
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
    bool exact = yonder_warp_exact(m);
    double a = alpha / 255.0;
    for (int32_t y = (int32_t)y0; y < (int32_t)y1; y++) {
        uint32_t *row = dst + (size_t)y * pitch;
        for (int32_t x = (int32_t)x0; x < (int32_t)x1; x++) {
            long double cx = x + 0.5L, cy = y + 0.5L;
            double u = (double)(inv[0] * cx + inv[2] * cy + inv[4]);
            double v = (double)(inv[1] * cx + inv[3] * cy + inv[5]);
            // Inside the sheets' landing a centre goes back inside the
            // sheets; this holds the index casts below to what they can say
            // should an inverse that does not undo `m` ever be handed in.
            if (!isfinite(u) || !isfinite(v) || fabs(u) > 1e15 || fabs(v) > 1e15)
                continue;
            double b[3], w[3];
            if (exact) {
                int64_t i = (int64_t)floor(u), j = (int64_t)floor(v);
                if (i < 0 || j < 0 || i >= s->w || j >= s->h)
                    continue;
                uint32_t pb = s->black[(size_t)j * (size_t)s->w + (size_t)i];
                uint32_t pw = s->white[(size_t)j * (size_t)s->w + (size_t)i];
                for (int c = 0; c < 3; c++) {
                    b[c] = part(pb, 16 - 8 * c);
                    w[c] = part(pw, 16 - 8 * c);
                }
            } else {
                // The four pixels whose centres are round (u, v); one off
                // the sheets is nothing painted, black on black and white
                // on white.
                double sx = u - 0.5, sy = v - 0.5;
                double fx = floor(sx), fy = floor(sy);
                if (fx < -1 || fy < -1 || fx >= s->w || fy >= s->h)
                    continue;
                double ax = sx - fx, ay = sy - fy;
                for (int c = 0; c < 3; c++)
                    b[c] = w[c] = 0;
                for (int k = 0; k < 4; k++) {
                    int64_t i = (int64_t)fx + (k & 1), j = (int64_t)fy + (k >> 1);
                    double wt = (k & 1 ? ax : 1 - ax) * (k >> 1 ? ay : 1 - ay);
                    bool in = i >= 0 && j >= 0 && i < s->w && j < s->h;
                    uint32_t pb = in ? s->black[(size_t)j * (size_t)s->w + (size_t)i] : 0;
                    uint32_t pw = in ? s->white[(size_t)j * (size_t)s->w + (size_t)i] : 0xffffffu;
                    for (int c = 0; c < 3; c++) {
                        b[c] += wt * part(pb, 16 - 8 * c);
                        w[c] += wt * part(pw, 16 - 8 * c);
                    }
                }
            }
            uint32_t d = row[x], out = 0;
            for (int c = 0; c < 3; c++)
                out |= lay(b[c], w[c], part(d, 16 - 8 * c), a) << (16 - 8 * c);
            row[x] = (d & 0xff000000u) | out;
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
    for (int64_t y = y0; y < y1; y++) {
        uint32_t *row = dst + (size_t)y * pitch;
        size_t j = (size_t)(y - dy);
        for (int64_t x = x0; x < x1; x++) {
            size_t at = j * (size_t)s->w + (size_t)(x - dx);
            uint32_t c = (mask[at] >> 8) & 0xffu;
            if (c == 0)
                continue;
            uint32_t pb = s->black[at], pw = s->white[at], d = row[x], out = 0;
            for (int k = 0; k < 3; k++)
                out |= lay(part(pb, 16 - 8 * k), part(pw, 16 - 8 * k), part(d, 16 - 8 * k),
                           c / 255.0) << (16 - 8 * k);
            row[x] = (d & 0xff000000u) | out;
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
