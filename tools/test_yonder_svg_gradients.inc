#include "test_svg_gradient_sampler.inc"
static void gradient_resource_contracts(void)
{
    size_t baseline = live;
    SvgLedger ledger = {.cap = 64 << 20};
    const char *bodies[] = {
        "<linearGradient id='a' href='#b'/><linearGradient id='b' href='#a'/><rect width='128' "
        "height='128' fill='url(#a) blue'/>",
        "<rect width='128' height='128' fill='url(https://example.invalid/g.svg#g) blue'/>",
        "<linearGradient id='a'><stop stop-color='blue'/><set attributeName='x2' to='0'/><script "
        "href='https://example.invalid/code.js'/></linearGradient><rect width='128' height='128' "
        "fill='url(#a)'/>"};
    for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); ++i) {
        char xml[2048];
        snprintf(xml, sizeof(xml), "<svg xmlns='http://www.w3.org/2000/svg'>%s</svg>", bodies[i]);
        yonder_svg_document_t *doc = NULL;
        yonder_svg_pixels_t pixels;
        assert(yonder_svg_document_parse(xml, strlen(xml), NULL, NULL, &doc, NULL) == SVG_OK);
        assert(image_draw(doc, &ledger, &pixels) == SVG_OK && pixel(&pixels, 64, 64) == 0xff0000ff);
        pixels_free(&ledger, &pixels);
        yonder_svg_document_free(doc);
        assert(live == baseline);
    }
    char xml[16384] = "<svg xmlns='http://www.w3.org/2000/svg'><linearGradient id='g'>";
    for (size_t i = 0; i < SVG_GRADIENT_STOPS_MAX + 1; ++i)
        strcat(xml, "<stop offset='.5' stop-color='blue'/>");
    strcat(xml, "</linearGradient><rect width='128' height='128' fill='url(#g)'/></svg>");
    yonder_svg_document_t *doc = NULL;
    yonder_svg_pixels_t pixels;
    assert(yonder_svg_document_parse(xml, strlen(xml), NULL, NULL, &doc, NULL) == SVG_OK);
    assert(image_draw(doc, &ledger, &pixels) == SVG_LIMIT && !pixels.pixels && !ledger.live);
    yonder_svg_document_free(doc);
    assert(live == baseline);
    printf("SVG gradient resources: cycles, blocked URLs, inert children and stop limits pass\n");
}

static void gradient_review_contracts(void)
{
    const struct {
        const char *name, *body;
        uint32_t left, right;
    } cases[] = {
        {"stop-color inherit",
         "<linearGradient id='g' stop-color='red'><stop "
         "stop-color='inherit'/></linearGradient><rect width='128' height='128' fill='url(#g)'/>",
         0xffff0000, 0xffff0000},
        {"stop-opacity inherit",
         "<linearGradient id='g' stop-opacity='0'><stop stop-color='red' "
         "stop-opacity='inherit'/></linearGradient><rect width='128' height='128' fill='url(#g)'/>",
         0, 0},
        {"stop-color initial",
         "<linearGradient id='g' stop-color='red'><stop "
         "stop-color='initial'/></linearGradient><rect width='128' height='128' fill='url(#g)'/>",
         0xff000000, 0xff000000},
        {"stop-color unset",
         "<linearGradient id='g' stop-color='red'><stop stop-color='unset'/></linearGradient><rect "
         "width='128' height='128' fill='url(#g)'/>",
         0xff000000, 0xff000000},
        {"stop-opacity initial",
         "<linearGradient id='g' stop-opacity='0'><stop stop-color='red' "
         "stop-opacity='initial'/></linearGradient><rect width='128' height='128' fill='url(#g)'/>",
         0xffff0000, 0xffff0000},
        {"stop-opacity unset",
         "<linearGradient id='g' stop-opacity='0'><stop stop-color='red' "
         "stop-opacity='unset'/></linearGradient><rect width='128' height='128' fill='url(#g)'/>",
         0xffff0000, 0xffff0000},
        {"stop color initial",
         "<linearGradient id='g' color='red'><stop color='initial' "
         "stop-color='currentColor'/></linearGradient><rect width='128' height='128' "
         "fill='url(#g)'/>",
         0xff000000, 0xff000000},
        {"stop color inherit",
         "<linearGradient id='g' color='red'><stop color='inherit' "
         "stop-color='currentColor'/></linearGradient><rect width='128' height='128' "
         "fill='url(#g)'/>",
         0xffff0000, 0xffff0000},
        {"stop color unset",
         "<linearGradient id='g' color='red'><stop color='unset' "
         "stop-color='currentColor'/></linearGradient><rect width='128' height='128' "
         "fill='url(#g)'/>",
         0xffff0000, 0xffff0000},
        {"gradient color initial",
         "<g color='red'><linearGradient id='g' color='initial'><stop "
         "stop-color='currentColor'/></linearGradient></g><rect width='128' height='128' "
         "fill='url(#g)'/>",
         0xff000000, 0xff000000},
        {"gradient stop inheritance",
         "<g stop-color='red'><linearGradient id='g' stop-color='inherit'><stop "
         "stop-color='inherit'/></linearGradient></g><rect width='128' height='128' "
         "fill='url(#g)'/>",
         0xffff0000, 0xffff0000},
        {"inherited currentColor",
         "<linearGradient id='g' color='red' stop-color='currentColor'><stop color='blue' "
         "stop-color='inherit'/></linearGradient><rect width='128' height='128' fill='url(#g)'/>",
         0xff0000ff, 0xff0000ff},
        {"hidden gradient fill",
         "<linearGradient id='g'><stop stop-color='red'/></linearGradient><rect width='128' "
         "height='128' visibility='hidden' fill='url(#g)'/><rect x='64' width='64' height='128' "
         "fill='#008000'/>",
         0, 0xff008000},
        {"hidden gradient stroke",
         "<linearGradient id='g'><stop stop-color='red'/></linearGradient><path d='M0 "
         "0H128V128H0Z' visibility='hidden' fill='none' stroke='url(#g)'/><rect x='64' width='64' "
         "height='128' fill='#008000'/>",
         0, 0xff008000},
        {"visible descendant",
         "<linearGradient id='g'><stop stop-color='red'/></linearGradient><g visibility='hidden' "
         "fill='url(#g)'><rect width='64' height='128'/><rect x='64' width='64' height='128' "
         "visibility='visible'/></g>",
         0, 0xffff0000},
        {"hidden geometry bounds",
         "<linearGradient id='g'><stop stop-color='red'/></linearGradient><clipPath id='c' "
         "clipPathUnits='objectBoundingBox'><rect x='.5' width='.5' height='1'/></clipPath><g "
         "clip-path='url(#c)'><rect width='128' height='128' visibility='hidden' "
         "fill='url(#g)'/><rect x='64' width='64' height='128' fill='#008000'/></g>",
         0, 0xff008000},
    };
    size_t baseline = live, failures = 0;
    SvgLedger ledger = {.cap = 64 << 20};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char xml[4096], html[4096];
        snprintf(xml, sizeof(xml),
                 "<svg xmlns='http://www.w3.org/2000/svg' width='128' height='128'>%s</svg>",
                 cases[i].body);
        snprintf(
            html, sizeof(html),
            "<!doctype html><body style='margin:0'><svg id='art' width='128' height='128'>%s</svg>",
            cases[i].body);
        yonder_svg_document_t *doc = NULL;
        assert(yonder_svg_document_parse(xml, strlen(xml), NULL, NULL, &doc, NULL) == SVG_OK);
        Fixture f = fixture_make(html);
        // Match the standalone image's black initial ink for exact comparison.
        f.env.ink = 0;
        yonder_svg_pixels_t image, inl;
        svg_status_t image_status = image_draw(doc, &ledger, &image);
        svg_status_t inline_status = draw_fixture(&f, &ledger, &inl, f.box->rect);
        bool ok = image_status == SVG_OK && inline_status == SVG_OK &&
                  pixel(&image, 32, 64) == cases[i].left &&
                  pixel(&image, 80, 64) == cases[i].right && image.bytes == inl.bytes &&
                  !memcmp(image.pixels, inl.pixels, image.bytes);
        if (!ok) {
            ++failures;
            fprintf(stderr,
                    "gradient review %s: image=%d inline=%d pixels=%08x/%08x expected=%08x/%08x\n",
                    cases[i].name, image_status, inline_status,
                    image_status == SVG_OK ? pixel(&image, 32, 64) : 0,
                    image_status == SVG_OK ? pixel(&image, 80, 64) : 0, cases[i].left,
                    cases[i].right);
        }
        os64_geometry_memory_t memory = {&ledger, svg_alloc_test, svg_free_test};
        yonder_svg_pixels_free(&memory, &image);
        yonder_svg_pixels_free(&memory, &inl);
        fixture_free(&f);
        yonder_svg_document_free(doc);
        assert(live == baseline && !ledger.live);
    }
    assert(!failures);
    printf("SVG gradient review regressions: %zu inline/image pairs pass\n",
           sizeof(cases) / sizeof(cases[0]));
}
static void gradient_transform_contracts(const char *directory)
{
    const struct {
        const char *name, *ancestors, *close, *attributes, *expected;
    } cases[] = {
        {"initial", "", "", "gradientTransform='rotate(90)' style='transform:initial'", ""},
        {"unset", "", "", "gradientTransform='rotate(90)' style='transform:unset'", ""},
        {"inherit default", "", "", "gradientTransform='rotate(90)' style='transform:inherit'", ""},
        {"inherit CSS", "<g style='transform:translate(32px,0px)'>", "</g>",
         "gradientTransform='rotate(90)' style='transform:inherit'", "translate(32 0)"},
        {"inherit attribute", "<g transform='translate(32 0)'>", "</g>",
         "style='transform:inherit'", "translate(32 0)"},
        {"inherit chain", "<g style='transform:translate(32px,0px)'><g style='transform:inherit'>",
         "</g></g>", "style='transform:inherit'", "translate(32 0)"},
        {"inherit stops at default", "<g transform='translate(32 0)'><g>", "</g></g>",
         "style='transform:inherit'", ""},
        {"inherit parent reset", "<g transform='translate(32 0)' style='transform:initial'>",
         "</g>", "style='transform:inherit'", ""},
        {"initial skips malformed attribute", "", "",
         "gradientTransform='broken' style='transform:initial'", ""},
        {"none overrides attribute", "", "",
         "gradientTransform='rotate(90)' style='transform:none'", ""},
        {"attribute fallback", "", "", "gradientTransform='translate(32 0)'", "translate(32 0)"},
        {"CSS replaces attribute", "", "",
         "gradientTransform='rotate(90)' style='transform:translate(32px,0px)'", "translate(32 0)"},
        {"template reset", "<linearGradient id='base' gradientTransform='rotate(90)'/>", "",
         "href='#base' style='transform:initial'", ""},
        {"template inherit uses source parent",
         "<linearGradient id='base' gradientTransform='rotate(90)'/><g transform='translate(32 "
         "0)'>",
         "</g>", "href='#base' style='transform:inherit'", "translate(32 0)"},
    };
    const char *tail =
        "<stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient>";
    const char *paint = "<rect width='128' height='96' fill='url(#g)'/><rect y='96' width='128' "
                        "height='32' fill='green'/>";
    size_t baseline = live, failures = 0;
    SvgLedger ledger = {.cap = 64 << 20};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char body[4096], xml[8192], html[8192], reference[8192], file[1024], name[64];
        snprintf(body, sizeof(body),
                 "%s<linearGradient id='g' gradientUnits='userSpaceOnUse' x2='128' %s>%s%s%s",
                 cases[i].ancestors, cases[i].attributes, tail, cases[i].close, paint);
        snprintf(xml, sizeof(xml),
                 "<svg xmlns='http://www.w3.org/2000/svg' width='128' height='128'>%s</svg>", body);
        snprintf(
            html, sizeof(html),
            "<!doctype html><body style='margin:0'><svg id='art' width='128' height='128'>%s</svg>",
            body);
        snprintf(reference, sizeof(reference),
                 "<svg xmlns='http://www.w3.org/2000/svg' width='128' height='128'><linearGradient "
                 "id='g' gradientUnits='userSpaceOnUse' x2='128' gradientTransform='%s'>%s%s</svg>",
                 cases[i].expected, tail, paint);
        yonder_svg_document_t *doc = NULL, *ref = NULL;
        assert(yonder_svg_document_parse(xml, strlen(xml), NULL, NULL, &doc, NULL) == SVG_OK);
        assert(yonder_svg_document_parse(reference, strlen(reference), NULL, NULL, &ref, NULL) ==
               SVG_OK);
        Fixture f = fixture_make(html);
        yonder_svg_pixels_t actual, inl, expected;
        svg_status_t status = image_draw(doc, &ledger, &actual);
        svg_status_t inline_status = draw_fixture(&f, &ledger, &inl, f.box->rect);
        assert(image_draw(ref, &ledger, &expected) == SVG_OK);
        bool ok = status == SVG_OK && inline_status == SVG_OK && actual.bytes == expected.bytes &&
                  inl.bytes == expected.bytes &&
                  !memcmp(actual.pixels, expected.pixels, expected.bytes) &&
                  !memcmp(inl.pixels, expected.pixels, expected.bytes);
        if (!ok) {
            ++failures;
            fprintf(stderr, "gradient transform %s: image=%d inline=%d match=%d\n", cases[i].name,
                    status, inline_status, ok);
        } else {
            snprintf(file, sizeof(file), "%s/gradient-transform-%02zu.html", directory, i);
            FILE *out = fopen(file, "wb");
            assert(out && fputs(html, out) >= 0 && !fclose(out));
            snprintf(name, sizeof(name), "gradient-transform-%02zu", i);
            save_ppm(directory, name, &actual);
        }
        os64_geometry_memory_t memory = {&ledger, svg_alloc_test, svg_free_test};
        yonder_svg_pixels_free(&memory, &actual);
        yonder_svg_pixels_free(&memory, &inl);
        yonder_svg_pixels_free(&memory, &expected);
        fixture_free(&f);
        yonder_svg_document_free(doc);
        yonder_svg_document_free(ref);
        assert(live == baseline && !ledger.live);
    }
    assert(!failures);
    printf("SVG gradient transforms: %zu inline/image/explicit-reference triples pass\n",
           sizeof(cases) / sizeof(cases[0]));
}
static void gradient_cases(const char *directory)
{
    gradient_sampler_contracts();
    gradient_resource_contracts();
    gradient_review_contracts();
    gradient_transform_contracts(directory);
    const char *bodies[] = {
        "<linearGradient id='g' ><stop offset='0' stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></linearGradient><rect x='16' y='16' width='96' height='64' "
        "fill='url(#g)'/>",
        "<linearGradient id='g' x2='0' y2='100%'><stop offset='0' stop-color='#e04020'/><stop "
        "offset='1' stop-color='#2040e0'/></linearGradient><rect x='16' y='16' width='96' "
        "height='64' fill='url(#g)'/>",
        "<linearGradient id='g' x2='100%' y2='100%'><stop offset='0' stop-color='#e04020'/><stop "
        "offset='1' stop-color='#2040e0'/></linearGradient><rect x='16' y='16' width='96' "
        "height='64' fill='url(#g)'/>",
        "<linearGradient id='g' gradientTransform='translate(1 0) rotate(90)'><stop offset='0' "
        "stop-color='#e04020'/><stop offset='1' stop-color='#2040e0'/></linearGradient><rect "
        "x='16' y='16' width='96' height='64' fill='url(#g)'/>",
        "<linearGradient id='g' gradientUnits='userSpaceOnUse' x1='16' x2='112'><stop offset='0' "
        "stop-color='#e04020'/><stop offset='1' stop-color='#2040e0'/></linearGradient><rect "
        "x='16' y='16' width='96' height='64' fill='url(#g)'/>",
        "<linearGradient id='g' x1='.2' x2='.4' spreadMethod='repeat'><stop offset='0' "
        "stop-color='#e04020'/><stop offset='1' stop-color='#2040e0'/></linearGradient><rect "
        "x='16' y='16' width='96' height='64' fill='url(#g)'/>",
        "<linearGradient id='g' x1='.2' x2='.4' spreadMethod='reflect'><stop offset='0' "
        "stop-color='#e04020'/><stop offset='1' stop-color='#2040e0'/></linearGradient><rect "
        "x='16' y='16' width='96' height='64' fill='url(#g)'/>",
        "<linearGradient id='g' ><stop stop-color='red' stop-opacity='0'/><stop offset='1' "
        "stop-color='blue'/></linearGradient><rect x='16' y='16' width='96' height='64' "
        "fill='url(#g)'/>",
        "<linearGradient id='g' ><stop stop-color='red'/><stop offset='.5' stop-color='red'/><stop "
        "offset='.5' stop-color='blue'/><stop offset='1' stop-color='blue'/></linearGradient><rect "
        "x='16' y='16' width='96' height='64' fill='url(#g)'/>",
        "<linearGradient id='g' ><stop offset='-.5' stop-color='red'/><stop offset='.6' "
        "stop-color='green'/><stop offset='.2' stop-color='blue'/><stop offset='150%' "
        "stop-color='white'/></linearGradient><rect x='16' y='16' width='96' height='64' "
        "fill='url(#g)'/>",
        "<linearGradient id='base' x2='.5'><stop offset='0' stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></linearGradient><linearGradient id='g' href='#base' "
        "spreadMethod='reflect'/><rect x='16' y='16' width='96' height='64' fill='url(#g)'/>",
        "<g color='red'><linearGradient id='base'><stop stop-color='currentColor'/><stop "
        "offset='1' stop-color='white'/></linearGradient></g><linearGradient id='g' href='#base' "
        "color='blue'/><rect x='16' y='16' width='96' height='64' fill='url(#g)'/>",
        "<style>.first{stop-color:#e04020;stop-opacity:50%}</style><linearGradient id='g' ><stop "
        "class='first' stop-color='black'/><stop offset='1' "
        "stop-color='#2040e0'/></linearGradient><rect x='16' y='16' width='96' height='64' "
        "fill='url(#g)'/>",
        "<linearGradient id='g' x2='0'><stop offset='0' stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></linearGradient><rect x='16' y='16' width='96' height='64' "
        "fill='url(#g)'/>",
        "<linearGradient id='g' ><stop stop-color='blue'/></linearGradient><rect x='16' y='16' "
        "width='96' height='64' fill='url(#g)'/>",
        "<linearGradient id='g' ></linearGradient><rect x='16' y='16' width='96' height='64' "
        "fill='url(#g)'/>",
        "<rect x='16' y='16' width='96' height='64' fill='url(#missing) blue'/>",
        "<linearGradient id='g' gradientTransform='scale(0)'><stop offset='0' "
        "stop-color='#e04020'/><stop offset='1' stop-color='#2040e0'/></linearGradient><rect "
        "x='16' y='16' width='96' height='64' fill='url(#g)'/>",
        "<linearGradient id='g' gradientTransform='rotate(90)' "
        "style='transform:translate(0px,0px)'><stop offset='0' stop-color='#e04020'/><stop "
        "offset='1' stop-color='#2040e0'/></linearGradient><rect x='16' y='16' width='96' "
        "height='64' fill='url(#g)'/>",
        "<linearGradient id='g' ><stop offset='0' stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></linearGradient><rect x='24' y='24' width='72' height='48' "
        "fill='none' stroke='url(#g)' stroke-width='12'/>",
        "<linearGradient id='g' ><stop offset='0' stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></linearGradient><g transform='translate(8 8) scale(.8 1.2)'><rect "
        "x='16' y='16' width='96' height='64' fill='url(#g)'/></g>",
        "<linearGradient id='g' ><stop offset='0' stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></linearGradient><clipPath id='c'><rect x='40' width='48' "
        "height='96'/></clipPath><g opacity='.5' clip-path='url(#c)'><rect x='16' y='16' "
        "width='96' height='64' fill='url(#g)'/></g>",
        "<linearGradient id='g' gradientUnits='userSpaceOnUse' x2='100%'><stop offset='0' "
        "stop-color='#e04020'/><stop offset='1' stop-color='#2040e0'/></linearGradient><svg x='16' "
        "y='16' width='96' height='64' viewBox='0 0 48 32'><rect width='48' height='32' "
        "fill='url(#g)'/></svg>",
        "<linearGradient id='g' ><stop offset='0' stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></linearGradient><linearGradient id='s' x2='0' y2='1'><stop "
        "stop-color='green'/><stop offset='1' stop-color='gold'/></linearGradient><rect x='24' "
        "y='24' width='72' height='48' fill='url(#g)' stroke='url(#s)' stroke-width='12'/>",
    };
    size_t baseline = live;
    SvgLedger ledger = {.cap = 64 << 20};
    for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); ++i) {
        char xml[8192], html[8192], file[1024], name[64];
        snprintf(xml, sizeof(xml),
                 "<svg xmlns='http://www.w3.org/2000/svg' width='128' height='128'>%s</svg>",
                 bodies[i]);
        snprintf(
            html, sizeof(html),
            "<!doctype html><body style='margin:0'><svg id='art' width='128' height='128'>%s</svg>",
            bodies[i]);
        snprintf(file, sizeof(file), "%s/gradient-%02zu.html", directory, i);
        FILE *f = fopen(file, "wb");
        assert(f && fputs(html, f) >= 0 && !fclose(f));
        yonder_svg_document_t *document = NULL;
        assert(yonder_svg_document_parse(xml, strlen(xml), NULL, NULL, &document, NULL) == SVG_OK);
        yonder_svg_pixels_t actual, reference;
        svg_status_t status = image_draw(document, &ledger, &actual);
        if (status != SVG_OK)
            fprintf(stderr, "gradient %zu status %d\n", i, status);
        assert(status == SVG_OK);
        Fixture inline_doc = fixture_make(html);
        assert(draw_fixture(&inline_doc, &ledger, &reference, inline_doc.box->rect) == SVG_OK);
        assert(actual.bytes == reference.bytes &&
               !memcmp(actual.pixels, reference.pixels, actual.bytes));
        snprintf(name, sizeof(name), "gradient-%02zu", i);
        save_ppm(directory, name, &actual);
        os64_geometry_memory_t memory = {&ledger, svg_alloc_test, svg_free_test};
        yonder_svg_pixels_free(&memory, &actual);
        yonder_svg_pixels_free(&memory, &reference);
        fixture_free(&inline_doc);
        yonder_svg_document_free(document);
        assert(live == baseline && !ledger.live);
    }
    printf("SVG gradients: %zu inline/image pairs pass\n", sizeof(bodies) / sizeof(bodies[0]));
}
