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

static const yonder_plane_input_t kAllGood = {true, true, false, false, false, true, true, false};

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
    in = kAllGood; in.over = true;
    v = yonder_plane_decide(&in);
    CHECK(v.reason == YONDER_PLANE_OVER && v.want, "something over it: wanted");
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
    for (int first = 0; first < 8; first++) {
        in = (yonder_plane_input_t){false, false, true, true, true, false, false, true};
        yonder_plane_reason_t want = YONDER_PLANE_OUT_OF_VIEW;
        if (first >= 1) { in.on_view = true; want = YONDER_PLANE_NOT_COVER; }
        if (first >= 2) { in.cover = true; want = YONDER_PLANE_BACKDROP; }
        if (first >= 3) { in.backdrop = false; want = YONDER_PLANE_CLIP_MEETS; }
        if (first >= 4) { in.clip_meets = false; want = YONDER_PLANE_OVER; }
        if (first >= 5) { in.over = false; want = YONDER_PLANE_NOT_READY; }
        if (first >= 6) { in.ready = true; in.shown = true; want = YONDER_PLANE_REFUSED; }
        if (first >= 7) { in.held_off = false; want = YONDER_PLANE_PLACED; }
        v = yonder_plane_decide(&in);
        CHECK(v.reason == want, "order %d: %s, wanted %s", first, yonder_plane_reason_name(v.reason),
              yonder_plane_reason_name(want));
    }
    // The record's words, each its own.
    static const char *const kWords[YONDER_PLANE_REASONS] = {
        "placed", "out of view", "not a cover", "a backdrop reader", "another plane's clip",
        "something over it", "not ready", "a refused publish",
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

int main(void)
{
    decide_cases();
    map_scenarios();
    map_properties();
    if (s_failed) {
        printf("plane_place: %d failed\n", s_failed);
        return 1;
    }
    printf("PASS plane_place: decisions, map scenarios, 3000 random paint sequences\n");
    return 0;
}
