/* Consumer contracts: fresh resolution precedes reuse, and outputs remain owned. */
static size_t cache_polls, cache_cancel_at;
static bool cancel_cache_test(void *context)
{
    (void)context;
    return ++cache_polls == cache_cancel_at;
}
static svg_status_t cache_image_draw(yonder_svg_document_t *doc, SvgLedger *ledger,
                                     yonder_svg_image_view_t view, yonder_svg_pixels_t *out)
{
    os64_geometry_memory_t memory = {ledger, svg_alloc_test, svg_free_test};
    return yonder_svg_document_render(doc, &view, &memory, os64_freetype_geometry_v1(), out);
}
static void svg_cache_adapter_contracts(void)
{
    size_t baseline = live;
    SvgLedger ledger = {.cap = 64u << 20};
    os64_geometry_memory_t memory = {&ledger, svg_alloc_test, svg_free_test};
    yonder_svg_cache_t *cache = yonder_svg_cache_create(&memory, 1u << 20, 8, 256u << 10);
    assert(cache);
    size_t create_calls = ledger.calls;
    yonder_svg_cache_free(cache);
    assert(!ledger.live);
    for (unsigned mode = 0; mode < 2; ++mode)
        for (size_t i = 1; i <= create_calls; ++i) {
            ledger.admission = mode;
            ledger.fail = ledger.calls + i;
            assert(!yonder_svg_cache_create(&memory, 1u << 20, 8, 256u << 10));
            assert(!ledger.live);
        }
    ledger.fail = 0;
    cache = yonder_svg_cache_create(&memory, 1u << 20, 8, 256u << 10);
    assert(cache);
    size_t empty = ledger.live;
    assert(empty == yonder_svg_cache_stats(cache).owned_bytes);
    const char *xml = "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>"
                      "<style>rect{fill:red}@media(prefers-color-scheme:dark){rect{fill:blue}}"
                      "@media(prefers-reduced-motion:reduce){rect{fill:green}}</style>"
                      "<rect width='64' height='64'/></svg>";
    yonder_svg_document_t *doc = NULL;
    assert(yonder_svg_document_parse(xml, strlen(xml), NULL, NULL, &doc, NULL) == SVG_OK);
    yonder_svg_image_view_t view = {.width = 64, .height = 64, .cache = cache};
    yonder_svg_pixels_t cold, warm, reference;
    assert(cache_image_draw(doc, &ledger, view, &cold) == SVG_OK);
    assert(cache_image_draw(doc, &ledger, view, &warm) == SVG_OK);
    assert(cold.pixels != warm.pixels && cold.bytes == warm.bytes &&
           !memcmp(cold.pixels, warm.pixels, cold.bytes));
    yonder_svg_cache_stats_t stats = yonder_svg_cache_stats(cache);
    assert(stats.rasterizations == 1 && stats.raster.stores == 1 && stats.raster.hits == 1);
    yonder_svg_pixels_free(&memory, &warm);
    yonder_svg_pixels_free(&memory, &cold);
    // Copied keys/pixels survive source destruction, without retaining its tree.
    yonder_svg_document_free(doc);
    assert(yonder_svg_document_parse(xml, strlen(xml), NULL, NULL, &doc, NULL) == SVG_OK);
    assert(cache_image_draw(doc, &ledger, view, &warm) == SVG_OK);
    assert(yonder_svg_cache_stats(cache).raster.hits == 2);
    yonder_svg_pixels_free(&memory, &warm);
    for (unsigned i = 0; i < 4; ++i) {
        view.dark = i == 0;
        view.reduced_motion = i == 1;
        view.pixel_width = view.pixel_height = i == 2 ? 256 : i == 3 ? 16 : 64;
        assert(cache_image_draw(doc, &ledger, view, &warm) == SVG_OK);
        yonder_svg_image_view_t plain = view;
        plain.cache = NULL;
        assert(cache_image_draw(doc, &ledger, plain, &reference) == SVG_OK);
        assert(warm.bytes == reference.bytes && !memcmp(warm.pixels, reference.pixels, warm.bytes));
        yonder_svg_pixels_free(&memory, &warm);
        yonder_svg_pixels_free(&memory, &reference);
    }
    view = (yonder_svg_image_view_t){.width = 64, .height = 64, .cache = cache};
    uint64_t rendered = yonder_svg_cache_stats(cache).rasterizations;
    assert(cache_image_draw(doc, &ledger, view, &warm) == SVG_OK);
    assert(yonder_svg_cache_stats(cache).rasterizations == rendered);
    yonder_svg_pixels_free(&memory, &warm);
    assert(ledger.live == yonder_svg_cache_stats(cache).owned_bytes);
    // Cancellation can happen during fresh preparation, a hit copy, or admission.
    view.cancelled = cancel_cache_test;
    cache_cancel_at = cache_polls = 0;
    assert(cache_image_draw(doc, &ledger, view, &warm) == SVG_OK);
    size_t warm_polls = cache_polls;
    yonder_svg_pixels_free(&memory, &warm);
    for (size_t i = 1; i <= warm_polls; ++i) {
        cache_polls = 0;
        cache_cancel_at = i;
        assert(cache_image_draw(doc, &ledger, view, &warm) == SVG_CANCELLED && !warm.pixels);
        assert(ledger.live == yonder_svg_cache_stats(cache).owned_bytes);
    }
    yonder_svg_cache_trim(cache);
    assert(ledger.live == empty);
    cache_cancel_at = cache_polls = 0;
    assert(cache_image_draw(doc, &ledger, view, &cold) == SVG_OK);
    size_t cold_polls = cache_polls;
    yonder_svg_pixels_free(&memory, &cold);
    for (size_t i = 1; i <= cold_polls; ++i) {
        yonder_svg_cache_trim(cache);
        cache_polls = 0;
        cache_cancel_at = i;
        assert(cache_image_draw(doc, &ledger, view, &cold) == SVG_CANCELLED && !cold.pixels);
        assert(yonder_svg_cache_stats(cache).raster.entries == 0 && ledger.live == empty);
    }
    view.cancelled = NULL;
    cache_cancel_at = 0;
    // Physical and budget refusals may bypass optimization, but cannot publish
    // partial pixels or leak transient allocations. Later calls can retry.
    size_t calls = ledger.calls;
    assert(cache_image_draw(doc, &ledger, view, &cold) == SVG_OK);
    size_t allocations = ledger.calls - calls;
    yonder_svg_pixels_free(&memory, &cold);
    for (unsigned mode = 0; mode < 2; ++mode)
        for (size_t i = 1; i <= allocations; ++i) {
            yonder_svg_cache_trim(cache);
            ledger.admission = mode;
            ledger.fail = ledger.calls + i;
            svg_status_t status = cache_image_draw(doc, &ledger, view, &cold);
            ledger.fail = 0;
            if (status == SVG_OK) {
                assert(pixel(&cold, 32, 32) == 0xffff0000);
                yonder_svg_pixels_free(&memory, &cold);
            } else
                assert((status == SVG_LIMIT || status == SVG_NO_MEMORY) && !cold.pixels);
            assert(ledger.live == yonder_svg_cache_stats(cache).owned_bytes);
            assert(cache_image_draw(doc, &ledger, view, &warm) == SVG_OK);
            assert(pixel(&warm, 32, 32) == 0xffff0000);
            yonder_svg_pixels_free(&memory, &warm);
        }
    yonder_svg_cache_free(cache);
    assert(!ledger.live);
    // Too-small admission/key bounds are optimization failures, not SVG failures.
    for (unsigned i = 0; i < 2; ++i) {
        cache = yonder_svg_cache_create(&memory, i ? 1u << 20 : 1, 1, i ? 1 : 256u << 10);
        assert(cache);
        view.cache = cache;
        assert(cache_image_draw(doc, &ledger, view, &cold) == SVG_OK);
        assert(pixel(&cold, 32, 32) == 0xffff0000);
        yonder_svg_pixels_free(&memory, &cold);
        assert(yonder_svg_cache_stats(cache).bypasses &&
               !yonder_svg_cache_stats(cache).raster.entries);
        yonder_svg_cache_free(cache);
        assert(!ledger.live);
    }
    yonder_svg_document_free(doc);
    assert(live == baseline);
    printf("SVG cache adapter: owned hits, media/zoom variants, %zu allocation cuts and "
           "%zu cold/%zu warm cancellation cuts pass\n",
           allocations, cold_polls, warm_polls);
}
