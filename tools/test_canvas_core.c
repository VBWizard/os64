#include <canvas/canvas.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define OK(x) CHECK((x) == CANVAS_OK)
typedef union { max_align_t align; size_t bytes; } Block;
typedef struct {
    size_t live, peak, calls, fail_at, budget;
    bool limit;
    canvas_context_t *reenter;
} Owner;
static os64_geometry_status_t alloc(void *arg, size_t bytes, void **out)
{
    Owner *o = arg;
    *out = NULL;
    if (o->reenter) CHECK(canvas_destroy(o->reenter) == CANVAS_BUSY);
    if (++o->calls == o->fail_at)
        return o->limit ? OS64_GEOMETRY_LIMIT : OS64_GEOMETRY_NO_MEMORY;
    if (o->budget && bytes > o->budget - o->live) return OS64_GEOMETRY_LIMIT;
    Block *p = malloc(sizeof(*p) + bytes);
    if (!p) return OS64_GEOMETRY_NO_MEMORY;
    p->bytes = bytes;
    o->live += bytes;
    if (o->peak < o->live) o->peak = o->live;
    *out = p + 1;
    return OS64_GEOMETRY_OK;
}
static void release(void *arg, void *p, size_t bytes)
{
    Owner *o = arg;
    if (o->reenter) CHECK(canvas_set_fill(o->reenter, 0) == CANVAS_BUSY);
    Block *b = (Block *)p - 1;
    CHECK(b->bytes == bytes && o->live >= bytes);
    o->live -= bytes;
    free(b);
}
static canvas_options_t options(Owner *o)
{ return canvas_options_default((os64_geometry_memory_t){o, alloc, release}); }
static canvas_bitmap_t bitmap(canvas_context_t *c)
{ canvas_bitmap_t v; OK(canvas_bitmap(c, &v)); return v; }
static canvas_state_t state(canvas_context_t *c)
{ canvas_state_t s; OK(canvas_state(c, &s)); return s; }
static void pixel(canvas_context_t *c, unsigned x, unsigned y, uint32_t expected)
{
    canvas_bitmap_t v = bitmap(c);
    CHECK(x < v.width && y < v.height);
    CHECK(v.pixels[y * (v.stride / 4) + x] == expected);
}
static void basics(void)
{
    Owner o = {0}; canvas_options_t opts = options(&o); canvas_context_t *c;
    OK(canvas_create(&opts, 4, 3, &c));
    CHECK(state(c).fill_argb == 0xff000000 && state(c).global_alpha == 1);
    for (unsigned i = 0; i < 12; ++i) CHECK(bitmap(c).pixels[i] == 0);
    OK(canvas_ack_damage(c));
    OK(canvas_set_fill(c, 0xffff0000));
    OK(canvas_fill_rect(c, 3, 2, -2, -1));
    pixel(c, 1, 1, 0xffff0000); pixel(c, 2, 1, 0xffff0000); pixel(c, 3, 1, 0);
    draw2d_damage_t d;
    OK(canvas_damage(c, &d)); CHECK(d.x0 == 1 && d.y0 == 1 && d.x1 == 3 && d.y1 == 2);
    OK(canvas_save(c)); OK(canvas_set_fill(c, 0xff0000ff)); OK(canvas_set_alpha(c, .5));
    OK(canvas_fill_rect(c, 1, 1, 1, 1)); pixel(c, 1, 1, 0xff7f0080);
    OK(canvas_restore(c)); CHECK(state(c).fill_argb == 0xffff0000 && state(c).global_alpha == 1);
    pixel(c, 1, 1, 0xff7f0080); /* Restoring state does not restore pixels. */
    OK(canvas_restore(c)); CHECK(state(c).fill_argb == 0xffff0000);
    OK(canvas_set_alpha(c, .25));
    const double invalid[] = {-1, 2, NAN, INFINITY, -INFINITY};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(*invalid); ++i) {
        OK(canvas_set_alpha(c, invalid[i])); CHECK(state(c).global_alpha == .25);
        OK(canvas_fill_rect(c, invalid[i], 0, 0, 1));
    }
    OK(canvas_set_alpha(c, 0)); OK(canvas_clear_rect(c, 1, 1, 1, 1)); pixel(c, 1, 1, 0);
    OK(canvas_save(c));
    const uint32_t *old = bitmap(c).pixels;
    size_t calls = o.calls;
    OK(canvas_reset(c, 4, 3)); CHECK(bitmap(c).pixels == old && o.calls == calls);
    CHECK(state(c).fill_argb == 0xff000000 && state(c).global_alpha == 1);
    OK(canvas_restore(c)); CHECK(state(c).global_alpha == 1);
    for (unsigned i = 0; i < 12; ++i) CHECK(bitmap(c).pixels[i] == 0);
    OK(canvas_damage(c, &d)); CHECK(d.x0 == 0 && d.y0 == 0 && d.x1 == 4 && d.y1 == 3);
    OK(canvas_reset(c, 0, 8)); CHECK(!bitmap(c).pixels && bitmap(c).height == 8);
    OK(canvas_fill_rect(c, -DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX));
    OK(canvas_reset(c, 2, 2));
    OK(canvas_fill_rect(c, -1, -1, DBL_MAX, DBL_MAX));
    for (unsigned i = 0; i < 4; ++i) CHECK(bitmap(c).pixels[i] == 0xff000000);
    OK(canvas_reset(c, 2, 2));
    OK(canvas_set_fill(c, 0xffffffff)); OK(canvas_fill_rect(c, .5, .5, 1, 1));
    for (unsigned i = 0; i < 4; ++i) CHECK(bitmap(c).pixels[i] == 0x40404040);
    OK(canvas_clear_rect(c, 0, 0, .5, 1)); pixel(c, 0, 0, 0x20202020);
    OK(canvas_ack_damage(c));
    OK(canvas_fill_rect(c, NAN, 0, 1, 1)); OK(canvas_fill_rect(c, 0, 0, INFINITY, 1));
    OK(canvas_damage(c, &d)); CHECK(d.x1 == 0);
    OK(canvas_destroy(c)); CHECK(o.live == 0);
}
static void surfaces(void)
{
    uint32_t pixels[10];
    for (unsigned i = 0; i < 10; ++i) pixels[i] = 0x12345678;
    pixels[0] = pixels[1] = pixels[4] = pixels[5] = 0;
    draw2d_surface_t s = {pixels, 2, 2, 16, 24, DRAW2D_ARGB8_PREMULTIPLIED};
    draw2d_solid_t red = {0xffff0000}; draw2d_composite_t over = {DRAW2D_SOURCE_OVER, 65535};
    draw2d_damage_t d;
    CHECK(draw2d_surface_validate(&s) == DRAW2D_OK);
    CHECK(draw2d_paint_rect(&s, -1, -1, 4, 4, red, over, 3, &d) == DRAW2D_LIMIT);
    CHECK(pixels[0] == 0 && pixels[5] == 0 && d.x1 == 0);
    CHECK(draw2d_paint_rect(&s, -1, -1, 4, 4, red, over, 4, &d) == DRAW2D_OK);
    for (unsigned i = 0; i < 10; ++i)
        CHECK(pixels[i] == (i == 0 || i == 1 || i == 4 || i == 5 ? 0xffff0000 : 0x12345678));
    s.capacity = 23; CHECK(draw2d_surface_validate(&s) == DRAW2D_LIMIT);
    s.capacity = SIZE_MAX; s.stride = SIZE_MAX - 3; s.height = 3;
    CHECK(draw2d_surface_validate(&s) == DRAW2D_LIMIT);
    s.stride = 4; CHECK(draw2d_surface_validate(&s) == DRAW2D_BAD_ARGUMENT);
    s.stride = 16; s.height = 2; s.pixels = (char *)pixels + 1;
    CHECK(draw2d_surface_validate(&s) == DRAW2D_BAD_ARGUMENT);
    s.pixels = pixels; s.format = (draw2d_format_t)99;
    CHECK(draw2d_surface_validate(&s) == DRAW2D_UNSUPPORTED);
    s.format = DRAW2D_ARGB8_STRAIGHT;
    CHECK(draw2d_paint_rect(&s, 0, 0, 1, 1, red, over, 4, &d) == DRAW2D_UNSUPPORTED);
    s.width = 0; s.pixels = NULL; CHECK(draw2d_surface_validate(&s) == DRAW2D_OK);
    draw2d_pixel_t wide[2] = {{0}, {0}};
    s = (draw2d_surface_t){wide, 2, 1, sizeof(wide), sizeof(wide), DRAW2D_ARGB16_PREMULTIPLIED};
    CHECK(draw2d_paint_rect(&s, 0, 0, 2, 1, red, over, 2, &d) == DRAW2D_OK);
    CHECK(wide[0].a == 65535 && wide[1].r == 65535 && wide[1].b == 0);
    /* Exhaustive alpha products against a floating rational reference. */
    for (unsigned a = 0; a < 256; ++a)
        for (unsigned b = 0; b < 256; ++b) {
            unsigned ref = (unsigned)((double)(a * 257u) * (b * 257u) / 65535.0 + .5);
            CHECK(draw2d_multiply(a * 257u, b * 257u) == ref);
        }
}
static void repeated_blends(void)
{
    Owner o = {0}; canvas_options_t opts = options(&o); canvas_context_t *c;
    OK(canvas_create(&opts, 1, 1, &c));
    /* Independent straight-source equations, rounding retained channels each
     * draw. Vary both source color and alpha, including transparent ink. */
    uint32_t seed = 19;
    unsigned dest[4] = {0};
    for (unsigned step = 0; step < 10000; ++step) {
        seed = seed * 1664525u + 1013904223u;
        OK(canvas_set_fill(c, seed)); OK(canvas_fill_rect(c, 0, 0, 1, 1));
        double a = (seed >> 24) / 255.0;
        unsigned src[4] = {seed >> 24, (seed >> 16) & 255, (seed >> 8) & 255, seed & 255};
        uint32_t actual = bitmap(c).pixels[0];
        for (unsigned channel = 0; channel < 4; ++channel) {
            double value = (channel ? src[channel] * a : src[channel]) + dest[channel] * (1 - a);
            unsigned ref = (unsigned)(value + .5);
            unsigned got = (actual >> ((3-channel)*8)) & 255;
            CHECK(got + 1 >= ref && ref + 1 >= got);
            dest[channel] = got;
        }
        CHECK(((actual >> 16) & 255) <= (actual >> 24));
        CHECK(((actual >> 8) & 255) <= (actual >> 24));
        CHECK((actual & 255) <= (actual >> 24));
    }
    OK(canvas_destroy(c)); CHECK(!o.live);
}
static void failures(void)
{
    for (unsigned limit = 0; limit < 2; ++limit) {
        for (size_t fail = 1; fail <= 7; ++fail) {
            Owner o = {.fail_at = fail, .limit = limit};
            canvas_options_t opts = options(&o); canvas_context_t *c = NULL;
            canvas_status_t refused = limit ? CANVAS_LIMIT : CANVAS_NO_MEMORY;
            canvas_status_t result = canvas_create(&opts, 2, 2, &c);
            if (fail <= 2) { CHECK(result == refused && !c && !o.live); continue; }
            OK(result); o.reenter = c;
            OK(canvas_set_fill(c, 0xff00ff00)); OK(canvas_fill_rect(c, 0, 0, 2, 2));
            for (unsigned i = 0; i < 3; ++i) {
                result = canvas_save(c);
                CHECK(result == (o.calls == fail ? refused : CANVAS_OK));
            }
            canvas_bitmap_t before = bitmap(c);
            canvas_state_t old = state(c);
            result = canvas_reset(c, 3, 3);
            if (o.calls == fail) {
                CHECK(result == refused && bitmap(c).pixels == before.pixels && bitmap(c).width == 2);
                CHECK(state(c).fill_argb == old.fill_argb); pixel(c, 0, 0, 0xff00ff00);
            } else OK(result);
            canvas_stats_t stats; OK(canvas_stats(c, &stats));
            CHECK(stats.live_bytes == o.live && stats.peak_bytes == o.peak);
            OK(canvas_destroy(c)); o.reenter = NULL; CHECK(!o.live);
        }
    }
    Owner o = {0}; canvas_options_t opts = options(&o); canvas_context_t *c;
    opts.max_saved_states = 1; opts.max_draw_pixels = 1;
    OK(canvas_create(&opts, 2, 2, &c)); OK(canvas_ack_damage(c));
    OK(canvas_save(c)); CHECK(canvas_save(c) == CANVAS_LIMIT);
    CHECK(canvas_fill_rect(c, 0, 0, 2, 2) == CANVAS_LIMIT); pixel(c, 0, 0, 0);
    draw2d_damage_t d; OK(canvas_damage(c, &d)); CHECK(!d.x1);
    CHECK(canvas_reset(c, UINT32_MAX, 1) == CANVAS_LIMIT); CHECK(bitmap(c).width == 2);
    OK(canvas_destroy(c)); CHECK(!o.live);
    opts = options(&o); opts.max_bytes = 0;
    CHECK(canvas_create(&opts, 1, 1, &c) == CANVAS_LIMIT && !c);
    opts = options(&o); opts.max_dimension = UINT32_MAX; opts.max_pixels = UINT64_MAX;
    CHECK(canvas_create(&opts, UINT32_MAX, UINT32_MAX, &c) == CANVAS_LIMIT && !c);
    opts = options(&o); OK(canvas_create(&opts, 0, 0, &c));
    canvas_stats_t stats; OK(canvas_stats(c, &stats)); OK(canvas_destroy(c));
    opts.max_bytes = stats.live_bytes + 16;
    OK(canvas_create(&opts, 2, 2, &c));
    CHECK(canvas_reset(c, 3, 3) == CANVAS_LIMIT); CHECK(bitmap(c).width == 2);
    OK(canvas_reset(c, 2, 2)); OK(canvas_destroy(c)); CHECK(!o.live);
    CHECK(canvas_create(NULL, 1, 1, &c) == CANVAS_BAD_ARGUMENT);
    CHECK(canvas_save(NULL) == CANVAS_BAD_ARGUMENT); OK(canvas_destroy(NULL));
}
static void shared_owner(void)
{
    Owner o = {0}; canvas_options_t opts = options(&o);
    canvas_context_t *a, *b;
    OK(canvas_create(&opts, 2, 2, &a));
    size_t one = o.live;
    o.budget = 2 * one;
    OK(canvas_create(&opts, 2, 2, &b));
    OK(canvas_set_fill(a, 0xffabcdef)); OK(canvas_fill_rect(a, 0, 0, 2, 2));
    OK(canvas_ack_damage(a));
    CHECK(canvas_save(a) == CANVAS_LIMIT);
    CHECK(canvas_reset(a, 3, 3) == CANVAS_LIMIT);
    pixel(a, 0, 0, 0xffabcdef); pixel(b, 0, 0, 0);
    draw2d_damage_t d; OK(canvas_damage(a, &d)); CHECK(!d.x1);
    OK(canvas_destroy(b));
    OK(canvas_save(a));
    OK(canvas_reset(a, 1, 4)); /* Same allocation size, different dimensions. */
    CHECK(bitmap(a).width == 1 && bitmap(a).height == 4);
    pixel(a, 0, 3, 0); CHECK(state(a).fill_argb == 0xff000000);
    canvas_stats_t stats; OK(canvas_stats(a, &stats)); CHECK(!stats.saved_states);
    OK(canvas_destroy(a)); CHECK(!o.live);
}
int main(void)
{
    basics(); surfaces(); repeated_blends(); failures(); shared_owner();
    printf("Canvas core: %zu assertions passed (pixels, rounding, bounds, state, reset, refusal, ownership)\n", checks);
    return 0;
}
