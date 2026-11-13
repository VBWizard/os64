// yonder's warp (userland/apps/yonder/warp.h) on the host, under the
// sanitizers, against references written here apart from it: a group's
// content, in straight alpha, painted over black and over white by plain
// source-over makes the two sheets; the warp must lay them onto any
// backdrop as the content itself would be laid there (PILE3.md §
// Transforms, the painter).

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "warp.h"

static int s_failed, s_checks;

static void expect(const char *what, int ok)
{
    s_checks++;
    if (!ok) {
        s_failed++;
        printf("FAIL %s\n", what);
    }
}

static uint32_t s_seed = 12345;
static uint32_t rnd(void)
{
    s_seed = s_seed * 1103515245u + 12345u;
    return s_seed >> 8;
}

// Straight-alpha content over an opaque pixel, rounded: the reference.
static uint32_t over(uint32_t argb, uint32_t d)
{
    uint32_t a = argb >> 24, out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t s = (argb >> sh) & 0xff, b = (d >> sh) & 0xff;
        out |= ((s * a + b * (255 - a) + 127) / 255) << sh;
    }
    return out;
}

static int near(uint32_t p, uint32_t q, int tol)
{
    for (int sh = 0; sh <= 16; sh += 8) {
        int a = (int)((p >> sh) & 0xff), b = (int)((q >> sh) & 0xff);
        if (abs(a - b) > tol)
            return 0;
    }
    return 1;
}

typedef struct {
    int32_t w, h;
    uint32_t *content, *black, *white;
} Group;

static void group_make(Group *g, int32_t w, int32_t h, int translucent)
{
    g->w = w;
    g->h = h;
    g->content = malloc((size_t)w * h * 4);
    g->black = malloc((size_t)w * h * 4);
    g->white = malloc((size_t)w * h * 4);
    for (int32_t k = 0; k < w * h; k++) {
        uint32_t a = translucent ? rnd() & 0xff : (rnd() & 1 ? 255 : 0);
        g->content[k] = a << 24 | (rnd() & 0xffffffu);
        g->black[k] = over(g->content[k], 0);
        g->white[k] = over(g->content[k], 0xffffffu);
    }
}

static void group_free(Group *g)
{
    free(g->content);
    free(g->black);
    free(g->white);
}

// A random backdrop, its top byte the surface's own, which must survive.
static uint32_t *backdrop(int32_t w, int32_t h)
{
    uint32_t *d = malloc((size_t)w * h * 4);
    for (int32_t k = 0; k < w * h; k++)
        d[k] = 0xab000000u | (rnd() & 0xffffffu);
    return d;
}

// The warp takes its matrix undone from its caller (flow_affine_invert in
// yonder); here the plain formula, or none where the matrix has none.
static bool lay(uint32_t *d, uint32_t pitch, os64_gui_rect_t clip, const yonder_sheets_t *s,
                const double m[6], uint8_t alpha)
{
    double det = m[0] * m[3] - m[1] * m[2];
    if (det == 0)
        return yonder_warp(d, pitch, clip, s, m, NULL, alpha);
    double inv[6] = {m[3] / det, -m[1] / det, -m[2] / det, m[0] / det, 0, 0};
    inv[4] = -(inv[0] * m[4] + inv[2] * m[5]);
    inv[5] = -(inv[1] * m[4] + inv[3] * m[5]);
    return yonder_warp(d, pitch, clip, s, m, inv, alpha);
}

static int blank_px(uint32_t b, uint32_t w)
{
    return (b & 0xffffffu) == 0 && (w & 0xffffffu) == 0xffffffu;
}

// The contract's pixel, worked here in doubles apart from the warp: dst
// (x, y)'s centre taken back through `m` and sampled from the sheets — by
// index if `exact`, else the four pixels round it, off the sheets nothing
// painted — then laid on `was` at `alpha`; only a pixel inside the whole
// pixels holding where the sheets land is laid. *touched is 0 where
// nothing painted reaches the centre: there the pixel must be `was`, to
// the bit.
static uint32_t contract(const Group *g, const double m[6], int exact, int32_t x, int32_t y,
                         uint32_t was, uint8_t alpha, int *touched)
{
    double lx0 = INFINITY, ly0 = INFINITY, lx1 = -INFINITY, ly1 = -INFINITY;
    for (int k = 0; k < 4; k++) {
        double cx = k & 1 ? g->w : 0, cy = k & 2 ? g->h : 0;
        lx0 = fmin(lx0, m[0] * cx + m[2] * cy + m[4]);
        lx1 = fmax(lx1, m[0] * cx + m[2] * cy + m[4]);
        ly0 = fmin(ly0, m[1] * cx + m[3] * cy + m[5]);
        ly1 = fmax(ly1, m[1] * cx + m[3] * cy + m[5]);
    }
    *touched = 0;
    if (x < floor(lx0 + 1e-6) || x >= ceil(lx1 - 1e-6) || y < floor(ly0 + 1e-6) ||
        y >= ceil(ly1 - 1e-6))
        return was;
    double det = m[0] * m[3] - m[1] * m[2];
    double px = x + 0.5 - m[4], py = y + 0.5 - m[5];
    double u = (m[3] * px - m[2] * py) / det, v = (m[0] * py - m[1] * px) / det;
    double b[3] = {0, 0, 0}, w[3] = {0, 0, 0};
    int painted = 0;
    if (exact) {
        int i = (int)floor(u), j = (int)floor(v);
        if (i >= 0 && j >= 0 && i < g->w && j < g->h) {
            uint32_t pb = g->black[j * g->w + i], pw = g->white[j * g->w + i];
            painted = !blank_px(pb, pw);
            for (int c = 0; c < 3; c++) {
                b[c] = (pb >> (16 - 8 * c)) & 0xff;
                w[c] = (pw >> (16 - 8 * c)) & 0xff;
            }
        }
    } else {
        double su = u - 0.5, sv = v - 0.5, fx = floor(su), fy = floor(sv);
        if (fx >= -1 && fy >= -1 && fx < g->w && fy < g->h)
            for (int k = 0; k < 4; k++) {
                int i = (int)fx + (k & 1), j = (int)fy + (k >> 1);
                double wt = (k & 1 ? su - fx : 1 - (su - fx)) * (k >> 1 ? sv - fy : 1 - (sv - fy));
                int in = i >= 0 && j >= 0 && i < g->w && j < g->h;
                uint32_t pb = in ? g->black[j * g->w + i] : 0;
                uint32_t pw = in ? g->white[j * g->w + i] : 0xffffffu;
                painted |= !blank_px(pb, pw);
                for (int c = 0; c < 3; c++) {
                    b[c] += wt * ((pb >> (16 - 8 * c)) & 0xff);
                    w[c] += wt * ((pw >> (16 - 8 * c)) & 0xff);
                }
            }
    }
    *touched = painted;
    if (!painted)
        return was;
    double a = alpha / 255.0;
    uint32_t out = was & 0xff000000u;
    for (int c = 0; c < 3; c++) {
        double d = (was >> (16 - 8 * c)) & 0xff, t = (w[c] - b[c]) / 255;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        out |= (uint32_t)(a * b[c] + (1 - a + a * t) * d + 0.5) << (16 - 8 * c);
    }
    return out;
}

// A group some of whose pixels are nothing painted: a third of them, the
// rest random content, translucent or not.
static void group_holes(Group *g, int32_t w, int32_t h, int translucent)
{
    group_make(g, w, h, translucent);
    for (int32_t k = 0; k < w * h; k++)
        if (rnd() % 3 == 0) {
            g->content[k] &= 0xffffffu;
            g->black[k] = over(g->content[k], 0);
            g->white[k] = over(g->content[k], 0xffffffu);
        }
}

int main(void)
{
    // Laid where it is, the content over any backdrop, opaque, empty or
    // translucent, to within the one level two roundings can part by.
    {
        Group g;
        group_make(&g, 16, 12, 1);
        uint32_t *d = backdrop(16, 12), *was = malloc(16 * 12 * 4);
        memcpy(was, d, 16 * 12 * 4);
        yonder_sheets_t s = {g.black, g.white, 16, 12};
        const double id[6] = {1, 0, 0, 1, 0, 0};
        int ok = lay(d, 16, (os64_gui_rect_t){0, 0, 16, 12}, &s, id, 255);
        for (int32_t k = 0; k < 16 * 12; k++)
            ok = ok && near(d[k], over(g.content[k], was[k]), 1) && (d[k] >> 24) == 0xab;
        expect("warp: in place, the content over the backdrop, its top byte kept", ok);
        free(d);
        free(was);
        group_free(&g);
    }

    // Opaque and empty paint is laid exactly: B is the colour or T is all
    // of the backdrop.
    {
        Group g;
        group_make(&g, 9, 7, 0);
        uint32_t *d = backdrop(9, 7), *was = malloc(9 * 7 * 4);
        memcpy(was, d, 9 * 7 * 4);
        yonder_sheets_t s = {g.black, g.white, 9, 7};
        const double id[6] = {1, 0, 0, 1, 0, 0};
        lay(d, 9, (os64_gui_rect_t){0, 0, 9, 7}, &s, id, 255);
        int ok = 1;
        for (int32_t k = 0; k < 9 * 7; k++)
            ok = ok && (d[k] & 0xffffffu) == (over(g.content[k], was[k]) & 0xffffffu);
        expect("warp: opaque and empty paint laid exactly", ok);
        free(d);
        free(was);
        group_free(&g);
    }

    // A quarter turn and a mirror at whole pixels are copies by index:
    // dst (x, y) is the sheet pixel the turn takes back to, exactly.
    {
        Group g;
        group_make(&g, 10, 6, 0);
        yonder_sheets_t s = {g.black, g.white, 10, 6};
        // (x, y) to (5 - y + 20, x + 3): a quarter turn clockwise, moved.
        const double turn[6] = {0, 1, -1, 0, 26, 3};
        uint32_t *d = backdrop(40, 20), *was = malloc(40 * 20 * 4);
        memcpy(was, d, 40 * 20 * 4);
        int ok = yonder_warp_exact(turn) && lay(d, 40, (os64_gui_rect_t){0, 0, 40, 20}, &s,
                                                        turn, 255);
        for (int32_t y = 0; y < 20; y++)
            for (int32_t x = 0; x < 40; x++) {
                // dst pixel (x, y) covers the sheet pixel (y - 3, 25 - x).
                int32_t i = y - 3, j = 25 - x;
                uint32_t want = i >= 0 && j >= 0 && i < 10 && j < 6
                                    ? over(g.content[j * 10 + i], was[y * 40 + x])
                                    : was[y * 40 + x];
                ok = ok && (d[y * 40 + x] & 0xffffffu) == (want & 0xffffffu);
            }
        expect("warp: a quarter turn copied by index", ok);
        const double mirror[6] = {-1, 0, 0, 1, 10, 0};
        memcpy(d, was, 40 * 20 * 4);
        ok = yonder_warp_exact(mirror) &&
             lay(d, 40, (os64_gui_rect_t){0, 0, 40, 20}, &s, mirror, 255);
        for (int32_t y = 0; y < 6; y++)
            for (int32_t x = 0; x < 10; x++)
                ok = ok && (d[y * 40 + x] & 0xffffffu) ==
                               (over(g.content[y * 10 + (9 - x)], was[y * 40 + x]) & 0xffffffu);
        expect("warp: a mirror copied by index", ok);
        const double half[6] = {0, 1, -1, 0, 26.5, 3};
        const double scale[6] = {2, 0, 0, 2, 0, 0};
        const double hair[6] = {0, 1, -1, 1e-9, 26, 3};
        expect("warp: a turn moved half a pixel, a scale, or a hair from a turn is no copy",
               !yonder_warp_exact(half) && !yonder_warp_exact(scale) && !yonder_warp_exact(hair));
        free(d);
        free(was);
        group_free(&g);
    }

    // Bilinear: a scale and a turn, each dst pixel against B and W mixed
    // from the four pixels round where its centre lands — off the sheets,
    // nothing painted — laid as B + T D, to within one level.
    {
        Group g;
        group_make(&g, 12, 8, 1);
        yonder_sheets_t s = {g.black, g.white, 12, 8};
        const double c = cos(0.5), sn = sin(0.5);
        const double ms[2][6] = {{1.5, 0, 0, 1.5, 3.25, 2.75}, {c, sn, -sn, c, 14, 1}};
        for (int t = 0; t < 2; t++) {
            uint32_t *d = backdrop(48, 32), *was = malloc(48 * 32 * 4);
            memcpy(was, d, 48 * 32 * 4);
            const double *m = ms[t];
            lay(d, 48, (os64_gui_rect_t){0, 0, 48, 32}, &s, m, 255);
            double det = m[0] * m[3] - m[1] * m[2];
            int ok = 1;
            for (int32_t y = 0; y < 32; y++)
                for (int32_t x = 0; x < 48; x++) {
                    double px = x + 0.5 - m[4], py = y + 0.5 - m[5];
                    double u = (m[3] * px - m[2] * py) / det - 0.5;
                    double v = (m[0] * py - m[1] * px) / det - 0.5;
                    double fx = floor(u), fy = floor(v);
                    uint32_t want = was[y * 48 + x];
                    if (fx >= -1 && fy >= -1 && fx < 12 && fy < 8) {
                        uint32_t out = 0;
                        for (int sh = 0; sh <= 16; sh += 8) {
                            double b = 0, w = 0;
                            for (int k = 0; k < 4; k++) {
                                int i = (int)fx + (k & 1), j = (int)fy + (k >> 1);
                                double wt = (k & 1 ? u - fx : 1 - (u - fx)) *
                                            (k >> 1 ? v - fy : 1 - (v - fy));
                                int in = i >= 0 && j >= 0 && i < 12 && j < 8;
                                b += wt * (in ? (g.black[j * 12 + i] >> sh) & 0xff : 0);
                                w += wt * (in ? (g.white[j * 12 + i] >> sh) & 0xff : 255);
                            }
                            double dd = (was[y * 48 + x] >> sh) & 0xff;
                            double vv = b + (w - b) / 255 * dd;
                            out |= (uint32_t)(vv + 0.5) << sh;
                        }
                        want = out;
                    }
                    ok = ok && near(d[y * 48 + x], want, 1);
                }
            expect(t == 0 ? "warp: a scale, bilinear" : "warp: a turn, bilinear", ok);
            free(d);
            free(was);
        }
        group_free(&g);
    }

    // Under its own opacity a, the group is a B + (1 - a + a T) D: the
    // content over D, laid over D at a.
    {
        Group g;
        group_make(&g, 8, 8, 1);
        uint32_t *d = backdrop(8, 8), *was = malloc(8 * 8 * 4);
        memcpy(was, d, 8 * 8 * 4);
        yonder_sheets_t s = {g.black, g.white, 8, 8};
        const double id[6] = {1, 0, 0, 1, 0, 0};
        lay(d, 8, (os64_gui_rect_t){0, 0, 8, 8}, &s, id, 128);
        int ok = 1;
        for (int32_t k = 0; k < 64; k++) {
            uint32_t g1 = over(g.content[k], was[k]);
            ok = ok && near(d[k], over(128u << 24 | g1, was[k]), 2);
        }
        expect("warp: the group's opacity over what it is laid on", ok);
        free(d);
        free(was);
        group_free(&g);
    }

    // Only inside the clip; and a matrix that cannot be undone lays nothing.
    {
        Group g;
        group_make(&g, 10, 10, 0);
        yonder_sheets_t s = {g.black, g.white, 10, 10};
        uint32_t *d = backdrop(10, 10), *was = malloc(400);
        memcpy(was, d, 400);
        const double id[6] = {1, 0, 0, 1, 0, 0};
        lay(d, 10, (os64_gui_rect_t){2, 3, 4, 5}, &s, id, 255);
        int ok = 1;
        for (int32_t y = 0; y < 10; y++)
            for (int32_t x = 0; x < 10; x++)
                if (x < 2 || x >= 6 || y < 3 || y >= 8)
                    ok = ok && d[y * 10 + x] == was[y * 10 + x];
        expect("warp: nothing laid outside the clip", ok);
        memcpy(d, was, 400);
        const double flat[6] = {1, 2, 2, 4, 0, 0};
        ok = !lay(d, 10, (os64_gui_rect_t){0, 0, 10, 10}, &s, flat, 255) &&
             memcmp(d, was, 400) == 0;
        expect("warp: with no matrix undone, nothing is laid", ok);
        free(d);
        free(was);
        group_free(&g);
    }

    // A finite matrix whose inverse's terms each pass a double and cancel:
    // matrix(1e-308, 0, 1, 1, -1e-307, 0) takes every destination centre
    // on the diagonal back to u = 10, inside a solid 20 by 20 sheet — so
    // the diagonal is laid, in the sheet's one colour.
    {
        uint32_t black[400], white[400];
        for (int k = 0; k < 400; k++)
            black[k] = white[k] = 0x336699u;
        yonder_sheets_t s = {black, white, 20, 20};
        uint32_t *d = backdrop(24, 24);
        const double m[6] = {1e-308, 0, 1, 1, -1e-307, 0};
        const double inv[6] = {1e308, 0, -1e308, 1, 10, 0};
        yonder_warp(d, 24, (os64_gui_rect_t){0, 0, 24, 24}, &s, m, inv, 255);
        int ok = 1;
        for (int y = 3; y < 17; y++)
            ok = ok && (d[y * 24 + y] & 0xffffffu) == 0x336699u;
        expect("warp: a diagonal whose inverse terms cancel past a double is laid", ok);
        free(d);
    }

    // Random groups with holes through random matrices — turns, scales and
    // shears, an eighth of them quarter turns and mirrors at whole pixels —
    // at random opacities and clips: each pixel the contract's, to within
    // the level two roundings part by (two under an opacity), and a pixel
    // nothing painted reaches is what it was, to the bit.
    {
        int ok = 1, laid = 0, kept = 0;
        for (int t = 0; t < 300 && ok; t++) {
            Group g;
            int32_t gw = 1 + (int32_t)(rnd() % 40), gh = 1 + (int32_t)(rnd() % 40);
            group_holes(&g, gw, gh, t & 1);
            yonder_sheets_t s = {g.black, g.white, gw, gh};
            double m[6];
            int exact = t % 8 == 0;
            if (exact) {
                static const double q[4][4] = {{0, 1, -1, 0}, {0, -1, 1, 0}, {-1, 0, 0, 1}, {1, 0, 0, -1}};
                const double *e = q[rnd() % 4];
                m[0] = e[0];
                m[1] = e[1];
                m[2] = e[2];
                m[3] = e[3];
                m[4] = 10 + (double)(rnd() % 50);
                m[5] = 10 + (double)(rnd() % 40);
            } else {
                double ang = (rnd() % 6283) / 1000.0, sc = 0.3 + (rnd() % 2700) / 1000.0;
                m[0] = cos(ang) * sc;
                m[1] = sin(ang) * sc;
                m[2] = -sin(ang) * sc * (0.5 + (rnd() % 1000) / 1000.0);
                m[3] = cos(ang) * sc;
                m[4] = 5 + (rnd() % 60000) / 1000.0;
                m[5] = 5 + (rnd() % 50000) / 1000.0;
            }
            uint8_t alpha = t % 3 == 0 ? (uint8_t)(rnd() & 0xff) : 255;
            int32_t dw = 90, dh = 80, cx = (int32_t)(rnd() % 20), cy = (int32_t)(rnd() % 20);
            os64_gui_rect_t clip = {cx, cy, 50 + (int32_t)(rnd() % (uint32_t)(dw - 50 - cx + 1)),
                                    40 + (int32_t)(rnd() % (uint32_t)(dh - 40 - cy + 1))};
            uint32_t *d = backdrop(dw, dh), *was = malloc((size_t)dw * dh * 4);
            memcpy(was, d, (size_t)dw * dh * 4);
            ok = ok && exact == yonder_warp_exact(m) && lay(d, (uint32_t)dw, clip, &s, m, alpha);
            for (int32_t y = 0; y < dh && ok; y++)
                for (int32_t x = 0; x < dw && ok; x++) {
                    int touched = 0, in = x >= clip.x && y >= clip.y && x < clip.x + clip.w &&
                                         y < clip.y + clip.h;
                    uint32_t want = contract(&g, m, exact, x, y, was[y * dw + x], alpha, &touched);
                    if (in && touched) {
                        laid++;
                        ok = near(d[y * dw + x], want, alpha == 255 ? 1 : 2) &&
                             (d[y * dw + x] >> 24) == 0xab;
                    } else {
                        kept++;
                        ok = d[y * dw + x] == was[y * dw + x];
                    }
                    if (!ok)
                        printf("  case %d: (%d, %d) is %06x, the contract %06x%s\n", t, x, y,
                               d[y * dw + x] & 0xffffffu, want & 0xffffffu,
                               in && touched ? "" : " (nothing painted reaches it)");
                }
            free(d);
            free(was);
            group_free(&g);
        }
        expect("warp: random groups and matrices laid as the contract says", ok && laid > 0 &&
                                                                              kept > 0);
    }

    // A row is stepped by adding, from each row's own centre: along rows
    // two thousand pixels long, a hair's turn of hard-edged content lands
    // where each pixel worked alone says, to within one level at the far
    // end too. A step held to 16 bits of a pixel drifts several levels
    // there.
    {
        Group g;
        group_make(&g, 2000, 6, 0);
        yonder_sheets_t s = {g.black, g.white, 2000, 6};
        const double m[6] = {cos(0.0007), sin(0.0007), -sin(0.0007), cos(0.0007), 3.37, 4.61};
        int32_t dw = 2010, dh = 14;
        uint32_t *d = backdrop(dw, dh), *was = malloc((size_t)dw * dh * 4);
        memcpy(was, d, (size_t)dw * dh * 4);
        int ok = lay(d, (uint32_t)dw, (os64_gui_rect_t){0, 0, dw, dh}, &s, m, 255);
        for (int32_t y = 0; y < dh; y++)
            for (int32_t x = 0; x < dw; x++) {
                int touched = 0;
                uint32_t want = contract(&g, m, 0, x, y, was[y * dw + x], 255, &touched);
                ok = ok && (touched ? near(d[y * dw + x], want, 1) : d[y * dw + x] == was[y * dw + x]);
            }
        expect("warp: rows two thousand pixels long, stepped, land where each pixel says", ok);
        free(d);
        free(was);
        group_free(&g);
    }

    // Sheets squeezed into one column, a millionth of a pixel wide: the
    // inverse's step along a row is ten billion pixels, past what a step
    // can hold, while the one centre goes back inside FAR — off the
    // sheets, so the column is as it was.
    {
        Group g;
        group_make(&g, 20000, 4, 0);
        yonder_sheets_t s = {g.black, g.white, 20000, 4};
        const double m[6] = {1e-10, 0, 0, 1, 5.45, 2};
        uint32_t *d = backdrop(12, 8), *was = malloc(12 * 8 * 4);
        memcpy(was, d, 12 * 8 * 4);
        int ok = lay(d, 12, (os64_gui_rect_t){0, 0, 12, 8}, &s, m, 255) && memcmp(d, was, 12 * 8 * 4) == 0;
        expect("warp: a step past what fixed point holds is not stepped", ok);
        free(d);
        free(was);
        group_free(&g);
    }

    // Nothing painted anywhere: turned and scaled, the sheets leave every
    // pixel as it was, to the bit.
    {
        uint32_t black[30 * 20], white[30 * 20];
        for (int k = 0; k < 30 * 20; k++) {
            black[k] = 0xff000000u;
            white[k] = 0xffffffffu;
        }
        yonder_sheets_t s = {black, white, 30, 20};
        uint32_t *d = backdrop(64, 64), *was = malloc(64 * 64 * 4);
        memcpy(was, d, 64 * 64 * 4);
        const double m[6] = {1.3 * cos(0.4), 1.3 * sin(0.4), -1.3 * sin(0.4), 1.3 * cos(0.4), 20.3, 2.7};
        int ok = lay(d, 64, (os64_gui_rect_t){0, 0, 64, 64}, &s, m, 200) && memcmp(d, was, 64 * 64 * 4) == 0;
        expect("warp: sheets with nothing painted leave the backdrop to the bit", ok);
        // Opaque black is black on both sheets, and is painted: laid
        // where it is, every pixel black.
        for (int k = 0; k < 30 * 20; k++)
            white[k] = 0xff000000u;
        memcpy(d, was, 64 * 64 * 4);
        const double id[6] = {1, 0, 0, 1, 0, 0}, turn[6] = {cos(0.4), sin(0.4), -sin(0.4), cos(0.4), 20, 2};
        ok = lay(d, 64, (os64_gui_rect_t){0, 0, 64, 64}, &s, id, 255);
        for (int y = 0; y < 20; y++)
            for (int x = 0; x < 30; x++)
                ok = ok && (d[y * 64 + x] & 0xffffffu) == 0;
        // Turned, the middle of it too.
        memcpy(d, was, 64 * 64 * 4);
        ok = ok && lay(d, 64, (os64_gui_rect_t){0, 0, 64, 64}, &s, turn, 255) &&
             (d[(2 + 13) * 64 + 20 + 10] & 0xffffffu) == 0;
        expect("warp: opaque black is painted, not taken for nothing", ok);
        free(d);
        free(was);
    }

    // Re-basing a sheet's matrix: cancelling huge terms come to what they
    // are — 1e308 x 5 - 1e308 x 5 is 0, not inf - inf — and an ordinary
    // turn moves by its own arithmetic.
    {
        const double cancel[6] = {1e308, 0, -1e308, 1, 0, 0};
        double out[6];
        yonder_warp_rebase(cancel, 5, 5, 3, 4, out);
        const double turn[6] = {0, 1, -1, 0, 60, -40};
        double t2[6];
        yonder_warp_rebase(turn, 10, 20, 1, 2, t2);
        expect("warp: a sheet's matrix re-based without inf - inf",
               out[4] == 3 && isfinite(out[5]) && out[5] == 9 && t2[4] == 41 && t2[5] == -28);
    }

    // A TEXT CLIP's group (PILE3.md § The text clip). Covered everywhere,
    // it is the opaque warp, to the bit: the oracle the design names.
    {
        Group g;
        group_make(&g, 13, 9, 1);
        uint32_t *d = backdrop(13, 9), *ref = malloc(13 * 9 * 4), *mask = malloc(13 * 9 * 4);
        memcpy(ref, d, 13 * 9 * 4);
        for (int32_t k = 0; k < 13 * 9; k++)
            mask[k] = 0xffffffffu;
        yonder_sheets_t s = {g.black, g.white, 13, 9};
        const double id[6] = {1, 0, 0, 1, 0, 0};
        os64_gui_rect_t all = {0, 0, 13, 9};
        int ok = yonder_warp_masked(d, 13, all, &s, mask, 0, 0) && lay(ref, 13, all, &s, id, 255);
        expect("text clip: a mask covering everything lays the group as the opaque warp does",
               ok && memcmp(d, ref, 13 * 9 * 4) == 0);
        free(d);
        free(ref);
        free(mask);
        group_free(&g);
    }

    // At each pixel's own coverage c: the content laid over the backdrop at
    // opacity c of 255 — a(content over D) + (1 - a) D, here apart from the
    // warp — to within the level two roundings part by; 0 leaves the pixel
    // as it is, the top byte kept everywhere.
    {
        Group g;
        group_make(&g, 11, 8, 1);
        uint32_t *d = backdrop(11, 8), *was = malloc(11 * 8 * 4), *mask = malloc(11 * 8 * 4);
        memcpy(was, d, 11 * 8 * 4);
        for (int32_t k = 0; k < 11 * 8; k++) {
            uint32_t c = k % 5 == 0 ? 0 : rnd() & 0xff;
            mask[k] = 0xff000000u | c << 16 | c << 8 | c;
        }
        yonder_sheets_t s = {g.black, g.white, 11, 8};
        int ok = yonder_warp_masked(d, 11, (os64_gui_rect_t){0, 0, 11, 8}, &s, mask, 0, 0);
        for (int32_t k = 0; k < 11 * 8; k++) {
            double a = ((mask[k] >> 8) & 0xff) / 255.0;
            uint32_t full = over(g.content[k], was[k]), want = 0;
            for (int sh = 0; sh <= 16; sh += 8) {
                double v = a * ((full >> sh) & 0xff) + (1 - a) * ((was[k] >> sh) & 0xff);
                want |= (uint32_t)(v + 0.5) << sh;
            }
            ok = ok && near(d[k], want, 1) && (d[k] >> 24) == 0xab &&
                 (((mask[k] >> 8) & 0xff) != 0 || d[k] == was[k]);
        }
        expect("text clip: each pixel laid at its own coverage, none where it is 0", ok);
        free(d);
        free(was);
        free(mask);
        group_free(&g);
    }

    // Moved: a 5 by 4 group at (3, 2) of a 12 by 10 target, inside a clip
    // 3 by 2 from (4, 3) that cuts it on every side — its pixel (i, j) on
    // (i + 3, j + 2), and nothing outside where it lands or outside the
    // clip touched.
    {
        Group g;
        group_make(&g, 5, 4, 0);
        uint32_t *d = backdrop(12, 10), *was = malloc(12 * 10 * 4), *mask = malloc(5 * 4 * 4);
        memcpy(was, d, 12 * 10 * 4);
        for (int32_t k = 0; k < 5 * 4; k++)
            mask[k] = 0xffffffffu;
        yonder_sheets_t s = {g.black, g.white, 5, 4};
        int ok = yonder_warp_masked(d, 12, (os64_gui_rect_t){4, 3, 3, 2}, &s, mask, 3, 2);
        for (int32_t y = 0; y < 10; y++)
            for (int32_t x = 0; x < 12; x++) {
                bool on = x >= 4 && y >= 3 && x < 4 + 3 && y < 3 + 2;
                uint32_t want = on ? over(g.content[(y - 2) * 5 + (x - 3)], was[y * 12 + x])
                                   : was[y * 12 + x];
                ok = ok && (d[y * 12 + x] & 0xffffffu) == (want & 0xffffffu);
            }
        expect("text clip: a group laid at its offset, inside the clip only", ok);
        expect("text clip: no mask or no sheets lays nothing",
               !yonder_warp_masked(d, 12, (os64_gui_rect_t){0, 0, 12, 10}, &s, NULL, 0, 0));
        free(d);
        free(was);
        free(mask);
        group_free(&g);
    }

    printf("yonder warp: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed != 0;
}
