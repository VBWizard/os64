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

static void gradient_cases(const char *directory)
{
    gradient_sampler_contracts();
    gradient_resource_contracts();
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
