// flowdump — libflow in the guest, with the real faces.
//
// `flowdump FILE [WIDTH]` lays FILE out at WIDTH pixels (800 unless told)
// and prints the laid-out tree, the same dump the host harness diffs, so a
// page can be compared across the two. The page's `style` elements are
// cascaded at WIDTH x 600, as yonder cascades them; a linked sheet is not
// fetched. With no arguments it lays out a
// page of its own and checks what real fonts must make true of any layout:
// a heading taller than a paragraph, a long paragraph wrapping into more
// lines at a narrower width, every run released.
//
// The faces are yonder's: the provider's family cache (brief 04), serif,
// sans and mono in all four styles, from fonts.conf or the shipped DejaVu.

#include "html/html.h"
#include "page/page.h"
#include "flow/flow.h"
#include "garb/cascade.h"
#include "os64/font_config.h"
#include "os64/font_provider.h"
#include "os64/os64.h"
#include "os64/text.h"
#include "os64/slurp.h"
#include "os64/mem.h"
#include "os64/str.h"

#define FLOWDUMP_OK 0x0F10D000u
#define FLOWDUMP_FAIL 0x0F10D001u

static void require(bool ok, const char *why)
{
    if (ok)
        return;
    os64_printf("flowdump: FAIL %s\n", why);
    os64_exit(FLOWDUMP_FAIL);
}

static void *text_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return os64_malloc(n);
}

static void text_free(void *ctx, void *p, size_t n)
{
    (void)ctx;
    (void)n;
    os64_free(p);
}

static os64_text_context_t *s_text;
static os64_font_family_cache_t *s_families;

// yonder's callback, the same way: the list is borrowed until the next
// call, the metrics are the primary face's with a line at least its ascent
// and descent tall.
static os64_font_status_t fonts(void *ctx, const flow_family_list_t *families, bool bold,
                                bool italic, uint32_t px, os64_text_font_t *const **list,
                                size_t *count, os64_font_face_info_t *primary)
{
    (void)ctx;
    os64_font_family_list_t asked = {
        .names = (const os64_font_family_name_t *)families->names,
        .count = families->count,
        .generic = (os64_font_family_t)families->generic,
    };
    os64_font_role_view_t view;
    os64_font_status_t st = os64_font_family_open(s_families, &asked, bold, italic, px, &view);
    if (st != OS64_FONT_OK)
        return st;
    *list = view.fonts;
    *count = view.font_count;
    *primary = view.primary;
    if (primary->line_height < primary->ascent + primary->descent)
        primary->line_height = primary->ascent + primary->descent;
    return OS64_FONT_OK;
}

// The viewport a dump is judged in: the width asked for, and this height,
// which the cascade and the initial containing block share.
#define DUMP_VIEWPORT_H 600

static flow_env_t s_env = {
    .fonts = fonts,
    .viewport_font_px = 16,
    .viewport_height = DUMP_VIEWPORT_H,
    .default_generic = FLOW_GENERIC_SERIF,
    .ink = 0x000000,
    .link_ink = 0x0000ee,
    .paper = 0xffffff,
};

static void setup(void)
{
    os64_text_options_t o = {.memory = {NULL, text_alloc, text_free},
                             .backend = os64_freetype_backend_v1()};
    require(os64_text_create(&o, &s_text) == OS64_FONT_OK, "no text context");
    s_env.text = s_text;
    // The configured families or nothing: yonder falls back to what it can
    // open, but a fixture that dumps a layout must not quietly measure it
    // in other faces than the ones it names.
    os64_font_config_t config;
    os64_font_config_error_t error;
    require(os64_font_config_read(&config, &error) == OS64_FONT_CONFIG_OK &&
                os64_font_config_family_prepare(s_text, &config, &s_families, &error) ==
                    OS64_FONT_CONFIG_OK,
            "the font families will not open (see fonts.conf)");
}

static void teardown(void)
{
    os64_font_family_cache_destroy(s_families);
    require(os64_text_destroy(s_text) == OS64_FONT_OK, "a run outlived its tree");
}

static os64_html_document_t *parse(const char *html, size_t len)
{
    os64_html_parser_t *p = os64_html_parser_new(NULL);
    require(p != NULL, "parser");
    os64_html_parser_feed(p, html, len);
    os64_html_document_t *doc = os64_html_parser_finish(p);
    require(doc != NULL, "parse");
    return doc;
}

// A page's `style` sheets, parsed and cascaded: they outlive its layout.
typedef struct {
    garb_parsed_t parsed[32];
    garb_sheet_in_t in[32];
    int32_t n;
    garb_cascade_t *cascade;
} Sheets;

static void sheets_open(Sheets *sh, const os64_html_document_t *doc, const os64_page_t *page,
                        int32_t width)
{
    os64_memset(sh, 0, sizeof(*sh));
    for (int32_t i = 0; i < os64_page_nsheets(page) && sh->n < 32; i++) {
        const os64_page_sheet_t *one = os64_page_sheet(page, i);
        if (one->linked)
            continue;
        garb_parse_style_element(one->node, &sh->parsed[sh->n]);
        sh->in[sh->n] = (garb_sheet_in_t){.sheet = &sh->parsed[sh->n], .media = one->media};
        sh->n++;
    }
    sh->cascade = garb_cascade(sh->in, sh->n, doc, (garb_env_t){width, DUMP_VIEWPORT_H});
    require(sh->cascade != NULL, "cascade memory");
}

static void sheets_close(Sheets *sh)
{
    garb_cascade_free(sh->cascade);
    for (int32_t i = 0; i < sh->n; i++)
        garb_free(&sh->parsed[i]);
}

static flow_tree_t *lay(os64_html_document_t *doc, os64_page_t **page, Sheets *sh,
                        int32_t width)
{
    *page = os64_page_build(doc, "file:///flowdump", NULL, NULL);
    require(*page != NULL, "page model");
    sheets_open(sh, doc, *page, width);
    s_env.cascade = sh->cascade;
    flow_tree_t *t = flow_layout(doc, *page, width, &s_env);
    require(t != NULL, "flow_layout answered NULL");
    return t;
}

static char *dump_of(const flow_tree_t *t)
{
    int64_t need = flow_dump(t, NULL, 0);
    char *text = os64_malloc((size_t)need + 1);
    require(text != NULL, "dump memory");
    flow_dump(t, text, (size_t)need + 1);
    return text;
}

static int32_t count(const char *text, const char *word)
{
    int32_t n = 0;
    size_t wl = os64_strlen(word), tl = os64_strlen(text);
    for (size_t i = 0; i + wl <= tl; i++)
        if (os64_memcmp(text + i, word, wl) == 0)
            n++;
    return n;
}

// The first `text` line's height after `tag` in a dump: its content area.
static int32_t text_height_after(const char *dump, const char *tag)
{
    // Every comparison stays inside the dump: a fixed-length compare near
    // its end would read past the NUL.
    const char *at = dump, *end = dump + os64_strlen(dump);
    size_t tl = os64_strlen(tag);
    while (at + tl <= end && os64_memcmp(at, tag, tl) != 0)
        at++;
    while (at + 6 <= end && os64_memcmp(at, "text \"", 6) != 0)
        at++;
    if (at + 6 > end)
        return -1;
    at += 6;
    while (at < end && *at != '"')
        at += *at == '\\' && at + 1 < end ? 2 : 1;
    int32_t field[4] = {0}, k = 0;
    while (*at != '\0' && k < 4) {
        while (*at == ' ' || *at == '"')
            at++;
        int32_t v = 0;
        while (*at >= '0' && *at <= '9')
            v = v * 10 + (*at++ - '0');
        field[k++] = v;
    }
    return field[3];
}

static const char kPage[] =
    "<!doctype html><h1>Layout</h1>"
    "<p>A paragraph long enough to wrap: the quick brown fox jumps over the lazy dog, "
    "and then does it again, and then once more for luck, until the line has no choice.</p>"
    "<ul><li>one<li>two</ul><pre>tab\there</pre>"
    "<font face=arial><p>inside a font</p></font><p><a href=x>a link</a></p>"
    "<style>h2 { display: none }</style><h2>hidden by the page's sheet</h2>";

int main(int argc, char **argv)
{
    setup();
    if (argc >= 2) {
        uint8_t *html = NULL;
        size_t len = 0;
        require(os64_slurp(argv[1], 64u * 1024u * 1024u, &html, &len) == OS64_SLURP_OK,
                "cannot read the file");
        int32_t width = 800;
        if (argc >= 3) {
            width = 0;
            for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++)
                width = width * 10 + (*p - '0');
        }
        os64_html_document_t *doc = parse((const char *)html, len);
        os64_page_t *page;
        Sheets sh;
        flow_tree_t *t = lay(doc, &page, &sh, width);
        char *text = dump_of(t);
        os64_write(1, text, os64_strlen(text));
        os64_free(text);
        flow_free(t);
        sheets_close(&sh);
        os64_page_free(page);
        os64_html_document_free(doc);
        os64_free(html);
        teardown();
        return 0;
    }

    os64_html_document_t *doc = parse(kPage, sizeof(kPage) - 1);
    os64_page_t *page;
    Sheets sh_wide, sh_narrow;
    flow_tree_t *wide = lay(doc, &page, &sh_wide, 800);
    require(!flow_incomplete(wide), "incomplete at 800");
    char *a = dump_of(wide);
    os64_page_t *page2;
    flow_tree_t *narrow = lay(doc, &page2, &sh_narrow, 300);
    require(!flow_incomplete(narrow), "incomplete at 300");
    char *b = dump_of(narrow);
    int32_t h1 = text_height_after(a, "block h1"), p = text_height_after(a, "block p");
    int32_t lines_wide = count(a, "line "), lines_narrow = count(b, "line ");
    os64_printf("flowdump: 800px tall %d, 300px tall %d; h1 text %dpx, p text %dpx; "
                "%d lines at 800, %d at 300\n", (int)flow_height(wide), (int)flow_height(narrow),
                (int)h1, (int)p, (int)lines_wide, (int)lines_narrow);
    require(h1 > p && p > 0, "a heading is not taller than a paragraph");
    require(lines_narrow > lines_wide, "a narrower page did not wrap into more lines");
    require(flow_height(narrow) > flow_height(wide), "a narrower page is not taller");
    require(count(a, "marker \"") == 2, "the list's markers");
    require(count(a, "underline link 0") == 1, "the link");
    require(count(a, "block h2") == 0, "the page's own sheet");
    os64_free(a);
    os64_free(b);
    flow_free(wide);
    flow_free(narrow);
    sheets_close(&sh_wide);
    sheets_close(&sh_narrow);
    os64_page_free(page);
    os64_page_free(page2);
    os64_html_document_free(doc);
    teardown();
    os64_printf("flowdump: PASS\n");
    return (int)FLOWDUMP_OK;
}
