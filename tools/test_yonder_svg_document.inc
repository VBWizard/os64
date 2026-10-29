#include "svg_document.h"
#include <os64/arena.h>
static yonder_svg_image_view_t image_view = {.width = 128, .height = 128};
static svg_status_t image_draw(yonder_svg_document_t *d, SvgLedger *l, yonder_svg_pixels_t *p)
{
    os64_geometry_memory_t m = {l, svg_alloc_test, svg_free_test};
    return yonder_svg_document_render(d, &image_view, &m, os64_freetype_geometry_v1(), p);
}
static void document_cases(const char *dir)
{
    static const struct {
        const char *attrs, *body, *reference;
    } cases[] = {
        {"", "<rect x='16' y='16' width='64' height='48' fill='blue'/>",
         "<rect x='16' y='16' width='64' height='48' fill='blue'/>"},
        {"",
         "<defs><rect id='s' width='64' height='48' fill='blue'/></defs><use "
         "xmlns:l='http://www.w3.org/1999/xlink' l:href='#s' x='16' y='16'/>",
         "<rect x='16' y='16' width='64' height='48' fill='blue'/>"},
        {"style='color:blue'",
         "<style>:root{color:blue} rect{fill:currentColor}</style><rect x='16' y='16' width='64' "
         "height='48'/>",
         "<rect x='16' y='16' width='64' height='48' fill='blue'/>"},
        {"",
         "<style><![CDATA[:root{--paint:blue} .Case {fill:var(--paint)} .case "
         "{fill:red}]]></style><rect class='Case' x='16' y='16' width='64' height='48'/>",
         "<rect x='16' y='16' width='64' height='48' fill='blue'/>"},
        {"opacity='.5'",
         "<rect width='64' height='64' fill='red'/><rect width='64' height='64' fill='blue'/>",
         "<rect width='64' height='64' fill='blue' opacity='.5'/>"},
        {"viewBox='0 0 64 64' transform='scale(.5)'", "<rect width='64' height='64' fill='blue'/>",
         "<rect x='32' y='32' width='64' height='64' fill='blue'/>"},
        {"viewBox='0 0 64 64' transform='translate(40)' style='transform:scale(.5)'",
         "<rect width='64' height='64' fill='blue'/>",
         "<rect x='32' y='32' width='64' height='64' fill='blue'/>"},
        {"viewBox='0 0 64 64' transform='translate(8)'",
         "<rect width='64' height='64' fill='blue'/>",
         "<rect x='8' width='120' height='128' fill='blue'/>"},
        {"viewBox='0 0 64 64' clip-path='url(#c)'",
         "<clipPath id='c' clipPathUnits='objectBoundingBox'><rect width='.5' "
         "height='1'/></clipPath><rect x='8' y='8' width='32' height='24' fill='blue'/>",
         "<rect x='16' y='16' width='48' height='48' fill='blue'/>"},
        {"onload=\"document.getElementById('r').setAttribute('fill','red')\"",
         "<script><![CDATA[throw new Error('must not execute')]]></script><script "
         "href='https://example.invalid/a.js'/><rect id='r' x='16' y='16' width='64' height='48' "
         "fill='blue'><set attributeName='fill' to='red' begin='0s'/></rect>",
         "<rect x='16' y='16' width='64' height='48' fill='blue'/>"},
        {"",
         "<?xml-stylesheet href='file:///secret.css' type='text/css'?><style>@import "
         "url(https://example.invalid/a.css);rect{fill:blue}</style><rect x='16' y='16' width='64' "
         "height='48'/>",
         "<rect x='16' y='16' width='64' height='48' fill='blue'/>"},
        {"",
         "<style type='text/plain'>rect{fill:red}</style><style "
         "media='(min-width:100px)'>rect{fill:blue}</style><rect x='16' y='16' width='64' "
         "height='48'/>",
         "<rect x='16' y='16' width='64' height='48' fill='blue'/>"},
        {"",
         "<metadata><x:data "
         "xmlns:x='urn:data'><x:script>anything</x:script></x:data></metadata><rect x='16' y='16' "
         "width='64' height='48' fill='blue'/>",
         "<rect x='16' y='16' width='64' height='48' fill='blue'/>"},
        {"",
         "<use href='https://example.invalid/a.svg#shape'/><use href='file:///secret.svg#s'/><rect "
         "x='16' y='16' width='64' height='48' fill='blue'/>",
         "<rect x='16' y='16' width='64' height='48' fill='blue'/>"},
        {"",
         "<defs><rect id='s' width='128' height='128' fill='red'/></defs><use "
         "xmlns:xlink='urn:fake' xlink:href='#s'/><rect x='16' y='16' width='64' height='48' "
         "fill='blue'/>",
         "<rect x='16' y='16' width='64' height='48' fill='blue'/>"},
        {"",
         "<defs><rect id='s' width='128' height='128' fill='red'/></defs><use "
         "xmlns:l='http://www.w3.org/1999/xlink' l:href='#s' href=''/><rect x='16' y='16' "
         "width='64' height='48' fill='blue'/>",
         "<rect x='16' y='16' width='64' height='48' fill='blue'/>"},
        {"viewBox='0 0 64 64'",
         "<svg x='8' y='8' width='40' height='24' viewBox='0 0 20 12'><rect width='20' height='12' "
         "fill='blue'/></svg>",
         "<rect x='16' y='16' width='80' height='48' fill='blue'/>"},
        {"viewBox='0 0 64 64' clip-path='url(#c)'",
         "<clipPath id='c'><rect x='16' y='16' width='32' height='32'/></clipPath>"
         "<rect width='64' height='64' fill='blue'/>",
         "<rect x='16' y='16' width='32' height='32' fill='blue'/>"},
        {"viewBox='0 0 64 32' clip-path='url(#c)' transform='translate(8)'",
         "<clipPath id='c' clipPathUnits='objectBoundingBox' transform='translate(8)'>"
         "<rect width='.5' height='1'/></clipPath><rect width='64' height='32' fill='blue'/>",
         "<rect x='16' y='32' width='64' height='64' fill='blue'/>"},
        {"viewBox='0 0 64 64'",
         "<clipPath id='c' clipPathUnits='objectBoundingBox'><rect width='.5' height='1'/>"
         "</clipPath><g clip-path='url(#c)'><rect x='8' y='8' width='32' height='24' "
         "fill='blue'/></g>",
         "<rect x='16' y='16' width='32' height='48' fill='blue'/>"},
    };
    SvgLedger l = {.cap = 64 << 20};
    size_t baseline = live;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char xml[4096], html[4096], path[1024], name[64];
        snprintf(xml, sizeof(xml),
                 "<svg xmlns='http://www.w3.org/2000/svg' width='128' height='128' %s>%s</svg>",
                 cases[i].attrs, cases[i].body);
        yonder_svg_document_t *d = NULL;
        os64_html_xml_result_t diag;
        assert(yonder_svg_document_parse(xml, strlen(xml), NULL, NULL, &d, &diag) == SVG_OK && d);
        assert(diag.status == XML_DONE && yonder_svg_document_cost(d) > 0);
        snprintf(path, sizeof(path), "%s/document-%02zu.svg", dir, i);
        FILE *f = fopen(path, "wb");
        assert(f && fputs(xml, f) >= 0 && !fclose(f));
        memset(xml, 'X', strlen(xml)); // The document owns its input and styles.
        yonder_svg_pixels_t actual, reference;
        svg_status_t status = image_draw(d, &l, &actual);
        if (status != SVG_OK)
            fprintf(stderr, "document %zu status %d\n", i, status);
        assert(status == SVG_OK);
        snprintf(html, sizeof(html),
                 "<!doctype html><body style='margin:0'><svg id=art width=128 height=128>%s</svg>",
                 cases[i].reference);
        snprintf(path, sizeof(path), "%s/document-%02zu-reference.html", dir, i);
        f = fopen(path, "wb");
        assert(f && fputs(html, f) >= 0 && !fclose(f));
        Fixture ref = fixture_make(html);
        assert(draw_fixture(&ref, &l, &reference, ref.box->rect) == SVG_OK);
        if (actual.bytes != reference.bytes ||
            memcmp(actual.pixels, reference.pixels, actual.bytes))
            fprintf(stderr, "document %zu differs\n", i);
        assert(actual.bytes == reference.bytes &&
               !memcmp(actual.pixels, reference.pixels, actual.bytes));
        snprintf(name, sizeof(name), "document-%02zu", i);
        save_ppm(dir, name, &actual);
        yonder_svg_document_free(d); // Pixels retain their separate ownership.
        os64_geometry_memory_t m = {&l, svg_alloc_test, svg_free_test};
        yonder_svg_pixels_free(&m, &actual);
        yonder_svg_pixels_free(&m, &reference);
        fixture_free(&ref);
        assert(!l.live && live == baseline);
    }
    printf("SVG documents: %zu exact standalone/inline references pass\n",
           sizeof(cases) / sizeof(cases[0]));
}
typedef struct {
    size_t polls, stop;
} ImageCancel;
static bool image_cancel(void *opaque)
{
    ImageCancel *c = opaque;
    return ++c->polls == c->stop;
}
static void document_foreign_metadata(void)
{
    const char *foreign[] = {
        "<e:namedview xmlns:e='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' "
        "pagecolor='#fff'/>",
        "<rect xmlns='urn:editor' style='transform-origin:50% 50%'>"
        "<path xmlns='http://www.w3.org/2000/svg' d='malformed'/></rect>",
        "<foreign xmlns=''><rect xmlns='http://www.w3.org/2000/svg' "
        "width='128' height='128' fill='red'/></foreign>",
    };
    const char *contexts[] = {
        "%s<rect x='16' y='16' width='64' height='48' fill='blue'/>",
        "<rect x='16' y='16' width='64' height='48' fill='blue'/>%s",
        "<clipPath id='c'>%s<rect x='16' y='16' width='64' height='48'/></clipPath>"
        "<rect width='128' height='128' fill='blue' clip-path='url(#c)'/>",
        "<linearGradient id='g'>%s<stop stop-color='blue'/></linearGradient>"
        "<rect x='16' y='16' width='64' height='48' fill='url(#g)'/>",
        "<linearGradient id='g'><stop stop-color='blue'/>%s</linearGradient>"
        "<rect x='16' y='16' width='64' height='48' fill='url(#g)'/>",
    };
    size_t baseline = live;
    SvgLedger ledger = {.cap = 64 << 20};
    os64_geometry_memory_t memory = {&ledger, svg_alloc_test, svg_free_test};
    for (size_t i = 0; i < sizeof(foreign) / sizeof(foreign[0]); ++i)
        for (size_t j = 0; j < sizeof(contexts) / sizeof(contexts[0]); ++j) {
            char body[2048], xml[2304];
            snprintf(body, sizeof(body), contexts[j], foreign[i]);
            snprintf(xml, sizeof(xml), "<svg xmlns='http://www.w3.org/2000/svg'>%s</svg>", body);
            yonder_svg_document_t *d = NULL;
            assert(yonder_svg_document_parse(xml, strlen(xml), NULL, NULL, &d, NULL) == SVG_OK);
            yonder_svg_pixels_t p;
            svg_status_t status = image_draw(d, &ledger, &p);
            if (status != SVG_OK)
                fprintf(stderr, "foreign metadata %zu context %zu: %d\n", i, j, status);
            assert(status == SVG_OK);
            for (int y = 0; y < 128; ++y)
                for (int x = 0; x < 128; ++x)
                    assert(pixel(&p, x, y) ==
                           (x >= 16 && x < 80 && y >= 16 && y < 64 ? 0xff0000ff : 0));
            yonder_svg_pixels_free(&memory, &p);
            yonder_svg_document_free(d);
            assert(live == baseline && !ledger.live);
        }
    puts("SVG foreign metadata: 15 scene, clip and gradient contexts preserve artwork");
}
static void document_inert_clips(void)
{
    const char *names[] = {"script", "animate", "animateMotion", "animateTransform", "set", "discard"};
    const char *shape = "<rect x='16' y='16' width='64' height='48'/>";
    const char *art = "<rect width='128' height='128' fill='blue' clip-path='url(#c)'/>";
    size_t baseline = live;
    SvgLedger ledger = {.cap = 64 << 20};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        for (int position = 0; position < 3; ++position) {
            char source[2048], expected[1024];
            // A namespaced inert element must be skipped before style/geometry
            // resolution, including its children, regardless of sibling order.
            snprintf(source, sizeof(source),
                     "<svg xmlns='http://www.w3.org/2000/svg' xmlns:s='http://www.w3.org/2000/svg'>"
                     "<clipPath id='c'>%s<s:%s attributeName='opacity' to='0' begin='0s' "
                     "style='transform-origin:50%% 50%%'><rect width='128' height='128'/>"
                     "</s:%s>%s</clipPath>%s</svg>",
                     position == 1 ? shape : "", names[i], names[i],
                     position == 0 ? shape : "", art);
            snprintf(expected, sizeof(expected),
                     "<svg xmlns='http://www.w3.org/2000/svg'><clipPath id='c'>%s</clipPath>%s</svg>",
                     position == 2 ? "" : shape, art);
            yonder_svg_document_t *actual_doc = NULL, *expected_doc = NULL;
            assert(yonder_svg_document_parse(source, strlen(source), NULL, NULL,
                                              &actual_doc, NULL) == SVG_OK);
            assert(yonder_svg_document_parse(expected, strlen(expected), NULL, NULL,
                                              &expected_doc, NULL) == SVG_OK);
            yonder_svg_pixels_t actual, reference;
            svg_status_t status = image_draw(actual_doc, &ledger, &actual);
            if (status != SVG_OK)
                fprintf(stderr, "inert clip %s position %d: status %d\n", names[i], position, status);
            assert(status == SVG_OK);
            assert(image_draw(expected_doc, &ledger, &reference) == SVG_OK);
            assert(actual.bytes == reference.bytes &&
                   (!actual.bytes || !memcmp(actual.pixels, reference.pixels, actual.bytes)));
            os64_geometry_memory_t memory = {&ledger, svg_alloc_test, svg_free_test};
            yonder_svg_pixels_free(&memory, &actual);
            yonder_svg_pixels_free(&memory, &reference);
            yonder_svg_document_free(actual_doc);
            yonder_svg_document_free(expected_doc);
            assert(live == baseline && !ledger.live);
        }
    }
    const char *refused[] = {"<foreignObject/>", "<Set/>", "<rect width='8' height='8'/>"};
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        char source[1024];
        snprintf(source, sizeof(source),
                 "<svg xmlns='http://www.w3.org/2000/svg'><clipPath id='c'>%s%s</clipPath>%s</svg>",
                 shape, refused[i], art);
        yonder_svg_document_t *d = NULL;
        yonder_svg_pixels_t pixels;
        assert(yonder_svg_document_parse(source, strlen(source), NULL, NULL, &d, NULL) == SVG_OK);
        assert(image_draw(d, &ledger, &pixels) == SVG_UNSUPPORTED && !pixels.pixels);
        yonder_svg_document_free(d);
        assert(live == baseline && !ledger.live);
    }
    printf("SVG inert clips: 18 exact references and 3 refusal controls pass\n");
}
static void document_contracts(void)
{
    document_foreign_metadata();
    document_inert_clips();
    const char *xml = "<s:svg xmlns:s='http://www.w3.org/2000/svg' viewBox='0 0 64 64'>"
                      "<s:style>:root{color:blue} rect{fill:currentColor}</s:style>"
                      "<s:rect width='64' height='64'/></s:svg>";
    size_t baseline = live;
    yonder_svg_document_t *d = NULL;
    os64_html_xml_result_t diagnostic;
    size_t calls = s_mallocs;
    ImageCancel cancel = {0};
    assert(yonder_svg_document_parse(xml, strlen(xml), image_cancel, &cancel, &d, &diagnostic) ==
           SVG_OK);
    size_t parse_allocations = s_mallocs - calls, parse_polls = cancel.polls;
    yonder_svg_document_free(d);
    assert(live == baseline);
    for (size_t i = 0; i < parse_allocations; ++i) {
        s_alloc_left = (long)i;
        svg_status_t status = yonder_svg_document_parse(xml, strlen(xml), NULL, NULL, &d, NULL);
        s_alloc_left = -1;
        assert((status == SVG_NO_MEMORY || status == SVG_LIMIT) && !d && live == baseline);
    }
    for (size_t i = 1; i <= parse_polls; ++i) {
        cancel = (ImageCancel){.stop = i};
        assert(yonder_svg_document_parse(xml, strlen(xml), image_cancel, &cancel, &d, NULL) ==
               SVG_CANCELLED);
        assert(!d && live == baseline);
    }
    assert(yonder_svg_document_parse(xml, strlen(xml), NULL, NULL, &d, NULL) == SVG_OK);
    size_t document_live = live;
    SvgLedger l = {.cap = 64 << 20};
    yonder_svg_pixels_t pixels;
    cancel = (ImageCancel){0};
    image_view.cancelled = image_cancel;
    image_view.cancel_context = &cancel;
    size_t before = l.calls;
    calls = s_mallocs;
    assert(image_draw(d, &l, &pixels) == SVG_OK);
    assert(pixel(&pixels, 64, 64) == 0xff0000ff);
    size_t render_allocations = l.calls - before, cascade_allocations = s_mallocs - calls,
           render_polls = cancel.polls;
    pixels_free(&l, &pixels);
    image_view.cancelled = NULL;
    image_view.cancel_context = NULL;
    for (size_t i = 0; i < cascade_allocations; ++i) {
        s_alloc_left = (long)i;
        svg_status_t status = image_draw(d, &l, &pixels);
        s_alloc_left = -1;
        assert((status == SVG_NO_MEMORY || status == SVG_LIMIT) && !pixels.pixels && !l.live &&
               live == document_live);
    }
    for (size_t i = 1; i <= render_allocations; ++i)
        for (int admission = 0; admission < 2; ++admission) {
            l.admission = admission;
            l.fail = l.calls + i;
            assert(image_draw(d, &l, &pixels) == (admission ? SVG_LIMIT : SVG_NO_MEMORY));
            assert(!pixels.pixels && !l.live && live == document_live);
            l.fail = 0;
        }
    for (size_t i = 1; i <= render_polls; ++i) {
        cancel = (ImageCancel){.stop = i};
        image_view.cancelled = image_cancel;
        image_view.cancel_context = &cancel;
        assert(image_draw(d, &l, &pixels) == SVG_CANCELLED && !pixels.pixels && !l.live &&
               live == document_live);
    }
    image_view = (yonder_svg_image_view_t){.width = 64, .height = 32};
    assert(image_draw(d, &l, &pixels) == SVG_OK && pixels.rect.w == 64 && pixels.rect.h == 32);
    assert(pixel(&pixels, 32, 16) == 0xff0000ff && !pixel(&pixels, 0, 16));
    pixels_free(&l, &pixels);
    image_view.width = 0;
    assert(image_draw(d, &l, &pixels) == SVG_OK && !pixels.pixels && !l.live);
    image_view.width = 16385;
    assert(image_draw(d, &l, &pixels) == SVG_LIMIT && !pixels.pixels && !l.live);
    yonder_svg_document_free(d);
    image_view = (yonder_svg_image_view_t){.width = 128, .height = 128};
    const char *bad[] = {"",
                         "<svg>",
                         "<svg xmlns='http://www.w3.org/2000/svg'><rect></svg>",
                         "<svg xmlns='http://www.w3.org/2000/svg' a='1' a='2'/>",
                         "<svg xmlns='http://www.w3.org/2000/svg'/><svg/>",
                         "<svg xmlns='http://www.w3.org/2000/svg'>&bogus;</svg>"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(yonder_svg_document_parse(bad[i], strlen(bad[i]), NULL, NULL, &d, &diagnostic) ==
               SVG_MALFORMED);
        assert(!d && diagnostic.status == XML_MALFORMED && live == baseline);
    }
    const char *wrong[] = {"<svg/>", "<SVG xmlns='http://www.w3.org/2000/svg'/>",
                           "<svg xmlns='urn:wrong'/>",
                           "<?xml version='1.0' encoding='Latin-1'?><svg/>"};
    for (size_t i = 0; i < sizeof(wrong) / sizeof(wrong[0]); ++i) {
        assert(yonder_svg_document_parse(wrong[i], strlen(wrong[i]), NULL, NULL, &d, NULL) ==
               SVG_UNSUPPORTED);
        assert(!d && live == baseline);
    }
    assert(yonder_svg_document_parse(NULL, 1, NULL, NULL, &d, NULL) == SVG_BAD_ARGUMENT && !d);
    assert(yonder_svg_document_parse("", YONDER_SVG_DOCUMENT_BYTES + 1, NULL, NULL, &d, NULL) ==
               SVG_LIMIT &&
           !d);
    // Unknown element spelling is omitted, not treated as a supported shape;
    // an unsupported effect on a recognized shape still refuses the root.
    const char *unsupported[] = {"<RECT width='10' height='10'/>",
                             "<foreignObject/>",
                             "<rect width='10' height='10' style='transform-origin:50% 50%'/>",
                             "<image href='https://example.invalid/image.png'/>"};
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
        char source[1024];
        snprintf(source, sizeof(source), "<svg xmlns='http://www.w3.org/2000/svg'>%s</svg>",
                 unsupported[i]);
        assert(yonder_svg_document_parse(source, strlen(source), NULL, NULL, &d, NULL) == SVG_OK);
        if (i == 2) {
            assert(image_draw(d, &l, &pixels) == SVG_UNSUPPORTED && !pixels.pixels && !l.live);
        } else {
            assert(image_draw(d, &l, &pixels) == SVG_OK && pixels.omitted_subtrees == 1);
            for (size_t p = 0; p < pixels.bytes / 4; ++p)
                assert(pixels.pixels[p] == 0);
            pixels_free(&l, &pixels);
        }
        yonder_svg_document_free(d);
        assert(live == baseline);
    }
    const char *media = "<svg xmlns='http://www.w3.org/2000/svg'><style>rect{fill:blue}"
                        "@media (prefers-color-scheme:dark){rect{fill:red}}"
                        "@media (scripting:enabled){rect{fill:green}}</style><rect width='128' "
                        "height='128'/></svg>";
    assert(yonder_svg_document_parse(media, strlen(media), NULL, NULL, &d, NULL) == SVG_OK);
    assert(image_draw(d, &l, &pixels) == SVG_OK && pixel(&pixels, 32, 32) == 0xff0000ff);
    pixels_free(&l, &pixels);
    image_view.dark = true;
    assert(image_draw(d, &l, &pixels) == SVG_OK && pixel(&pixels, 32, 32) == 0xffff0000);
    pixels_free(&l, &pixels);
    image_view.dark = false;
    yonder_svg_document_t *other = NULL;
    const char *plain =
        "<svg xmlns='http://www.w3.org/2000/svg'><rect width='128' height='128'/></svg>";
    assert(yonder_svg_document_parse(plain, strlen(plain), NULL, NULL, &other, NULL) == SVG_OK);
    assert(image_draw(other, &l, &pixels) == SVG_OK && pixel(&pixels, 32, 32) == 0xff000000);
    pixels_free(&l, &pixels);
    yonder_svg_document_free(d);
    assert(image_draw(other, &l, &pixels) == SVG_OK && pixel(&pixels, 32, 32) == 0xff000000);
    pixels_free(&l, &pixels);
    yonder_svg_document_free(other);
    // UTF-16 bytes reach the same namespace-aware tree and renderer.
    size_t length = strlen(plain);
    unsigned char utf16[1024];
    assert(2 + length * 2 <= sizeof(utf16));
    for (int be = 0; be < 2; ++be) {
        utf16[0] = be ? 0xfe : 0xff;
        utf16[1] = be ? 0xff : 0xfe;
        for (size_t i = 0; i < length; ++i) {
            utf16[2 + 2 * i] = be ? 0 : plain[i];
            utf16[3 + 2 * i] = be ? plain[i] : 0;
        }
        assert(yonder_svg_document_parse(utf16, 2 + 2 * length, NULL, NULL, &d, NULL) == SVG_OK);
        assert(image_draw(d, &l, &pixels) == SVG_OK && pixel(&pixels, 32, 32) == 0xff000000);
        pixels_free(&l, &pixels);
        yonder_svg_document_free(d);
    }
    char many[4096] = "<svg xmlns='http://www.w3.org/2000/svg'>";
    for (size_t i = 0; i < YONDER_SVG_DOCUMENT_SHEETS + 1; ++i)
        strcat(many, "<style/> ");
    strcat(many, "</svg>");
    assert(yonder_svg_document_parse(many, strlen(many), NULL, NULL, &d, NULL) == SVG_LIMIT && !d);
    size_t bigsize = YONDER_SVG_DOCUMENT_STYLE_BYTES + 128;
    char *big = malloc(bigsize);
    assert(big);
    size_t prefix = (size_t)sprintf(big, "<svg xmlns='http://www.w3.org/2000/svg'><style>");
    memset(big + prefix, ' ', YONDER_SVG_DOCUMENT_STYLE_BYTES + 1);
    strcpy(big + prefix + YONDER_SVG_DOCUMENT_STYLE_BYTES + 1, "</style></svg>");
    assert(yonder_svg_document_parse(big, strlen(big), NULL, NULL, &d, NULL) == SVG_LIMIT && !d);
    free(big);
    assert(live == baseline && !l.live);
    printf("SVG document contracts: %zu parse/%zu cascade/%zu render allocation positions, %zu "
           "parse/%zu render cancellation positions; ownership, namespaces, encodings, isolation, "
           "media and limits pass\n",
           parse_allocations, cascade_allocations, render_allocations, parse_polls, render_polls);
}

static void image_fixed_dimension_contracts(void)
{
    // A bottom-right mark must move with the image, rather than be cropped
    // at its original coordinates when the host gives it a smaller box.
    static const struct {
        const char *attrs;
        int x, y, right, bottom;
    } cases[] = {
        {"width='120' height='60'", 40, 40, 80, 80},
        {"width='120'", 40, 30, 80, 60},
        {"height='60'", 60, 40, 80, 80},
        {"", 60, 30, 80, 60},
        {"width='100%' height='100%'", 60, 30, 80, 60},
        {"width='120' height='60' preserveAspectRatio='xMidYMid meet'", 40, 40, 80, 80},
        {"width='120' height='60' viewBox='0 0 120 60'", 40, 40, 80, 60},
        {"width='1.25in' height='45pt'", 40, 40, 80, 80},
        {"width='120' height='60' transform='translate(12)'", 52, 40, 80, 80},
    };
    size_t baseline = live;
    SvgLedger ledger = {.cap = 64 << 20};
    os64_geometry_memory_t memory = {&ledger, svg_alloc_test, svg_free_test};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char xml[1024];
        snprintf(xml, sizeof(xml), "<svg xmlns='http://www.w3.org/2000/svg' %s>"
                 "<style>rect{fill:red}@media(min-width:100px){rect{fill:blue}}</style>"
                 "<rect x='60' y='30' width='60' height='30'/></svg>", cases[i].attrs);
        yonder_svg_document_t *d = NULL;
        assert(yonder_svg_document_parse(xml, strlen(xml), NULL, NULL, &d, NULL) == SVG_OK);
        const uint32_t extents[] = {20, 80, 320};
        for (size_t z = 0; z < sizeof(extents) / sizeof(extents[0]); ++z) {
            uint32_t extent = extents[z];
            yonder_svg_image_view_t view = {.width = 80, .height = 80,
                .pixel_width = extent, .pixel_height = extent};
            yonder_svg_pixels_t p;
            assert(yonder_svg_document_render(d, &view, &memory,
                       os64_freetype_geometry_v1(), &p) == SVG_OK);
            for (int y = 0; y < p.rect.h; ++y)
                for (int x = 0; x < p.rect.w; ++x) {
                    double left = cases[i].x * extent / 80.0;
                    double top = cases[i].y * extent / 80.0;
                    double right = cases[i].right * extent / 80.0;
                    double bottom = cases[i].bottom * extent / 80.0;
                    // At 25%, fractional edges have antialias coverage;
                    // the interior and exterior still have exact colours.
                    if (fabs(x + .5 - left) < .5 || fabs(x + .5 - right) < .5 ||
                        fabs(y + .5 - top) < .5 || fabs(y + .5 - bottom) < .5)
                        continue;
                    bool inside = x + .5 >= left && x + .5 < right &&
                        y + .5 >= top && y + .5 < bottom;
                    uint32_t want = inside ? 0xffff0000 : 0;
                    if (pixel(&p, x, y) != want)
                        fprintf(stderr, "image fixed dimensions case %zu extent %u at %d,%d\n",
                                i, extent, x, y);
                    assert(pixel(&p, x, y) == want);
                }
            pixels_free(&ledger, &p);
        }
        yonder_svg_document_free(d);
        assert(live == baseline && !ledger.live);
    }
    puts("SVG image fixed dimensions: complete bounds, per-axis scaling, units, viewBox and media pass");
}
static void image_route_contracts(const char *directory)
{
    image_fixed_dimension_contracts();
    static const struct { const char *attrs, *body; uint32_t w, h; } sizes[] = {
        {"width='80' height='40'", "", 80, 40},
        {"viewBox='0 0 60 30'", "", 300, 150},
        {"viewBox='0 0 30 60'", "", 75, 150},
        {"width='80' viewBox='0 0 60 30'", "", 80, 40},
        {"height='40' viewBox='0 0 60 30'", "", 80, 40},
        {"", "", 300, 150},
        {"width='100%' height='100%' viewBox='0 0 20 20'", "", 150, 150},
        {"width='1in' height='36pt'", "", 96, 48},
        {"width='10' height='20' style='width:80px;height:40px'", "", 80, 40},
        {"width='10' height='20'", "<style>:root{width:80px;height:40px}</style>", 80, 40},
        {"width='0' height='20'", "", 0, 20},
    };
    size_t baseline = live;
    yonder_svg_document_t *d = NULL;
    char filename[1024];
    snprintf(filename,sizeof(filename),"%s/image-sizing.json",directory);
    FILE *manifest=fopen(filename,"w"); assert(manifest); fputs("[",manifest);
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); ++i) {
        char xml[1024];
        snprintf(xml,sizeof(xml),"<svg xmlns='http://www.w3.org/2000/svg' %s>%s</svg>",
                 sizes[i].attrs,sizes[i].body);
        assert(yonder_svg_document_parse(xml,strlen(xml),NULL,NULL,&d,NULL)==SVG_OK);
        uint32_t w,h;
        assert(yonder_svg_document_size(d,false,false,NULL,NULL,&w,&h)==SVG_OK);
        if(w!=sizes[i].w || h!=sizes[i].h) fprintf(stderr,"size %zu: %u,%u\n",i,w,h);
        assert(w==sizes[i].w && h==sizes[i].h);
        snprintf(filename,sizeof(filename),"%s/image-size-%02zu.svg",directory,i);
        FILE *source=fopen(filename,"w");assert(source);fputs(xml,source);assert(!fclose(source));
        fprintf(manifest,"%s{\"file\":\"image-size-%02zu.svg\",\"width\":%u,\"height\":%u}",i?",":"",i,w,h);
        size_t calls=s_mallocs;
        assert(yonder_svg_document_size(d,false,false,NULL,NULL,&w,&h)==SVG_OK && s_mallocs==calls);
        yonder_svg_document_free(d);
        assert(live==baseline);
    }
    fputs("]\n",manifest);assert(!fclose(manifest));
    const char *xml="<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 128 128'>"
        "<style>rect{fill:red}@media(min-width:100px){rect{fill:blue}}"
        "@media(prefers-color-scheme:dark){:root{width:80px;height:40px}}</style>"
        "<rect width='128' height='128'/></svg>";
    assert(yonder_svg_document_parse(xml,strlen(xml),NULL,NULL,&d,NULL)==SVG_OK);
    uint32_t w,h;
    assert(yonder_svg_document_size(d,false,false,NULL,NULL,&w,&h)==SVG_OK && w==150 && h==150);
    assert(yonder_svg_document_size(d,true,false,NULL,NULL,&w,&h)==SVG_OK && w==80 && h==40);
    SvgLedger ledger={.cap=64<<20};
    os64_geometry_memory_t memory={&ledger,svg_alloc_test,svg_free_test};
    for(uint32_t extent=32;extent<=512;extent*=4) {
        yonder_svg_image_view_t view={.width=128,.height=128,.pixel_width=extent,.pixel_height=extent};
        yonder_svg_pixels_t pixels;
        assert(yonder_svg_document_render(d,&view,&memory,os64_freetype_geometry_v1(),&pixels)==SVG_OK);
        assert(pixels.rect.w==(int32_t)extent && pixel(&pixels,extent/2,extent/2)==0xff0000ff);
        pixels_free(&ledger,&pixels);
    }
    yonder_svg_image_view_t invalid={.width=128,.height=128,.pixel_width=32};
    yonder_svg_pixels_t pixels;
    assert(yonder_svg_document_render(d,&invalid,&memory,os64_freetype_geometry_v1(),&pixels)==SVG_BAD_ARGUMENT && !pixels.pixels);
    invalid=(yonder_svg_image_view_t){.width=NAN,.height=128};
    assert(yonder_svg_document_render(d,&invalid,&memory,os64_freetype_geometry_v1(),&pixels)==SVG_LIMIT);
    yonder_svg_document_free(d);
    char many[8192]="<svg xmlns='http://www.w3.org/2000/svg'>";
    for(size_t i=0;i<YONDER_SVG_ATTRIBUTES+1;i++) strcat(many,"<rect style='fill:blue' width='1' height='1'/>");
    strcat(many,"</svg>");
    assert(yonder_svg_document_parse(many,strlen(many),NULL,NULL,&d,NULL)==SVG_OK);
    assert(yonder_svg_document_size(d,false,false,NULL,NULL,&w,&h)==SVG_LIMIT);
    assert(image_draw(d,&ledger,&pixels)==SVG_LIMIT && !pixels.pixels);
    yonder_svg_document_free(d);
    garb_parsed_t limited={0};
    assert(garb_parse_block_limited("fill:blue",9,0,&limited)==GARB_NO_MEMORY && limited.incomplete);
    garb_free(&limited);
    assert(garb_parse_block_limited("fill:blue",9,1024,&limited)!=GARB_OK || limited.incomplete);
    garb_free(&limited);
    assert(garb_parse_block_limited("fill:blue",9,2u<<20,&limited)==GARB_OK && !limited.incomplete);
    assert(os64_arena_stats(limited.arena).reserved_bytes<=(2u<<20));
    garb_free(&limited);
    assert(live==baseline && !ledger.live);
    puts("SVG image sizing: 11 natural sizes, preference cache, independent CSS/raster extents and bounded attribute cascades pass");
}
