#include "test_svg_radial_sampler.inc"
static void radial_resource_contracts(void)
{
    size_t baseline = live;
    SvgLedger ledger = {.cap = 64 << 20};
    const char *bad[] = {"r='-1'", "fr='-1'", "fx='NaN'"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        char xml[1024];
        snprintf(xml, sizeof(xml),
                 "<svg xmlns='http://www.w3.org/2000/svg'>"
                 "<radialGradient id='g' %s><stop stop-color='blue'/></radialGradient>"
                 "<rect width='128' height='128' fill='url(#g)'/></svg>",
                 bad[i]);
        yonder_svg_document_t *doc = NULL;
        yonder_svg_pixels_t pixels = {0};
        assert(yonder_svg_document_parse(xml, strlen(xml), NULL, NULL, &doc, NULL) == SVG_OK);
        assert(image_draw(doc, &ledger, &pixels) != SVG_OK && !pixels.pixels && !ledger.live);
        yonder_svg_document_free(doc);
        assert(live == baseline);
    }
    const char *xml = "<svg xmlns='http://www.w3.org/2000/svg'>"
                      "<radialGradient id='a' href='#b'/><linearGradient id='b' href='#a'/>"
                      "<rect width='128' height='128' fill='url(#a) blue'/></svg>";
    yonder_svg_document_t *doc = NULL;
    yonder_svg_pixels_t pixels;
    assert(yonder_svg_document_parse(xml, strlen(xml), NULL, NULL, &doc, NULL) == SVG_OK);
    assert(image_draw(doc, &ledger, &pixels) == SVG_OK && pixel(&pixels, 64, 64) == 0xff0000ff);
    pixels_free(&ledger, &pixels);
    yonder_svg_document_free(doc);
    assert(live == baseline && !ledger.live);
    puts("SVG radial resources: malformed geometry and mixed-type cycles clean up");
}

static void radial_cases(const char *directory)
{
    radial_sampler_contracts();
    radial_resource_contracts();
    const char *bodies[] = {
        "<radialGradient id='g' ><stop stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></radialGradient><rect x='16' y='16' width='96' height='80' "
        "fill='url(#g)'/>",
        "<radialGradient id='g' cx='.3' cy='.7'><stop stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></radialGradient><rect x='16' y='16' width='96' height='80' "
        "fill='url(#g)'/>",
        "<radialGradient id='g' fx='.25' fy='.35'><stop stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></radialGradient><rect x='16' y='16' width='96' height='80' "
        "fill='url(#g)'/>",
        "<radialGradient id='g' fr='.15'><stop stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></radialGradient><rect x='16' y='16' width='96' height='80' "
        "fill='url(#g)'/>",
        "<radialGradient id='g' fx='.3' fy='.4' fr='.1'><stop stop-color='#e04020'/><stop "
        "offset='1' stop-color='#2040e0'/></radialGradient><rect x='16' y='16' width='96' "
        "height='80' fill='url(#g)'/>",
        "<radialGradient id='g' r='.18' spreadMethod='repeat'><stop stop-color='#e04020'/><stop "
        "offset='1' stop-color='#2040e0'/></radialGradient><rect x='16' y='16' width='96' "
        "height='80' fill='url(#g)'/>",
        "<radialGradient id='g' r='.18' spreadMethod='reflect'><stop stop-color='#e04020'/><stop "
        "offset='1' stop-color='#2040e0'/></radialGradient><rect x='16' y='16' width='96' "
        "height='80' fill='url(#g)'/>",
        "<radialGradient id='g' fr='.2' r='.4' spreadMethod='repeat'><stop "
        "stop-color='#e04020'/><stop offset='1' stop-color='#2040e0'/></radialGradient><rect "
        "x='16' y='16' width='96' height='80' fill='url(#g)'/>",
        "<radialGradient id='g' fr='.2' r='.4' spreadMethod='reflect'><stop "
        "stop-color='#e04020'/><stop offset='1' stop-color='#2040e0'/></radialGradient><rect "
        "x='16' y='16' width='96' height='80' fill='url(#g)'/>",
        "<radialGradient id='g' gradientTransform='translate(.1 -.1) skewX(20)'><stop "
        "stop-color='#e04020'/><stop offset='1' stop-color='#2040e0'/></radialGradient><rect "
        "x='16' y='16' width='96' height='80' fill='url(#g)'/>",
        "<radialGradient id='g' gradientUnits='userSpaceOnUse' cx='64' cy='56' r='32'><stop "
        "stop-color='#e04020'/><stop offset='1' stop-color='#2040e0'/></radialGradient><rect "
        "x='16' y='16' width='96' height='80' fill='url(#g)'/>",
        "<radialGradient id='g' gradientUnits='userSpaceOnUse' cx='50%' cy='50%' r='40%'><stop "
        "stop-color='#e04020'/><stop offset='1' stop-color='#2040e0'/></radialGradient><rect "
        "x='16' y='16' width='96' height='80' fill='url(#g)'/>",
        "<radialGradient id='g' ><stop stop-color='red' stop-opacity='0'/><stop offset='1' "
        "stop-color='blue'/></radialGradient><rect x='16' y='16' width='96' height='80' "
        "fill='url(#g)'/>",
        "<radialGradient id='g' r='.35'><stop stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></radialGradient><rect x='24' y='24' width='72' height='56' "
        "fill='none' stroke='url(#g)' stroke-width='12'/>",
        "<radialGradient id='g' r='0'><stop stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></radialGradient><rect x='16' y='16' width='96' height='80' "
        "fill='url(#g)'/>",
        "<radialGradient id='g' fr='.5' r='.5'><stop stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></radialGradient><rect x='16' y='16' width='96' height='80' "
        "fill='url(#g)'/>",
        "<radialGradient id='g' fr='.6' r='.2'><stop stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></radialGradient><rect x='16' y='16' width='96' height='80' "
        "fill='url(#g)'/>",
        "<radialGradient id='g' fx='1' fy='.5'><stop stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></radialGradient><rect x='16' y='16' width='96' height='80' "
        "fill='url(#g)'/>",
        "<radialGradient id='g' fx='1' fy='.5' spreadMethod='repeat'><stop "
        "stop-color='#e04020'/><stop offset='1' stop-color='#2040e0'/></radialGradient><rect "
        "x='16' y='16' width='96' height='80' fill='url(#g)'/>",
        "<radialGradient id='g' fx='1' fy='.5' spreadMethod='reflect'><stop "
        "stop-color='#e04020'/><stop offset='1' stop-color='#2040e0'/></radialGradient><rect "
        "x='16' y='16' width='96' height='80' fill='url(#g)'/>",
        "<radialGradient id='g' fx='1.2' fy='.5'><stop stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></radialGradient><rect x='16' y='16' width='96' height='80' "
        "fill='url(#g)'/>",
        "<radialGradient id='g' fx='1.2' fy='.5' spreadMethod='reflect'><stop "
        "stop-color='#e04020'/><stop offset='1' stop-color='#2040e0'/></radialGradient><rect "
        "x='16' y='16' width='96' height='80' fill='url(#g)'/>",
        "<radialGradient id='g' fx='.8' fy='.5' fr='.2' r='.5'><stop stop-color='#e04020'/><stop "
        "offset='1' stop-color='#2040e0'/></radialGradient><rect x='16' y='16' width='96' "
        "height='80' fill='url(#g)'/>",
        "<radialGradient id='g' fx='1.2' fy='.5' fr='.2' r='.3'><stop stop-color='#e04020'/><stop "
        "offset='1' stop-color='#2040e0'/></radialGradient><rect x='16' y='16' width='96' "
        "height='80' fill='url(#g)'/>",
        "<radialGradient id='g' ><stop stop-color='blue'/></radialGradient><rect x='16' y='16' "
        "width='96' height='80' fill='url(#g)'/>",
        "<radialGradient id='g' ></radialGradient><rect x='16' y='16' width='96' height='80' "
        "fill='url(#g)'/>",
        "<radialGradient id='g' gradientTransform='scale(0)'><stop stop-color='#e04020'/><stop "
        "offset='1' stop-color='#2040e0'/></radialGradient><rect x='16' y='16' width='96' "
        "height='80' fill='url(#g)'/>",
        "<radialGradient id='g' cx='.25'><stop stop-color='red'/><stop offset='.5' "
        "stop-color='red'/><stop offset='.5' stop-color='blue'/><stop offset='1' "
        "stop-color='blue'/></radialGradient><rect x='16' y='16' width='96' height='80' "
        "fill='url(#g)'/>",
        "<radialGradient id='base' cx='.3' cy='.4' r='.6'><stop stop-color='#e04020'/><stop "
        "offset='1' stop-color='#2040e0'/></radialGradient><radialGradient id='g' href='#base' "
        "fx='.2'/><rect x='16' y='16' width='96' height='80' fill='url(#g)'/>",
        "<linearGradient id='base' spreadMethod='reflect'><stop stop-color='#e04020'/><stop "
        "offset='1' stop-color='#2040e0'/></linearGradient><radialGradient id='g' href='#base' "
        "r='.2'/><rect x='16' y='16' width='96' height='80' fill='url(#g)'/>",
        "<radialGradient id='base'><stop stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></radialGradient><linearGradient id='g' href='#base'/><rect x='16' "
        "y='16' width='96' height='80' fill='url(#g)'/>",
        "<radialGradient id='g' gradientTransform='scale(0)' "
        "style='transform:translate(.1,.1)'><stop stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></radialGradient><rect x='16' y='16' width='96' height='80' "
        "fill='url(#g)'/>",
        "<radialGradient id='g' ><stop stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></radialGradient><g opacity='.5' transform='translate(8 8) scale(.8 "
        "1.1)'><rect x='16' y='16' width='96' height='80' fill='url(#g)'/></g>",
        "<radialGradient id='g' gradientUnits='userSpaceOnUse' cx='24' cy='16' r='12'><stop "
        "stop-color='#e04020'/><stop offset='1' stop-color='#2040e0'/></radialGradient><svg x='16' "
        "y='16' width='96' height='64' viewBox='0 0 48 32'><rect width='48' height='32' "
        "fill='url(#g)'/></svg>",
        "<radialGradient id='g' r='.3' fr='0.3' fx='.7' fy='.5'><stop stop-color='#e04020'/><stop "
        "offset='1' stop-color='#2040e0'/></radialGradient><rect x='16' y='16' width='96' "
        "height='80' fill='url(#g)'/>",
        "<radialGradient id='g' r='0' fr='.3'><stop stop-color='#e04020'/><stop offset='1' "
        "stop-color='#2040e0'/></radialGradient><rect x='16' y='16' width='96' height='80' "
        "fill='url(#g)'/>",
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
        snprintf(file, sizeof(file), "%s/radial-%02zu.html", directory, i);
        FILE *f = fopen(file, "wb");
        assert(f && fputs(html, f) >= 0 && !fclose(f));
        yonder_svg_document_t *document = NULL;
        assert(yonder_svg_document_parse(xml, strlen(xml), NULL, NULL, &document, NULL) == SVG_OK);
        yonder_svg_pixels_t actual, reference;
        svg_status_t status = image_draw(document, &ledger, &actual);
        if (status != SVG_OK)
            fprintf(stderr, "radial %zu status %d\n", i, status);
        assert(status == SVG_OK);
        Fixture inline_doc = fixture_make(html);
        assert(draw_fixture(&inline_doc, &ledger, &reference, inline_doc.box->rect) == SVG_OK);
        assert(actual.bytes == reference.bytes &&
               !memcmp(actual.pixels, reference.pixels, actual.bytes));
        snprintf(name, sizeof(name), "radial-%02zu", i);
        save_ppm(directory, name, &actual);
        os64_geometry_memory_t memory = {&ledger, svg_alloc_test, svg_free_test};
        yonder_svg_pixels_free(&memory, &actual);
        yonder_svg_pixels_free(&memory, &reference);
        fixture_free(&inline_doc);
        yonder_svg_document_free(document);
        assert(live == baseline && !ledger.live);
    }
    printf("SVG radial gradients: %zu inline/image pairs pass\n",
           sizeof(bodies) / sizeof(bodies[0]));
}
