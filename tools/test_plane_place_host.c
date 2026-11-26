// test_plane_place_host.c — yonder's plane decisions and the map of what
// lies over a picture (userland/apps/yonder/plane_place.c), on the host.
//
// The decision: each reason alone, then the order they are asked in, and
// whether each wants the plane kept current. The map: scenarios, then
// random paints against a truth kept per pixel (which pixels were ever
// painted, and whether each was inked at its latest paint):
//   - not inked and known says no pixel of the clip is inked;
//   - known and not inked says every pixel of the clip has been painted
//     (a tile a partial paint inked is known: inked, whatever the rest);
//   - one clean paint of the whole clip makes the map known and clear.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plane_place.h"

static int s_failed;
#define CHECK(c, ...)                                                                                              \
    do {                                                                                                           \
        if (!(c)) {                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                            \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
            s_failed++;                                                                                            \
        }                                                                                                          \
    } while (0)

static uint32_t s_rand = 12345;
static uint32_t rnd(uint32_t n)
{
    s_rand = s_rand * 1103515245u + 12345u;
    return n ? (s_rand >> 8) % n : 0;
}

// ---------------------------------------------------------------- the decision

static const yonder_plane_input_t kAllGood = {true, true, false, false, false, false, false, true, true, false};

static void decide_cases(void)
{
    yonder_plane_verdict_t v = yonder_plane_decide(&kAllGood);
    CHECK(v.reason == YONDER_PLANE_PLACED && v.want, "all good is placed and wanted");

    yonder_plane_input_t in;
    in = kAllGood; in.on_view = false;
    v = yonder_plane_decide(&in);
    CHECK(v.reason == YONDER_PLANE_OUT_OF_VIEW && !v.want, "out of view: not wanted");
    in = kAllGood; in.cover = false;
    v = yonder_plane_decide(&in);
    CHECK(v.reason == YONDER_PLANE_NOT_COVER && v.want, "not a cover: wanted");
    in = kAllGood; in.backdrop = true;
    v = yonder_plane_decide(&in);
    CHECK(v.reason == YONDER_PLANE_BACKDROP && v.want, "a backdrop reader: wanted");
    in = kAllGood; in.clip_meets = true;
    v = yonder_plane_decide(&in);
    CHECK(v.reason == YONDER_PLANE_CLIP_MEETS && v.want, "another plane's clip: wanted");
    in = kAllGood; in.not_layer = true;
    v = yonder_plane_decide(&in);
    CHECK(v.reason == YONDER_PLANE_NOT_LAYER && v.want, "not a layer: wanted");
    in = kAllGood; in.over_budget = true;
    v = yonder_plane_decide(&in);
    CHECK(v.reason == YONDER_PLANE_BUDGET && v.want, "the overlays' budget: wanted");
    in = kAllGood; in.unpainted = true;
    v = yonder_plane_decide(&in);
    CHECK(v.reason == YONDER_PLANE_UNPAINTED && v.want, "not yet painted: wanted");
    in = kAllGood; in.ready = false;
    v = yonder_plane_decide(&in);
    CHECK(v.reason == YONDER_PLANE_NOT_READY && v.want, "not ready: wanted");
    in = kAllGood; in.shown = false;
    v = yonder_plane_decide(&in);
    CHECK(v.reason == YONDER_PLANE_NOT_READY && v.want, "ready but not shown's picture: not ready, wanted");
    in = kAllGood; in.held_off = true;
    v = yonder_plane_decide(&in);
    CHECK(v.reason == YONDER_PLANE_REFUSED && v.want, "a refused publish's hold-off: wanted");

    // The order: with every reason true, each one masks the ones after it.
    for (int first = 0; first < 10; first++) {
        in = (yonder_plane_input_t){false, false, true, true, true, true, true, false, false, true};
        yonder_plane_reason_t want = YONDER_PLANE_OUT_OF_VIEW;
        if (first >= 1) { in.on_view = true; want = YONDER_PLANE_NOT_COVER; }
        if (first >= 2) { in.cover = true; want = YONDER_PLANE_BACKDROP; }
        if (first >= 3) { in.backdrop = false; want = YONDER_PLANE_CLIP_MEETS; }
        if (first >= 4) { in.clip_meets = false; want = YONDER_PLANE_NOT_LAYER; }
        if (first >= 5) { in.not_layer = false; want = YONDER_PLANE_BUDGET; }
        if (first >= 6) { in.over_budget = false; want = YONDER_PLANE_UNPAINTED; }
        if (first >= 7) { in.unpainted = false; want = YONDER_PLANE_NOT_READY; }
        if (first >= 8) { in.ready = true; in.shown = true; want = YONDER_PLANE_REFUSED; }
        if (first >= 9) { in.held_off = false; want = YONDER_PLANE_PLACED; }
        v = yonder_plane_decide(&in);
        CHECK(v.reason == want, "order %d: %s, wanted %s", first, yonder_plane_reason_name(v.reason),
              yonder_plane_reason_name(want));
    }
    // The record's words, each its own.
    static const char *const kWords[YONDER_PLANE_REASONS] = {
        "placed", "out of view", "not a cover", "a backdrop reader", "another plane's clip",
        "not a layer", "the overlays' budget", "not yet painted", "not ready", "a refused publish",
    };
    for (int r = 0; r < YONDER_PLANE_REASONS; r++)
        CHECK(strcmp(yonder_plane_reason_name((yonder_plane_reason_t)r), kWords[r]) == 0, "reason %d is called %s",
              r, kWords[r]);
    CHECK(strcmp(yonder_plane_reason_name(YONDER_PLANE_REASONS), "?") == 0, "past the reasons: ?");
}

// ---------------------------------------------------------------- the map

#define W 200
#define H 120

// The truth: ink rectangles of the paint under way, and per pixel whether
// it was ever painted and inked at its latest paint (window coordinates,
// 0..W, 0..H).
static yonder_plane_rect_t s_ink[4];
static int s_nink;
static bool s_painted[H][W], s_inked[H][W];
static int s_ink_calls;
static bool s_part_ok;
static yonder_plane_rect_t s_paint_rect, s_paint_clip;

static bool meets(yonder_plane_rect_t a, yonder_plane_rect_t b)
{
    return a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0 && a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h &&
           b.y < a.y + a.h;
}
static bool inside(yonder_plane_rect_t in, yonder_plane_rect_t out)
{
    return in.x >= out.x && in.y >= out.y && in.x + in.w <= out.x + out.w && in.y + in.h <= out.y + out.h;
}

static bool truth_ink(void *ctx, yonder_plane_rect_t part)
{
    (void)ctx;
    s_ink_calls++;
    // The part asked about lies in the paint and the clip, and inside one tile.
    if (!inside(part, s_paint_rect) || !inside(part, s_paint_clip) || part.w > YONDER_PLANE_TILE ||
        part.h > YONDER_PLANE_TILE || part.w <= 0 || part.h <= 0)
        s_part_ok = false;
    for (int i = 0; i < s_nink; i++)
        if (meets(part, s_ink[i]))
            return true;
    return false;
}

// One paint of `r` inside `clip` with the ink rectangles set, the truth
// and the map both told.
static void paint(yonder_plane_map_t *m, yonder_plane_rect_t r, yonder_plane_rect_t clip)
{
    s_paint_rect = r;
    s_paint_clip = clip;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            yonder_plane_rect_t px = {x, y, 1, 1};
            if (!inside(px, r))
                continue;
            s_painted[y][x] = true;
            bool ink = false;
            for (int i = 0; i < s_nink; i++)
                ink |= inside(px, s_ink[i]);
            s_inked[y][x] = ink;
        }
    yonder_plane_map_paint(m, r, clip, truth_ink, NULL);
}

// Ink only counts on the picture: the map lies on it, not past it.
static bool truth_inked(yonder_plane_rect_t clip, yonder_plane_rect_t origin)
{
    for (int y = clip.y; y < clip.y + clip.h; y++)
        for (int x = clip.x; x < clip.x + clip.w; x++) {
            yonder_plane_rect_t px = {x, y, 1, 1};
            if (x >= 0 && y >= 0 && x < W && y < H && inside(px, origin) && s_inked[y][x])
                return true;
        }
    return false;
}
static bool truth_all_painted(yonder_plane_rect_t clip, yonder_plane_rect_t origin)
{
    for (int y = clip.y; y < clip.y + clip.h; y++)
        for (int x = clip.x; x < clip.x + clip.w; x++) {
            yonder_plane_rect_t px = {x, y, 1, 1};
            if (inside(px, origin) && !s_painted[y][x])
                return false;
        }
    return true;
}

static void truth_reset(void)
{
    memset(s_painted, 0, sizeof(s_painted));
    memset(s_inked, 0, sizeof(s_inked));
    s_nink = 0;
}

static uint8_t s_tiles[1024];

static void map_scenarios(void)
{
    yonder_plane_map_t m;
    yonder_plane_rect_t origin = {10, 6, 100, 50};   // 7 x 4 tiles, the last column and row partial
    CHECK(yonder_plane_map_tiles(origin) == 28, "tiles for 100x50: %zu", yonder_plane_map_tiles(origin));
    CHECK(!yonder_plane_map_reset(&m, origin, s_tiles, 27), "too few tiles refused");
    CHECK(m.tiles == NULL && !yonder_plane_map_known(&m, origin), "a refused map knows nothing");
    CHECK(!yonder_plane_map_reset(&m, (yonder_plane_rect_t){0, 0, 0, 5}, s_tiles, 1024), "an empty origin refused");
    CHECK(yonder_plane_map_reset(&m, origin, s_tiles, 1024), "reset");
    yonder_plane_rect_t clip = origin;
    CHECK(!yonder_plane_map_known(&m, clip) && !yonder_plane_map_inked(&m, clip), "fresh: unknown, not inked");

    truth_reset();
    paint(&m, origin, clip);
    CHECK(yonder_plane_map_known(&m, clip) && !yonder_plane_map_inked(&m, clip), "a clean whole paint: known, clear");

    // Ink in part of one tile, painted partly: inked.
    s_ink[0] = (yonder_plane_rect_t){30, 10, 3, 3};
    s_nink = 1;
    paint(&m, (yonder_plane_rect_t){28, 8, 8, 8}, clip);
    CHECK(yonder_plane_map_inked(&m, clip), "partial ink: inked");
    // The ink goes; a partial clean paint does not clear the tile.
    s_nink = 0;
    paint(&m, (yonder_plane_rect_t){28, 8, 8, 8}, clip);
    CHECK(yonder_plane_map_inked(&m, clip), "a partial clean paint leaves the ink");
    // A paint of the whole tile does.
    paint(&m, (yonder_plane_rect_t){26, 6, 16, 16}, clip);
    CHECK(!yonder_plane_map_inked(&m, clip) && yonder_plane_map_known(&m, clip), "a whole tile's clean paint clears");

    // A clip that shows only part of the picture (scrolled): known when its
    // part is painted, whatever lies outside it.
    CHECK(yonder_plane_map_reset(&m, origin, s_tiles, 1024), "reset again");
    truth_reset();
    yonder_plane_rect_t top = {10, 6, 100, 20};
    paint(&m, top, top);
    CHECK(yonder_plane_map_known(&m, top) && !yonder_plane_map_inked(&m, top), "the shown part known and clear");
    CHECK(!yonder_plane_map_known(&m, origin), "the whole picture is not");
    // A tile cut by the clip's edge is whole once its part inside is.
    yonder_plane_rect_t cutclip = {10, 6, 100, 21};   // row 1 shows 5 of its 16 rows
    CHECK(yonder_plane_map_reset(&m, origin, s_tiles, 1024), "reset for a cut tile");
    truth_reset();
    paint(&m, cutclip, cutclip);
    CHECK(yonder_plane_map_known(&m, cutclip), "tiles cut by the clip, painted inside it: known");
    // Ink outside the clip is no ink of the clip's.
    CHECK(yonder_plane_map_reset(&m, origin, s_tiles, 1024), "reset for ink outside");
    truth_reset();
    s_ink[0] = (yonder_plane_rect_t){50, 40, 4, 4};
    s_nink = 1;
    paint(&m, origin, top);
    CHECK(!yonder_plane_map_inked(&m, top), "ink below a clip's edge does not ink it");
    // A paint that misses the origin changes nothing; one that misses the
    // clip asks nothing.
    s_ink_calls = 0;
    paint(&m, (yonder_plane_rect_t){150, 80, 20, 20}, origin);
    CHECK(s_ink_calls == 0, "a paint off the picture asks nothing");
    paint(&m, (yonder_plane_rect_t){10, 30, 30, 20}, top);
    CHECK(s_ink_calls == 0, "a paint outside the clip asks nothing");
    // Empty clips and paints.
    CHECK(!yonder_plane_map_known(&m, (yonder_plane_rect_t){10, 6, 0, 0}), "an empty clip is not known");
    CHECK(!yonder_plane_map_known(&m, (yonder_plane_rect_t){150, 80, 10, 10}), "a clip off the picture is not known");
}

static yonder_plane_rect_t random_rect(void)
{
    int x = (int)rnd(W) - 10, y = (int)rnd(H) - 10;
    return (yonder_plane_rect_t){x, y, (int)rnd(80) + 1, (int)rnd(60) + 1};
}

static void map_properties(void)
{
    for (int round = 0; round < 3000; round++) {
        yonder_plane_map_t m;
        yonder_plane_rect_t origin = {(int)rnd(60), (int)rnd(40), (int)rnd(130) + 1, (int)rnd(70) + 1};
        yonder_plane_rect_t clip = {origin.x + (int)rnd(20) - 5, origin.y + (int)rnd(20) - 5, (int)rnd(140) + 1,
                                    (int)rnd(80) + 1};
        if (!yonder_plane_map_reset(&m, origin, s_tiles, sizeof(s_tiles)))
            continue;
        truth_reset();
        int steps = (int)rnd(12) + 1;
        s_part_ok = true;
        for (int s = 0; s < steps; s++) {
            s_nink = (int)rnd(3);
            for (int i = 0; i < s_nink; i++)
                s_ink[i] = (yonder_plane_rect_t){(int)rnd(W), (int)rnd(H), (int)rnd(12) + 1, (int)rnd(12) + 1};
            yonder_plane_rect_t r = rnd(4) == 0 ? clip : random_rect();
            paint(&m, r, clip);
            bool known = yonder_plane_map_known(&m, clip), inked = yonder_plane_map_inked(&m, clip);
            CHECK(!(known && !inked && truth_inked(clip, origin)), "round %d step %d: clear and known, yet inked", round, s);
            CHECK(!known || inked || truth_all_painted(clip, origin),
                  "round %d step %d: known and clear, yet a pixel unpainted", round, s);
        }
        CHECK(s_part_ok, "round %d: a part asked about strayed", round);
        // One clean paint of the whole clip settles it.
        s_nink = 0;
        paint(&m, clip, clip);
        bool any = meets(clip, origin);
        CHECK(!any || (yonder_plane_map_known(&m, clip) && !yonder_plane_map_inked(&m, clip)),
              "round %d: a clean whole paint leaves it unknown or inked", round);
    }
}

// ---------------------------------------------------------------- the layer

static void layer_cases(void)
{
    enum { N = 4096 };
    static uint32_t b[N], w[N], out[N];
    uint8_t want_a[N];
    for (int i = 0; i < N; i++) {
        uint32_t a = rnd(256), px = 0, pw = 0;
        if (i < 4)
            a = (uint32_t)(i == 0 ? 0 : i == 1 ? 255 : i == 2 ? 1 : 254);
        for (int c = 0; c < 3; c++) {
            uint32_t col = rnd(256);
            // Source over black and over white, rounded as a blend does.
            uint32_t over_b = (col * a + 127) / 255, over_w = (col * a + 255 * (255 - a) + 127) / 255;
            px |= over_b << (8 * c);
            pw |= over_w << (8 * c);
        }
        b[i] = 0xff000000u | px;
        w[i] = 0xff000000u | pw;
        want_a[i] = (uint8_t)a;
    }
    yonder_plane_layer_counts_t counts;
    yonder_plane_layer(b, w, N, out, &counts);
    int far = 0, wrong_colour = 0;
    size_t inked = 0;
    for (int i = 0; i < N; i++) {
        int got = (int)(out[i] >> 24), d = got - want_a[i];
        far += d > 1 || d < -1;
        wrong_colour += (out[i] & 0xffffffu) != (b[i] & 0xffffffu);
        inked += got > 0;
    }
    CHECK(far == 0, "layer: a within 1 of the coverage (%d far)", far);
    CHECK((out[0] >> 24) == 0 && (out[1] >> 24) == 255, "layer: clear is 0 and opaque is 255, exactly");
    CHECK(wrong_colour == 0, "layer: the colour is the paint over black");
    CHECK(counts.disagree == 0 && counts.inked == inked, "layer: one coverage agrees; inked counted (%zu)",
          counts.disagree);
    // A pixel whose channels say different coverages is no layer's.
    uint32_t bd = 0xff000000u, wd = 0xff00ff80u, o;
    yonder_plane_layer(&bd, &wd, 1, &o, &counts);
    CHECK(counts.disagree == 1, "layer: channels that disagree are counted");
    // Channels one apart agree; past one they do not, and the layer's
    // coverage is green's. A white pass darker than the black one (no
    // paint makes it) reads as opaque.
    uint32_t b1[2] = {0xff000000u, 0xff808080u}, w1[2] = {0xff373736u, 0xff707070u}, o1[2];
    yonder_plane_layer(b1, w1, 2, o1, &counts);
    CHECK(counts.disagree == 0 && (o1[0] >> 24) == 255 - 0x37, "layer: one apart agrees; the coverage is green's");
    CHECK((o1[1] >> 24) == 255, "layer: a coverage past opaque is opaque");
    // Out in place.
    yonder_plane_layer(b, w, N, b, &counts);
    CHECK(memcmp(b, out, sizeof(out)) == 0, "layer: written over the black pass gives the same");
}

// ---------------------------------------------------------------- overlays

static uint64_t brute_area(const yonder_plane_rect_t *r, uint32_t n)
{
    uint64_t area = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            yonder_plane_rect_t px = {x, y, 1, 1};
            for (uint32_t k = 0; k < n; k++)
                if (inside(px, r[k])) {
                    area++;
                    break;
                }
        }
    return area;
}

static void forget_cases(void)
{
    yonder_plane_map_t m;
    yonder_plane_rect_t origin = {10, 6, 100, 50};
    CHECK(yonder_plane_map_reset(&m, origin, s_tiles, 1024), "forget: reset");
    for (uint32_t i = 0; i < m.cols * m.rows; i++)
        s_tiles[i] = YONDER_PLANE_TILE_CLEAR;
    CHECK(yonder_plane_map_known(&m, origin), "forget: all clear is known");
    yonder_plane_rect_t part = {10 + 20, 6 + 3, 2, 2};     // inside tile (1, 0)
    yonder_plane_map_forget(&m, part);
    CHECK(s_tiles[1] == YONDER_PLANE_TILE_UNKNOWN && s_tiles[0] == YONDER_PLANE_TILE_CLEAR &&
              s_tiles[2] == YONDER_PLANE_TILE_CLEAR && s_tiles[m.cols + 1] == YONDER_PLANE_TILE_CLEAR,
          "forget: the tile the part meets, and only it");
    CHECK(!yonder_plane_map_known(&m, origin), "forget: the clip no longer known");
    yonder_plane_rect_t across = {10 + 15, 6 + 15, 2, 2};  // the corner of four tiles
    yonder_plane_map_forget(&m, across);
    CHECK(s_tiles[0] == YONDER_PLANE_TILE_UNKNOWN && s_tiles[m.cols] == YONDER_PLANE_TILE_UNKNOWN &&
              s_tiles[m.cols + 1] == YONDER_PLANE_TILE_UNKNOWN && s_tiles[2] == YONDER_PLANE_TILE_CLEAR,
          "forget: a part across four tiles forgets all four");
}

static void overlay_cases(void)
{
    yonder_plane_map_t m;
    yonder_plane_rect_t origin = {10, 6, 100, 50}, out[8];
    uint64_t area;
    CHECK(yonder_plane_map_reset(&m, origin, s_tiles, 1024), "overlays: reset");
    CHECK(yonder_plane_map_overlays(&m, origin, out, 8, &area) == 0 && area == 0, "overlays: nothing inked, none");
    // One tile inked: its part, cut to the clip.
    s_tiles[1 * m.cols + 2] = YONDER_PLANE_TILE_INKED;
    yonder_plane_rect_t clip = {10, 6, 100, 30};
    uint32_t n = yonder_plane_map_overlays(&m, clip, out, 8, &area);
    CHECK(n == 1 && out[0].x == 10 + 32 && out[0].y == 6 + 16 && out[0].w == 16 && out[0].h == 14 && area == 16 * 14,
          "overlays: one tile, cut to the clip (%u: %d,%d %dx%d)", n, out[0].x, out[0].y, out[0].w, out[0].h);
    // A block of tiles joins into one rectangle.
    yonder_plane_map_reset(&m, origin, s_tiles, 1024);
    for (uint32_t r = 0; r < 3; r++)
        for (uint32_t c = 1; c < 4; c++)
            s_tiles[r * m.cols + c] = YONDER_PLANE_TILE_INKED;
    n = yonder_plane_map_overlays(&m, origin, out, 8, &area);
    CHECK(n == 1 && out[0].w == 48 && out[0].h == 48 && area == 48 * 48, "overlays: a block is one rectangle (%u)", n);
    // Two tiles apart, the cap eight: two rectangles, not joined.
    yonder_plane_map_reset(&m, origin, s_tiles, 1024);
    s_tiles[0] = s_tiles[2 * m.cols + 5] = YONDER_PLANE_TILE_INKED;
    n = yonder_plane_map_overlays(&m, origin, out, 8, &area);
    CHECK(n == 2 && area == 2 * 16 * 16, "overlays: under the cap, nothing joined (%u)", n);
    // Random maps: at most the cap, inside the clip, every inked part
    // covered, the area the union's.
    for (int round = 0; round < 400; round++) {
        yonder_plane_rect_t o = {(int)rnd(40), (int)rnd(30), (int)rnd(150) + 1, (int)rnd(80) + 1};
        yonder_plane_rect_t cl = {o.x + (int)rnd(10), o.y + (int)rnd(10), 0, 0};
        cl.w = (int)rnd((uint32_t)(W - cl.x)) + 1;     // inside the test's W x H, where the area is counted
        cl.h = (int)rnd((uint32_t)(H - cl.y)) + 1;
        if (!yonder_plane_map_reset(&m, o, s_tiles, sizeof(s_tiles)))
            continue;
        uint32_t density = rnd(100) + 1;
        for (uint32_t i = 0; i < m.cols * m.rows; i++)
            s_tiles[i] = rnd(100) < density ? YONDER_PLANE_TILE_INKED : YONDER_PLANE_TILE_CLEAR;
        uint32_t cap = 1 + rnd(8);
        n = yonder_plane_map_overlays(&m, cl, out, cap, &area);
        CHECK(n <= cap, "overlays round %d: %u past the cap %u", round, n, cap);
        bool in_clip = true, covered = true;
        for (uint32_t k = 0; k < n; k++)
            in_clip &= inside(out[k], cl);
        for (uint32_t r = 0; r < m.rows; r++)
            for (uint32_t c = 0; c < m.cols; c++) {
                if (s_tiles[r * m.cols + c] != YONDER_PLANE_TILE_INKED)
                    continue;
                yonder_plane_rect_t t = {o.x + (int)c * 16, o.y + (int)r * 16, 16, 16}, part;
                yonder_plane_rect_t t2;
                if (!meets(t, o) || !meets(t, cl))
                    continue;
                // The tile's part inside the origin and the clip.
                int x0 = t.x > o.x ? t.x : o.x, y0 = t.y > o.y ? t.y : o.y;
                int x1 = t.x + t.w < o.x + o.w ? t.x + t.w : o.x + o.w, y1 = t.y + t.h < o.y + o.h ? t.y + t.h : o.y + o.h;
                t2 = (yonder_plane_rect_t){x0, y0, x1 - x0, y1 - y0};
                if (!meets(t2, cl))
                    continue;
                x0 = t2.x > cl.x ? t2.x : cl.x; y0 = t2.y > cl.y ? t2.y : cl.y;
                x1 = t2.x + t2.w < cl.x + cl.w ? t2.x + t2.w : cl.x + cl.w;
                y1 = t2.y + t2.h < cl.y + cl.h ? t2.y + t2.h : cl.y + cl.h;
                part = (yonder_plane_rect_t){x0, y0, x1 - x0, y1 - y0};
                bool in = false;
                for (uint32_t k = 0; k < n && !in; k++)
                    in = inside(part, out[k]);
                covered &= in;
            }
        CHECK(in_clip, "overlays round %d: a rectangle leaves the clip", round);
        CHECK(covered, "overlays round %d: an inked part left out", round);
        CHECK(area == brute_area(out, n), "overlays round %d: the area is not the union's", round);
    }
    // The union's area against counting.
    for (int round = 0; round < 300; round++) {
        yonder_plane_rect_t r[8];
        uint32_t k = 1 + rnd(8);
        for (uint32_t i = 0; i < k; i++)
            r[i] = (yonder_plane_rect_t){(int)rnd(W - 60), (int)rnd(H - 40), (int)rnd(60), (int)rnd(40)};
        CHECK(yonder_plane_union_area(r, k) == brute_area(r, k), "union area round %d", round);
    }
}

int main(void)
{
    decide_cases();
    map_scenarios();
    map_properties();
    layer_cases();
    forget_cases();
    overlay_cases();
    if (s_failed) {
        printf("plane_place: %d failed\n", s_failed);
        return 1;
    }
    printf("PASS plane_place: decisions, map scenarios, 3000 random paint sequences, the layer, overlays\n");
    return 0;
}
