#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <svg/svg.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            abort();                                                                               \
        }                                                                                          \
        ++checks;                                                                                  \
    } while (0)
static size_t checks;
typedef struct {
    size_t live, peak, allocations, fail, polls, cancel, cap;
    bool admission, reenter;
    svg_context_t *context;
} Ledger;
typedef union {
    max_align_t alignment;
    struct {
        size_t bytes;
        Ledger *owner;
    } data;
} Header;
static os64_geometry_status_t allocate(void *opaque, size_t bytes, void **out)
{
    Ledger *l = opaque;
    *out = NULL;
    if (++l->allocations == l->fail)
        return l->admission ? OS64_GEOMETRY_LIMIT : OS64_GEOMETRY_NO_MEMORY;
    if (l->cap && bytes > l->cap - l->live)
        return OS64_GEOMETRY_LIMIT;
    Header *h = malloc(sizeof(*h) + bytes);
    CHECK(h);
    h->data.bytes = bytes;
    h->data.owner = l;
    l->live += bytes;
    if (l->peak < l->live)
        l->peak = l->live;
    *out = h + 1;
    return OS64_GEOMETRY_OK;
}
static void release(void *opaque, void *p, size_t bytes)
{
    Ledger *l = opaque;
    Header *h = (Header *)p - 1;
    CHECK(h->data.owner == l && h->data.bytes == bytes && bytes <= l->live);
    l->live -= bytes;
    free(h);
}
static bool cancelled(void *opaque)
{
    Ledger *l = opaque;
    if (l->reenter && l->context) {
        svg_stats_t s;
        CHECK(svg_stats(l->context, &s) == SVG_BUSY);
    }
    return ++l->polls == l->cancel;
}
typedef struct {
    size_t parent;
    svg_style_t style;
    const char *d;
    svg_clip_source_t clip;
} Node;
typedef struct {
    Node nodes[32];
    size_t count, pins, reads;
} Document;
static size_t add(Document *d, size_t parent, const char *path, uint32_t fill)
{
    size_t i = d->count++;
    CHECK(i < 32);
    d->nodes[i] = (Node){.parent = parent, .style = svg_initial_style(), .d = path};
    d->nodes[i].style.fill.argb = fill;
    return i;
}
static svg_status_t acquire(void *opaque, svg_mode_t mode, void **snapshot, uint64_t *root)
{
    Document *d = opaque;
    CHECK(mode == SVG_MODE_INLINE);
    ++d->pins;
    *snapshot = d;
    *root = 1;
    return SVG_OK;
}
static void unpin(void *opaque, void *snapshot)
{
    Document *d = opaque;
    CHECK(snapshot == d && d->pins);
    --d->pins;
}
static svg_status_t node(void *opaque, void *snapshot, uint64_t id, svg_node_t *out)
{
    Document *d = opaque;
    CHECK(snapshot == d && id && id <= d->count);
    ++d->reads;
    size_t i = (size_t)id - 1;
    *out = (svg_node_t){.kind = d->nodes[i].d ? SVG_NODE_PATH : SVG_NODE_GROUP};
    for (size_t j = i + 1; j < d->count; ++j) {
        if (!out->first_child && d->nodes[j].parent == i)
            out->first_child = j + 1;
        if (!out->next_sibling && d->nodes[j].parent == d->nodes[i].parent)
            out->next_sibling = j + 1;
    }
    return SVG_OK;
}
static svg_status_t style(void *opaque, void *snapshot, const svg_style_request_t *r,
                          svg_style_t *s, svg_string_t *path)
{
    Document *d = opaque;
    CHECK(snapshot == d && r->source && r->source <= d->count);
    ++d->reads;
    Node *n = &d->nodes[r->source - 1];
    *s = n->style;
    *path = (svg_string_t){n->d, n->d ? strlen(n->d) : 0};
    return SVG_OK;
}
static svg_status_t clip_source(void *opaque, void *snapshot, const svg_style_request_t *r,
                                svg_clip_source_t *out)
{
    Document *d = opaque;
    CHECK(snapshot == d && r->source && r->source <= d->count);
    *out = d->nodes[r->source - 1].clip;
    return SVG_OK;
}
static svg_context_t *create(Ledger *l)
{
    svg_options_t o = {.memory = {l, allocate, release},
                       .cancel_context = l,
                       .cancelled = cancelled,
                       .limits = svg_default_limits()};
    svg_context_t *c = NULL;
    CHECK(svg_create(&o, &c) == SVG_OK);
    l->context = c;
    return c;
}
static svg_scene_t *prepare(svg_context_t *c, Document *d)
{
    svg_provider_t p = {d, acquire, unpin, node, style, NULL, NULL, clip_source};
    os64_geometry_map_t map = {1, 0, 0, 1, 0, 0};
    svg_scene_t *scene = NULL;
    CHECK(svg_scene_prepare(c, &p, SVG_MODE_INLINE, &map, &scene) == SVG_OK);
    return scene;
}
static svg_render_target_t target(uint32_t *pixels)
{
    return (svg_render_target_t){
        .width = 64, .height = 64, .stride = 68 * 4, .capacity = 68 * 64 * 4, .pixels = pixels};
}
static void sentinel(uint32_t *p)
{
    for (size_t i = 0; i < 68 * 64; ++i)
        p[i] = 0xdeadbeef;
}
static void padding(const uint32_t *p)
{
    for (size_t y = 0; y < 64; ++y)
        for (size_t x = 64; x < 68; ++x)
            CHECK(p[y * 68 + x] == 0xdeadbeef);
}
static void stroke(Node *n, uint32_t color, double width)
{
    n->style.stroke = (svg_paint_t){.kind = SVG_PAINT_COLOR, .argb = color};
    n->style.stroke_style.width = width;
}
static Document fixture(unsigned which)
{
    Document d = {0};
    add(&d, SIZE_MAX, NULL, 0xff000000);
    switch (which) {
    case 0:
        d.nodes[0].style.opacity = .5;
        add(&d, 0, "M8 8H40V40H8Z", 0xff2091de);
        add(&d, 0, "M24 24H56V56H24Z", 0xff2091de);
        break;
    case 1:
        add(&d, 0, "M12 12H52V52H12Z", 0xff2091de);
        stroke(&d.nodes[1], 0xffe87925, 8);
        d.nodes[1].style.opacity = .5;
        break;
    case 2:
        add(&d, 0, NULL, 0xff000000);
        d.nodes[1].style.opacity = .5;
        add(&d, 1, "M4 4H44V44H4Z", 0xffff0000);
        add(&d, 1, NULL, 0xff000000);
        d.nodes[3].style.opacity = .5;
        add(&d, 3, "M20 20H60V60H20Z", 0xff0000ff);
        break;
    case 3:
        add(&d, 0, "M4 4H60V60H4ZM20 20H44V44H20Z", 0xff2091de);
        d.nodes[1].style.even_odd = true;
        break;
    case 4:
        add(&d, 0, "M4 4H60V60H4ZM20 20H44V44H20Z", 0xff2091de);
        break;
    case 5:
        add(&d, 0, "M4 24L12 24", 0);
        stroke(&d.nodes[1], 0xff2091de, 8);
        d.nodes[1].style.stroke_style.cap = OS64_GEOMETRY_ROUND_CAP;
        d.nodes[1].style.transform = (os64_geometry_map_t){4, 0, 0, 1, 0, 8};
        break;
    case 6:
        add(&d, 0, "M12 28L32 8L52 28", 0);
        stroke(&d.nodes[1], 0xff2091de, 10);
        d.nodes[1].style.stroke_style.join = OS64_GEOMETRY_MITER;
        d.nodes[1].style.stroke_style.cap = OS64_GEOMETRY_SQUARE;
        break;
    case 7:
        add(&d, 0, NULL, 0);
        d.nodes[1].style.visible = false;
        add(&d, 1, "M10 10H54V54H10Z", 0xff2091de);
        break;
    case 8:
        add(&d, 0, "M8 8H56V56H8Z", 0x012091de);
        break;
    case 9:
        add(&d, 0, "M-10 10H44V44H-10Z", 0xff2091de);
        d.nodes[1].style.transform = (os64_geometry_map_t){1, .2, .3, 1, .25, -.25};
        stroke(&d.nodes[1], 0xffe87925, 3);
        break;
    case 10:
        add(&d, 0, "M12 12H52V52H12Z", 0xff2091de);
        stroke(&d.nodes[1], 0xffe87925, 8);
        d.nodes[1].style.fill_opacity = .5;
        d.nodes[1].style.stroke_opacity = .5;
        d.nodes[1].style.opacity = .5;
        break;
    case 11:
        add(&d, 0, "M52 32A20 20 0 1 1 12 32A20 20 0 1 1 52 32Z", 0xff2091de);
        break;
    default:
        CHECK(false);
    }
    return d;
}
static void write_color(FILE *f, svg_paint_t p)
{
    if (p.kind == SVG_PAINT_NONE)
        fputs("none", f);
    else
        fprintf(f, "rgba(%u,%u,%u,%.17g)", (p.argb >> 16) & 255, (p.argb >> 8) & 255, p.argb & 255,
                (p.argb >> 24) / 255.0);
}
static void write_nodes(FILE *f, Document *d, size_t parent)
{
    for (size_t i = 0; i < d->count; ++i) {
        Node *n = &d->nodes[i];
        if (n->parent != parent)
            continue;
        svg_style_t s = n->style;
        os64_geometry_map_t m = s.transform;
        fprintf(f, "<%s ", n->d ? "path" : "g");
        if (n->d)
            fprintf(f, "d=\"%s\" ", n->d);
        fprintf(f, "transform=\"matrix(%.17g %.17g %.17g %.17g %.17g %.17g)\" fill=\"", m.a, m.b,
                m.c, m.d, m.tx, m.ty);
        write_color(f, s.fill);
        fputs("\" stroke=\"", f);
        write_color(f, s.stroke);
        const char *caps[] = {"butt", "round", "square"};
        const char *joins[] = {"miter", "miter-clip", "round", "bevel", "arcs"};
        fprintf(f,
                "\" stroke-width=\"%.17g\" stroke-linecap=\"%s\" stroke-linejoin=\"%s\" "
                "stroke-miterlimit=\"%.17g\" opacity=\"%.17g\" fill-opacity=\"%.17g\" "
                "stroke-opacity=\"%.17g\" fill-rule=\"%s\" visibility=\"%s\" style=\"display:%s\">",
                s.stroke_style.width, caps[s.stroke_style.cap], joins[s.stroke_style.join],
                s.stroke_style.miter_limit, s.opacity, s.fill_opacity, s.stroke_opacity,
                s.even_odd ? "evenodd" : "nonzero", s.visible ? "visible" : "hidden",
                s.display ? "inline" : "none");
        write_nodes(f, d, i);
        fprintf(f, "</%s>", n->d ? "path" : "g");
    }
}
static void save(const char *dir, unsigned which, Document *d, uint32_t *pixels)
{
    const uint32_t bg[] = {0xfff2f5fa, 0xff141a26};
    for (unsigned theme = 0; theme < 2; ++theme) {
        char name[1024];
        snprintf(name, sizeof(name), "%s/paint-%u-%u.ppm", dir, which, theme);
        FILE *f = fopen(name, "wb");
        CHECK(f);
        fprintf(f, "P6\n64 64\n255\n");
        for (size_t y = 0; y < 64; ++y)
            for (size_t x = 0; x < 64; ++x) {
                uint32_t p = pixels[y * 68 + x], a = p >> 24;
                for (int shift = 16; shift >= 0; shift -= 8)
                    fputc((int)((((p >> shift) & 255) * a +
                                 ((bg[theme] >> shift) & 255) * (255 - a) + 127) /
                                255),
                          f);
            }
        CHECK(!fclose(f));
        snprintf(name, sizeof(name), "%s/paint-%u-%u.html", dir, which, theme);
        f = fopen(name, "w");
        CHECK(f);
        fprintf(f,
                "<!doctype "
                "html><style>html,body{margin:0;background:#%06x}svg{display:block}</style><svg "
                "xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"64\">",
                bg[theme] & 0xffffff);
        write_nodes(f, d, SIZE_MAX);
        fputs("</svg>", f);
        CHECK(!fclose(f));
    }
}
static void analytic(const char *dir, unsigned which, const uint32_t *pixels)
{
    uint8_t rgba[64 * 64 * 4];
    double q = 5 * sqrt(2.0);
    const double px[] = {12 - q, 32, 52 + q, 52, 32, 12};
    const double py[] = {28, 8 - q, 28, 28 + q, 8 + q, 28 + q};
    unsigned maximum = 0;
    double total = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            unsigned covered = 0, fill = 0;
            for (int sy = 0; sy < 64; ++sy)
                for (int sx = 0; sx < 64; ++sx) {
                    double ax = x + (sx + .5) / 64, ay = y + (sy + .5) / 64;
                    bool inside = false;
                    if (which == 11)
                        inside = (ax - 32) * (ax - 32) + (ay - 32) * (ay - 32) < 400;
                    else if (which == 9) {
                        double ux = ((ax - .25) - .3 * (ay + .25)) / .94;
                        double uy = (-.2 * (ax - .25) + (ay + .25)) / .94;
                        fill += ux >= -10 && ux <= 44 && uy >= 10 && uy <= 44;
                        bool outer = ux >= -11.5 && ux <= 45.5 && uy >= 8.5 && uy <= 45.5;
                        bool inner = ux > -8.5 && ux < 42.5 && uy > 11.5 && uy < 42.5;
                        inside = outer && !inner;
                    } else
                        for (unsigned i = 0, j = 5; i < 6; j = i++)
                            if ((py[i] > ay) != (py[j] > ay) &&
                                ax < (px[j] - px[i]) * (ay - py[i]) / (py[j] - py[i]) + px[i])
                                inside = !inside;
                    covered += inside;
                }
            double stroke_a = covered / 4096.0, fill_a = fill / 4096.0;
            double a = stroke_a + fill_a * (1 - stroke_a);
            uint32_t color = 0;
            if (a > 0) {
                unsigned red =
                    (unsigned)((which == 9 ? (232 * stroke_a + 32 * fill_a * (1 - stroke_a)) / a
                                           : 32) +
                               .5);
                unsigned green =
                    (unsigned)((which == 9 ? (121 * stroke_a + 145 * fill_a * (1 - stroke_a)) / a
                                           : 145) +
                               .5);
                unsigned blue =
                    (unsigned)((which == 9 ? (37 * stroke_a + 222 * fill_a * (1 - stroke_a)) / a
                                           : 222) +
                               .5);
                color = ((uint32_t)(a * 255 + .5) << 24) | (red << 16) | (green << 8) | blue;
            }
            size_t offset = 4 * (y * 64 + x);
            rgba[offset] = (uint8_t)(color >> 16);
            rgba[offset + 1] = (uint8_t)(color >> 8);
            rgba[offset + 2] = (uint8_t)color;
            rgba[offset + 3] = (uint8_t)(color >> 24);
            unsigned delta = (unsigned)abs((int)(pixels[y * 68 + x] >> 24) - (int)(color >> 24));
            if (delta > maximum)
                maximum = delta;
            total += delta / 255.0;
        }
    printf("Analytic fixture %u: alpha max %u/255, L1 %.6f\n", which, maximum, total);
    CHECK(maximum <= (which == 11 ? 40u : 8u) && total <= (which == 11 ? 10 : 2));
    char name[1024];
    snprintf(name, sizeof(name), "%s/oracle-%u.rgba", dir, which);
    FILE *f = fopen(name, "wb");
    CHECK(f);
    CHECK(fwrite(rgba, 1, sizeof(rgba), f) == sizeof(rgba));
    CHECK(!fclose(f));
}
static void pictures(const char *dir)
{
    Ledger l = {0};
    svg_context_t *c = create(&l);
    uint32_t pixels[68 * 64];
    svg_render_options_t o = svg_render_options_default(os64_freetype_geometry_v1());
    svg_render_target_t t = target(pixels);
    for (unsigned i = 0; i < 12; ++i) {
        Document d = fixture(i);
        svg_scene_t *s = prepare(c, &d);
        size_t reads = d.reads;
        sentinel(pixels);
        svg_render_stats_t stats;
        CHECK(svg_scene_render(c, s, &o, &t, &stats) == SVG_OK);
        padding(pixels);
        CHECK(d.reads == reads && d.pins == 1 && stats.geometry_work <= o.max_geometry_work);
        if (i == 0)
            CHECK(pixels[12 * 68 + 12] == 0x802091de && pixels[28 * 68 + 28] == 0x802091de);
        if (i == 1)
            CHECK(pixels[32 * 68 + 32] == 0x802091de && pixels[14 * 68 + 14] == 0x80e87925);
        if (i == 2)
            CHECK((pixels[28 * 68 + 28] >> 24) == 128 && (pixels[52 * 68 + 52] >> 24) == 64);
        if (i == 3)
            CHECK(!pixels[32 * 68 + 32]);
        if (i == 4)
            CHECK(pixels[32 * 68 + 32] == 0xff2091de);
        if (i == 7)
            CHECK(pixels[32 * 68 + 32] == 0xff2091de);
        if (i == 8)
            CHECK(pixels[32 * 68 + 32] == 0x012091de);
        if (i == 10)
            CHECK((pixels[14 * 68 + 14] >> 24) == 96 && (pixels[32 * 68 + 32] >> 24) == 64);
        CHECK(pixels[63 * 68 + 63] == 0);
        save(dir, i, &d, pixels);
        if (i == 6 || i == 9 || i == 11)
            analytic(dir, i, pixels);
        CHECK(svg_scene_release(c, s) == SVG_OK && d.pins == 0);
    }
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
}
static os64_geometry_status_t broken_raster(os64_geometry_context_t *c,
                                            const os64_geometry_outline_t *outline,
                                            const os64_geometry_map_t *map,
                                            const os64_geometry_target_t *target)
{
    os64_geometry_status_t result = os64_freetype_geometry_v1()->raster(c, outline, map, target);
    return result == OS64_GEOMETRY_OK ? OS64_GEOMETRY_ENGINE_ERROR : result;
}
static void limits(void)
{
    Ledger l = {0};
    svg_context_t *c = create(&l);
    Document d = fixture(1);
    svg_scene_t *s = prepare(c, &d);
    svg_render_options_t good = svg_render_options_default(os64_freetype_geometry_v1()), o;
    uint32_t pixels[68 * 64];
    svg_render_target_t t = target(pixels), bad;
    sentinel(pixels);
    o = good;
    o.max_pixels = 0;
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_LIMIT);
    o = good;
    o.max_dimension = 63;
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_LIMIT);
    o = good;
    o.max_work = 0;
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_LIMIT);
    o = good;
    o.max_raster_work = 0;
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_LIMIT);
    o = good;
    o.max_geometry_work = 1;
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_LIMIT);
    o = good;
    o.arc_error = NAN;
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_BAD_ARGUMENT);
    bad = t;
    bad.capacity = 4;
    CHECK(svg_scene_render(c, s, &good, &bad, NULL) == SVG_LIMIT);
    bad = t;
    bad.stride = 255;
    CHECK(svg_scene_render(c, s, &good, &bad, NULL) == SVG_BAD_ARGUMENT);
    bad = t;
    bad.stride = SIZE_MAX - 3;
    CHECK(svg_scene_render(c, s, &good, &bad, NULL) == SVG_LIMIT);
    bad = t;
    bad.pixels = (uint32_t *)((char *)pixels + 1);
    CHECK(svg_scene_render(c, s, &good, &bad, NULL) == SVG_BAD_ARGUMENT);
    os64_geometry_backend_t old = *good.geometry;
    old.struct_size = offsetof(os64_geometry_backend_t, bounds);
    o = good;
    o.geometry = &old;
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_UNSUPPORTED);
    old.struct_size = sizeof(old);
    old.bounds = NULL;
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_BAD_ARGUMENT);
    old = *good.geometry;
    old.raster = broken_raster;
    size_t retained = l.live;
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_ENGINE_ERROR && l.live == retained);
    l.cap = l.live + 100;
    CHECK(svg_scene_render(c, s, &good, &t, NULL) == SVG_LIMIT);
    l.cap = 0;
    l.reenter = true;
    CHECK(svg_scene_render(c, s, &good, &t, NULL) == SVG_OK);
    l.reenter = false;
    padding(pixels);
    CHECK(svg_scene_release(c, s) == SVG_OK);
    const char *empty_paths[] = {"", "M", "M12 12"};
    for (size_t i = 0; i < 3; ++i) {
        d = fixture(1);
        d.nodes[1].d = empty_paths[i];
        s = prepare(c, &d);
        CHECK(svg_scene_render(c, s, &good, &t, NULL) == SVG_OK);
        for (size_t y = 0; y < 64; ++y)
            for (size_t x = 0; x < 64; ++x)
                CHECK(pixels[y * 68 + x] == 0);
        CHECK(svg_scene_release(c, s) == SVG_OK);
    }
    d = fixture(1);
    d.nodes[1].style.stroke_style.join = OS64_GEOMETRY_ARCS;
    s = prepare(c, &d);
    CHECK(svg_scene_render(c, s, &good, &t, NULL) == SVG_UNSUPPORTED);
    CHECK(svg_scene_release(c, s) == SVG_OK);
    d = fixture(5);
    d.nodes[1].d = "M8 8L8 8";
    s = prepare(c, &d);
    CHECK(svg_scene_render(c, s, &good, &t, NULL) == SVG_OK);
    CHECK(pixels[16 * 68 + 32] == 0xff2091de);
    CHECK(svg_scene_release(c, s) == SVG_OK);
    d = fixture(0);
    d.nodes[0].style.opacity = 0;
    s = prepare(c, &d);
    CHECK(svg_scene_render(c, s, &good, &t, NULL) == SVG_OK);
    for (size_t y = 0; y < 64; ++y)
        for (size_t x = 0; x < 64; ++x)
            CHECK(pixels[y * 68 + x] == 0);
    CHECK(svg_scene_release(c, s) == SVG_OK);
    d = (Document){0};
    add(&d, SIZE_MAX, NULL, 0);
    d.nodes[0].style.opacity = .5;
    add(&d, 0, "M8 8H40V40H8Z", 0xff2091de);
    add(&d, 0, NULL, 0);
    d.nodes[2].style.opacity = .2;
    add(&d, 2, "M8 8H40V40H8Z", 0xffff0000);
    d.nodes[3].style.visible = false;
    add(&d, 0, "M24 24H56V56H24Z", 0xff2091de);
    s = prepare(c, &d);
    CHECK(svg_scene_render(c, s, &good, &t, NULL) == SVG_OK);
    CHECK(pixels[28 * 68 + 28] == 0x802091de && pixels[52 * 68 + 52] == 0x802091de);
    CHECK(svg_scene_release(c, s) == SVG_OK);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
}
static void failure_sweeps(void)
{
    Ledger l = {0};
    svg_context_t *c = create(&l);
    Document d = fixture(2);
    /* Include both paint operations under nested opacity and preserve a sibling
     * scene across failures; release must leave its pins and allocations intact. */
    stroke(&d.nodes[2], 0xff2091de, 4);
    const double dash[] = {0, 8, 12, 4};
    d.nodes[2].style.dash = (svg_dash_t){dash, 4, 0};
    d.nodes[2].style.stroke_style.cap = OS64_GEOMETRY_ROUND_CAP;
    svg_scene_t *s = prepare(c, &d), *other = prepare(c, &d);
    uint32_t pixels[68 * 64];
    svg_render_target_t t = target(pixels);
    svg_render_options_t o = svg_render_options_default(os64_freetype_geometry_v1());
    size_t baseline = l.live, alloc_start = l.allocations, poll_start = l.polls;
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_OK);
    size_t allocations = l.allocations - alloc_start, polls = l.polls - poll_start;
    CHECK(l.live == baseline);
    for (size_t i = 1; i <= allocations; ++i)
        for (int admission = 0; admission < 2; ++admission) {
            l.admission = admission;
            l.fail = l.allocations + i;
            sentinel(pixels);
            CHECK(svg_scene_render(c, s, &o, &t, NULL) == (admission ? SVG_LIMIT : SVG_NO_MEMORY));
            CHECK(l.live == baseline && d.pins == 2);
            padding(pixels);
            l.fail = 0;
        }
    for (size_t i = 1; i <= polls; ++i) {
        l.cancel = l.polls + i;
        sentinel(pixels);
        CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_CANCELLED);
        CHECK(l.live == baseline && d.pins == 2);
        padding(pixels);
        l.cancel = 0;
    }
    CHECK(svg_scene_render(c, other, &o, &t, NULL) == SVG_OK);
    printf("Paint failure sweeps: %zu allocation positions (both refusal modes), %zu cancellation "
           "positions; peak %zu bytes\n",
           allocations, polls, l.peak);
    CHECK(svg_scene_release(c, other) == SVG_OK && svg_scene_release(c, s) == SVG_OK && !d.pins);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
}
static void clipping(void)
{
    Ledger l = {0};
    svg_context_t *c = create(&l);
    Document d = fixture(6);
    svg_scene_t *s = prepare(c, &d);
    svg_render_options_t o = svg_render_options_default(os64_freetype_geometry_v1());
    uint32_t whole[68 * 64], part[68 * 64];
    svg_render_target_t a = target(whole), b = target(part);
    CHECK(svg_scene_render(c, s, &o, &a, NULL) == SVG_OK);
    b.origin_x = 8;
    b.origin_y = 5;
    b.width = 24;
    b.height = 30;
    sentinel(part);
    CHECK(svg_scene_render(c, s, &o, &b, NULL) == SVG_OK);
    size_t changed = 0;
    for (size_t y = 0; y < 30; ++y)
        for (size_t x = 0; x < 24; ++x) {
            if (part[y * 68 + x] != whole[(y + 5) * 68 + x + 8]) {
                if (changed < 8)
                    fprintf(stderr, "crop (%zu,%zu) %08x vs %08x\n", x, y, part[y * 68 + x],
                            whole[(y + 5) * 68 + x + 8]);
                ++changed;
            }
        }
    CHECK(!changed);
    for (size_t y = 0; y < 64; ++y)
        for (size_t x = 0; x < 68; ++x)
            if (y >= 30 || x >= 24)
                CHECK(part[y * 68 + x] == 0xdeadbeef);
    b = target(part);
    b.origin_x = .25;
    b.origin_y = .75;
    o.viewport_to_device.tx = .25;
    o.viewport_to_device.ty = .75;
    b.capacity = (b.height - 1) * b.stride + b.width * 4;
    CHECK(svg_scene_render(c, s, &o, &b, NULL) == SVG_OK);
    for (size_t y = 0; y < 64; ++y)
        for (size_t x = 0; x < 64; ++x)
            CHECK(part[y * 68 + x] == whole[y * 68 + x]);
    o = svg_render_options_default(os64_freetype_geometry_v1());
    CHECK(svg_scene_release(c, s) == SVG_OK);
    d = fixture(1);
    d.nodes[0].style.transform.tx = 1000;
    s = prepare(c, &d);
    svg_render_stats_t stats;
    CHECK(svg_scene_render(c, s, &o, &a, &stats) == SVG_OK && stats.peak_layers == 0);
    CHECK(svg_scene_release(c, s) == SVG_OK);
    d = fixture(1);
    d.nodes[0].style.transform = (os64_geometry_map_t){0, 0, 0, 0, 32, 32};
    s = prepare(c, &d);
    CHECK(svg_scene_render(c, s, &o, &a, NULL) == SVG_OK);
    for (size_t y = 0; y < 64; ++y)
        for (size_t x = 0; x < 64; ++x)
            CHECK(whole[y * 68 + x] == 0);
    CHECK(svg_scene_release(c, s) == SVG_OK);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
}
static void dash_contracts(void)
{
    Ledger l = {0};
    svg_context_t *c = create(&l);
    Document d = {0};
    add(&d, SIZE_MAX, NULL, 0);
    add(&d, 0, "M4 16H60", 0);
    stroke(&d.nodes[1], 0xff2091de, 4);
    double dash[] = {8, 4};
    d.nodes[1].style.dash = (svg_dash_t){dash, 2, 0};
    svg_scene_t *s = prepare(c, &d);
    dash[0] = 60;
    dash[1] = 0;
    uint32_t pixels[68 * 64];
    svg_render_target_t t = target(pixels);
    os64_geometry_backend_t old = *os64_freetype_geometry_v1();
    old.struct_size = offsetof(os64_geometry_backend_t, stroke_with_caps);
    old.stroke_with_caps = NULL;
    svg_render_options_t o = svg_render_options_default(&old);
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_OK);
    CHECK(pixels[16 * 68 + 8] == 0xff2091de && !pixels[16 * 68 + 14]);
    CHECK(svg_scene_release(c, s) == SVG_OK);
    d.nodes[1].d = "M16 16h0";
    d.nodes[1].style.stroke_style.cap = OS64_GEOMETRY_ROUND_CAP;
    s = prepare(c, &d);
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_UNSUPPORTED);
    CHECK(svg_scene_release(c, s) == SVG_OK);
    d.nodes[1].d = "M4 16H60";
    d.nodes[1].style.stroke_style.cap = OS64_GEOMETRY_BUTT;
    dash[0] = dash[1] = .000001;
    s = prepare(c, &d);
    o = svg_render_options_default(os64_freetype_geometry_v1());
    size_t baseline = l.live;
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_LIMIT && l.live == baseline);
    CHECK(svg_scene_release(c, s) == SVG_OK);
    svg_provider_t provider = {&d, acquire, unpin, node, style, NULL, NULL, NULL};
    os64_geometry_map_t map = {1, 0, 0, 1, 0, 0};
    dash[0] = -1;
    CHECK(svg_scene_prepare(c, &provider, SVG_MODE_INLINE, &map, &s) == SVG_PROVIDER_ERROR && !s);
    dash[0] = NAN;
    CHECK(svg_scene_prepare(c, &provider, SVG_MODE_INLINE, &map, &s) == SVG_PROVIDER_ERROR && !s);
    d.nodes[1].style.dash.count = SVG_DASH_MAX + 1;
    CHECK(svg_scene_prepare(c, &provider, SVG_MODE_INLINE, &map, &s) == SVG_LIMIT && !s);
    CHECK(!d.pins && svg_destroy(c) == SVG_OK && !l.live);
}
static void dashed_circle(void)
{
    Ledger l = {0};
    svg_context_t *c = create(&l);
    Document d = {0};
    add(&d, SIZE_MAX, NULL, 0);
    add(&d, 0, "M56 32A24 24 0 0 1 32 56A24 24 0 0 1 8 32A24 24 0 0 1 32 8A24 24 0 0 1 56 32Z", 0);
    stroke(&d.nodes[1], 0xff2091de, 4);
    const double dash[] = {8, 4};
    d.nodes[1].style.dash = (svg_dash_t){dash, 2, 0};
    d.nodes[1].style.stroke_style.cap = OS64_GEOMETRY_ROUND_CAP;
    svg_scene_t *s = prepare(c, &d);
    uint32_t pixels[68 * 64];
    svg_render_target_t t = target(pixels);
    svg_render_options_t o = svg_render_options_default(os64_freetype_geometry_v1());
    CHECK(svg_scene_render(c, s, &o, &t, NULL) == SVG_OK);
    /* Independent circular annular sectors plus endpoint disks. No native
     * lowering or stroking routine participates in this area reference. */
    double total = 48 * acos(-1), cx[26], cy[26];
    size_t n = 0;
    for (double at = 0; at < total; at += 12) {
        cx[n] = 24 * cos(at / 24);
        cy[n++] = 24 * sin(at / 24);
        double end = fmin(at + 8, total);
        cx[n] = 24 * cos(end / 24);
        cy[n++] = 24 * sin(end / 24);
    }
    unsigned maximum = 0;
    double sum = 0;
    for (unsigned y = 0; y < 64; ++y)
        for (unsigned x = 0; x < 64; ++x) {
            unsigned hit = 0;
            for (unsigned j = 0; j < 32; ++j)
                for (unsigned i = 0; i < 32; ++i) {
                    double dx = x + (i + .5) / 32 - 32, dy = y + (j + .5) / 32 - 32;
                    double angle = atan2(dy, dx);
                    if (angle < 0)
                        angle += 2 * acos(-1);
                    bool inside = fmod(angle * 24, 12) <= 8 && fabs(hypot(dx, dy) - 24) <= 2;
                    for (size_t k = 0; !inside && k < n; ++k) {
                        double ax = dx - cx[k], ay = dy - cy[k];
                        inside = ax * ax + ay * ay <= 4;
                    }
                    hit += inside;
                }
            unsigned expected = (hit * 255 + 512) / 1024;
            unsigned delta = (unsigned)abs((int)(pixels[y * 68 + x] >> 24) - (int)expected);
            if (delta > maximum)
                maximum = delta;
            sum += delta / 255.0;
        }
    printf("Analytic dashed circle: alpha max %u/255, L1 %.6f\n", maximum, sum);
    CHECK(maximum <= 48 && sum <= 16);
    CHECK(svg_scene_release(c, s) == SVG_OK && svg_destroy(c) == SVG_OK && !l.live);
}
static void viewport_contracts(void)
{
    Ledger l = {0};
    svg_context_t *c = create(&l);
    Document d = {0};
    add(&d, SIZE_MAX, NULL, 0);
    add(&d, 0, NULL, 0);
    d.nodes[1].style.transform.tx = d.nodes[1].style.transform.ty = 16;
    d.nodes[1].style.viewport = (svg_viewport_t){true, true, 32, 24, {2, 0, 0, 2, 0, 0}};
    add(&d, 1, "M-100 -100H100V100H-100Z", 0xffff0000);
    add(&d, 1, NULL, 0);
    d.nodes[3].style.transform.tx = 6;
    d.nodes[3].style.transform.ty = 4;
    d.nodes[3].style.viewport = (svg_viewport_t){true, true, 10, 10, {1, 0, 0, 1, 0, 0}};
    add(&d, 3, "M-100 -100H100V100H-100Z", 0xff0000ff);
    add(&d, 0, "M2 2H10V10H2Z", 0xff008000);
    svg_scene_t *scene = prepare(c, &d);
    uint32_t pixels[68 * 64], crop[68 * 64];
    svg_render_target_t t = target(pixels);
    svg_render_options_t o = svg_render_options_default(os64_freetype_geometry_v1());
    size_t baseline = l.live, calls = l.allocations, polls = l.polls;
    sentinel(pixels);
    CHECK(svg_scene_render(c, scene, &o, &t, NULL) == SVG_OK);
    calls = l.allocations - calls;
    polls = l.polls - polls;
    CHECK(l.live == baseline);
    for (size_t y = 0; y < 64; ++y)
        for (size_t x = 0; x < 64; ++x) {
            uint32_t expected = x >= 16 && x < 48 && y >= 16 && y < 40
                                    ? (x >= 28 && y >= 24 ? 0xff0000ff : 0xffff0000)
                                    : 0;
            if (x >= 2 && x < 10 && y >= 2 && y < 10)
                expected = 0xff008000;
            CHECK(pixels[y * 68 + x] == expected);
        }
    padding(pixels);
    svg_render_target_t part = target(crop);
    part.origin_x = 17;
    part.origin_y = 19;
    part.width = 21;
    part.height = 18;
    sentinel(crop);
    CHECK(svg_scene_render(c, scene, &o, &part, NULL) == SVG_OK && l.live == baseline);
    for (size_t y = 0; y < 64; ++y)
        for (size_t x = 0; x < 68; ++x)
            CHECK(crop[y * 68 + x] ==
                  (y < 18 && x < 21 ? pixels[(y + 19) * 68 + x + 17] : 0xdeadbeef));
    for (size_t i = 1; i <= calls; ++i)
        for (int admission = 0; admission < 2; ++admission) {
            l.admission = admission;
            l.fail = l.allocations + i;
            CHECK(svg_scene_render(c, scene, &o, &t, NULL) ==
                  (admission ? SVG_LIMIT : SVG_NO_MEMORY));
            CHECK(l.live == baseline);
            l.fail = 0;
        }
    for (size_t i = 1; i <= polls; ++i) {
        l.cancel = l.polls + i;
        CHECK(svg_scene_render(c, scene, &o, &t, NULL) == SVG_CANCELLED && l.live == baseline);
        l.cancel = 0;
    }
    CHECK(svg_scene_release(c, scene) == SVG_OK);
    d.nodes[1].style.viewport.width = 0;
    scene = prepare(c, &d);
    CHECK(svg_scene_render(c, scene, &o, &t, NULL) == SVG_OK);
    CHECK(!pixels[20 * 68 + 20] && pixels[4 * 68 + 4] == 0xff008000);
    CHECK(svg_scene_release(c, scene) == SVG_OK);
    svg_provider_t provider = {&d, acquire, unpin, node, style, NULL, NULL, NULL};
    os64_geometry_map_t map = {1, 0, 0, 1, 0, 0};
    d.nodes[1].style.viewport.width = -1;
    CHECK(svg_scene_prepare(c, &provider, SVG_MODE_INLINE, &map, &scene) == SVG_PROVIDER_ERROR &&
          !scene);
    d.nodes[1].style.viewport.width = 32;
    d.nodes[1].style.viewport.content_transform.tx = INFINITY;
    CHECK(svg_scene_prepare(c, &provider, SVG_MODE_INLINE, &map, &scene) == SVG_PROVIDER_ERROR &&
          !scene);
    CHECK(!d.pins && svg_destroy(c) == SVG_OK && !l.live);
    printf("Viewport masks: %zu allocation cuts in both modes, %zu cancellation cuts, exact crop "
           "and bounds pass\n",
           calls, polls);
}

static void clip_contracts(bool object_bbox)
{
    Ledger l = {0};
    svg_context_t *c = create(&l);
    Document d = {0};
    add(&d, SIZE_MAX, NULL, 0);
    add(&d, 0, NULL, 0);
    const char *window = object_bbox ? "M.125 .125H.875V.875H.125ZM.375 .375H.625V.625H.375Z"
                                     : "M8 8H56V56H8ZM24 24H40V40H24Z";
    d.nodes[1].clip = (svg_clip_source_t){.active = true,
                                          .even_odd = true,
                                          .transform = {1, 0, 0, 1, 0, 0},
                                          .kind = SVG_NODE_PATH,
                                          .d = {window, strlen(window)}};
    add(&d, 1, "M0 0H64V64H0Z", 0xffff0000);
    add(&d, 1, "M0 0H64V64H0Z", 0xff0000ff);
    d.nodes[3].clip =
        (svg_clip_source_t){.active = true,
                            .transform = {1, 0, 0, 1, 0, 0},
                            .kind = SVG_NODE_SHAPE,
                            .shape = {.kind = SVG_SHAPE_RECT, .width = 32, .height = 64}};
    if (object_bbox) {
        d.nodes[1].clip.object_bbox = true;
        d.nodes[1].clip.outer_transform = (os64_geometry_map_t){1,0,0,1,0,0};
        d.nodes[3].clip.outer_transform = (os64_geometry_map_t){1,0,0,1,0,0};
        d.nodes[3].clip.object_bbox = true;
        d.nodes[3].clip.shape.width = .5;
        d.nodes[3].clip.shape.height = 1;
    }
    size_t baseline = l.live, before = l.allocations;
    svg_scene_t *scene = prepare(c, &d);
    size_t prep_cuts = l.allocations - before;
    CHECK(svg_scene_release(c, scene) == SVG_OK);
    svg_provider_t provider = {&d, acquire, unpin, node, style, NULL, NULL, clip_source};
    os64_geometry_map_t map = {1, 0, 0, 1, 0, 0};
    for (size_t i = 1; i <= prep_cuts; ++i)
        for (int admission = 0; admission < 2; ++admission) {
            l.admission = admission;
            l.fail = l.allocations + i;
            CHECK(svg_scene_prepare(c, &provider, SVG_MODE_INLINE, &map, &scene) ==
                  (admission ? SVG_LIMIT : SVG_NO_MEMORY));
            CHECK(!scene && !d.pins && l.live == baseline);
            l.fail = 0;
        }
    scene = prepare(c, &d);
    baseline = l.live;
    uint32_t pixels[68 * 64], crop[68 * 64];
    svg_render_target_t t = target(pixels);
    svg_render_options_t o = svg_render_options_default(os64_freetype_geometry_v1());
    before = l.allocations;
    size_t polls = l.polls;
    sentinel(pixels);
    CHECK(svg_scene_render(c, scene, &o, &t, NULL) == SVG_OK);
    size_t cuts = l.allocations - before;
    polls = l.polls - polls;
    for (size_t y = 0; y < 64; ++y)
        for (size_t x = 0; x < 64; ++x) {
            bool visible =
                x >= 8 && x < 56 && y >= 8 && y < 56 && !(x >= 24 && x < 40 && y >= 24 && y < 40);
            CHECK(pixels[y * 68 + x] == (visible ? (x < 32 ? 0xff0000ff : 0xffff0000) : 0));
        }
    padding(pixels);
    svg_render_target_t part = target(crop);
    part.origin_x = 13;
    part.origin_y = 19;
    part.width = 21;
    part.height = 18;
    sentinel(crop);
    CHECK(svg_scene_render(c, scene, &o, &part, NULL) == SVG_OK);
    for (size_t y = 0; y < 64; ++y)
        for (size_t x = 0; x < 68; ++x)
            CHECK(crop[y * 68 + x] ==
                  (y < 18 && x < 21 ? pixels[(y + 19) * 68 + x + 13] : 0xdeadbeef));
    for (size_t i = 1; i <= cuts; ++i)
        for (int admission = 0; admission < 2; ++admission) {
            l.admission = admission;
            l.fail = l.allocations + i;
            CHECK(svg_scene_render(c, scene, &o, &t, NULL) ==
                  (admission ? SVG_LIMIT : SVG_NO_MEMORY));
            CHECK(l.live == baseline);
            l.fail = 0;
        }
    for (size_t i = 1; i <= polls; ++i) {
        l.cancel = l.polls + i;
        CHECK(svg_scene_render(c, scene, &o, &t, NULL) == SVG_CANCELLED);
        CHECK(l.live == baseline);
        l.cancel = 0;
    }
    CHECK(svg_scene_release(c, scene) == SVG_OK);
    d.nodes[1].clip.transform.tx = INFINITY;
    CHECK(svg_scene_prepare(c, &provider, SVG_MODE_INLINE, &map, &scene) == SVG_PROVIDER_ERROR &&
          !scene);
    CHECK(!d.pins && svg_destroy(c) == SVG_OK && !l.live);
    printf("Clip silhouettes: %zu preparation and %zu render allocation cuts in both modes, %zu "
           "cancellation cuts, exact crop pass\n",
           prep_cuts, cuts, polls);
}

static void object_bounds_contracts(void)
{
    Ledger l = {0};
    svg_context_t *c = create(&l);
    Document d = {0};
    add(&d, SIZE_MAX, NULL, 0);
    d.nodes[0].style.transform = (os64_geometry_map_t){3,0,0,3,100,100};
    add(&d, 0, NULL, 0);
    d.nodes[1].style.transform.tx = 10;
    d.nodes[1].style.viewport = (svg_viewport_t){true,true,20,20,{2,0,0,3,7,11}};
    add(&d, 1, "M0 0Q10 20 20 0Z", 0xff0000ff);
    d.nodes[2].style.transform = (os64_geometry_map_t){1,0,1,1,0,0};
    d.nodes[2].style.visible = false;
    d.nodes[2].style.opacity = 0;
    add(&d, 0, "M200 200L300 300", 0);
    d.nodes[3].style.display = false;
    add(&d, 0, "M90 90L100 100", 0);
    d.nodes[4].style.fill.kind = SVG_PAINT_NONE;
    svg_scene_t *scene = prepare(c, &d);
    size_t allocations = l.allocations, polls = l.polls;
    svg_bounds_t b;
    CHECK(svg_scene_object_bounds(c, scene, 0, &b) == SVG_OK && !b.empty);
    CHECK(b.min_x == 17 && b.min_y == 11 && b.max_x == 100 && b.max_y == 100);
    polls = l.polls - polls;
    CHECK(svg_scene_object_bounds(c, scene, 1, &b) == SVG_OK && !b.empty);
    CHECK(b.min_x == 0 && b.min_y == 0 && b.max_x == 22.5 && b.max_y == 10);
    CHECK(svg_scene_object_bounds(c, scene, 2, &b) == SVG_OK && !b.empty);
    CHECK(b.min_x == 0 && b.min_y == 0 && b.max_x == 20 && b.max_y == 10);
    CHECK(l.allocations == allocations);
    for (size_t i = 1; i <= polls; ++i) {
        l.cancel = l.polls + i;
        CHECK(svg_scene_object_bounds(c, scene, 0, &b) == SVG_CANCELLED && b.empty);
        l.cancel = 0;
    }
    CHECK(svg_scene_object_bounds(c, scene, 99, &b) == SVG_BAD_ARGUMENT && b.empty);
    CHECK(svg_scene_release(c, scene) == SVG_OK);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
}

#include "test_svg_gradient_sampler.inc"
#include "test_svg_radial_sampler.inc"
static void gradient_scene_contracts(bool radial)
{
    gradient_sampler_contracts();
    Ledger l = {0};
    svg_context_t *c = create(&l);
    size_t baseline = l.live;
    Document d = {0};
    add(&d, SIZE_MAX, NULL, 0);
    add(&d, 0, "M8 8H56V56H8Z", 0);
    svg_gradient_stop_t stops[] = {{0, 0xffff0000}, {1, 0xff0000ff}};
    os64_geometry_map_t map = {1, 0, 0, 1, 0, 0};
    svg_linear_gradient_t g = {
        .x2 = 1, .object_bbox = true, .transform = map, .stops = stops, .count = 2};
    d.nodes[1].style.fill = (svg_paint_t){.kind = SVG_PAINT_LINEAR, .linear = &g};
    svg_radial_gradient_t rg = {.cx = .5,
                                .cy = .5,
                                .fx = .5,
                                .fy = .5,
                                .r = .5,
                                .object_bbox = true,
                                .transform = map,
                                .stops = stops,
                                .count = 2};
    if (radial)
        d.nodes[1].style.fill = (svg_paint_t){.kind = SVG_PAINT_RADIAL, .radial = &rg};
    d.nodes[1].style.stroke = d.nodes[1].style.fill;
    d.nodes[1].style.stroke_style.width = 4;
    svg_provider_t provider = {&d, acquire, unpin, node, style, NULL, NULL, clip_source};
    svg_scene_t *scene = NULL;
    size_t allocations = l.allocations, polls = l.polls;
    CHECK(svg_scene_prepare(c, &provider, SVG_MODE_INLINE, &map, &scene) == SVG_OK);
    size_t prepare_allocations = l.allocations - allocations, prepare_polls = l.polls - polls;
    uint32_t pixels[68 * 64];
    sentinel(pixels);
    svg_render_target_t t = target(pixels);
    svg_render_options_t options = svg_render_options_default(os64_freetype_geometry_v1());
    // Provider storage may be reused as soon as preparation returns.
    stops[0].offset = NAN;
    stops[0].argb = 0;
    g.count = 999;
    rg.count = 999;
    allocations = l.allocations;
    polls = l.polls;
    CHECK(svg_scene_render(c, scene, &options, &t, NULL) == SVG_OK);
    size_t render_allocations = l.allocations - allocations, render_polls = l.polls - polls;
    if (radial) {
        CHECK(pixels[32 * 68 + 8] == pixels[32 * 68 + 55]);
        CHECK((pixels[32 * 68 + 32] & 0xff0000) > 0xf00000);
    } else
        CHECK(pixels[32 * 68 + 8] == 0xfffc0003 && pixels[32 * 68 + 55] == 0xff0300fc);
    padding(pixels);
    size_t kept = l.live;
    for (unsigned mode = 0; mode < 2; ++mode)
        for (size_t i = 1; i <= render_allocations; ++i) {
            l.admission = mode;
            l.fail = l.allocations + i;
            CHECK(svg_scene_render(c, scene, &options, &t, NULL) ==
                  (mode ? SVG_LIMIT : SVG_NO_MEMORY));
            l.fail = 0;
            CHECK(l.live == kept);
        }
    for (size_t i = 1; i <= render_polls; ++i) {
        l.cancel = l.polls + i;
        CHECK(svg_scene_render(c, scene, &options, &t, NULL) == SVG_CANCELLED);
        l.cancel = 0;
        CHECK(l.live == kept);
    }
    CHECK(svg_scene_release(c, scene) == SVG_OK && l.live == baseline && !d.pins);
    stops[0] = (svg_gradient_stop_t){0, 0xffff0000};
    g.count = 2;
    rg.count = 2;
    for (unsigned mode = 0; mode < 2; ++mode)
        for (size_t i = 1; i <= prepare_allocations; ++i) {
            scene = NULL;
            l.admission = mode;
            l.fail = l.allocations + i;
            CHECK(svg_scene_prepare(c, &provider, SVG_MODE_INLINE, &map, &scene) ==
                  (mode ? SVG_LIMIT : SVG_NO_MEMORY));
            l.fail = 0;
            CHECK(!scene && !d.pins && l.live == baseline);
        }
    for (size_t i = 1; i <= prepare_polls; ++i) {
        scene = NULL;
        l.cancel = l.polls + i;
        CHECK(svg_scene_prepare(c, &provider, SVG_MODE_INLINE, &map, &scene) == SVG_CANCELLED);
        l.cancel = 0;
        CHECK(!scene && !d.pins && l.live == baseline);
    }
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
    printf("SVG gradient ownership: %zu preparation/%zu render allocation cuts, %zu/%zu "
           "cancellation cuts pass\n",
           prepare_allocations, render_allocations, prepare_polls, render_polls);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    setvbuf(stdout, NULL, _IONBF, 0);
    radial_sampler_contracts();
    gradient_scene_contracts(false);
    gradient_scene_contracts(true);
    clip_contracts(false);
    clip_contracts(true);
    object_bounds_contracts();
    viewport_contracts();
    pictures(argv[1]);
    limits();
    clipping();
    dash_contracts();
    dashed_circle();
    failure_sweeps();
    printf("SVG paint: %zu assertions passed\n", checks);
    return 0;
}
