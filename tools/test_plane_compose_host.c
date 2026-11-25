// Host test of the plane composer (kernel/src/gui/plane_compose.c), beside a
// model that decides each pixel alone: outside the clip the content; inside
// it, in any overlay (cut to the clip) the content over the picture, the
// content alone where the picture does not reach; elsewhere the picture, or
// the content as opaque where the picture does not reach. Random contents,
// pictures, places (origins partly outside their clips, overlays that
// overlap and leave gaps) and areas, every pixel and every count equal; the
// blend against its formula for every pair of a top byte and a channel; the
// overlay union's area and the place checks by hand and against the model.
// Run by tools/test_plane_compose_host.sh.

#include "gui/plane_compose.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failed;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            s_failed++;                                                                \
        }                                                                              \
    } while (0)

static uint64_t s_seed = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 7;
    s_seed ^= s_seed << 17;
    return (uint32_t)s_seed;
}
static int32_t range(int32_t lo, int32_t hi) { return lo + (int32_t)(rnd() % (uint32_t)(hi - lo + 1)); }

static bool in_rect(const pc_rect_t *r, int32_t x, int32_t y)
{
    return r->w > 0 && r->h > 0 && x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

// The blend by its formula, channel by channel.
static uint32_t model_blend(uint32_t c, uint32_t p)
{
    uint32_t a = c >> 24, out = 0;
    for (int s = 0; s < 32; s += 8) {
        uint32_t cc = (c >> s) & 0xFF, pp = (p >> s) & 0xFF;
        uint32_t v = a == 255 ? cc : a == 0 ? pp : cc + (pp * (255 - a) + 127) / 255;
        if (v > 255)
            v = 255;
        out |= v << s;
    }
    return out;
}

static void model(uint32_t *out, const pc_image_t *content, const pc_image_t *picture, const pc_place_t *place,
                  pc_rect_t area, pc_counts_t *counts)
{
    for (int32_t y = area.y; y < area.y + area.h; y++)
        for (int32_t x = area.x; x < area.x + area.w; x++) {
            uint32_t c = content->pixels[(size_t)y * content->pitch_px + x];
            uint32_t *o = &out[(size_t)(y - area.y) * area.w + (x - area.x)];
            if (!in_rect(&place->clip, x, y)) {
                *o = c;
                continue;
            }
            int64_t px = (int64_t)x - place->origin.x, py = (int64_t)y - place->origin.y;
            bool pic = px >= 0 && py >= 0 && px < (int64_t)picture->w && py < (int64_t)picture->h;
            uint32_t p = pic ? picture->pixels[(size_t)py * picture->pitch_px + px] : 0;
            bool over = false;
            for (uint32_t i = 0; i < place->noverlays; i++)
                over |= in_rect(&place->overlay[i], x, y);
            if (over && pic) {
                *o = model_blend(c, p);
                counts->blended++;
            } else if (pic) {
                *o = p;
                counts->copied++;
            } else {
                *o = c;
                counts->opaque++;
            }
        }
}

static uint64_t model_area(const pc_place_t *place)
{
    uint64_t n = 0;
    for (int32_t y = place->clip.y; y < place->clip.y + place->clip.h; y++)
        for (int32_t x = place->clip.x; x < place->clip.x + place->clip.w; x++) {
            bool over = false;
            for (uint32_t i = 0; i < place->noverlays; i++)
                over |= in_rect(&place->overlay[i], x, y);
            n += over;
        }
    return n;
}

// Every top byte, every content byte and every picture byte: the blend
// works two channels a lane, so each lane position gets every pair (the
// red and green lanes one way round, blue and the top byte the other),
// content that is not premultiplied included, which saturates. Then
// channels that differ from one another, at random.
static void blend_cases(void)
{
    for (uint32_t a = 0; a < 256; a++)
        for (uint32_t c = 0; c < 256; c++)
            for (uint32_t p = 0; p < 256; p++) {
                uint32_t content = a << 24 | c << 16 | p << 8 | c, picture = p << 24 | p << 16 | c << 8 | p;
                uint32_t got = plane_blend(content, picture), want = model_blend(content, picture);
                if (got != want) {
                    fprintf(stderr, "blend %08x over %08x: %08x, want %08x\n", content, picture, got, want);
                    CHECK(got == want);
                    return;
                }
            }
    for (int i = 0; i < 1000000; i++) {
        uint32_t content = rnd(), picture = rnd();
        if (plane_blend(content, picture) != model_blend(content, picture)) {
            CHECK(plane_blend(content, picture) == model_blend(content, picture));
            return;
        }
    }
    CHECK(plane_blend(0xFF123456u, 0xFFABCDEFu) == 0xFF123456u);
    CHECK(plane_blend(0x00000000u, 0xFFABCDEFu) == 0xFFABCDEFu);
    // A half-covering black content: rounded to nearest, 0x80 * 127 / 255 = 63.75 -> 64.
    CHECK(plane_blend(0x80000000u, 0x00808080u) == 0x80404040u);
}

static void area_cases(void)
{
    pc_place_t p = {.clip = {10, 10, 100, 50}};
    CHECK(plane_overlay_area(&p) == 0);
    p.overlay[0] = (pc_rect_t){20, 20, 10, 10};
    p.overlay[1] = (pc_rect_t){25, 25, 10, 10};   // overlaps the first by 5 x 5
    p.overlay[2] = (pc_rect_t){90, 50, 10, 10};   // a gap from the others
    p.noverlays = 3;
    CHECK(plane_overlay_area(&p) == 100 + 100 - 25 + 100);
    CHECK(plane_place_valid(&p, 200, 100, 275) && !plane_place_valid(&p, 200, 100, 274));
    // An overlay past its clip, a clip past the content, too many overlays,
    // a negative size and an overflowing edge are all refused.
    pc_place_t q = p;
    q.overlay[2] = (pc_rect_t){105, 50, 10, 10};
    CHECK(!plane_place_valid(&q, 200, 100, 1000));
    q = p;
    CHECK(!plane_place_valid(&q, 100, 100, 1000));
    q = p;
    q.noverlays = PLANE_OVERLAYS_MAX + 1;
    CHECK(!plane_place_valid(&q, 200, 100, 1000));
    q = p;
    q.overlay[0].w = -1;
    CHECK(!plane_place_valid(&q, 200, 100, 1000));
    q = p;
    q.overlay[0].h = -1;
    CHECK(!plane_place_valid(&q, 200, 100, 1000));
    q = p;
    q.origin = (pc_rect_t){INT32_MAX - 5, 0, 10, 10};
    CHECK(!plane_place_valid(&q, 200, 100, 1000));
    // The empty place is valid whatever else it says.
    q = (pc_place_t){.clip = {500, 500, 0, 0}};
    CHECK(plane_place_valid(&q, 10, 10, 0));
}

static void compose_cases(unsigned rounds)
{
    enum { CW = 96, CH = 72 };
    static uint32_t content_px[CW * CH], picture_px[CW * CH * 2], got[CW * CH], want[CW * CH];
    for (unsigned r = 0; r < rounds; r++) {
        for (int i = 0; i < CW * CH; i++)
            content_px[i] = rnd();
        // Premultiplied content inside the overlays matters only to the
        // clamp; any top byte is legal input. Some fully see-through and
        // fully opaque pixels too.
        for (int i = 0; i < CW * CH; i += 7)
            content_px[i] &= 0x00FFFFFFu;
        for (int i = 3; i < CW * CH; i += 11)
            content_px[i] |= 0xFF000000u;
        pc_image_t content = {content_px, CW, CW, CH};
        uint32_t pw = (uint32_t)range(1, CW), ph = (uint32_t)range(1, CH), ppitch = pw + (uint32_t)range(0, 8);
        for (uint32_t i = 0; i < ppitch * ph; i++)
            picture_px[i] = rnd();
        pc_image_t picture = {picture_px, ppitch, pw, ph};
        pc_place_t place = {0};
        place.clip = (pc_rect_t){range(0, CW - 1), range(0, CH - 1), 0, 0};
        place.clip.w = range(0, CW - place.clip.x);
        place.clip.h = range(0, CH - place.clip.y);
        // The origin anywhere near, partly outside the clip and the content.
        place.origin = (pc_rect_t){range(-20, CW), range(-20, CH), (int32_t)pw, (int32_t)ph};
        place.noverlays = (uint32_t)range(0, PLANE_OVERLAYS_MAX);
        for (uint32_t i = 0; i < place.noverlays; i++) {
            pc_rect_t *o = &place.overlay[i];
            if (place.clip.w == 0 || place.clip.h == 0) {
                *o = (pc_rect_t){0, 0, 0, 0};
                continue;
            }
            o->x = place.clip.x + range(0, place.clip.w - 1);
            o->y = place.clip.y + range(0, place.clip.h - 1);
            o->w = range(0, place.clip.x + place.clip.w - o->x);
            o->h = range(0, place.clip.y + place.clip.h - o->y);
        }
        pc_rect_t area = {range(0, CW - 1), range(0, CH - 1), 0, 0};
        area.w = range(1, CW - area.x);
        area.h = range(1, CH - area.y);
        pc_image_t dst = {got, (uint32_t)area.w, (uint32_t)area.w, (uint32_t)area.h};
        pc_counts_t gc = {0}, wc = {0};
        memset(got, 0xA5, sizeof(got));
        plane_compose(&dst, &content, &picture, &place, area, &gc);
        model(want, &content, &picture, &place, area, &wc);
        if (memcmp(got, want, (size_t)area.w * area.h * 4) != 0 || gc.copied != wc.copied ||
            gc.blended != wc.blended || gc.opaque != wc.opaque) {
            fprintf(stderr, "round %u: clip %d,%d %dx%d origin %d,%d picture %ux%u overlays %u area %d,%d %dx%d; "
                    "counts got %lu/%lu/%lu want %lu/%lu/%lu\n", r, place.clip.x, place.clip.y, place.clip.w,
                    place.clip.h, place.origin.x, place.origin.y, pw, ph, place.noverlays, area.x, area.y, area.w,
                    area.h, (unsigned long)gc.copied, (unsigned long)gc.blended, (unsigned long)gc.opaque,
                    (unsigned long)wc.copied, (unsigned long)wc.blended, (unsigned long)wc.opaque);
            s_failed++;
            return;
        }
        if (plane_overlay_area(&place) != model_area(&place)) {
            fprintf(stderr, "round %u: overlay area %lu, model %lu\n", r, (unsigned long)plane_overlay_area(&place),
                    (unsigned long)model_area(&place));
            s_failed++;
            return;
        }
        CHECK(plane_place_valid(&place, CW, CH, model_area(&place)));
    }
}

// Origins as far as a valid place allows, the picture out of reach of
// every content pixel: the distance from the origin overflows 32 bits, and
// the content must come out opaque, with no undefined arithmetic on the way
// (the suite runs under UBSan).
static void far_origin_cases(void)
{
    enum { W = 4, H = 4 };
    static const pc_rect_t origins[] = {
        {INT32_MIN, INT32_MIN, 1, 1}, {0, INT32_MIN, 1, 1}, {INT32_MIN, 0, 1, 1},
        {INT32_MAX - 1, 0, 1, 1}, {0, INT32_MAX - 1, 1, 1},
    };
    uint32_t content_px[W * H], picture_px[3 * 3], got[W * H], want[W * H];
    for (int i = 0; i < W * H; i++)
        content_px[i] = 0x80000000u | (uint32_t)i;
    for (int i = 0; i < 9; i++)
        picture_px[i] = 0x00ABCDEFu;
    pc_image_t content = {content_px, W, W, H};
    pc_image_t picture = {picture_px, 3, 3, 3};
    for (unsigned i = 0; i < sizeof(origins) / sizeof(origins[0]); i++) {
        pc_place_t place = {.origin = origins[i], .clip = {0, 0, W, H}, .noverlays = 1};
        place.overlay[0] = (pc_rect_t){0, 0, W, 2};
        CHECK(plane_place_valid(&place, W, H, 1000));
        pc_counts_t c = {0}, m = {0};
        pc_image_t dst = {got, W, W, H};
        plane_compose(&dst, &content, &picture, &place, (pc_rect_t){0, 0, W, H}, &c);
        model(want, &content, &picture, &place, (pc_rect_t){0, 0, W, H}, &m);
        CHECK(memcmp(got, want, sizeof(got)) == 0 && memcmp(got, content_px, sizeof(got)) == 0);
        CHECK(c.copied == 0 && c.blended == 0 && c.opaque == W * H && m.opaque == c.opaque);
    }
}

int main(int argc, char **argv)
{
    unsigned rounds = argc > 1 ? (unsigned)atoi(argv[1]) : 3000;
    blend_cases();
    area_cases();
    compose_cases(rounds);
    far_origin_cases();
    if (s_failed != 0) {
        fprintf(stderr, "plane compose: %d failed\n", s_failed);
        return 1;
    }
    printf("plane compose: all cases passed (%u random rounds)\n", rounds);
    return 0;
}
