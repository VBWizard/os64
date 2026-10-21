// yonder's moving-background layer (userland/apps/yonder/backdrop.h) on the
// host, under the sanitizers, against references written here apart from
// it: a pixel is a stack of `over`s painted straight onto what is behind,
// and the layer's three paintings are that stack over black, over white and
// over what is behind the moving layer (BACKDROP_LAYER.md § Numbers).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backdrop.h"

static int s_failed, s_checks;

static void expect(const char *what, int ok)
{
    s_checks++;
    if (!ok) {
        s_failed++;
        if (s_failed < 40)
            printf("FAIL %s\n", what);
    }
}

static uint32_t s_seed = 20261020;
static uint32_t rnd(void)
{
    s_seed = s_seed * 1103515245u + 12345u;
    return s_seed >> 8;
}

// `argb` over the opaque pixel `d`, rounded: yonder's `over`.
static uint32_t over(uint32_t argb, uint32_t d)
{
    uint32_t a = argb >> 24, out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t s = (argb >> sh) & 0xff, b = (d >> sh) & 0xff;
        out |= ((s * a + b * (255 - a) + 127) / 255) << sh;
    }
    return out;
}

static int diff(uint32_t p, uint32_t q)
{
    int most = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int d = abs((int)((p >> sh) & 0xff) - (int)((q >> sh) & 0xff));
        most = d > most ? d : most;
    }
    return most;
}

static uint32_t alpha_of(uint32_t kind)
{
    switch (kind % 4) {
    case 0: return 0;
    case 1: return 255;
    default: return rnd() % 256;
    }
}

// ── The pixel ───────────────────────────────────────────────────────────

static void pixel_cases(void)
{
    int exact_seen = 0, bound_seen = 0;
    for (int trial = 0; trial < 400000; trial++) {
        uint32_t back = rnd() & 0xffffff;
        uint32_t ba = alpha_of(rnd());
        uint32_t b = ba << 24 | (rnd() & 0xffffff);
        int n = (int)(rnd() % 4), translucent = 0;
        uint32_t front[3];
        bool opaque_front = false;
        for (int i = 0; i < n; i++) {
            uint32_t a = alpha_of(rnd());
            front[i] = a << 24 | (rnd() & 0xffffff);
            opaque_front |= a == 255;
            translucent += a != 0 && a != 255;
        }
        uint32_t direct = over(b, back), k = 0, w = 0xffffff, p = back;
        for (int i = 0; i < n; i++) {
            direct = over(front[i], direct);
            k = over(front[i], k);
            w = over(front[i], w);
            p = over(front[i], p);
        }
        uint32_t got = yonder_backdrop_pixel(k, w, p, b);
        bool nothing_in_front = translucent == 0 && !opaque_front;
        if (opaque_front || ba == 0 || (nothing_in_front && ba == 255)) {
            exact_seen++;
            if (diff(got, direct) != 0) {
                char what[160];
                snprintf(what, sizeof(what),
                         "pixel exact: back %06x b %08x n %d -> %06x, painted %06x", back, b, n, got,
                         direct);
                expect(what, 0);
            } else {
                expect("pixel exact", 1);
            }
        } else {
            // One rounding for the moving layer's own blend when its alpha
            // is partial, and one for each translucent layer in front.
            int bound = translucent + (ba != 255);
            bound_seen++;
            if (diff(got, direct) > bound) {
                char what[160];
                snprintf(what, sizeof(what),
                         "pixel bound %d: back %06x b %08x n %d -> %06x, painted %06x", bound, back,
                         b, n, got, direct);
                expect(what, 0);
            } else {
                expect("pixel bound", 1);
            }
        }
    }
    expect("pixel: exact cases met", exact_seen > 1000);
    expect("pixel: bounded cases met", bound_seen > 1000);
}

// ── The runs ────────────────────────────────────────────────────────────

static void changes_cases(void)
{
    for (int trial = 0; trial < 300; trial++) {
        int32_t w = 1 + (int32_t)(rnd() % 40), h = 1 + (int32_t)(rnd() % 30);
        uint32_t *a = malloc(sizeof(uint32_t) * (size_t)(w * h));
        uint32_t *b = malloc(sizeof(uint32_t) * (size_t)(w * h));
        int64_t want = 0;
        uint32_t odds = 1 + rnd() % 8;
        for (int32_t i = 0; i < w * h; i++) {
            a[i] = rnd();
            b[i] = rnd() % odds == 0 ? a[i] ^ (1u + rnd() % 0xffffffu) : a[i];
            want += a[i] != b[i];
        }
        yonder_backdrop_run_t runs[2048];
        int64_t changed = -1;
        int32_t n = yonder_backdrop_changes(a, b, w, h, runs, 2048, &changed);
        expect("changes: count of pixels", changed == want);
        expect("changes: fitted", n >= 0);
        // Every changed pixel in exactly one run, every run all changed,
        // runs maximal and in order.
        unsigned char *hit = calloc((size_t)(w * h), 1);
        bool ok = true;
        for (int32_t r = 0; r < n; r++) {
            if (runs[r].n <= 0 || runs[r].x < 0 || runs[r].x + runs[r].n > w || runs[r].y < 0 ||
                runs[r].y >= h)
                ok = false;
            for (int32_t x = runs[r].x; ok && x < runs[r].x + runs[r].n; x++) {
                int32_t i = runs[r].y * w + x;
                ok = a[i] != b[i] && !hit[i];
                hit[i] = 1;
            }
            if (ok && runs[r].x > 0)
                ok = a[runs[r].y * w + runs[r].x - 1] == b[runs[r].y * w + runs[r].x - 1];
            if (ok && runs[r].x + runs[r].n < w)
                ok = a[runs[r].y * w + runs[r].x + runs[r].n] == b[runs[r].y * w + runs[r].x + runs[r].n];
            if (ok && r > 0)
                ok = runs[r - 1].y < runs[r].y ||
                     (runs[r - 1].y == runs[r].y && runs[r - 1].x + runs[r - 1].n < runs[r].x);
        }
        for (int32_t i = 0; ok && i < w * h; i++)
            ok = hit[i] == (a[i] != b[i]);
        expect("changes: runs are exactly the changed pixels", ok);
        // Exactly enough slots fit; one too few is -1, the count still whole.
        if (n > 0) {
            int64_t exact = -1;
            expect("changes: exactly enough slots fit",
                   yonder_backdrop_changes(a, b, w, h, runs, n, &exact) == n && exact == want);
        }
        if (n > 1) {
            int64_t again = -1;
            expect("changes: overflow says so",
                   yonder_backdrop_changes(a, b, w, h, runs, n - 1, &again) == -1);
            expect("changes: overflow still counts", again == want);
        }
        free(hit);
        free(a);
        free(b);
    }
    uint32_t same[6] = {1, 2, 3, 4, 5, 6};
    yonder_backdrop_run_t one[1];
    int64_t c = -1;
    expect("changes: none", yonder_backdrop_changes(same, same, 3, 2, one, 1, &c) == 0 && c == 0);
}

// ── Composing ───────────────────────────────────────────────────────────

#define VW 61
#define VH 47
#define CANARY 0x00c0ffeeu

typedef struct {
    uint32_t k[VW * VH], w[VW * VH], p[VW * VH];
} Sheets;

static void random_sheets(Sheets *s)
{
    for (int i = 0; i < VW * VH; i++) {
        uint32_t c = rnd() & 0xffffff;
        switch (rnd() % 3) {
        case 0: // nothing in front
            s->k[i] = 0, s->w[i] = 0xffffff, s->p[i] = rnd() & 0xffffff;
            break;
        case 1: // opaque front
            s->k[i] = s->w[i] = s->p[i] = c;
            break;
        default: { // a translucent layer
            uint32_t f = (rnd() % 254 + 1) << 24 | c, back = rnd() & 0xffffff;
            s->k[i] = over(f, 0), s->w[i] = over(f, 0xffffff), s->p[i] = over(f, back);
        }
        }
    }
}

// The moving layer's pixel at view (x, y), from first principles.
static uint32_t ref_layer(const yonder_backdrop_tile_t *t, int32_t x, int32_t y)
{
    int64_t u = (int64_t)x - t->x, v = (int64_t)y - t->y;
    if ((!t->repeat_x && (u < 0 || u >= t->w)) || (!t->repeat_y && (v < 0 || v >= t->h)))
        return 0;
    u = ((u % t->w) + t->w) % t->w;
    v = ((v % t->h) + t->h) % t->h;
    return t->px[v * t->w + u];
}

static bool inside(os64_gui_rect_t r, int32_t x, int32_t y)
{
    return r.w > 0 && r.h > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static os64_gui_rect_t random_rect(int32_t spread)
{
    int32_t x = (int32_t)(rnd() % (VW + 2 * spread)) - spread;
    int32_t y = (int32_t)(rnd() % (VH + 2 * spread)) - spread;
    return (os64_gui_rect_t){x, y, (int32_t)(rnd() % VW), (int32_t)(rnd() % VH)};
}

static void random_tile(yonder_backdrop_tile_t *t, uint32_t *px, int32_t w, int32_t h)
{
    for (int32_t i = 0; i < w * h; i++)
        px[i] = alpha_of(rnd()) << 24 | (rnd() & 0xffffff);
    *t = (yonder_backdrop_tile_t){px, w, h, (int32_t)(rnd() % 200) - 100,
                                  (int32_t)(rnd() % 200) - 100, rnd() % 4 != 0, rnd() % 4 != 0};
    if (!t->repeat_x && rnd() % 2)
        t->x = (int32_t)(rnd() % VW) - w / 2;
    if (!t->repeat_y && rnd() % 2)
        t->y = (int32_t)(rnd() % VH) - h / 2;
}

static bool damage_covers(const os64_gui_rect_t *d, uint32_t n, int32_t x, int32_t y)
{
    for (uint32_t i = 0; i < n; i++)
        if (inside(d[i], x, y))
            return true;
    return false;
}

static void area_cases(void)
{
    static Sheets s;
    static uint32_t dst[VW * VH], px[24 * 24];
    for (int trial = 0; trial < 3000; trial++) {
        random_sheets(&s);
        yonder_backdrop_tile_t t;
        int32_t tw = 1 + (int32_t)(rnd() % 24), th = 1 + (int32_t)(rnd() % 24);
        random_tile(&t, px, tw, th);
        os64_gui_rect_t part = random_rect(10), area = random_rect(10);
        os64_gui_rect_t skip[4];
        uint32_t nskip = rnd() % 5;
        for (uint32_t i = 0; i < nskip; i++)
            skip[i] = random_rect(5);
        os64_gui_rect_t damage[3];
        yonder_backdrop_out_t out = {skip, nskip, damage, 0, 3, 128, 0};
        for (int i = 0; i < VW * VH; i++)
            dst[i] = CANARY;
        const yonder_backdrop_sheets_t sh = {s.k, s.w, s.p, VW, VW, VH};
        yonder_backdrop_compose_area(dst, VW, &sh, &t, part, area, &out);
        bool ok = true, covered = true;
        int64_t want = 0;
        for (int32_t y = 0; y < VH; y++)
            for (int32_t x = 0; x < VW; x++) {
                bool skipped = false;
                for (uint32_t i = 0; i < nskip; i++)
                    skipped |= inside(skip[i], x, y);
                int i = y * VW + x;
                if (inside(part, x, y) && inside(area, x, y) && !skipped) {
                    want++;
                    ok &= dst[i] == yonder_backdrop_pixel(s.k[i], s.w[i], s.p[i], ref_layer(&t, x, y));
                    covered &= damage_covers(damage, out.ndamage, x, y);
                } else {
                    ok &= dst[i] == CANARY;
                }
            }
        expect("area: every pixel of area ∩ part outside the skips, nothing else", ok);
        expect("area: written counted", out.written == want);
        expect("area: damage covers what was written", covered);
    }
}

static void runs_cases(void)
{
    static Sheets s;
    static uint32_t dst[VW * VH], whole[VW * VH], was[24 * 24], now[24 * 24];
    static yonder_backdrop_run_t runs[24 * 24];
    for (int trial = 0; trial < 3000; trial++) {
        random_sheets(&s);
        yonder_backdrop_tile_t t;
        int32_t tw = 1 + (int32_t)(rnd() % 24), th = 1 + (int32_t)(rnd() % 24);
        random_tile(&t, was, tw, th);
        uint32_t odds = 1 + rnd() % 30;
        for (int32_t i = 0; i < tw * th; i++)
            now[i] = rnd() % odds == 0 ? alpha_of(rnd()) << 24 | (rnd() & 0xffffff) : was[i];
        os64_gui_rect_t part = random_rect(10);
        os64_gui_rect_t skip[3];
        uint32_t nskip = rnd() % 4;
        for (uint32_t i = 0; i < nskip; i++)
            skip[i] = random_rect(5);
        const yonder_backdrop_sheets_t sh = {s.k, s.w, s.p, VW, VW, VH};
        os64_gui_rect_t everything = {-1000, -1000, 4000, 4000};
        // The view as it shows the old frame, then the runs of the new one.
        for (int i = 0; i < VW * VH; i++)
            dst[i] = whole[i] = CANARY;
        yonder_backdrop_out_t first = {skip, nskip, NULL, 0, 0, 0, 0};
        yonder_backdrop_compose_area(dst, VW, &sh, &t, part, everything, &first);
        uint32_t before[VW * VH];
        memcpy(before, dst, sizeof(before));
        int64_t changed;
        int32_t n = yonder_backdrop_changes(was, now, tw, th, runs, tw * th, &changed);
        yonder_backdrop_tile_t tn = t;
        tn.px = now;
        os64_gui_rect_t damage[8];
        yonder_backdrop_out_t out = {skip, nskip, damage, 0, 8, 128, 0};
        yonder_backdrop_compose_runs(dst, VW, &sh, &tn, part, runs, n, &out);
        // The same as composing the new frame whole.
        yonder_backdrop_out_t again = {skip, nskip, NULL, 0, 0, 0, 0};
        yonder_backdrop_compose_area(whole, VW, &sh, &tn, part, everything, &again);
        expect("runs: the view shows the new frame", memcmp(dst, whole, sizeof(whole)) == 0);
        bool covered = true;
        for (int32_t y = 0; y < VH; y++)
            for (int32_t x = 0; x < VW; x++)
                if (dst[y * VW + x] != before[y * VW + x])
                    covered &= damage_covers(damage, out.ndamage, x, y);
        expect("runs: damage covers every pixel that changed", covered);
        // Exactly the pixels of the part, outside the skips, whose layer
        // pixel the new frame changed: no copy that is not laid, no pixel
        // twice.
        int64_t want = 0;
        yonder_backdrop_tile_t to = t;
        for (int32_t y = 0; y < VH; y++)
            for (int32_t x = 0; x < VW; x++) {
                bool skipped = false;
                for (uint32_t i = 0; i < nskip; i++)
                    skipped |= inside(skip[i], x, y);
                want += inside(part, x, y) && !skipped && ref_layer(&to, x, y) != ref_layer(&tn, x, y);
            }
        expect("runs: wrote exactly the changed pixels", out.written == want);
    }
}

// A tile copy far from the origin, both signs: where a copy starts is the
// tile's origin plus whole tiles, never the view's.
static void origin_cases(void)
{
    static uint32_t k[VW * VH], w[VW * VH], p[VW * VH], dst[VW * VH], px[7 * 5];
    for (int i = 0; i < VW * VH; i++)
        k[i] = 0, w[i] = 0xffffff, p[i] = 0x123456;
    for (int i = 0; i < 35; i++)
        px[i] = 0xff000000u | (uint32_t)i;
    const yonder_backdrop_sheets_t sh = {k, w, p, VW, VW, VH};
    int32_t origins[] = {-1000003, -7, 0, 3, 999999};
    for (int a = 0; a < 5; a++)
        for (int b = 0; b < 5; b++) {
            yonder_backdrop_tile_t t = {px, 7, 5, origins[a], origins[b], true, true};
            yonder_backdrop_out_t out = {NULL, 0, NULL, 0, 0, 0, 0};
            yonder_backdrop_compose_area(dst, VW, &sh, &t, (os64_gui_rect_t){0, 0, VW, VH},
                                         (os64_gui_rect_t){0, 0, VW, VH}, &out);
            bool ok = true;
            for (int32_t y = 0; y < VH; y++)
                for (int32_t x = 0; x < VW; x++)
                    ok &= dst[y * VW + x] == (ref_layer(&t, x, y) & 0xffffff);
            expect("origin: copies laid from the tile's origin", ok);
        }
}

// ── The policy ──────────────────────────────────────────────────────────

static void policy_cases(void)
{
    yonder_backdrop_policy_t p;
    yonder_backdrop_policy_init(&p);
    expect("policy: unmeasured, no build", !yonder_backdrop_should_build(&p));
    yonder_backdrop_quiet_frame(&p);
    yonder_backdrop_quiet_frame(&p);
    yonder_backdrop_quiet_frame(&p);
    expect("policy: quiet but no paint measured, no build", !yonder_backdrop_should_build(&p));
    yonder_backdrop_ordinary(&p);
    yonder_backdrop_painted(&p, 1e6, 1000);
    expect("policy: not quiet, no build", !yonder_backdrop_should_build(&p));
    yonder_backdrop_quiet_frame(&p);
    expect("policy: one quiet frame is not enough", !yonder_backdrop_should_build(&p));
    yonder_backdrop_quiet_frame(&p);
    expect("policy: two quiet frames build", yonder_backdrop_should_build(&p));
    yonder_backdrop_ordinary(&p);
    expect("policy: an ordinary paint ends the quiet", !yonder_backdrop_should_build(&p));

    // Built: unmeasured composing composes.
    yonder_backdrop_built(&p, 3e6, 1000);
    expect("policy: first frame composes", yonder_backdrop_should_compose(&p, 1000, 0, 1000));
    // paint 1000 ns/px; composing 10 ns/px of 10 px saves ~1e6 a frame.
    yonder_backdrop_composed(&p, 100, 10, 0, 0, 1000);
    expect("policy: not yet paid", !p.paid);
    yonder_backdrop_composed(&p, 100, 10, 0, 0, 1000);
    yonder_backdrop_composed(&p, 100, 10, 0, 0, 1000);
    yonder_backdrop_composed(&p, 100, 10, 0, 0, 1000);
    expect("policy: paid after enough frames", p.paid);
    expect("policy: sparse frames compose", yonder_backdrop_should_compose(&p, 10, 0, 1000));
    // Invalid pixels cost three paintings each.
    expect("policy: rebuilding most of the part is dearer",
           !yonder_backdrop_should_compose(&p, 10, 400, 1000));
    expect("policy: a dearer frame with invalid pixels does not count", p.dearer == 0);

    // A loss after paying keeps the quiet asked; one before doubles it.
    yonder_backdrop_lost(&p);
    expect("policy: paid loss keeps the need", p.need == YONDER_BACKDROP_QUIET);
    yonder_backdrop_built(&p, 3e6, 1000);
    yonder_backdrop_lost(&p);
    expect("policy: unpaid loss doubles the need", p.need == 2 * YONDER_BACKDROP_QUIET);
    for (int i = 0; i < 20; i++) {
        yonder_backdrop_built(&p, 3e6, 1000);
        yonder_backdrop_lost(&p);
    }
    expect("policy: the need stops at its most", p.need == YONDER_BACKDROP_QUIET_MOST);
    p.quiet = YONDER_BACKDROP_QUIET_MOST - 1;
    expect("policy: backed off, not yet", !yonder_backdrop_should_build(&p));
    yonder_backdrop_quiet_frame(&p);
    expect("policy: backed off, now", yonder_backdrop_should_build(&p));
    // Paying resets the need.
    yonder_backdrop_built(&p, 1, 1000);
    yonder_backdrop_composed(&p, 0, 10, 0, 0, 1000);
    expect("policy: paying resets the need", p.paid && p.need == YONDER_BACKDROP_QUIET);
    // A new build starts its own books: what the last one saved does not
    // pay for it.
    for (int i = 0; i < 3; i++)
        yonder_backdrop_composed(&p, 0, 10, 0, 0, 1000);
    double before = p.saved_cost;
    expect("policy: the books so far are past the next build's cost", before > 3e6);
    yonder_backdrop_built(&p, 3e6, 1000);
    expect("policy: a new build is unpaid", !p.paid);
    yonder_backdrop_composed(&p, 0, 10, 0, 0, 1000);
    expect("policy: a new build is paid by its own frames", !p.paid);
    // Rebuilding is charged to the frame that did it.
    yonder_backdrop_policy_t r;
    yonder_backdrop_policy_init(&r);
    yonder_backdrop_painted(&r, 1e6, 1000);    // 1000 ns a pixel
    yonder_backdrop_built(&r, 1e5, 1000);
    double pass = r.pass_cost;
    yonder_backdrop_composed(&r, 10, 10, 2e6, 600, 1000);
    expect("policy: a rebuild's cost is charged", !r.paid && r.saved_cost < 0);
    expect("policy: a rebuild measures the passes", r.pass_cost > 2 * pass);

    // Dearer on a valid layer, running: given up for good.
    yonder_backdrop_policy_t q;
    yonder_backdrop_policy_init(&q);
    yonder_backdrop_painted(&q, 1000, 1000);       // 1 ns a pixel
    yonder_backdrop_built(&q, 3000, 1000);
    yonder_backdrop_composed(&q, 5000, 1000, 0, 0, 1000); // 5 ns a pixel
    for (uint32_t i = 1; i < YONDER_BACKDROP_DEARER; i++)
        expect("policy: dearer, not yet given up",
               !yonder_backdrop_should_compose(&q, 1000, 0, 1000) && !q.unprofitable);
    expect("policy: one cheaper frame resets the run",
           yonder_backdrop_should_compose(&q, 1, 0, 1000) && q.dearer == 0);
    for (uint32_t i = 0; i < YONDER_BACKDROP_DEARER; i++)
        (void)yonder_backdrop_should_compose(&q, 1000, 0, 1000);
    expect("policy: dearer running, given up", q.unprofitable);
    q.quiet = 1000;
    expect("policy: given up, never built", !yonder_backdrop_should_build(&q));
}

int main(void)
{
    pixel_cases();
    changes_cases();
    area_cases();
    runs_cases();
    origin_cases();
    policy_cases();
    printf("backdrop: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed != 0;
}
