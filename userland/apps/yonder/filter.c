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
