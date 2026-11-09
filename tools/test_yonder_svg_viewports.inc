/* Real adapter fixtures with explicit flattened references. */
static void viewport_cases(const char *dir)
{
    static const struct {
        const char *actual, *reference;
    } cases[] = {
        {"<defs><symbol id=s viewBox='0 0 10 10'><rect width=10 height='10'/></symbol></defs><use "
         "href='#s' x=8 y=8 width=40 height='20'/>",
         "<rect x=18 y=8 width=20 height='20'/>"},
        {"<defs><symbol id=s viewBox='0 0 10 10' preserveAspectRatio=none><rect width=10 "
         "height='10'/></symbol></defs><use href='#s' x=8 y=8 width=40 height='20'/>",
         "<rect x=8 y=8 width=40 height='20'/>"},
        {"<defs><symbol id=s viewBox='0 0 10 10' preserveAspectRatio='xMidYMid slice'><rect "
         "width=10 height='10'/></symbol></defs><use href='#s' x=8 y=8 width=40 height='20'/>",
         "<rect x=8 y=8 width=40 height='20'/>"},
        {"<defs><symbol id=s viewBox='0 0 10 10' preserveAspectRatio=xMinYMin><rect width=10 "
         "height='10'/></symbol></defs><use href='#s' x=8 y=8 width=40 height='20'/>",
         "<rect x=8 y=8 width=20 height='20'/>"},
        {"<defs><symbol id=s viewBox='0 0 10 10' preserveAspectRatio=xMaxYMax><rect width=10 "
         "height='10'/></symbol></defs><use href='#s' x=8 y=8 width=40 height='20'/>",
         "<rect x=28 y=8 width=20 height='20'/>"},
        {"<svg x=16 y=8 width=40 height=24 viewBox='0 0 20 12'><rect width='100%' "
         "height='100%'/></svg>",
         "<rect x=16 y=8 width=40 height='24'/>"},
        {"<defs><symbol id=s viewBox='0 0 10 10'><rect width='50%' "
         "height='50%'/></symbol></defs><use href='#s' x=8 y=8 width=40 height=40 "
         "fill='blue'/><use "
         "href='#s' x=64 y=8 width=20 height=20 fill='red'/>",
         "<rect x=8 y=8 width=20 height=20 fill='blue'/><rect x=64 y=8 width=10 height=10 "
         "fill='red'/>"},
        {"<defs><svg id=s x=5 y=7 width=20 height=20 viewBox='0 0 10 10'><rect width=10 "
         "height='10'/></svg></defs><use href='#s' x=10 y=10 width=40 height='40'/>",
         "<rect x=15 y=17 width=40 height='40'/>"},
        {"<defs><symbol id=s width=20 height=10 viewBox='0 0 10 10'><rect width=10 "
         "height='10'/></symbol></defs><use href='#s' x=8 y='8'/>",
         "<rect x=13 y=8 width=10 height='10'/>"},
        {"<defs><symbol id=s width=20 height=20><rect width='100%' "
         "height='100%'/></symbol></defs><use href='#s' x=8 y=8 width=40 height='20'/>",
         "<rect x=8 y=8 width=40 height='20'/>"},
        {"<svg x=16 y=16 width=32 height=16><rect x=-20 y=-20 width=80 height='80'/></svg>",
         "<rect x=16 y=16 width=32 height='16'/>"},
        {"<svg x=16 y=16 width=32 height=16 overflow=visible><rect x=-20 y=-20 width=80 "
         "height='80'/></svg>",
         "<rect x=-4 y=-4 width=80 height='80'/>"},
        {"<svg x=16 y=16 width=32 height=16 overflow=hidden style='overflow:visible'><rect x=-20 "
         "y=-20 width=80 height='80'/></svg>",
         "<rect x=-4 y=-4 width=80 height='80'/>"},
        {"<svg x=16 y=16 width=40 height=40><svg x=24 y=24 width=40 height=40><rect width=40 "
         "height='40'/></svg></svg>",
         "<rect x=40 y=40 width=16 height='16'/>"},
        {"<svg width=32 height=16 transform='matrix(1 1 -1 1 64 16)'><rect x=-40 y=-40 width=120 "
         "height='120'/></svg>",
         "<path d='M64 16L96 48L80 64L48 32Z'/>"},
        {"<svg width=32 height=16 transform='matrix(1 1 -1 1 64 16)' opacity='.5'><rect x=-40 "
         "y=-40 width=120 height=120 fill='red'/><rect x=-20 y=-20 width=80 height=80 "
         "fill='blue'/></svg>",
         "<path d='M64 16L96 48L80 64L48 32Z' fill=blue opacity='.5'/>"},
        {"<svg x=8 y=8 width=32 height=16 viewBox='10 20 16 8'><rect x=10 y=20 width=16 "
         "height='8'/></svg>",
         "<rect x=8 y=8 width=32 height='16'/>"},
        {"<svg x='25%' y='12.5%' width='50%' height='25%'><rect x='25%' y='25%' width='50%' "
         "height='50%'/></svg>",
         "<rect x=48 y=24 width=32 height='16'/>"},
        {"<svg x=8 y=8 width=0 height=20><rect width=128 height='128'/></svg><svg width=20 "
         "height=20 viewBox='0 0 0 10'><rect width=128 height='128'/></svg><rect x=80 y=80 width=8 "
         "height='8'/>",
         "<rect x=80 y=80 width=8 height='8'/>"},
        {"<symbol id=s viewBox='0 0 10 10'><rect width=10 height='10'/></symbol><use href='#s' "
         "x=32 y=16 width=20 height='20'/>",
         "<rect x=32 y=16 width=20 height='20'/>"},
        {"<defs><symbol id=s viewBox='0 0 10 10'><rect width=10 height=10 "
         "fill='currentColor'/></symbol></defs><use href='#s' width=20 height=20 "
         "color='blue'/><use "
         "href='#s' x=32 width=40 height=40 color='red'/>",
         "<rect width=20 height=20 fill='blue'/><rect x=32 width=40 height=40 fill='red'/>"},
        {"<defs><symbol id=s viewBox='0 0 10 10'><rect width=10 height='10'/></symbol></defs><use "
         "href='#s' x=4 y=8 width=20 height=20 transform='scale(2 1)'/>",
         "<rect x=8 y=8 width=40 height='20'/>"},
        {"<svg x=8 y=8 width=40 height=20 viewBox='0 0 20 10'><path d='M0 5H20' fill=none "
         "stroke=blue stroke-width='20%'/></svg>",
         "<svg x=8 y=8 width=40 height=20 viewBox='0 0 20 10'><path d='M0 5H20' fill=none "
         "stroke=blue stroke-width='3.1622776601683795'/></svg>"},
        {"<svg x=8 y=8 width=32 height=16 style='overflow-x:hidden;overflow-y:visible'><rect x=-20 "
         "y=-20 width=80 height='80'/></svg>",
         "<rect x=8 y=8 width=32 height='16'/>"},
        {"<defs><symbol id=s width=16 height=16 overflow=inherit><rect x=-8 y=-8 width=32 "
         "height='32'/></symbol></defs><use href='#s' x=24 y='24'/>",
         "<rect x=16 y=16 width=32 height='32'/>"},
        {"<defs><symbol id=s viewBox='0 0 10 10'><rect width=10 height='10'/></symbol></defs><use "
         "href='#s' width=0 height='20'/><rect x=80 y=80 width=8 height='8'/>",
         "<rect x=80 y=80 width=8 height='8'/>"},
        {"<defs><g id=g><symbol><rect width=128 height='128'/></symbol><rect x=8 y=8 width=16 "
         "height='16'/></g></defs><use href='#g'/>",
         "<rect x=8 y=8 width=16 height='16'/>"},
        {"<defs><symbol id=s viewBox='0 0 10 10' preserveAspectRatio=none><rect width=10 "
         "height='10'/></symbol></defs><use href='#s' x=8 y=8 width=20 height=20 "
         "style='width:40px'/>",
         "<rect x=8 y=8 width=40 height='20'/>"},
    };
    SvgLedger l = {.cap = 64 << 20};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        Fixture fixtures[2];
        yonder_svg_pixels_t images[2];
        for (int j = 0; j < 2; ++j) {
            char html[4096], filename[1024];
            snprintf(
                html, sizeof(html),
                "<!doctype html><body style='margin:0'><svg id=art width=128 height=128>%s</svg>",
                j ? cases[i].reference : cases[i].actual);
            snprintf(filename, sizeof(filename), "%s/viewport-%02zu-%s.html", dir, i,
                     j ? "reference" : "actual");
            FILE *file = fopen(filename, "wb");
            assert(file && fputs(html, file) >= 0 && !fclose(file));
            fixtures[j] = fixture_make(html);
            svg_status_t status = draw_fixture(&fixtures[j], &l, &images[j], fixtures[j].box->rect);
            if (status != SVG_OK)
                fprintf(stderr, "viewport %zu side %d status %d\n", i, j, status);
            assert(status == SVG_OK);
        }
        if (images[0].bytes != images[1].bytes ||
            memcmp(images[0].pixels, images[1].pixels, images[0].bytes))
            fprintf(stderr, "viewport %zu differs\n", i);
        assert(images[0].bytes == images[1].bytes &&
               !memcmp(images[0].pixels, images[1].pixels, images[0].bytes));
        char name[64];
        snprintf(name, sizeof(name), "viewport-%02zu", i);
        save_ppm(dir, name, &images[0]);
        os64_geometry_memory_t m = {&l, svg_alloc_test, svg_free_test};
        for (int j = 0; j < 2; ++j) {
            yonder_svg_pixels_free(&m, &images[j]);
            fixture_free(&fixtures[j]);
        }
        assert(!l.live);
    }
    const char *refused[] = {"overflow:auto", "overflow:scroll",
                             "overflow-x:clip;overflow-y:visible"};
    for (size_t i = 0; i < 3; ++i) {
        char html[512];
        snprintf(html, sizeof(html),
                 "<svg id=art width=128 height=128><svg width=32 height=16 style='%s'><rect "
                 "width=32 height='16'/></svg></svg>",
                 refused[i]);
        Fixture bad = fixture_make(html);
        yonder_svg_pixels_t pixels;
        assert(draw_fixture(&bad, &l, &pixels, bad.box->rect) == SVG_UNSUPPORTED);
        assert(!pixels.pixels && !l.live);
        fixture_free(&bad);
    }
    Fixture f = fixture_make("<svg id=art width=128 height=128><defs><symbol id=s viewBox='0 0 10 "
                             "10'><rect x=-10 y=-10 width=30 height='30'/></symbol></defs><use "
                             "id=copy href='#s' x=8 y=8 width=20 height='20'/></svg>");
    yonder_svg_pixels_t p;
    size_t before = l.calls;
    assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_OK);
    size_t cuts = l.calls - before;
    assert(pixel(&p, 24, 16) == 0xff000000 && !pixel(&p, 32, 16));
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
    assert(os64_html_set_attr(f.doc, named(f.doc->document, "copy"), "width", "40", 2) == 0);
    layout_fixture(&f, 256, 1000, true);
    assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_OK && pixel(&p, 40, 16) == 0xff000000 &&
           !pixel(&p, 52, 16));
    pixels_free(&l, &p);
    fixture_free(&f);
    printf(
        "SVG viewports: %zu exact references, %zu allocation cuts in both modes, mutation pass\n",
        sizeof(cases) / sizeof(cases[0]), cuts);
}
