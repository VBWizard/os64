// filter.c — a filtered group's pixels (filter.h; FILTER.md).

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

void yonder_filter_recover(const yonder_sheets_t *s, uint32_t *out)
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

// One box blur of radius `r` along rows (or columns), each of a pixel's
// four 8-bit channels apart, reading nothing (0) past the edges.
static void box_pass(uint32_t *px, int32_t w, int32_t h, int32_t r, bool rows, uint32_t *tmp)
{
    int32_t lines = rows ? h : w, len = rows ? w : h;
    size_t step = rows ? 1 : (size_t)w;
    uint32_t span = 2 * (uint32_t)r + 1;
    for (int32_t l = 0; l < lines; l++) {
        uint32_t *v = px + (rows ? (size_t)l * (size_t)w : (size_t)l);
        uint32_t sum[4] = {0, 0, 0, 0};
        for (int32_t i = 0; i <= r && i < len; i++)
            for (int c = 0; c < 4; c++)
                sum[c] += chan(v[(size_t)i * step], 8 * c);
        for (int32_t i = 0; i < len; i++) {
            uint32_t o = 0;
            for (int c = 0; c < 4; c++)
                o |= ((sum[c] + span / 2) / span) << (8 * c);
            tmp[i] = o;
            int32_t out = i - r, in = i + r + 1;
            for (int c = 0; c < 4; c++) {
                if (in < len)
                    sum[c] += chan(v[(size_t)in * step], 8 * c);
                if (out >= 0)
                    sum[c] -= chan(v[(size_t)out * step], 8 * c);
            }
        }
        for (int32_t i = 0; i < len; i++)
            v[(size_t)i * step] = tmp[i];
    }
}

// Three box blurs each way, for a shadow blur radius `blur` (flow.h's).
static void blur_run(uint32_t *px, int32_t w, int32_t h, int32_t blur, uint32_t *tmp)
{
    int32_t most = 2 * FLOW_FILTER_BLUR_MAX;
    int32_t r = (blur < most ? blur : most) / 2;
    if (r <= 0)
        return;
    for (int k = 0; k < 3; k++) {
        box_pass(px, w, h, r, true, tmp);
        box_pass(px, w, h, r, false, tmp);
    }
}

// The layer's coverage, blurred, moved and tinted, laid under it.
static void shadow_run(uint32_t *px, int32_t w, int32_t h, const flow_filter_t *f, uint32_t current,
                       uint32_t *mask, uint32_t *tmp)
{
    size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++)
        mask[i] = chan(px[i], 24) << 24;
    blur_run(mask, w, h, f->shadow.blur, tmp);
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

size_t yonder_filter_work_bytes(int32_t w, int32_t h, const flow_filter_t *f, int32_t n)
{
    size_t bytes = (size_t)(w > h ? w : h) * sizeof(uint32_t);
    for (int32_t i = 0; i < n; i++)
        if (f[i].fn == FLOW_FILTER_DROP_SHADOW) {
            bytes += (size_t)w * (size_t)h * sizeof(uint32_t);
            break;
        }
    return bytes;
}

void yonder_filter_run(uint32_t *px, int32_t w, int32_t h, const flow_filter_t *f, int32_t n,
                       uint32_t current, void *work)
{
    if (w <= 0 || h <= 0)
        return;
    uint32_t *tmp = work;
    uint32_t *mask = tmp + (w > h ? w : h);
    size_t count = (size_t)w * (size_t)h;
    for (int32_t i = 0; i < n; i++) {
        Colour c;
        switch (f[i].fn) {
        case FLOW_FILTER_URL: break;
        case FLOW_FILTER_BLUR: blur_run(px, w, h, f[i].shadow.blur, tmp); break;
        case FLOW_FILTER_DROP_SHADOW: shadow_run(px, w, h, &f[i], current, mask, tmp); break;
        default:
            colour_of(&f[i], &c);
            colour_run(px, count, &c);
            break;
        }
    }
}

void yonder_filter_lay(uint32_t *dst, uint32_t pitch, os64_gui_rect_t clip, const uint32_t *px,
                       int32_t w, int32_t h, int32_t dx, int32_t dy, uint8_t alpha)
{
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
            for (int c = 0; c < 3; c++) {
                uint32_t v = (chan(s, 8 * c) * alpha * 255 + chan(*d, 8 * c) * keep + 255 * 255 / 2) /
                             (255 * 255);
                out |= (v > 255 ? 255 : v) << (8 * c);
            }
            *d = out;
        }
}
