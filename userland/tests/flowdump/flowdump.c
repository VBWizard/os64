// flowdump — libflow in the guest, with the real faces.
//
// `flowdump FILE [WIDTH]` lays FILE out at WIDTH pixels (800 unless told)
// and prints the laid-out tree, the same dump the host harness diffs, so a
// page can be compared across the two. With no arguments it lays out a
// page of its own and checks what real fonts must make true of any layout:
// a heading taller than a paragraph, a long paragraph wrapping into more
// lines at a narrower width, every run released.
//
// The faces are the two the image ships, /etc/fonts/DejaVuSans.ttf and
// DejaVuSansMono.ttf; serif and bold ask for sans until the web faces
// arrive (docs/yonder/04-web-faces.md).

#include "html/html.h"
#include "page/page.h"
#include "flow/flow.h"
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
static uint8_t *s_sans, *s_mono;
static size_t s_sans_len, s_mono_len;

static struct {
    bool mono;
    uint32_t px;
    os64_text_font_t *font;
    os64_font_face_info_t info;
} s_fonts[64];
static int s_nfonts;

static os64_font_status_t fonts(void *ctx, const flow_family_list_t *families, bool bold,
                                bool italic, uint32_t px, os64_text_font_t *const **list,
                                size_t *count, os64_font_face_info_t *primary)
{
    (void)ctx;
    (void)bold;
    (void)italic;
    bool mono = families->generic == FLOW_GENERIC_MONO;
    int i = 0;
    while (i < s_nfonts && !(s_fonts[i].mono == mono && s_fonts[i].px == px))
        i++;
    if (i == s_nfonts) {
        if (s_nfonts == 64)
            return OS64_FONT_LIMIT;
        os64_font_face_options_t o = {px, OS64_FONT_HINT_NORMAL};
        os64_font_status_t st = os64_text_font_open(s_text, mono ? s_mono : s_sans,
                                                    mono ? s_mono_len : s_sans_len, &o,
                                                    &s_fonts[i].font);
        if (st != OS64_FONT_OK)
            return st;
        // The face's metrics, read the way the engine reports them: an
        // empty run carries its primary face's.
        os64_text_layout_t lo = {.fonts = &s_fonts[i].font, .font_count = 1,
                                 .tab_interval = 64 * 8};
        os64_text_run_t *run = NULL;
        st = os64_text_layout(s_text, NULL, 0, &lo, &run);
        if (st != OS64_FONT_OK)
            return st;
        os64_text_run_view_t v;
        os64_text_run_view(run, &v);
        os64_memset(&s_fonts[i].info, 0, sizeof(s_fonts[i].info));
        s_fonts[i].info.ascent = v.ascent;
        s_fonts[i].info.descent = v.descent;
        s_fonts[i].info.line_height = v.line_height;
        os64_text_run_release(run);
        s_fonts[i].mono = mono;
        s_fonts[i].px = px;
        s_nfonts++;
    }
    *list = &s_fonts[i].font;
    *count = 1;
    *primary = s_fonts[i].info;
    return OS64_FONT_OK;
}

static flow_env_t s_env = {
    .fonts = fonts,
    .viewport_font_px = 16,
    .default_generic = FLOW_GENERIC_SANS,
    .ink = 0x000000,
    .link_ink = 0x0000ee,
    .paper = 0xffffff,
};

static void setup(void)
{
    require(os64_slurp("/etc/fonts/DejaVuSans.ttf", OS64_FONT_FILE_MAX, &s_sans, &s_sans_len) ==
                OS64_SLURP_OK, "no /etc/fonts/DejaVuSans.ttf");
    require(os64_slurp("/etc/fonts/DejaVuSansMono.ttf", OS64_FONT_FILE_MAX, &s_mono,
                       &s_mono_len) == OS64_SLURP_OK, "no /etc/fonts/DejaVuSansMono.ttf");
    os64_text_options_t o = {.memory = {NULL, text_alloc, text_free},
                             .backend = os64_freetype_backend_v1()};
    require(os64_text_create(&o, &s_text) == OS64_FONT_OK, "no text context");
    s_env.text = s_text;
}

static void teardown(void)
{
    for (int i = 0; i < s_nfonts; i++)
        os64_text_font_release(s_fonts[i].font);
    require(os64_text_destroy(s_text) == OS64_FONT_OK, "a run outlived its tree");
    os64_free(s_sans);
    os64_free(s_mono);
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

static flow_tree_t *lay(os64_html_document_t *doc, os64_page_t **page, int32_t width)
{
    *page = os64_page_build(doc, "file:///flowdump", NULL);
    require(*page != NULL, "page model");
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
    size_t wl = os64_strlen(word);
    for (const char *p = text; *p != '\0'; p++)
        if (os64_memcmp(p, word, wl) == 0)
            n++;
    return n;
}

// The first `text` line's height after `tag` in a dump: its content area.
static int32_t text_height_after(const char *dump, const char *tag)
{
    const char *at = dump;
    size_t tl = os64_strlen(tag);
    while (*at != '\0' && os64_memcmp(at, tag, tl) != 0)
        at++;
    while (*at != '\0' && os64_memcmp(at, "text \"", 6) != 0)
        at++;
    if (*at == '\0')
        return -1;
    at += 6;
    while (*at != '\0' && *at != '"')
        at += *at == '\\' ? 2 : 1;
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
    "<font face=arial><p>inside a font</p></font><p><a href=x>a link</a></p>";

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
        flow_tree_t *t = lay(doc, &page, width);
        char *text = dump_of(t);
        os64_write(1, text, os64_strlen(text));
        os64_free(text);
        flow_free(t);
        os64_page_free(page);
        os64_html_document_free(doc);
        os64_free(html);
        teardown();
        return 0;
    }

    os64_html_document_t *doc = parse(kPage, sizeof(kPage) - 1);
    os64_page_t *page;
    flow_tree_t *wide = lay(doc, &page, 800);
    require(!flow_incomplete(wide), "incomplete at 800");
    char *a = dump_of(wide);
    os64_page_t *page2;
    flow_tree_t *narrow = lay(doc, &page2, 300);
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
    os64_free(a);
    os64_free(b);
    flow_free(wide);
    flow_free(narrow);
    os64_page_free(page);
    os64_page_free(page2);
    os64_html_document_free(doc);
    teardown();
    os64_printf("flowdump: PASS\n");
    return (int)FLOWDUMP_OK;
}
