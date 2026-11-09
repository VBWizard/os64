/* Private access supplies key comparisons, prepared-value perturbations, forced
 * digest collisions and work-budget control. Public operations exercise ownership/storage. */
#include "../userland/libsvg/internal.h"
static svg_raster_key_t *cache_key(svg_context_t *c, svg_scene_t *s, svg_render_options_t *o,
                                   svg_render_target_t *t)
{
    svg_raster_key_t *key = NULL;
    CHECK(svg_raster_key_create(c, s, o, t, 65536, &key) == SVG_OK && key);
    return key;
}
static bool key_equal(const svg_raster_key_t *a, const svg_raster_key_t *b)
{
    return a->backend == b->backend && a->size == b->size && !memcmp(a->data, b->data, a->size);
}
static void cache_key_contracts(void)
{
    Ledger l = {0};
    svg_context_t *c = create(&l);
    Document d = {0};
    add(&d, SIZE_MAX, NULL, 0);
    add(&d, 0, "M8 8H56V56H8Z", 0xffcc8844);
    svg_gradient_stop_t stops[] = {{0, 0xffff0000}, {1, 0xff0000ff}};
    Map map = {1, 0, 0, 1, 0, 0};
    svg_linear_gradient_t linear = {.x2 = 1, .transform = map, .stops = stops, .count = 2};
    svg_radial_gradient_t radial = {.r = 20, .transform = map, .stops = stops, .count = 2};
    d.nodes[1].style.fill = (svg_paint_t){.kind = SVG_PAINT_LINEAR, .linear = &linear};
    d.nodes[1].style.stroke = (svg_paint_t){.kind = SVG_PAINT_RADIAL, .radial = &radial};
    double dash[] = {3, 4};
    d.nodes[1].style.dash = (svg_dash_t){dash, 2, 1};
    d.nodes[0].style.viewport = (svg_viewport_t){true, true, 64, 64, map};
    d.nodes[1].clip = (svg_clip_source_t){.active = true,
                                          .kind = SVG_NODE_PATH,
                                          .d = {"M0 0H1V1H0Z", 11},
                                          .transform = map,
                                          .outer_transform = map,
                                          .object_bbox = true};
    svg_scene_t *s = prepare(c, &d);
    uint32_t pixels[68 * 64];
    svg_render_target_t t = target(pixels);
    svg_render_options_t o = svg_render_options_default(os64_freetype_geometry_v1());
    svg_raster_key_t *base = cache_key(c, s, &o, &t), *key;
#define DIFFERENT(field, value)                                                                    \
    do {                                                                                           \
        __typeof__(field) saved = (field);                                                         \
        (field) = (value);                                                                         \
        key = cache_key(c, s, &o, &t);                                                             \
        CHECK(!key_equal(base, key));                                                              \
        CHECK(svg_raster_key_release(c, key) == SVG_OK);                                           \
        (field) = saved;                                                                           \
    } while (0)
    DIFFERENT(s->mode, SVG_MODE_IMAGE);
    DIFFERENT(t.origin_x, .25);
    DIFFERENT(t.origin_y, .5);
    DIFFERENT(t.width, 63);
    DIFFERENT(t.height, 63);
    DIFFERENT(o.viewport_to_device.a, 2);
    DIFFERENT(o.viewport_to_device.ty, .125);
    DIFFERENT(o.arc_error, .125);
    DIFFERENT(o.quantization_error, .125);
    DIFFERENT(s->items[1].parent, SIZE_MAX);
    DIFFERENT(s->items[1].kind, SVG_NODE_SHAPE);
    DIFFERENT(s->items[1].user_to_viewport.tx, 2);
    DIFFERENT(s->items[1].style.transform.tx, 2);
    DIFFERENT(s->items[1].style.fill_opacity, .7);
    DIFFERENT(s->items[1].style.stroke_opacity, .7);
    DIFFERENT(s->items[1].style.opacity, .7);
    DIFFERENT(s->items[1].style.stroke_style.width, 7);
    DIFFERENT(s->items[1].style.stroke_style.miter_limit, 7);
    DIFFERENT(s->items[1].style.stroke_style.cap, OS64_GEOMETRY_ROUND_CAP);
    DIFFERENT(s->items[1].style.stroke_style.join, OS64_GEOMETRY_BEVEL);
    DIFFERENT(s->items[1].style.dash.offset, 2);
    DIFFERENT(((double *)s->items[1].style.dash.values)[0], 2);
    DIFFERENT(s->items[1].style.visible, false);
    DIFFERENT(s->items[1].style.even_odd, true);
    DIFFERENT(s->items[0].style.viewport.clip, false);
    DIFFERENT(s->items[0].style.viewport.width, 63);
    DIFFERENT(s->items[0].style.viewport.content_transform.a, 2);
    DIFFERENT(s->items[1].clip.even_odd, true);
    DIFFERENT(s->items[1].clip.object_bbox, false);
    DIFFERENT(s->items[1].clip.transform.a, 2);
    DIFFERENT(s->items[1].clip.outer_transform.tx, 2);
    DIFFERENT(((svg_path_t *)s->items[1].clip.path)->commands[1].p[0].x, .5);
    DIFFERENT(((svg_path_t *)s->items[1].path)->commands[1].p[0].x, 55);
    svg_linear_gradient_t *lg = (svg_linear_gradient_t *)s->items[1].style.fill.linear;
    svg_radial_gradient_t *rg = (svg_radial_gradient_t *)s->items[1].style.stroke.radial;
    DIFFERENT(lg->x1, 1);
    DIFFERENT(lg->y1, 1);
    DIFFERENT(lg->x2, 2);
    DIFFERENT(lg->y2, 1);
    DIFFERENT(lg->spread, SVG_SPREAD_REFLECT);
    DIFFERENT(lg->object_bbox, true);
    DIFFERENT(lg->transform.a, 2);
    DIFFERENT(((svg_gradient_stop_t *)lg->stops)[0].offset, .1);
    DIFFERENT(((svg_gradient_stop_t *)lg->stops)[0].argb, 0xff00ffff);
    DIFFERENT(rg->cx, 1);
    DIFFERENT(rg->cy, 1);
    DIFFERENT(rg->r, 21);
    DIFFERENT(rg->fx, 1);
    DIFFERENT(rg->fy, 1);
    DIFFERENT(rg->fr, 1);
    DIFFERENT(rg->spread, SVG_SPREAD_REPEAT);
    DIFFERENT(rg->object_bbox, true);
    DIFFERENT(rg->transform.d, 2);
    DIFFERENT(((svg_gradient_stop_t *)rg->stops)[1].argb, 0xffffff00);
#undef DIFFERENT
    // Identities, unused command slots, storage padding and admission quotas
    // do not describe pixels. Signed zero has one representation in the key.
    s->items[1].source = 999;
    s->items[1].instance = 777;
    ((svg_path_t *)s->items[1].path)->commands[1].p[2].x = NAN;
    t.origin_x = -0.0;
    t.stride = 64 * 4;
    o.max_work = o.max_geometry_work = o.max_raster_work = 0;
    key = cache_key(c, s, &o, &t);
    CHECK(key_equal(base, key));
    CHECK(svg_raster_key_release(c, key) == SVG_OK);
    lg->x1 = NAN;
    key = (void *)1;
    CHECK(svg_raster_key_create(c, s, &o, &t, 65536, &key) == SVG_BAD_ARGUMENT && !key);
    lg->x1 = 0;
    CHECK(svg_raster_key_create(c, s, &o, &t, base->size - 1, &key) == SVG_LIMIT && !key);
    CHECK(svg_raster_key_create(c, s, &o, &t, base->size, &key) == SVG_OK);
    CHECK(key_equal(base, key));
    CHECK(svg_raster_key_release(c, key) == SVG_OK);
    CHECK(svg_scene_release(c, s) == SVG_OK && !d.pins);
    CHECK(svg_destroy(c) == SVG_BUSY);
    CHECK(svg_raster_key_release(c, base) == SVG_OK);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
}
static void cache_storage_contracts(void)
{
    Ledger l = {0}, owner = {0};
    svg_context_t *c = create(&l), *cc = create(&owner);
    svg_raster_cache_t *cache = NULL;
    CHECK(svg_raster_cache_create(cc, 65536, 2, &cache) == SVG_OK);
    size_t baseline = owner.live;
    Document d = {0};
    add(&d, SIZE_MAX, NULL, 0);
    add(&d, 0, "M8 8H56V56H8Z", 0xff0088cc);
    uint32_t pixels[68 * 64], copied[68 * 64];
    svg_render_target_t t = target(pixels), dst = target(copied);
    svg_render_options_t o = svg_render_options_default(os64_freetype_geometry_v1());
    svg_scene_t *s = prepare(c, &d);
    svg_raster_key_t *a = cache_key(c, s, &o, &t);
    sentinel(pixels);
    sentinel(copied);
    bool hit = true;
    CHECK(svg_raster_cache_read(cc, cache, a, &dst, &hit) == SVG_OK && !hit);
    CHECK(copied[32 * 68 + 32] == 0xdeadbeef);
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_OK);
    CHECK(svg_raster_cache_store(cc, cache, a, &t, SVG_LIMIT) == SVG_LIMIT);
    CHECK(svg_raster_cache_read(cc, cache, a, &dst, &hit) == SVG_OK && !hit);
    CHECK(svg_raster_cache_store(cc, cache, a, &t, SVG_OK) == SVG_OK);
    svg_raster_cache_stats_t stats;
    CHECK(svg_raster_cache_stats(cache, &stats) == SVG_OK && stats.entries == 1);
    size_t entry_cost = stats.bytes;
    CHECK(owner.live - baseline == entry_cost);
    CHECK(svg_raster_cache_store(cc, cache, a, &t, SVG_OK) == SVG_OK);
    CHECK(svg_raster_cache_stats(cache, &stats) == SVG_OK && stats.entries == 1 &&
          stats.stores == 1);
    CHECK(svg_scene_release(c, s) == SVG_OK);
    uint64_t digest = a->digest;
    CHECK(svg_raster_key_release(c, a) == SVG_OK);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
    // A new document/context with identical values retrieves owned pixels after
    // the source context, key, scene and provider data have been discarded.
    memset(&d, 0, sizeof(d));
    memset(pixels, 0, sizeof(pixels));
    c = create(&l);
    add(&d, SIZE_MAX, NULL, 0);
    add(&d, 0, "m8 8h48v48h-48z", 0xff0088cc);
    s = prepare(c, &d);
    a = cache_key(c, s, &o, &t);
    CHECK(a->digest == digest);
    CHECK(svg_raster_cache_read(cc, cache, a, &dst, &hit) == SVG_OK && hit);
    CHECK(copied[32 * 68 + 32] == 0xff0088cc);
    padding(copied);
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_OK);
    for (size_t y = 0; y < 64; ++y)
        CHECK(!memcmp(pixels + y * 68, copied + y * 68, 64 * 4));
    s->items[1].style.fill.argb = 0xffcc8800;
    svg_raster_key_t *b = cache_key(c, s, &o, &t);
    b->digest = a->digest; // Force a candidate collision through the private test seam.
    sentinel(copied);
    CHECK(svg_raster_cache_read(cc, cache, b, &dst, &hit) == SVG_OK && !hit);
    CHECK(copied[32 * 68 + 32] == 0xdeadbeef);
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_OK);
    CHECK(svg_raster_cache_store(cc, cache, b, &t, SVG_OK) == SVG_OK);
    CHECK(svg_raster_cache_read(cc, cache, b, &dst, &hit) == SVG_OK && hit);
    CHECK(copied[32 * 68 + 32] == 0xffcc8800);
    CHECK(svg_raster_cache_read(cc, cache, a, &dst, &hit) == SVG_OK && hit);
    CHECK(copied[32 * 68 + 32] == 0xff0088cc);
    s->items[1].style.fill.argb = 0xff00cc88;
    svg_raster_key_t *third = cache_key(c, s, &o, &t);
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_OK);
    CHECK(svg_raster_cache_store(cc, cache, third, &t, SVG_OK) == SVG_OK);
    CHECK(svg_raster_cache_read(cc, cache, b, &dst, &hit) == SVG_OK && !hit);
    CHECK(svg_raster_cache_read(cc, cache, a, &dst, &hit) == SVG_OK && hit);
    CHECK(svg_raster_cache_read(cc, cache, third, &dst, &hit) == SVG_OK && hit);
    os64_geometry_backend_t backend = *o.geometry;
    const os64_geometry_backend_t *saved_backend = third->backend;
    third->backend = &backend;
    CHECK(svg_raster_cache_read(cc, cache, third, &dst, &hit) == SVG_OK && !hit);
    third->backend = saved_backend;
    dst.origin_x = .5;
    CHECK(svg_raster_cache_read(cc, cache, third, &dst, &hit) == SVG_BAD_ARGUMENT && !hit);
    dst.origin_x = 0;
    dst.capacity = 1;
    CHECK(svg_raster_cache_read(cc, cache, third, &dst, &hit) == SVG_LIMIT && !hit);
    dst = target(copied);
    CHECK(svg_raster_cache_stats(cache, &stats) == SVG_OK && stats.entries == 2 &&
          stats.evictions == 1);
    CHECK(svg_raster_cache_clear(cc, cache) == SVG_OK && owner.live == baseline);
    CHECK(svg_destroy(cc) == SVG_BUSY);
    CHECK(svg_raster_cache_release(cc, cache) == SVG_OK);
    // A byte cap, separately from the entry cap, evicts before allocating.
    CHECK(svg_raster_cache_create(cc, entry_cost, 100, &cache) == SVG_OK);
    baseline = owner.live;
    owner.peak = owner.live;
    s->items[1].style.fill.argb = 0xff0088cc;
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_OK);
    CHECK(svg_raster_cache_store(cc, cache, a, &t, SVG_OK) == SVG_OK);
    s->items[1].style.fill.argb = 0xff00cc88;
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_OK);
    CHECK(svg_raster_cache_store(cc, cache, third, &t, SVG_OK) == SVG_OK);
    CHECK(owner.peak <= baseline + entry_cost);
    CHECK(svg_raster_cache_stats(cache, &stats) == SVG_OK && stats.entries == 1 &&
          stats.bytes == entry_cost);
    CHECK(svg_raster_cache_release(cc, cache) == SVG_OK);
    CHECK(svg_raster_cache_create(cc, entry_cost - 1, 100, &cache) == SVG_OK);
    CHECK(svg_raster_cache_store(cc, cache, a, &t, SVG_OK) == SVG_LIMIT);
    CHECK(svg_raster_cache_release(cc, cache) == SVG_OK);
    CHECK(svg_raster_cache_create(cc, 0, 0, &cache) == SVG_OK);
    CHECK(svg_raster_cache_store(cc, cache, a, &t, SVG_OK) == SVG_LIMIT);
    CHECK(svg_raster_cache_read(cc, cache, a, &dst, &hit) == SVG_OK && !hit);
    CHECK(svg_raster_cache_release(cc, cache) == SVG_OK);
    CHECK(svg_raster_key_release(c, a) == SVG_OK);
    CHECK(svg_raster_key_release(c, b) == SVG_OK);
    CHECK(svg_raster_key_release(c, third) == SVG_OK);
    CHECK(svg_scene_release(c, s) == SVG_OK);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
    CHECK(svg_destroy(cc) == SVG_OK && !owner.live);
}
static void cache_failure_contracts(void)
{
    Ledger l = {0};
    svg_context_t *c = create(&l);
    Document d = {0};
    add(&d, SIZE_MAX, NULL, 0);
    add(&d, 0, "M8 8H56V56H8Z", 0xff0088cc);
    svg_scene_t *s = prepare(c, &d);
    uint32_t pixels[68 * 64];
    svg_render_target_t t = target(pixels);
    svg_render_options_t o = svg_render_options_default(os64_freetype_geometry_v1());
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_OK);
    svg_raster_key_t *key = NULL;
    size_t polls = l.polls;
    key = cache_key(c, s, &o, &t);
    size_t key_polls = l.polls - polls;
    CHECK(svg_raster_key_release(c, key) == SVG_OK);
    size_t baseline = l.live;
    for (unsigned mode = 0; mode < 2; ++mode) {
        l.admission = mode;
        l.fail = l.allocations + 1;
        CHECK(svg_raster_key_create(c, s, &o, &t, 65536, &key) ==
              (mode ? SVG_LIMIT : SVG_NO_MEMORY));
        CHECK(!key && l.live == baseline);
        l.fail = 0;
    }
    for (size_t i = 1; i <= key_polls; ++i) {
        l.cancel = l.polls + i;
        CHECK(svg_raster_key_create(c, s, &o, &t, 65536, &key) == SVG_CANCELLED);
        CHECK(!key && l.live == baseline);
        l.cancel = 0;
    }
    key = cache_key(c, s, &o, &t);
    svg_raster_cache_t *cache = NULL;
    baseline = l.live;
    for (unsigned mode = 0; mode < 2; ++mode) {
        l.admission = mode;
        l.fail = l.allocations + 1;
        CHECK(svg_raster_cache_create(c, 65536, 2, &cache) == (mode ? SVG_LIMIT : SVG_NO_MEMORY));
        CHECK(!cache && l.live == baseline);
        l.fail = 0;
    }
    CHECK(svg_raster_cache_create(c, 65536, 2, &cache) == SVG_OK);
    baseline = l.live;
    polls = l.polls;
    CHECK(svg_raster_cache_store(c, cache, key, &t, SVG_OK) == SVG_OK);
    size_t store_polls = l.polls - polls;
    polls = l.polls;
    bool hit;
    CHECK(svg_raster_cache_read(c, cache, key, &t, &hit) == SVG_OK && hit);
    size_t read_polls = l.polls - polls;
    size_t kept = l.live;
    l.reenter = true;
    for (size_t i = 1; i <= read_polls; ++i) {
        l.cancel = l.polls + i;
        CHECK(svg_raster_cache_read(c, cache, key, &t, &hit) == SVG_CANCELLED && !hit);
        CHECK(l.live == kept);
        l.cancel = 0;
    }
    l.reenter = false;
    CHECK(svg_raster_cache_clear(c, cache) == SVG_OK && l.live == baseline);
    for (size_t i = 1; i <= store_polls; ++i) {
        l.cancel = l.polls + i;
        CHECK(svg_raster_cache_store(c, cache, key, &t, SVG_OK) == SVG_CANCELLED);
        CHECK(l.live == baseline);
        l.cancel = 0;
        svg_raster_cache_stats_t stats;
        CHECK(svg_raster_cache_stats(cache, &stats) == SVG_OK && !stats.entries);
    }
    for (unsigned mode = 0; mode < 2; ++mode) {
        l.admission = mode;
        l.fail = l.allocations + 1;
        CHECK(svg_raster_cache_store(c, cache, key, &t, SVG_OK) ==
              (mode ? SVG_LIMIT : SVG_NO_MEMORY));
        CHECK(l.live == baseline);
        l.fail = 0;
    }
    uint64_t work = c->options.limits.work;
    c->options.limits.work = 0;
    CHECK(svg_raster_cache_read(c, cache, key, &t, &hit) == SVG_LIMIT && !hit);
    CHECK(svg_raster_cache_store(c, cache, key, &t, SVG_OK) == SVG_LIMIT);
    c->options.limits.work = work;
    CHECK(svg_raster_cache_store(c, cache, key, &t, SVG_OK) == SVG_OK);
    CHECK(svg_raster_cache_read(c, cache, key, &t, &hit) == SVG_OK && hit);
    svg_raster_cache_stats_t stats;
    CHECK(svg_raster_cache_stats(cache, &stats) == SVG_OK);
    size_t entry_cost = stats.bytes;
    CHECK(svg_raster_cache_release(c, cache) == SVG_OK);
    CHECK(svg_raster_cache_create(c, entry_cost, 2, &cache) == SVG_OK);
    CHECK(svg_raster_cache_store(c, cache, key, &t, SVG_OK) == SVG_OK);
    s->items[1].style.fill.argb = 0xffcc8800;
    svg_raster_key_t *replacement = cache_key(c, s, &o, &t);
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_OK);
    baseline = l.live - entry_cost;
    l.admission = false;
    l.fail = l.allocations + 1;
    CHECK(svg_raster_cache_store(c, cache, replacement, &t, SVG_OK) == SVG_NO_MEMORY);
    l.fail = 0;
    CHECK(l.live == baseline);
    CHECK(svg_raster_cache_stats(cache, &stats) == SVG_OK && !stats.entries && !stats.bytes);
    CHECK(svg_raster_cache_read(c, cache, replacement, &t, &hit) == SVG_OK && !hit);
    l.cancel = l.polls + 1;
    CHECK(svg_raster_cache_read(c, cache, replacement, &t, &hit) == SVG_CANCELLED && !hit);
    l.cancel = 0;
    CHECK(svg_raster_cache_store(c, cache, replacement, &t, SVG_OK) == SVG_OK);
    CHECK(svg_raster_cache_read(c, cache, replacement, &t, &hit) == SVG_OK && hit);
    CHECK(svg_raster_key_release(c, replacement) == SVG_OK);
    CHECK(svg_raster_cache_release(c, cache) == SVG_OK);
    CHECK(svg_raster_key_release(c, key) == SVG_OK);
    CHECK(svg_scene_release(c, s) == SVG_OK);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
    printf("SVG cache: %zu key, %zu store, %zu read cancellation cuts pass\n", key_polls,
           store_polls, read_polls);
}
static void cache_curve_contracts(void)
{
    Ledger l = {0};
    svg_context_t *c = create(&l);
    Document d = {0};
    add(&d, SIZE_MAX, NULL, 0);
    add(&d, 0, "M8 8Q16 4 24 8C28 12 32 20 40 24A10 8 20 0 1 48 40Z", 0xff0088cc);
    svg_scene_t *s = prepare(c, &d);
    uint32_t pixels[68 * 64];
    svg_render_target_t t = target(pixels);
    svg_render_options_t o = svg_render_options_default(os64_freetype_geometry_v1());
    svg_raster_key_t *base = cache_key(c, s, &o, &t);
    svg_command_t *commands = ((svg_path_t *)s->items[1].path)->commands;
    CHECK(commands[1].verb == SVG_QUADRATIC && commands[2].verb == SVG_CUBIC &&
          commands[3].verb == SVG_ARC);
    double *fields[] = {&commands[0].p[0].x,  &commands[1].p[0].x, &commands[1].p[1].y,
                        &commands[2].p[0].x,  &commands[2].p[1].y, &commands[2].p[2].x,
                        &commands[3].p[0].y,  &commands[3].rx,     &commands[3].ry,
                        &commands[3].rotation};
    for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); ++i) {
        double saved = *fields[i];
        *fields[i] += .125;
        svg_raster_key_t *key = cache_key(c, s, &o, &t);
        CHECK(!key_equal(base, key));
        CHECK(svg_raster_key_release(c, key) == SVG_OK);
        *fields[i] = saved;
    }
    for (unsigned i = 0; i < 2; ++i) {
        bool *flag = i ? &commands[3].sweep : &commands[3].large;
        *flag = !*flag;
        svg_raster_key_t *key = cache_key(c, s, &o, &t);
        CHECK(!key_equal(base, key));
        CHECK(svg_raster_key_release(c, key) == SVG_OK);
        *flag = !*flag;
    }
    CHECK(svg_raster_key_release(c, base) == SVG_OK);
    CHECK(svg_scene_release(c, s) == SVG_OK);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
}
static void cache_painter_roundtrips(void)
{
    Ledger l = {0};
    svg_context_t *c = create(&l);
    svg_raster_cache_t *cache = NULL;
    CHECK(svg_raster_cache_create(c, 65536, 3, &cache) == SVG_OK);
    uint32_t original[68 * 64], copied[68 * 64];
    svg_render_target_t t = target(original), dst = target(copied);
    t.origin_x = dst.origin_x = .375;
    t.origin_y = dst.origin_y = -.125;
    svg_render_options_t o = svg_render_options_default(os64_freetype_geometry_v1());
    Map identity = {1, 0, 0, 1, 0, 0};
    svg_gradient_stop_t stops[] = {{0, 0xffffdd22}, {.4, 0x8099ccff}, {1, 0}};
    svg_linear_gradient_t linear = {.x2 = 1,
                                    .transform = identity,
                                    .object_bbox = true,
                                    .stops = stops,
                                    .count = 3,
                                    .spread = SVG_SPREAD_REFLECT};
    svg_radial_gradient_t radial = {.cx = .5,
                                    .cy = .5,
                                    .r = .5,
                                    .fx = .3,
                                    .fy = .2,
                                    .fr = .1,
                                    .transform = identity,
                                    .object_bbox = true,
                                    .stops = stops,
                                    .count = 3};
    for (unsigned i = 0; i < 15; ++i) {
        Document d = fixture(i < 12 ? i : 1);
        if (i == 12)
            d.nodes[1].style.fill = (svg_paint_t){.kind = SVG_PAINT_LINEAR, .linear = &linear};
        if (i == 13)
            d.nodes[1].style.stroke = (svg_paint_t){.kind = SVG_PAINT_RADIAL, .radial = &radial};
        if (i == 14)
            d.nodes[0].style.display = false;
        if (i >= 12 && i < 14) {
            d.nodes[0].style.viewport = (svg_viewport_t){true, true, 64, 64, identity};
            d.nodes[1].clip = (svg_clip_source_t){.active = true,
                                                  .kind = SVG_NODE_PATH,
                                                  .d = {"M0 0H1V1H0Z", 11},
                                                  .transform = identity,
                                                  .outer_transform = identity,
                                                  .object_bbox = true};
        }
        svg_scene_t *scene = prepare(c, &d);
        svg_raster_key_t *key = cache_key(c, scene, &o, &t);
        sentinel(original);
        sentinel(copied);
        CHECK(svg_scene_render(c, scene, &o, &t, NULL) == SVG_OK);
        CHECK(svg_raster_cache_store(c, cache, key, &t, SVG_OK) == SVG_OK);
        bool hit;
        CHECK(svg_raster_cache_read(c, cache, key, &dst, &hit) == SVG_OK && hit);
        CHECK(!memcmp(original, copied, sizeof(original)));
        padding(copied);
        CHECK(svg_raster_key_release(c, key) == SVG_OK);
        CHECK(svg_scene_release(c, scene) == SVG_OK && !d.pins);
    }
    CHECK(svg_raster_cache_release(c, cache) == SVG_OK);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
}
static void raster_cache_contracts(void)
{
    cache_key_contracts();
    cache_curve_contracts();
    cache_painter_roundtrips();
    cache_storage_contracts();
    cache_failure_contracts();
}
