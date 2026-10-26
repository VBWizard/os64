#define main existing_yonder_suite
#include "test_yonder_host.c"
#undef main
#include "svg.h"
#include <assert.h>

typedef struct {
    size_t live, peak, calls, fail, cap;
    bool admission;
} SvgLedger;
static os64_geometry_status_t svg_alloc_test(void *ctx, size_t bytes, void **out)
{
    SvgLedger *l = ctx;
    *out = NULL;
    if (++l->calls == l->fail)
        return l->admission ? OS64_GEOMETRY_LIMIT : OS64_GEOMETRY_NO_MEMORY;
    if (bytes > l->cap - l->live)
        return OS64_GEOMETRY_LIMIT;
    *out = malloc(bytes);
    if (!*out)
        return OS64_GEOMETRY_NO_MEMORY;
    l->live += bytes;
    if (l->peak < l->live)
        l->peak = l->live;
    return OS64_GEOMETRY_OK;
}
static void svg_free_test(void *ctx, void *p, size_t bytes)
{
    SvgLedger *l = ctx;
    assert(bytes <= l->live);
    l->live -= bytes;
    free(p);
}
typedef struct {
    os64_html_document_t *doc;
    os64_page_t *page;
    garb_cascade_t *cascade;
    garb_parsed_t sheets[8];
    int n;
    flow_tree_t *tree;
    flow_env_t env;
    os64_html_node_t *root;
    const flow_box_t *box;
} Fixture;
static os64_html_node_t *named(os64_html_node_t *n, const char *id)
{
    for (; n; n = n->next) {
        const os64_html_attr_t *a = os64_html_attr(n, "id");
        if (a && !strcmp(a->value, id))
            return n;
        os64_html_node_t *found = named(n->first_child, id);
        if (found)
            return found;
    }
    return NULL;
}
static bool svg_size_test(void *ctx, const os64_html_node_t *n, int32_t *w, int32_t *h)
{
    (void)ctx;
    return yonder_svg_size(n, w, h);
}
static void layout_fixture(Fixture *f, int width, int zoom, bool enabled)
{
    if (f->tree)
        flow_free(f->tree);
    if (f->cascade)
        garb_cascade_free(f->cascade);
    garb_sheet_in_t in[8];
    for (int i = 0; i < f->n; ++i)
        in[i] = (garb_sheet_in_t){.sheet = &f->sheets[i]};
    f->cascade = garb_cascade(in, f->n, f->doc,
                              (garb_env_t){.width = width, .height = 256, .modern_off = !enabled});
    assert(f->cascade && !garb_cascade_incomplete(f->cascade));
    f->env = kEnv;
    f->env.cascade = f->cascade;
    f->env.svg = enabled;
    f->env.zoom = zoom;
    f->env.viewport_height = 256;
    f->env.replaced_size = svg_size_test;
    f->tree = flow_layout(f->doc, f->page, width, &f->env);
    assert(f->tree);
    f->box = flow_box_for(f->tree, f->root);
}
static Fixture fixture_make(const char *html)
{
    Fixture f = {0};
    f.doc = parse(html, strlen(html));
    assert(f.doc);
    f.page = os64_page_build(f.doc, kPage, NULL, NULL);
    assert(f.page);
    for (int32_t i = 0; i < os64_page_nsheets(f.page); ++i) {
        const os64_page_sheet_t *sh = os64_page_sheet(f.page, i);
        assert(!sh->linked && f.n < 8);
        assert(garb_parse_style_element(sh->node, &f.sheets[f.n++]) == GARB_OK);
    }
    f.root = named(f.doc->document, "art");
    assert(f.root);
    layout_fixture(&f, 256, 1000, true);
    assert(f.box && f.box->replaced);
    return f;
}
static void fixture_free(Fixture *f)
{
    flow_free(f->tree);
    garb_cascade_free(f->cascade);
    for (int i = 0; i < f->n; ++i)
        garb_free(&f->sheets[i]);
    os64_page_free(f->page);
    os64_html_document_free(f->doc);
}
static svg_status_t draw_fixture(Fixture *f, SvgLedger *l, yonder_svg_pixels_t *pixels,
                                 os64_gui_rect_t clip)
{
    os64_geometry_memory_t m = {l, svg_alloc_test, svg_free_test};
    return yonder_svg_render(f->doc, f->cascade, f->box, f->box->rect, clip, (uint32_t)f->env.zoom,
                             f->env.ink ^ 0xff000000u, &m, os64_freetype_geometry_v1(), pixels);
}
static void pixels_free(SvgLedger *l, yonder_svg_pixels_t *p)
{
    os64_geometry_memory_t m = {l, svg_alloc_test, svg_free_test};
    yonder_svg_pixels_free(&m, p);
    assert(!l->live);
}
static uint32_t pixel(yonder_svg_pixels_t *p, int x, int y)
{
    assert(p->pixels && x >= 0 && y >= 0 && x < p->rect.w && y < p->rect.h);
    return p->pixels[y * p->rect.w + x];
}
static void save_ppm(const char *dir, const char *name, yonder_svg_pixels_t *p)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    assert(f);
    fprintf(f, "P6\n%d %d\n255\n", p->rect.w, p->rect.h);
    for (int i = 0; i < p->rect.w * p->rect.h; ++i) {
        uint32_t v = p->pixels[i], a = v >> 24;
        for (int shift = 16; shift >= 0; shift -= 8)
            fputc((((v >> shift) & 255) * a + 255 * (255 - a) + 127) / 255, f);
    }
    assert(!fclose(f));
}
static void svg_dimension_cases(void)
{
    static const struct {
        const char *attrs;
        int w, h;
    } cases[] = {
        {"width='100%' viewBox='0 0 400 100'", 300, 75},
        {"height='100%' viewBox='0 0 100 200'", 75, 150},
        {"width='12bogus' viewBox='0 0 400 100'", 300, 75},
        {"height='99bogus' viewBox='0 0 100 200'", 75, 150},
        {"width='-5' height='-9'", 300, 150},
        {"width='320px' viewBox='0 0 400 100'", 320, 80},
        {"width='100%' height='40' viewBox='0 0 400 100'", 160, 40},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char html[512];
        snprintf(html, sizeof(html), "<!doctype html><body style='margin:0'><svg id=art %s></svg>",
                 cases[i].attrs);
        Fixture f = fixture_make(html);
        int32_t w = 0, h = 0;
        assert(yonder_svg_size(f.root, &w, &h) && w == cases[i].w && h == cases[i].h);
        if (i == 0)
            assert(f.box->rect.w == 256 && f.box->rect.h == 64);
        fixture_free(&f);
    }
}
static void svg_cases(const char *dir)
{
    const char *html =
        "<!doctype html><style>body{margin:0}svg{display:block} .ink{fill:currentColor}"
        "#override{fill:#2091de} #css-transform{transform:translate(8px,0px)}</style>"
        "<div style='color:#e87925'><svg id=art width=64 height=64 viewBox='0 0 64 64'>"
        "<g opacity='.5' fill='#2091de'><path d='M4 4H28V28H4Z'/><path d='M16 16H40V40H16Z'/></g>"
        "<path id=override fill=red d='M44 4H60V20H44Z'/>"
        "<g fill=currentColor><path id=child style='color:#00ff00' d='M4 44H20V60H4Z'/></g>"
        "<path id=css-transform transform='translate(40 0)' class=ink d='M20 44H28V60H20Z'/>"
        "<defs><rect width=100 height=100/></defs><g display=none><rect/></g></svg></div>";
    Fixture f = fixture_make(html);
    SvgLedger l = {.cap = 64 << 20};
    yonder_svg_pixels_t p;
    assert(f.box->rect.w == 64 && f.box->rect.h == 64);
    assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_OK);
    assert(pixel(&p, 8, 8) == 0x802091de && pixel(&p, 20, 20) == 0x802091de);
    assert(pixel(&p, 50, 10) == 0xff2091de && pixel(&p, 8, 50) == 0xff00ff00);
    assert(pixel(&p, 30, 50) == 0xffe87925 && pixel(&p, 60, 50) == 0);
    save_ppm(dir, "connection", &p);
    pixels_free(&l, &p);
    Rec rec = {{0}, {0, 0, 256, 256}, false, f.page};
    yonder_verbs_t verbs = verbs_of(&rec);
    yonder_paint(f.tree, rec.view, (flow_point_t){0}, 0xffffffff, NULL, NULL, NULL, &verbs);
    assert(rec.out.v && strstr(rec.out.v, "image"));
    free(rec.out.v);
    // Change the real document, rebuild its cascade/layout, then render again.
    os64_html_node_t *child = named(f.doc->document, "child");
    assert(os64_html_set_attr(f.doc, child, "style", "color:#ff0000", 13) == 0);
    layout_fixture(&f, 256, 2000, true);
    assert(f.box->rect.w == 128 && f.box->rect.h == 128);
    assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_OK);
    assert(pixel(&p, 16, 100) == 0xffff0000);
    pixels_free(&l, &p);
    layout_fixture(&f, 256, 1000, true);
    // Crop is in page coordinates; placement preserves the full viewBox map.
    os64_gui_rect_t crop = f.box->rect;
    crop.x += 16;
    crop.y += 16;
    crop.w = crop.h = 16;
    assert(draw_fixture(&f, &l, &p, crop) == SVG_OK);
    assert(pixel(&p, 4, 4) == 0x802091de);
    pixels_free(&l, &p);
    size_t start = l.calls;
    assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_OK);
    size_t count = l.calls - start;
    pixels_free(&l, &p);
    for (size_t i = 1; i <= count; ++i)
        for (int admission = 0; admission < 2; ++admission) {
            l.fail = l.calls + i;
            l.admission = admission;
            assert(draw_fixture(&f, &l, &p, f.box->rect) ==
                   (admission ? SVG_LIMIT : SVG_NO_MEMORY));
            assert(!p.pixels && !l.live);
            l.fail = 0;
        }
    assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_OK);
    pixels_free(&l, &p);
    printf("SVG connection: %zu allocation cuts in both refusal modes; peak %zu bytes\n", count,
           l.peak);
    layout_fixture(&f, 256, 1000, false);
    assert(!f.box || !f.box->replaced);
    fixture_free(&f);
    f = fixture_make(
        "<!doctype html><body style='margin:0'><svg id=art width=128 height=64 viewBox='0 0 64 "
        "64'><style>path{fill:#2091de}</style><path d='M0 0H64V64H0Z'/></svg>");
    assert(f.n == 1 && draw_fixture(&f, &l, &p, f.box->rect) == SVG_OK);
    assert(pixel(&p, 10, 20) == 0 && pixel(&p, 40, 20) == 0xff2091de && pixel(&p, 110, 20) == 0);
    save_ppm(dir, "viewbox-meet", &p);
    pixels_free(&l, &p);
    fixture_free(&f);
    // Presentation attributes have zero author specificity; a universal rule
    // wins, and a var() substitution still participates in the real cascade.
    f = fixture_make("<!doctype html><style>*{fill:var(--ink,#2091de)}</style>"
                     "<svg id=art width=' 64 ' height=64><path fill=red d='M0 0H64V64H0Z'/></svg>");
    assert(f.box->rect.w == 64);
    assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_OK);
    assert(pixel(&p, 16, 16) == 0xff2091de);
    pixels_free(&l, &p);
    fixture_free(&f);
    f = fixture_make("<!doctype html><svg id=art width=64 height=64>"
                     "<path fill=currentColor d='M0 0H64V64H0Z'/></svg>");
    assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_OK);
    assert(pixel(&p, 16, 16) == (f.box->style->color ^ 0xff000000u));
    pixels_free(&l, &p);
    fixture_free(&f);
    f = fixture_make("<!doctype html><svg id=art width=64 height=64 style='color:red'>"
                     "<path style='color:initial' fill=currentColor d='M0 0H64V64H0Z'/></svg>");
    assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_OK);
    assert(pixel(&p, 16, 16) == (f.env.ink ^ 0xff000000u));
    pixels_free(&l, &p);
    fixture_free(&f);
    const char *align[] = {" xMaxYMin meet  ", "none", "xMidYMid slice"};
    for (int i = 0; i < 3; ++i) {
        char page[512];
        snprintf(page, sizeof(page),
                 "<!doctype html><svg id=art width=128 height=64 "
                 "viewBox='10 20 64 64' preserveAspectRatio='%s'>"
                 "<path fill=red d='M10 20H74V84H10Z'/></svg>",
                 align[i]);
        f = fixture_make(page);
        assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_OK);
        assert(pixel(&p, 100, 20) == 0xffff0000);
        assert(pixel(&p, 10, 20) == (i ? 0xffff0000 : 0));
        pixels_free(&l, &p);
        fixture_free(&f);
    }
    f = fixture_make("<!doctype html><svg id=art width=64 height=64 "
                     "transform='translate(10 0)'><path fill=red d='M0 0H8V8H0Z'/></svg>");
    layout_fixture(&f, 256, 2000, true);
    assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_OK);
    assert(pixel(&p, 24, 4) == 0xffff0000 && pixel(&p, 12, 4) == 0);
    pixels_free(&l, &p);
    fixture_free(&f);
    f = fixture_make("<!doctype html><svg id=art width=64 height=64 style='opacity:calc(.5)'>"
                     "<path style='opacity:inherit' fill=red d='M0 0H32V32H0Z'/></svg>");
    assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_UNSUPPORTED && !p.pixels && !l.live);
    fixture_free(&f);
    const char *bad[] = {"<rect width=20 height=20/>",
                         "<path style='transform:translateX(20px)' d='M0 0H20V20Z'/>",
                         "<path fill='url(#gradient)' d='M0 0H20V20Z'/>",
                         "<path fill=none stroke=red stroke-dasharray='2 2' d='M0 0L20 20'/>"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        char page[1024];
        snprintf(page, sizeof(page), "<!doctype html><svg id=art width=64 height=64>%s</svg>",
                 bad[i]);
        f = fixture_make(page);
        assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_UNSUPPORTED && !p.pixels && !l.live);
        fixture_free(&f);
    }
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    setvbuf(stdout, NULL, _IONBF, 0);
    os64_text_options_t o = {.memory = {NULL, text_alloc_cb, text_free_cb},
                             .backend = flow_test_backend()};
    assert(os64_text_create(&o, &s_text) == OS64_FONT_OK);
    kEnv.text = s_text;
    size_t base = live;
    svg_dimension_cases();
    svg_cases(argv[1]);
    for (int i = 0; i < s_nfonts; ++i)
        os64_text_font_release(s_fonts[i].font);
    assert(os64_text_destroy(s_text) == OS64_FONT_OK && live == base);
    puts("SVG Yonder connection: HTML, cascade, layout, paint dispatch, pixels, mutation and "
         "ownership pass");
    return 0;
}
