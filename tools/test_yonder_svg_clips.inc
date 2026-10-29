/* Clip silhouettes are compared with independently flattened colored geometry. */
static void clip_cases(const char *dir)
{
    static const struct {
        const char *actual, *reference;
    } cases[] = {
        {"<clipPath id=c><rect x=16 y=8 width=40 height='24'/></clipPath><rect width=128 "
         "height=128 fill=blue clip-path='url(#c)'/>",
         "<rect x=16 y=8 width=40 height=24 fill='blue'/>"},
        {"<clipPath id=c><circle cx=64 cy=64 r='32'/></clipPath><g clip-path='url(#c)'><rect "
         "width=128 height=128 fill='blue'/></g>",
         "<circle cx=64 cy=64 r=32 fill='blue'/>"},
        {"<clipPath id=c clip-rule=evenodd><path d='M16 16H80V80H16ZM32 "
         "32H64V64H32Z'/></clipPath><rect width=128 height=128 fill=blue clip-path='url(#c)'/>",
         "<path d='M16 16H80V80H16ZM32 32H64V64H32Z' fill=blue fill-rule='evenodd'/>"},
        {"<clipPath id=c fill-rule=evenodd><path d='M16 16H80V80H16ZM32 "
         "32H64V64H32Z'/></clipPath><rect width=128 height=128 fill=blue clip-path='url(#c)'/>",
         "<rect x=16 y=16 width=64 height=64 fill='blue'/>"},
        {"<clipPath id=c><rect x=16 y=8 width=40 height=24 fill=none stroke=red stroke-width=12 "
         "opacity='0'/></clipPath><rect width=128 height=128 fill=blue clip-path='url(#c)'/>",
         "<rect x=16 y=8 width=40 height=24 fill='blue'/>"},
        {"<clipPath id=c transform='translate(8 16)'><rect width=24 height=16 "
         "transform='translate(8 0)'/></clipPath><g transform='scale(2 1)' "
         "clip-path='url(#c)'><rect width=128 height=128 fill='blue'/></g>",
         "<rect x=32 y=16 width=48 height=16 fill='blue'/>"},
        {"<clipPath id=c><rect x=16 y=8 width=40 height='24'/></clipPath><rect width=128 "
         "height=128 fill=blue clip-path='url(#c)' style='clip-path:none'/>",
         "<rect width=128 height=128 fill='blue'/>"},
        {"<style>#paint{clip-path:url(#c)}</style><clipPath id=c><rect x=16 y=8 width=40 "
         "height='24'/></clipPath><rect id=paint width=128 height=128 fill='blue'/>",
         "<rect x=16 y=8 width=40 height=24 fill='blue'/>"},
        {"<clipPath id=c><rect x=8 y=8 width=40 height='40'/></clipPath><clipPath id=d><rect x=24 "
         "y=24 width=40 height='40'/></clipPath><g clip-path='url(#c)'><g "
         "clip-path='url(#d)'><rect width=128 height=128 fill='blue'/></g></g>",
         "<rect x=24 y=24 width=24 height=24 fill='blue'/>"},
        {"<clipPath id=c><rect x=8 y=8 width=40 height='40'/></clipPath><g clip-path='url(#c)' "
         "opacity='.5'><rect width=128 height=128 fill='red'/><rect width=128 height=128 "
         "fill='blue'/></g>",
         "<rect x=8 y=8 width=40 height=40 fill=blue opacity='.5'/>"},
        {"<clipPath id=c></clipPath><rect width=128 height=128 clip-path='url(#c)'/><rect x=80 "
         "y=80 width=8 height=8 fill='blue'/>",
         "<rect x=80 y=80 width=8 height=8 fill='blue'/>"},
        {"<rect width=24 height=16 fill=blue clip-path='url(#missing)'/>",
         "<rect width=24 height=16 fill='blue'/>"},
        {"<g id=c></g><rect width=24 height=16 fill=blue clip-path='url(#c)'/>",
         "<rect width=24 height=16 fill='blue'/>"},
        {"<g display=none><clipPath id=c display=none><rect x=8 y=8 width=24 "
         "height='16'/></clipPath></g><rect width=128 height=128 fill=blue clip-path='url(#c)'/>",
         "<rect x=8 y=8 width=24 height=16 fill='blue'/>"},
        {"<clipPath id=c><rect width=128 height=128 display='none'/><rect x=8 y=8 width=24 "
         "height='16'/></clipPath><rect width=128 height=128 fill=blue clip-path='url(#c)'/>",
         "<rect x=8 y=8 width=24 height=16 fill='blue'/>"},
        {"<clipPath id=c><rect width=128 height=128 visibility='hidden'/></clipPath><rect "
         "width=128 height=128 clip-path='url(#c)'/><rect x=80 y=80 width=8 height=8 fill='blue'/>",
         "<rect x=80 y=80 width=8 height=8 fill='blue'/>"},
        {"<clipPath id=c><rect width='20' height='10'/></clipPath><svg x=8 y=8 width=80 "
         "height=40 viewBox='0 0 40 20' clip-path='url(#c)'><rect width=40 height=20 "
         "fill='blue'/></svg>",
         "<rect x=8 y=8 width=40 height=20 fill='blue'/>"},
        {"<clipPath id=c><rect x=8 y=8 width=24 height='16'/></clipPath><defs><g id=s "
         "clip-path='url(#c)'><rect width=128 height=128 fill='blue'/></g></defs><use "
         "href='#s'/><use href='#s' x='40'/>",
         "<rect x=8 y=8 width=24 height=16 fill='blue'/><rect x=48 y=8 width=24 height=16 "
         "fill='blue'/>"},
        {"<clipPath id=c><rect x=16 y=16 width=32 height='32'/></clipPath><path d='M0 32H96' "
         "fill=none stroke=blue stroke-width=64 clip-path='url(#c)'/>",
         "<rect x=16 y=16 width=32 height=32 fill='blue'/>"},
        {"<clipPath id=c><polygon points='16,16 64,16 64,64 16,64'/></clipPath><rect width=128 "
         "height=128 fill=blue clip-path='url(#c)'/>",
         "<rect x=16 y=16 width=48 height=48 fill='blue'/>"},
        {"<clipPath id=c clip-rule=evenodd><path style='clip-rule:nonzero' d='M16 16H80V80H16ZM32 "
         "32H64V64H32Z'/></clipPath><rect width=128 height=128 fill=blue clip-path='url(#c)'/>",
         "<rect x=16 y=16 width=64 height=64 fill='blue'/>"},
        {"<clipPath id=c><path d='M16 16H80V80H16ZM32 32H64V64H32Z'/></clipPath><g "
         "clip-rule=evenodd "
         "clip-path='url(#c)'><rect width=128 height=128 fill='blue'/></g>",
         "<rect x=16 y=16 width=64 height=64 fill='blue'/>"}};
    SvgLedger l = {.cap = 64 << 20};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        Fixture f[2];
        yonder_svg_pixels_t p[2];
        for (int j = 0; j < 2; ++j) {
            char html[4096], file[1024];
            snprintf(
                html, sizeof(html),
                "<!doctype html><body style='margin:0'><svg id=art width=128 height=128>%s</svg>",
                j ? cases[i].reference : cases[i].actual);
            snprintf(file, sizeof(file), "%s/clip-%02zu-%s.html", dir, i,
                     j ? "reference" : "actual");
            FILE *out = fopen(file, "wb");
            assert(out && fputs(html, out) >= 0 && !fclose(out));
            f[j] = fixture_make(html);
            svg_status_t status = draw_fixture(&f[j], &l, &p[j], f[j].box->rect);
            if (status != SVG_OK)
                fprintf(stderr, "clip %zu/%d status %d\n", i, j, status);
            assert(status == SVG_OK);
        }
        if (p[0].bytes != p[1].bytes || memcmp(p[0].pixels, p[1].pixels, p[0].bytes))
            fprintf(stderr, "clip %zu differs\n", i);
        assert(p[0].bytes == p[1].bytes && !memcmp(p[0].pixels, p[1].pixels, p[0].bytes));
        char name[64];
        snprintf(name, sizeof(name), "clip-%02zu", i);
        save_ppm(dir, name, &p[0]);
        for (int j = 0; j < 2; ++j) {
            os64_geometry_memory_t memory = {&l, svg_alloc_test, svg_free_test};
            yonder_svg_pixels_free(&memory, &p[j]);
            fixture_free(&f[j]);
        }
        assert(!l.live);
    }
    const char *refused[] = {
        "<clipPath id=c><rect width='50%' height='20'/></clipPath>",
        "<clipPath id=c><line x1='50%' x2=20 y2='20'/></clipPath>",
        "<clipPath id=c xml:base='https://example.invalid/'><rect width=20 "
        "height='20'/></clipPath>",
        "<g clip-path='url(https://example.invalid/art.svg#c)'><rect width=20 height='20'/></g>",
        "<clipPath id=c><rect width=20 height='20'/><circle r='10'/></clipPath>",
        "<clipPath id=c clipPathUnits=objectBoundingBox><rect width='50%' height='1'/></clipPath>",
        "<clipPath id=c clip-path='url(#c)'><rect width=20 height='20'/></clipPath>",
        "<clipPath id=c><rect width=20 height=20 clip-path='url(#c)'/></clipPath>",
        "<clipPath id=c><use href='#shape'/></clipPath><rect id=shape width=20 height='20'/>"};
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        char html[2048];
        snprintf(html, sizeof(html),
                 "<svg id=art width=128 height=128>%s<rect width=128 height=128 "
                 "clip-path='url(#c)'/></svg>",
                 refused[i]);
        Fixture f = fixture_make(html);
        yonder_svg_pixels_t p;
        assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_UNSUPPORTED && !p.pixels && !l.live);
        fixture_free(&f);
    }
    Fixture f = fixture_make("<svg id=art width=128 height=128><clipPath id=c><rect id=window x=8 "
                             "y=8 width=20 height='20'/></clipPath><g clip-path='url(#c)'><rect "
                             "width=128 height=128 fill='blue'/></g></svg>");
    yonder_svg_pixels_t p;
    size_t calls = l.calls;
    assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_OK);
    size_t cuts = l.calls - calls;
    pixels_free(&l, &p);
    for (size_t i = 1; i <= cuts; ++i)
        for (int admission = 0; admission < 2; ++admission) {
            l.admission = admission;
            l.fail = l.calls + i;
            assert(draw_fixture(&f, &l, &p, f.box->rect) ==
                   (admission ? SVG_LIMIT : SVG_NO_MEMORY));
            assert(!p.pixels && !l.live);
            l.fail = 0;
        }
    assert(!os64_html_set_attr(f.doc, named(f.doc->document, "window"), "width", "40", 2));
    layout_fixture(&f, 256, 1000, true);
    assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_OK && pixel(&p, 40, 16) == 0xff0000ff &&
           !pixel(&p, 52, 16));
    pixels_free(&l, &p);
    fixture_free(&f);
    printf("SVG clips: %zu exact references, %zu allocation cuts in both modes, mutation pass\n",
           sizeof(cases) / sizeof(cases[0]), cuts);
}
