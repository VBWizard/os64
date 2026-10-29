/* Object-coordinate clips checked against explicit user-space geometry. */
static void bounds_cases(const char *dir)
{
    static const struct {
        const char *actual, *reference;
    } cases[] = {
        {"<clipPath id=c clipPathUnits='objectBoundingBox'><rect width='.5' "
         "height='1'/></clipPath><rect x=16 y=16 width=64 height=64 fill=blue "
         "clip-path='url(#c)'/>",
         "<rect x=16 y=16 width=32 height=64 fill='blue'/>"},
        {"<clipPath id=c clipPathUnits='objectBoundingBox'><rect width='.5' "
         "height='1'/></clipPath><g transform='translate(8 4) scale(1.5 .5)' "
         "clip-path='url(#c)'><rect x=16 y=16 width=64 height=64 fill='blue'/></g>",
         "<rect x=32 y=12 width=48 height=32 fill='blue'/>"},
        {"<clipPath id=c clipPathUnits='objectBoundingBox'><rect width='.5' "
         "height='1'/></clipPath><g clip-path='url(#c)'><rect x=16 y=16 width=16 height=64 "
         "fill='blue'/><rect x=64 y=16 width=16 height=64 fill='red'/></g>",
         "<rect x=16 y=16 width=16 height=64 fill='blue'/>"},
        {"<clipPath id=c clipPathUnits=objectBoundingBox><rect width=1 "
         "height='.5'/></clipPath><path d='M16 16C16 56 56 56 56 16Z' fill=blue "
         "clip-path='url(#c)'/>",
         "<clipPath id=c ><rect x=16 y=16 width=40 height='15'/></clipPath><path d='M16 16C16 56 "
         "56 56 56 16Z' fill=blue clip-path='url(#c)'/>"},
        {"<clipPath id=c clipPathUnits='objectBoundingBox'><rect width='.5' "
         "height='1'/></clipPath><g transform='translate(16 16) scale(3)' "
         "clip-path='url(#c)'><path d='M0 0Q10 20 20 0Z' transform='matrix(1 0 1 1 0 0)' "
         "fill='blue'/></g>",
         "<clipPath id=c ><rect width='11.25' height='10'/></clipPath><g transform='translate(16 "
         "16) scale(3)' clip-path='url(#c)'><path d='M0 0Q10 20 20 0Z' transform='matrix(1 0 1 1 0 "
         "0)' fill='blue'/></g>"},
        {"<clipPath id=c clipPathUnits='objectBoundingBox'><rect width='.5' "
         "height='1'/></clipPath><rect x=32 y=32 width=32 height=32 fill=none stroke=blue "
         "stroke-width=16 clip-path='url(#c)'/>",
         "<clipPath id=c ><rect x=32 y=32 width=16 height='32'/></clipPath><rect x=32 y=32 "
         "width=32 height=32 fill=none stroke=blue stroke-width=16 clip-path='url(#c)'/>"},
        {"<clipPath id=c clipPathUnits='objectBoundingBox'><rect width='.5' "
         "height='1'/></clipPath><g clip-path='url(#c)'><rect x=16 y=16 width=32 height=32 "
         "fill='blue'/><rect x=48 y=16 width=48 height=32 visibility='hidden'/></g>",
         "<rect x=16 y=16 width=32 height=32 fill='blue'/>"},
        {"<clipPath id=c clipPathUnits='objectBoundingBox'><rect width='.5' "
         "height='1'/></clipPath><g clip-path='url(#c)'><rect x=16 y=16 width=32 height=32 "
         "fill='blue'/><rect x=48 y=16 width=48 height=32 opacity='0'/></g>",
         "<rect x=16 y=16 width=32 height=32 fill='blue'/>"},
        {"<clipPath id=c clipPathUnits='objectBoundingBox'><rect width='.5' "
         "height='1'/></clipPath><g clip-path='url(#c)'><rect x=16 y=16 width=32 height=32 "
         "fill='blue'/><rect x=48 y=16 width=48 height=32 fill='none'/></g>",
         "<rect x=16 y=16 width=32 height=32 fill='blue'/>"},
        {"<clipPath id=c clipPathUnits='objectBoundingBox'><rect width='.5' "
         "height='1'/></clipPath><g clip-path='url(#c)'><rect x=16 y=16 width=64 height=64 "
         "fill='blue'/><rect x=96 width=32 height=32 display='none'/></g>",
         "<rect x=16 y=16 width=32 height=64 fill='blue'/>"},
        {"<clipPath id=c clipPathUnits='objectBoundingBox'><rect width='.5' "
         "height='1'/></clipPath><clipPath id=d><rect x=16 y=16 width=16 "
         "height='64'/></clipPath><g clip-path='url(#c)'><rect x=16 y=16 width=64 height=64 "
         "fill=blue clip-path='url(#d)'/></g>",
         "<rect x=16 y=16 width=16 height=64 fill='blue'/>"},
        {"<clipPath id=c clipPathUnits='objectBoundingBox'><rect width='.5' "
         "height='1'/></clipPath><svg x=8 y=8 width=96 height=64 viewBox='0 0 48 32' "
         "clip-path='url(#c)'><rect x=4 y=4 width=40 height=24 fill='blue'/></svg>",
         "<rect x=16 y=16 width=40 height=48 fill='blue'/>"},
        {"<clipPath id=c clipPathUnits='objectBoundingBox'><rect width='.5' "
         "height='1'/></clipPath><g clip-path='url(#c)'><svg x=8 y=8 width=96 height=64 viewBox='0 "
         "0 48 32'><rect x=4 y=4 width=40 height=24 fill='blue'/></svg></g>",
         "<rect x=16 y=16 width=40 height=48 fill='blue'/>"},
        {"<clipPath id=c clipPathUnits='objectBoundingBox'><rect width='.5' "
         "height='1'/></clipPath><defs><rect id=s x=8 y=8 width=64 height=64 "
         "fill='blue'/></defs><use href='#s' x=8 y=8 clip-path='url(#c)'/>",
         "<rect x=16 y=16 width=32 height=64 fill='blue'/>"},
        {"<clipPath id=c clipPathUnits=objectBoundingBox transform='translate(8 4)'><rect "
         "width='.5' height='.5' transform='translate(.125 .125)'/></clipPath><rect x=16 y=16 "
         "width=64 height=64 fill=blue clip-path='url(#c)'/>",
         "<rect x=32 y=28 width=32 height=32 fill='blue'/>"},
        {"<clipPath id=c clipPathUnits=objectBoundingBox><rect x='-.5' y='-.5' width=1 "
         "height='2'/></clipPath><rect x=16 y=16 width=64 height=64 fill=blue "
         "clip-path='url(#c)'/>",
         "<rect x=16 y=16 width=32 height=64 fill='blue'/>"},
        {"<clipPath id=c clipPathUnits=objectBoundingBox></clipPath><rect x=16 y=16 width=64 "
         "height=64 fill=blue clip-path='url(#c)'/><rect x=96 y=96 width=8 height=8 fill='red'/>",
         "<rect x=96 y=96 width=8 height=8 fill='red'/>"},
        {"<clipPath id=c clipPathUnits='objectBoundingBox'><rect width='.5' "
         "height='1'/></clipPath><path d='M16 32H80' stroke=blue stroke-width=16 "
         "clip-path='url(#c)'/>",
         "<path d='M16 32H80' stroke=blue stroke-width='16'/>"},
        {"<clipPath id=c clipPathUnits='objectBoundingBox'><rect width='.5' "
         "height='1'/></clipPath><g clip-path='url(#c)'><ellipse cx=32 cy=32 rx=16 ry=12 fill=blue "
         "transform='matrix(1 0 1 1 0 0)'/></g>",
         "<clipPath id=c ><rect x=44 y=20 width=20 height='24'/></clipPath><g "
         "clip-path='url(#c)'><ellipse cx=32 cy=32 rx=16 ry=12 fill=blue transform='matrix(1 0 1 1 "
         "0 0)'/></g>"},
        {"<clipPath id=c clipPathUnits='objectBoundingBox'><rect width='.5' "
         "height='1'/></clipPath><g clip-path='url(#c)'><rect x=16 y=16 width=64 height=64 "
         "fill='blue'/></g><rect x=96 y=96 width=8 height=8 fill='red'/>",
         "<rect x=16 y=16 width=32 height=64 fill='blue'/><rect x=96 y=96 width=8 height=8 "
         "fill='red'/>"},
    };
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
            snprintf(file, sizeof(file), "%s/bounds-%02zu-%s.html", dir, i,
                     j ? "reference" : "actual");
            FILE *out = fopen(file, "wb");
            assert(out && fputs(html, out) >= 0 && !fclose(out));
            f[j] = fixture_make(html);
            svg_status_t status = draw_fixture(&f[j], &l, &p[j], f[j].box->rect);
            if (status != SVG_OK)
                fprintf(stderr, "bounds %zu/%d status %d\n", i, j, status);
            assert(status == SVG_OK);
        }
        if (p[0].bytes != p[1].bytes || memcmp(p[0].pixels, p[1].pixels, p[0].bytes))
            fprintf(stderr, "bounds %zu differs\n", i);
        assert(p[0].bytes == p[1].bytes && !memcmp(p[0].pixels, p[1].pixels, p[0].bytes));
        char name[64];
        snprintf(name, sizeof(name), "bounds-%02zu", i);
        save_ppm(dir, name, &p[0]);
        for (int j = 0; j < 2; ++j) {
            os64_geometry_memory_t memory = {&l, svg_alloc_test, svg_free_test};
            yonder_svg_pixels_free(&memory, &p[j]);
            fixture_free(&f[j]);
        }
        assert(!l.live);
    }
    Fixture f = fixture_make(
        "<svg id=art width=128 height=128><clipPath id=c clipPathUnits=objectBoundingBox><rect "
        "width='.5' height='1'/></clipPath><rect id=changing x=16 y=16 width=32 height=64 "
        "fill=blue clip-path='url(#c)'/></svg>");
    yonder_svg_pixels_t p;
    size_t calls = l.calls;
    assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_OK);
    size_t cuts = l.calls - calls;
    assert(pixel(&p, 24, 32) == 0xff0000ff && !pixel(&p, 40, 32));
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
    assert(!os64_html_set_attr(f.doc, named(f.doc->document, "changing"), "width", "64", 2));
    layout_fixture(&f, 256, 1000, true);
    assert(draw_fixture(&f, &l, &p, f.box->rect) == SVG_OK);
    assert(pixel(&p, 40, 32) == 0xff0000ff && !pixel(&p, 56, 32));
    pixels_free(&l, &p);
    fixture_free(&f);
    printf("SVG object bounds: %zu exact references, %zu allocation cuts in both modes, mutation "
           "pass\n",
           sizeof(cases) / sizeof(cases[0]), cuts);
}
