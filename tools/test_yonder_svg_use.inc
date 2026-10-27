/* Included by the real adapter harness, sharing its document and pixel helpers. */
static void use_cases(const char *dir)
{
    static const struct {
        const char *actual, *reference;
    } cases[] = {
        {"<defs fill=red><rect id=r width=24 height='24'/></defs><use href='#r' fill=blue x=8 "
         "y='8'/>",
         "<rect x=8 y=8 width=24 height=24 fill='blue'/>"},
        {"<defs><path id=r d='M0 0H24V24H0Z'/></defs><use href='#r' fill=red x=8 y='8'/><use "
         "href='#r' fill=blue x=48 y='8'/>",
         "<path d='M8 8H32V32H8Z' fill='red'/><path d='M48 8H72V32H48Z' fill='blue'/>"},
        {"<defs><rect id=r width=24 height=24 fill='red'/></defs><use href='#r' fill=blue x=8 "
         "y='8'/>",
         "<rect x=8 y=8 width=24 height=24 fill='red'/>"},
        {"<defs color=red><g id=r fill=currentColor><rect width=24 height='24'/><rect x=30 "
         "width=24 height=24 color='green'/></g></defs><use href='#r' color=blue x=8 y='8'/>",
         "<g transform='translate(8 8)'><rect width=24 height=24 fill='blue'/><rect x=30 width=24 "
         "height=24 fill='green'/></g>"},
        {"<defs><rect id=r width=16 height='16'/><use id=s href='#r' x=8 y='8'/></defs><use "
         "href='#s' x=16 y=16 fill='blue'/>",
         "<rect x=24 y=24 width=16 height=16 fill='blue'/>"},
        {"<defs><rect id=r width=16 height='16'/></defs><use href='#r' x=8 y=12 transform='scale(2 "
         "1)'/>",
         "<rect x=16 y=12 width=32 height='16'/>"},
        {"<defs><rect id=r width=16 height='16'/></defs><use href='#r' x=8 y=12 "
         "transform='scale(4)' style='transform:scale(2,1)'/>",
         "<rect x=16 y=12 width=32 height='16'/>"},
        {"<defs><rect id=r width=16 height='16'/></defs><use href='#r' x=8 y=8 width=100 "
         "height='100'/>",
         "<rect x=8 y=8 width=16 height='16'/>"},
        {"<defs><rect id=r width=16 height='16'/><rect width=128 height=128 "
         "fill='red'/></defs><use href='#r' x=8 y=8><rect width=128 height=128 "
         "fill='green'/></use>",
         "<rect x=8 y=8 width=16 height='16'/>"},
        {"<defs><rect id=r width=16 height='16'/></defs><use xlink:href='#r' x=8 y='8'/>",
         "<rect x=8 y=8 width=16 height='16'/>"},
        {"<defs><rect id=r width=128 height='128'/></defs><use href='' xlink:href='#r'/><use "
         "href='#missing'/><use href='#'/><rect x=8 y=8 width=16 height=16 fill='blue'/>",
         "<rect x=8 y=8 width=16 height=16 fill='blue'/>"},
        {"<defs><rect id='some:id' width=16 height='16'/></defs><use href='#some%3Aid' x=8 y='8'/>",
         "<rect x=8 y=8 width=16 height='16'/>"},
        {"<defs><use id=a href='#b'/><use id=b href='#a'/></defs><use href='#a'/><use id=self "
         "href='#self'/><rect x=8 y=8 width=16 height='16'/>",
         "<rect x=8 y=8 width=16 height='16'/>"},
        {"<g id=r><rect x=8 y=8 width=16 height='16'/><use href='#r'/></g>",
         "<rect x=8 y=8 width=16 height='16'/>"},
        {"<defs><rect id=r width=24 height=24 visibility='visible'/></defs><use href='#r' "
         "visibility=hidden x=8 y='8'/>",
         "<rect x=8 y=8 width=24 height='24'/>"},
        {"<defs><rect id=r width=24 height=24 display='none'/></defs><use href='#r'/><rect x=8 y=8 "
         "width=16 height='16'/>",
         "<rect x=8 y=8 width=16 height='16'/>"},
        {"<style>defs rect {fill:red} use rect {fill:green}</style><defs><rect id=r width=24 "
         "height='24'/></defs><use href='#r' x=8 y=8 fill='blue'/>",
         "<rect x=8 y=8 width=24 height=24 fill='red'/>"},
        {"<defs><g id=r><rect width=24 height='24'/><rect x=8 y=8 width=24 "
         "height='24'/></g></defs><use href='#r' x=8 y=8 opacity='.5' fill='red'/>",
         "<g opacity='.5' fill=red transform='translate(8 8)'><rect width=24 height='24'/><rect "
         "x=8 y=8 width=24 height='24'/></g>"},
        {"<defs style='x:60px;y:60px'><rect id=r style='x:inherit;y:inherit' width=16 "
         "height='16'/></defs><use href='#r' x=8 y='12'/>",
         "<rect x=16 y=24 width=16 height='16'/>"},
        {"<defs><rect id=r width=16 height='16'/></defs><use href='#r' x='25%' y='12.5%'/>",
         "<rect x=32 y=16 width=16 height='16'/>"},
        {"<defs><rect id=r width=16 height='16'/></defs><use href='#r' "
         "style='--ink:blue;fill:var(--ink)' x=8 y='8'/>",
         "<rect x=8 y=8 width=16 height=16 fill='blue'/>"},
        {"<defs><path id=r d='M8 24H88' fill='none'/></defs><use href='#r' stroke=blue "
         "stroke-width=4 stroke-dasharray='8 8'/>",
         "<path d='M8 24H88' fill=none stroke=blue stroke-width=4 stroke-dasharray='8 8'/>"},
        {"<defs><rect id=r width=16 height=16 fill='inherit'/><rect id=r width=128 "
         "height='128'/></defs><use href='#r' fill=blue x=8 y='8'/>",
         "<rect x=8 y=8 width=16 height=16 fill='blue'/>"},
        {"<defs><g id=r opacity='.5'><rect width=24 height='24'/><rect x=8 y=8 width=24 "
         "height='24'/></g></defs><use href='#r' x=8 y=8 opacity='.5' fill='blue'/>",
         "<g transform='translate(8 8)' opacity='.5'><g opacity='.5' fill=blue><rect width=24 "
         "height='24'/><rect x=8 y=8 width=24 height='24'/></g></g>"},
        {"<defs><g id=a><g id=b><use href='#c'/></g></g><use id=c href='#a'/></defs>"
         "<use href='#b'/><rect x=8 y=8 width=16 height=16 fill='blue'/>",
         "<rect x=8 y=8 width=16 height=16 fill='blue'/>"},
        {"<defs><g id=a><rect x=40 width=16 height=16 fill='red'/>"
         "<g id=b><rect x=20 width=16 height=16 fill='orange'/><use href='#c'/>"
         "<rect x=60 width=16 height=16 fill='green'/></g>"
         "<rect x=80 width=16 height=16 fill='purple'/></g><use id=c href='#a'/></defs>"
         "<use href='#b'/><rect y=32 width=16 height=16 fill='blue'/>",
         "<rect x=20 width=16 height=16 fill='orange'/><rect x=40 width=16 height=16 fill='red'/>"
         "<rect x=60 width=16 height=16 fill='green'/><rect x=80 width=16 height=16 fill='purple'/>"
         "<rect y=32 width=16 height=16 fill='blue'/>"},
    };
    SvgLedger l = {.cap = 64 << 20};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        yonder_svg_pixels_t images[2];
        Fixture fixtures[2];
        for (int j = 0; j < 2; ++j) {
            char html[4096], filename[1024];
            snprintf(html, sizeof(html),
                     "<!doctype html><body style='margin:0'>"
                     "<svg id=art width=128 height=128>%s</svg>",
                     j ? cases[i].reference : cases[i].actual);
            snprintf(filename, sizeof(filename), "%s/use-%02zu-%s.html", dir, i,
                     j ? "reference" : "actual");
            FILE *file = fopen(filename, "wb");
            assert(file && fputs(html, file) >= 0 && !fclose(file));
            fixtures[j] = fixture_make(html);
            svg_status_t status = draw_fixture(&fixtures[j], &l, &images[j], fixtures[j].box->rect);
            if (status != SVG_OK)
                fprintf(stderr, "use %zu side %d status %d\n", i, j, status);
            assert(status == SVG_OK);
        }
        if (images[0].bytes != images[1].bytes ||
            memcmp(images[0].pixels, images[1].pixels, images[0].bytes))
            fprintf(stderr, "use %zu differs from expanded reference\n", i);
        assert(images[0].bytes == images[1].bytes &&
               !memcmp(images[0].pixels, images[1].pixels, images[0].bytes));
        char name[64];
        snprintf(name, sizeof(name), "use-%02zu", i);
        save_ppm(dir, name, &images[0]);
        os64_geometry_memory_t m = {&l, svg_alloc_test, svg_free_test};
        for (int j = 0; j < 2; ++j) {
            yonder_svg_pixels_free(&m, &images[j]);
            fixture_free(&fixtures[j]);
        }
        assert(!l.live);
    }
    const char *refused[] = {
        "<use href='other.svg#r'/>",
        "<defs><symbol id=r><rect width=16 height='16'/></symbol></defs><use href='#r'/>",
        "<defs><svg id=r><rect width=16 height='16'/></svg></defs><use href='#r'/>",
        "<defs style='--ink:red'><rect id=r width=16 height=16 "
        "style='fill:var(--ink)'/></defs><use href='#r' style='--ink:blue'/>",
        "<defs><g id=r style='--ink:red'><rect width=16 height=16 "
        "style='fill:var(--ink)'/></g></defs><use href='#r'/>",
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        char html[2048];
        snprintf(html, sizeof(html), "<svg id=art width=128 height=128>%s</svg>", refused[i]);
        Fixture f = fixture_make(html);
        yonder_svg_pixels_t p;
        assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_UNSUPPORTED && !p.pixels && !l.live);
        fixture_free(&f);
    }
    Fixture hidden = fixture_make("<svg id=art width=128 height=128><use href='external.svg#r' "
                                  "display='none'/></svg>");
    yonder_svg_pixels_t hidden_pixels;
    assert(draw_fixture(&hidden, &l, &hidden_pixels, hidden.box->rect) == SVG_OK);
    pixels_free(&l, &hidden_pixels);
    fixture_free(&hidden);
    Fixture base =
        fixture_make("<base href='https://example.invalid/'><svg id=art width=128 height=128>"
                     "<defs><rect id=r width=16 height='16'/></defs><use href='#r'/></svg>");
    assert(draw_fixture(&base, &l, &hidden_pixels, base.box->rect) == SVG_UNSUPPORTED);
    assert(!hidden_pixels.pixels && !l.live);
    fixture_free(&base);
    // A shallow doubling graph must hit the visit bound without unbounded expansion.
    char bomb[8192] = "<svg id=art width=128 height=128><defs><g id=b0></g>";
    for (int i = 1; i <= 14; ++i) {
        char group[128];
        snprintf(group, sizeof(group), "<g id=b%d><use href='#b%d'/><use href='#b%d'/></g>", i,
                 i - 1, i - 1);
        strcat(bomb, group);
    }
    strcat(bomb, "</defs><use href='#b14'/></svg>");
    Fixture expansion = fixture_make(bomb);
    assert(draw_fixture(&expansion, &l, &hidden_pixels, expansion.box->rect) == SVG_LIMIT);
    assert(!hidden_pixels.pixels && !l.live);
    fixture_free(&expansion);
    // Cross-root references use the destination viewport and instance inheritance.
    Fixture outside = fixture_make(
        "<svg style='display:none'><rect id=r width=16 height=16 fill='currentColor'/></svg>"
        "<svg id=art width=128 height=128 color=blue><use href='#r'/></svg>");
    yonder_svg_pixels_t p;
    assert(draw_fixture(&outside, &l, &p, outside.box->rect) == SVG_OK);
    assert(pixel(&p, 4, 4) == 0xff0000ff);
    pixels_free(&l, &p);
    fixture_free(&outside);
    Fixture duplicate =
        fixture_make("<div id=r></div><svg id=art width=128 height=128><defs>"
                     "<rect id=r width=128 height='128'/></defs><use href='#r'/></svg>");
    assert(draw_fixture(&duplicate, &l, &p, duplicate.box->rect) == SVG_OK);
    assert(!pixel(&p, 4, 4));
    pixels_free(&l, &p);
    fixture_free(&duplicate);
    // Enough copies to grow the instance table and exercise every allocation cut.
    char html[8192] =
        "<svg id=art width=128 height=128><defs><rect id=r width=8 height='8'/></defs>";
    for (int i = 0; i < 20; ++i)
        strcat(html, "<use href='#r' fill='blue'/>");
    strcat(html, "</svg>");
    Fixture f = fixture_make(html);
    size_t start = l.calls;
    assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_OK);
    size_t cuts = l.calls - start;
    assert(pixel(&p, 4, 4) == 0xff0000ff && !pixel(&p, 12, 4));
    pixels_free(&l, &p);
    for (size_t i = 1; i <= cuts; ++i)
        for (int admission = 0; admission < 2; ++admission) {
            l.fail = l.calls + i;
            l.admission = admission;
            assert(draw_fixture(&f, &l, &p, f.box->rect) ==
                   (admission ? SVG_LIMIT : SVG_NO_MEMORY));
            assert(!p.pixels && !l.live);
            l.fail = 0;
        }
    assert(os64_html_set_attr(f.doc, named(f.doc->document, "r"), "width", "16", 2) == 0);
    layout_fixture(&f, 256, 1000, true);
    assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_OK && pixel(&p, 12, 4) == 0xff0000ff);
    pixels_free(&l, &p);
    fixture_free(&f);
    printf("SVG use: %zu exact expanded references, %zu allocation cuts in both modes\n",
           sizeof(cases) / sizeof(cases[0]), cuts);
}
