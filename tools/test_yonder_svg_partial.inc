// The supported drawing is the oracle: omitted subtrees must leave its pixels
// unchanged, including descendants that would otherwise paint over it.
static void partial_element_cases(void)
{
    static const char *omitted[] = {
        "<text x='8' y='24'>A label</text>",
        "<image href='https://example.invalid/never-fetch.png' width='64' height='64'/>",
        "<foreignObject width='64' height='64'><div xmlns='http://www.w3.org/1999/xhtml'>"
        "<svg xmlns='http://www.w3.org/2000/svg'><rect width='64' height='64' fill='red'/></svg>"
        "</div></foreignObject>",
        "<unknown><rect width='64' height='64' fill='red'/></unknown>",
        "<text transform='not-a-transform(' fill='url(#absent)' filter='url(#f)' "
        "style='stroke-width:2em'>Omitted as a whole</text>",
        "<defs><text id='label'>Shared</text></defs><use href='#label'/><use href='#label'/>",
        "<text display='none' transform='bad('>Hidden</text>",
        "<g display='none'><text>Hidden subtree</text></g>",
        "<defs><text>Definition only</text></defs><pattern/><mask/><filter/><marker/>",
    };
    const char *art = "<rect x='8' y='8' width='48' height='48' fill='blue'/>";
    SvgLedger ledger = {.cap = 64u << 20};
    os64_geometry_memory_t memory = {&ledger, svg_alloc_test, svg_free_test};
    size_t baseline = live;
    for (size_t i = 0; i < sizeof(omitted) / sizeof(omitted[0]); ++i) {
        char page[2048], reference[512];
        snprintf(page, sizeof(page), "<svg xmlns='http://www.w3.org/2000/svg' id='art' "
                 "width='64' height='64'>%s%s</svg>", art, omitted[i]);
        snprintf(reference, sizeof(reference), "<svg xmlns='http://www.w3.org/2000/svg' id='art' "
                 "width='64' height='64'>%s</svg>", art);
        Fixture actual = fixture_make(page), expected = fixture_make(reference);
        for (unsigned zoom = 250; zoom <= 4000; zoom *= 2) {
            layout_fixture(&actual, 256, zoom, true);
            layout_fixture(&expected, 256, zoom, true);
            yonder_svg_pixels_t a, b;
            svg_status_t status = draw_fixture(&actual, &ledger, &a, actual.box->rect);
            if (status != SVG_OK)
                fprintf(stderr, "partial inline case %zu zoom %u: %d\n", i, zoom, status);
            assert(status == SVG_OK);
            assert(a.omitted_subtrees == (i == 5 ? 2u : i < 6 ? 1u : 0u));
            assert(draw_fixture(&expected, &ledger, &b, expected.box->rect) == SVG_OK);
            assert(a.bytes == b.bytes && !memcmp(a.pixels, b.pixels, a.bytes) && !b.omitted_subtrees);
            yonder_svg_pixels_free(&memory, &a);
            yonder_svg_pixels_free(&memory, &b);
            assert(!ledger.live);
        }
        fixture_free(&actual);
        fixture_free(&expected);
        yonder_svg_document_t *a_doc = NULL, *b_doc = NULL;
        assert(yonder_svg_document_parse(page, strlen(page), NULL, NULL, &a_doc, NULL) == SVG_OK);
        assert(yonder_svg_document_parse(reference, strlen(reference), NULL, NULL, &b_doc, NULL) == SVG_OK);
        yonder_svg_pixels_t a, b;
        assert(image_draw(a_doc, &ledger, &a) == SVG_OK);
        assert(a.omitted_subtrees == (i == 5 ? 2u : i < 6 ? 1u : 0u));
        assert(image_draw(b_doc, &ledger, &b) == SVG_OK);
        assert(a.bytes == b.bytes && !memcmp(a.pixels, b.pixels, a.bytes) && !b.omitted_subtrees);
        yonder_svg_pixels_free(&memory, &a);
        yonder_svg_pixels_free(&memory, &b);
        assert(!ledger.live);
        yonder_svg_document_free(a_doc);
        yonder_svg_document_free(b_doc);
        assert(!ledger.live && live == baseline);
    }
    puts("SVG partial elements: inline zoom and isolated image references preserve supported pixels");
}

// Omitting a drawing occurrence must not weaken referenced resource validation
// or turn a cancelled/failed job into a partially published success.
static void partial_element_contracts(void)
{
    const char *refused[] = {
        "<pattern id='p'/><rect width='64' height='64' fill='url(#p)'/>",
        "<rect width='64' height='64' filter='url(#f)'/>",
        "<rect width='64' height='64' mask='url(#m)'/>",
        "<clipPath id='c'><text>Clip</text></clipPath>"
        "<rect width='64' height='64' clip-path='url(#c)'/>",
        "<linearGradient id='p'><text>Not a stop</text></linearGradient>"
        "<rect width='64' height='64' fill='url(#p)'/>",
    };
    SvgLedger ledger = {.cap = 64u << 20};
    size_t baseline = live;
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        char source[2048];
        snprintf(source, sizeof(source), "<svg xmlns='http://www.w3.org/2000/svg' id='art' "
                 "width='64' height='64'><text>Omit before refusal</text>%s</svg>", refused[i]);
        Fixture f = fixture_make(source);
        yonder_svg_pixels_t pixels;
        assert(draw_fixture(&f, &ledger, &pixels, f.box->rect) == SVG_UNSUPPORTED);
        assert(!pixels.pixels && !pixels.omitted_subtrees && !ledger.live);
        fixture_free(&f);
        yonder_svg_document_t *doc = NULL;
        assert(yonder_svg_document_parse(source, strlen(source), NULL, NULL, &doc, NULL) == SVG_OK);
        assert(image_draw(doc, &ledger, &pixels) == SVG_UNSUPPORTED);
        assert(!pixels.pixels && !pixels.omitted_subtrees && !ledger.live);
        yonder_svg_document_free(doc);
        assert(live == baseline);
    }
    const char *source = "<svg xmlns='http://www.w3.org/2000/svg' id='art' width='64' height='64'>"
        "<text>Omit</text><rect width='64' height='64' fill='blue'/></svg>";
    Fixture f = fixture_make(source);
    yonder_svg_pixels_t pixels;
    size_t start = ledger.calls;
    assert(draw_fixture(&f, &ledger, &pixels, f.box->rect) == SVG_OK && pixels.omitted_subtrees == 1);
    size_t cuts = ledger.calls - start;
    pixels_free(&ledger, &pixels);
    for (size_t i = 1; i <= cuts; ++i) {
        for (unsigned mode = 0; mode < 2; ++mode) {
            ledger.fail = ledger.calls + i;
            ledger.admission = mode;
            assert(draw_fixture(&f, &ledger, &pixels, f.box->rect) ==
                   (mode ? SVG_LIMIT : SVG_NO_MEMORY));
            assert(!pixels.pixels && !pixels.omitted_subtrees && !ledger.live);
        }
    }
    ledger.fail = 0;
    fixture_free(&f);
    yonder_svg_document_t *doc = NULL;
    assert(yonder_svg_document_parse(source, strlen(source), NULL, NULL, &doc, NULL) == SVG_OK);
    yonder_svg_image_view_t view = {.width = 64, .height = 64, .cancelled = cancel_cache_test};
    cache_polls = cache_cancel_at = 0;
    assert(cache_image_draw(doc, &ledger, view, &pixels) == SVG_OK && pixels.omitted_subtrees == 1);
    size_t polls = cache_polls;
    pixels_free(&ledger, &pixels);
    for (size_t i = 1; i <= polls; ++i) {
        cache_polls = 0;
        cache_cancel_at = i;
        assert(cache_image_draw(doc, &ledger, view, &pixels) == SVG_CANCELLED);
        assert(!pixels.pixels && !pixels.omitted_subtrees && !ledger.live);
    }
    cache_cancel_at = 0;
    yonder_svg_document_free(doc);
    assert(live == baseline);
    printf("SVG partial contracts: %zu refusals in both routes, %zu allocation cuts in both modes, "
           "%zu cancellation cuts pass\n", sizeof(refused) / sizeof(refused[0]), cuts, polls);
}
