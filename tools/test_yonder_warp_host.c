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

    printf("yonder warp: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed != 0;
}
