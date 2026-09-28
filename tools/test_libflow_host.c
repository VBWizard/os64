// test_libflow_host.c — markup in, the page's styles and boxes out, checked
// on the host against dumps computed by hand from the standard.
//
// THIS FILE IS THE DURABLE ARTEFACT (LAYOUT.md § Proof before integration).
// Every expected dump here was worked out from the Rendering chapter and
// CSS 2.1 with a pencil, not captured from the engine: a case that passes
// says the engine agrees with the rule.

#include <stdarg.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "html/html.h"
#include "page/page.h"
#include "flow/flow.h"
#include "internal.h"
#include "os64/text.h"
#include "test_libflow_fonts.h"

// libos64's formatter reaches for the write syscall on paths nothing here
// prints through; answering "all of it went out" keeps the link honest.
int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    (void)handle;
    (void)buf;
    return (int64_t)len;
}

// ── Allocation, and failing it on purpose ───────────────────────────────

static size_t allocations, fail_at, live;
// Fail ONE allocation (the fail_at'th) rather than every one from there
// on: the shape of a budget the text engine hits while malloc still works.
static bool fail_single;

void *os64_malloc(size_t size)
{
    allocations++;
    if (fail_at != 0 && (fail_single ? allocations == fail_at : allocations >= fail_at))
        return NULL;
    void *at = malloc(size != 0 ? size : 1);
    if (at != NULL)
        live++;
    return at;
}

void *os64_calloc(size_t count, size_t size)
{
    void *at = os64_malloc(count * size);
    if (at != NULL)
        memset(at, 0, count * size);
    return at;
}

void *os64_realloc(void *ptr, size_t size)
{
    allocations++;
    if (fail_at != 0 && (fail_single ? allocations == fail_at : allocations >= fail_at))
        return NULL;
    void *at = realloc(ptr, size != 0 ? size : 1);
    if (at != NULL && ptr == NULL)
        live++;
    return at;
}

void os64_free(void *ptr)
{
    if (ptr != NULL)
        live--;
    free(ptr);
}

// ── Checking ────────────────────────────────────────────────────────────

static int checks, failures;

static void expect(const char *name, bool ok, const char *detail)
{
    checks++;
    if (!ok) {
        failures++;
        fprintf(stderr, "FAIL %s%s%s\n", name, detail != NULL ? ": " : "",
                detail != NULL ? detail : "");
    }
}

// ── Fonts: the resolver a face would hand in, over the test backend ──────

static os64_text_context_t *s_text;

static void *text_alloc_cb(void *ctx, size_t n)
{
    (void)ctx;
    return malloc(n);
}

static void text_free_cb(void *ctx, void *p, size_t n)
{
    (void)ctx;
    (void)n;
    free(p);
}

// One font per (generic, size), opened once and kept for the run.
static struct {
    char kind;
    uint32_t px;
    os64_text_font_t *font;
} s_fonts[128];
static int s_nfonts;

static os64_font_status_t test_fonts(void *ctx, const flow_family_list_t *families, bool bold,
                                     bool italic, uint32_t px, os64_text_font_t *const **list,
                                     size_t *count, os64_font_face_info_t *primary)
{
    (void)ctx;
    (void)bold;
    (void)italic;
    char kind = families->generic == FLOW_GENERIC_MONO ? 'M'
              : families->generic == FLOW_GENERIC_SANS ? 'A' : 'S';
    int i = 0;
    while (i < s_nfonts && !(s_fonts[i].kind == kind && s_fonts[i].px == px))
        i++;
    if (i == s_nfonts) {
        if (s_nfonts == (int)(sizeof(s_fonts) / sizeof(s_fonts[0])))
            return OS64_FONT_LIMIT;
        os64_font_face_options_t o = {.pixel_height = px, .hint = OS64_FONT_HINT_NONE};
        os64_font_status_t st = os64_text_font_open(s_text, (const uint8_t *)&kind, 1, &o,
                                                    &s_fonts[i].font);
        if (st != OS64_FONT_OK)
            return st;
        s_fonts[i].kind = kind;
        s_fonts[i].px = px;
        s_nfonts++;
    }
    *list = &s_fonts[i].font;
    *count = 1;
    memset(primary, 0, sizeof(*primary));
    primary->ascent = (int32_t)px * 48;
    primary->descent = (int32_t)px * 16;
    primary->line_height = (int32_t)px * 80;
    return OS64_FONT_OK;
}

// The face's measure of a picture: one whose src says "known" is 40x30,
// "tall" and "wide" are the most skewed a face may report (1 by INT32_MAX
// and back), and everything else has not arrived.
static bool test_oracle(void *ctx, const os64_html_node_t *node, int32_t *w, int32_t *h)
{
    (void)ctx;
    const os64_html_attr_t *src = os64_html_attr(node, "src");
    if (src == NULL || src->value == NULL)
        return false;
    if (strstr(src->value, "tall") != NULL) {
        *w = 1;
        *h = INT32_MAX;
        return true;
    }
    if (strstr(src->value, "wide") != NULL) {
        *w = INT32_MAX;
        *h = 1;
        return true;
    }
    if (strstr(src->value, "known") == NULL)
        return false;
    *w = 40;
    *h = 30;
    return true;
}

// The theme a face would hand in, in colours no page case uses, so the
// dump's `ink`, `link` and `paper` can only mean the environment's.
static flow_env_t kEnv = {
    .fonts = test_fonts,
    .replaced_size = test_oracle,
    .viewport_font_px = 16,
    .default_generic = FLOW_GENERIC_SERIF,
    .ink = 0x101010,
    .link_ink = 0x1010ee,
    .paper = 0xfefefe,
};

static void text_setup(void)
{
    os64_text_options_t o = {
        .memory = {NULL, text_alloc_cb, text_free_cb},
        .backend = flow_test_backend(),
    };
    if (os64_text_create(&o, &s_text) != OS64_FONT_OK) {
        fprintf(stderr, "no text context\n");
        exit(2);
    }
    kEnv.text = s_text;
}

static void text_teardown(void)
{
    for (int i = 0; i < s_nfonts; i++)
        os64_text_font_release(s_fonts[i].font);
    if (os64_text_destroy(s_text) != OS64_FONT_OK)
        expect("text context: every run released", false, NULL);
}

static const char *kPage = "http://host/dir/page.html";

static os64_html_document_t *parse(const char *html)
{
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = "utf-8";
    os64_html_parser_t *p = os64_html_parser_new(&opt);
    if (p == NULL)
        return NULL;
    os64_html_parser_feed(p, html, strlen(html));
    return os64_html_parser_finish(p);
}

static char *style_dump_of(const char *html)
{
    os64_html_document_t *doc = parse(html);
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    FStyles *styles = f_style_build(doc, page, &kEnv);
    char *text = NULL;
    if (styles != NULL) {
        int64_t need = f_style_dump(styles, NULL, 0);
        text = malloc((size_t)need + 1);
        f_style_dump(styles, text, (size_t)need + 1);
    }
    f_style_free(styles);
    os64_page_free(page);
    os64_html_document_free(doc);
    return text;
}

static void style_case(const char *name, const char *html, const char *expected)
{
    char *got = style_dump_of(html);
    bool ok = got != NULL && strcmp(got, expected) == 0;
    expect(name, ok, NULL);
    if (!ok)
        fprintf(stderr, "---- expected\n%s---- got\n%s----\n", expected, got ? got : "(null)\n");
    free(got);
}

#include "test_libflow_attrs.inc"
#include "test_libflow_style.inc"
#include "test_libflow_boxes.inc"
#include "test_libflow_layout.inc"
#include "test_libflow_tables.inc"

// ── The corpus, and the allocation sweep ────────────────────────────────

static const char *const kCorpus[] = {
    "68k-news", "example", "floodgap", "hacker-news", "textfiles-computers", "wikipedia-html",
};

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf != NULL && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf != NULL) {
        buf[n] = '\0';
        *len = (size_t)n;
    }
    return buf;
}

// Every corpus page styles whole, the same twice over, with nothing leaked.
static void corpus(void)
{
    for (size_t i = 0; i < sizeof(kCorpus) / sizeof(kCorpus[0]); i++) {
        char path[256];
        snprintf(path, sizeof(path), "tools/html_corpus/%s.html", kCorpus[i]);
        size_t len = 0;
        char *html = slurp(path, &len);
        expect(path, html != NULL, "unreadable");
        if (html == NULL)
            continue;
        char *a = style_dump_of(html);
        char *b = style_dump_of(html);
        expect(path, a != NULL && b != NULL && strcmp(a, b) == 0, "not deterministic");
        size_t lines = 0;
        for (const char *p = a; p != NULL && *p != '\0'; p++)
            lines += *p == '\n';
        char *x = boxes_dump_of(html);
        char *y = boxes_dump_of(html);
        expect(path, x != NULL && y != NULL && strcmp(x, y) == 0, "boxes not deterministic");
        expect(path, x != NULL && strstr(x, "incomplete") == NULL, "boxes incomplete");
        size_t boxes = 0, items = 0;
        for (const char *p = x; p != NULL && *p != '\0'; p++)
            if (*p == '\n') {
                const char *q = p + 1;
                while (*q == ' ')
                    q++;
                if (strncmp(q, "block", 5) == 0 || strncmp(q, "table", 5) == 0 ||
                    strncmp(q, "row", 3) == 0 || strncmp(q, "cell", 4) == 0 ||
                    strncmp(q, "caption", 7) == 0 || strncmp(q, "column", 6) == 0 ||
                    strncmp(q, "replaced", 8) == 0)
                    boxes++;
                else if (*q != '\0')
                    items++;
            }
        int32_t widths[2] = {800, 400};
        size_t heights[2] = {0, 0};
        for (int w = 0; w < 2; w++) {
            char *l1 = layout_dump_of(html, widths[w]);
            char *l2 = layout_dump_of(html, widths[w]);
            expect(path, l1 != NULL && l2 != NULL && strcmp(l1, l2) == 0,
                   "layout not deterministic");
            expect(path, l1 != NULL && strstr(l1, "incomplete") == NULL, "layout incomplete");
            long pw = 0, ph = 0;
            if (l1 != NULL)
                sscanf(l1, "page %ld %ld", &pw, &ph);
            heights[w] = (size_t)ph;
            free(l1);
            free(l2);
        }
        printf("corpus %-22s %5zu styled, %5zu boxes, %5zu items, %6zupx tall at 800, %6zupx at 400\n",
               kCorpus[i], lines, boxes + 1, items, heights[0], heights[1]);
        free(x);
        free(y);
        free(a);
        free(b);
        free(html);
    }
}

// Every allocation of a real page fails in turn: the style table is all or
// nothing, so each attempt is NULL or whole, and nothing leaks either way.
static void allocation_sweep(void)
{
    size_t len = 0;
    char *html = slurp("tools/html_corpus/floodgap.html", &len);
    if (html == NULL) {
        expect("sweep", false, "no page");
        return;
    }
    os64_html_document_t *doc = parse(html);
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    char *whole = style_dump_of(html);
    size_t base_live = live;
    allocations = 0;
    FStyles *s = f_style_build(doc, page, &kEnv);
    size_t count = allocations;
    f_style_free(s);
    size_t tried = 0;
    for (size_t at = 1; at <= count; at++) {
        allocations = 0;
        fail_at = at;
        s = f_style_build(doc, page, &kEnv);
        fail_at = 0;
        tried++;
        if (s != NULL) {
            int64_t need = f_style_dump(s, NULL, 0);
            char *got = malloc((size_t)need + 1);
            f_style_dump(s, got, (size_t)need + 1);
            expect("sweep: a build that succeeds is whole", strcmp(got, whole) == 0, NULL);
            free(got);
        }
        f_style_free(s);
        if (live != base_live) {
            expect("sweep: nothing leaked", false, NULL);
            break;
        }
    }
    printf("libflow sweep: each of %zu allocations failed in turn, nothing leaked\n", tried);
    free(whole);
    os64_page_free(page);
    os64_html_document_free(doc);
    free(html);
}

// Every allocation of a whole layout fails in turn: NULL, or a tree that
// says it is incomplete, or the whole one — never a crash, never a leak.
// Codex #147 round 2: a page taller than an int32_t, and a text wider than
// the engine's signed 26.6 coordinates.
// `open` repeated n times round an `x`, each closed by `close`, parsed
// allowing `max_depth` open elements.
static os64_html_document_t *nested(const char *open, const char *close, int n, size_t max_depth)
{
    size_t cap = (strlen(open) + strlen(close)) * (size_t)n + 64, at = 0;
    char *html = malloc(cap);
    at += (size_t)snprintf(html, cap, "<!doctype html>");
    for (int i = 0; i < n; i++)
        at += (size_t)snprintf(html + at, cap - at, "%s", open);
    at += (size_t)snprintf(html + at, cap - at, "x");
    for (int i = 0; i < n; i++)
        at += (size_t)snprintf(html + at, cap - at, "%s", close);
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = "utf-8";
    opt.max_depth = max_depth;
    os64_html_parser_t *p = os64_html_parser_new(&opt);
    os64_html_document_t *doc = NULL;
    if (p != NULL) {
        os64_html_parser_feed(p, html, strlen(html));
        doc = os64_html_parser_finish(p);
    }
    free(html);
    return doc;
}

// Whether a page nested n deep lays out whole (1), incomplete (0), or not
// at all (-1).
static int nested_whole(const char *open, const char *close, int n, size_t max_depth)
{
    os64_html_document_t *doc = nested(open, close, n, max_depth);
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    flow_tree_t *t = flow_layout(doc, page, 800, &kEnv);
    int whole = t == NULL ? -1 : !flow_incomplete(t);
    flow_free(t);
    os64_page_free(page);
    os64_html_document_free(doc);
    return whole;
}

// Whether the laid-out page has a fragment line ending in `tail` — the
// decorations and link after its geometry — for a case about paint, not
// position.
static bool fragment_ends(const char *html, const char *text, const char *tail)
{
    char *dump = layout_dump_of(html, 400);
    bool found = false;
    for (char *line = dump; line != NULL && *line != '\0' && !found;) {
        char *end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t)(end - line) : strlen(line);
        char *at = strstr(line, text);
        found = at != NULL && at < line + len && len >= strlen(tail) &&
                strncmp(line + len - strlen(tail), tail, strlen(tail)) == 0;
        line = end != NULL ? end + 1 : NULL;
    }
    free(dump);
    return found;
}

// The first text fragment whose text starts with `text`, found by walking
// the laid-out boxes, or NULL.
static const FFrag *find_frag(const FBox *b, const char *text)
{
    for (const FLine *ln = b != NULL ? b->lines : NULL; ln != NULL; ln = ln->next)
        for (const FFrag *fr = ln->frags; fr != NULL; fr = fr->next)
            if (fr->kind == FF_TEXT && fr->end > fr->begin &&
                strncmp(fr->text + fr->begin, text, strlen(text)) == 0)
                return fr;
    for (const FBox *c = b != NULL ? b->first : NULL; c != NULL; c = c->next) {
        const FFrag *fr = find_frag(c, text);
        if (fr != NULL)
            return fr;
    }
    return NULL;
}

// Codex #147 round 3: each decoration is drawn in the colour of the element
// that asked for it — the u's underline in ink, the s's line-through red.
static void decoration_colour_cases(void)
{
    os64_html_document_t *doc =
        parse("<!doctype html><u><font color=red><s>x</s></font></u>");
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    FStyles *styles = f_style_build(doc, page, &kEnv);
    FBoxes *boxes = f_boxes_build(doc, page, styles, &kEnv);
    FLayout *lay = f_layout(boxes, doc, page, &kEnv, 400);
    const FFrag *fr = lay != NULL ? find_frag(boxes->root, "x") : NULL;
    expect("each decoration keeps the colour of the element that drew it",
           fr != NULL &&
               fr->decoration == (FLOW_DECORATION_UNDERLINE | FLOW_DECORATION_LINE_THROUGH) &&
               fr->decoration_colors.underline == kEnv.ink &&
               fr->decoration_colors.line_through == 0xFF0000, NULL);
    f_layout_free(lay);
    f_boxes_free(boxes);
    f_style_free(styles);
    os64_page_free(page);
    os64_html_document_free(doc);
}

// Decorations propagate to every descendant but stop at an atom's content
// and, in full quirks, at a table; the atom's own and the table's own
// still reach inside. Pass 1 answers this per element (FStyled).
static void paint_cases(void)
{
    expect("an underline reaches a table's text in no-quirks mode",
           fragment_ends("<!doctype html><u><table><tr><td>t</table></u>",
                         "text \"t\"", "serif 16 underline"), NULL);
    expect("an underline stops at a table in full quirks",
           fragment_ends("<u><table><tr><td>t</table></u>", "text \"t\"", "serif 16"), NULL);
    expect("a table's own decoration still reaches its text in full quirks",
           fragment_ends("<u><table><tr><td><s>t</s></table></u>",
                         "text \"t\"", "serif 16 line-through"), NULL);
    expect("an underline stops at an inline-block's content",
           fragment_ends("<!doctype html><u><marquee>m</marquee></u>", "text \"m\"",
                         "serif 16"), NULL);
    expect("decorations inside an inline-block are its own",
           fragment_ends("<!doctype html><u><marquee><s>m</s></marquee></u>",
                         "text \"m\"", "serif 16 line-through"), NULL);
}

typedef struct {
    const os64_html_document_t *doc;
    const os64_page_t *page;
    int whole;
} Deep;

static void *deep_layout(void *arg)
{
    Deep *d = arg;
    flow_tree_t *t = flow_layout(d->doc, d->page, 800, &kEnv);
    d->whole = t == NULL ? -1 : !flow_incomplete(t);
    flow_free(t);
    return NULL;
}

static void limit_cases(void)
{
    // Codex #147 round 6: an item whose first content is a nested list
    // shares that list's first line, and both markers stand on it. The
    // 48px outer bullet's font box (60 tall, baseline 42) holds the line
    // open, so the bullet sits inside its item, 48 + 42 - 36 = 54.
    {
        char *got = layout_dump_of("<!doctype html><font size=7><ul><li><font size=1><ul><li>x"
                                   "</ul></font><li>y</ul></font>", 300);
        expect("every marker waiting on a shared first line holds it open",
               got != NULL && strstr(got, "      block li 48 48 244 60\n"
                                          "        marker \"\xe2\x80\xa2 \" 12 54 36 48\n") != NULL,
               got);
        free(got);
    }
    // Codex #147 round 6: a picture's shape scales one side by the other,
    // and the face may report 1 by INT32_MAX; with a width of a million
    // percent three tables deep the product overflowed int64_t. Both
    // skews, both sides given, under UBSan: held, not wrapped.
    {
        static const char *const pages[] = {
            "<!doctype html><table width=1000000%><tr><td><table width=1000000%><tr><td>"
            "<table width=1000000%><tr><td><img src=tall.png width=1000000%></table></table></table>",
            "<!doctype html><img src=wide.png height=1000000>",
            "<!doctype html><img src=tall.png width=1000000>",
        };
        for (int i = 0; i < F_ARRAY(pages); i++) {
            os64_html_document_t *doc = parse(pages[i]);
            os64_page_t *page = os64_page_build(doc, kPage, NULL);
            flow_tree_t *t = flow_layout(doc, page, 800, &kEnv);
            expect("a skewed picture's shape is held, not overflowed",
                   t != NULL && !flow_incomplete(t) && flow_height(t) > 0 && flow_width(t) > 0,
                   pages[i]);
            flow_free(t);
            os64_page_free(page);
            os64_html_document_free(doc);
        }
    }
    // Found auditing the same invariant: a font size compounds down the tree
    // (each <big> is 6/5 of its parent's), and 150 of them wrapped negative.
    // It is held to F_INT_MAX pixels, and drawn at the largest the engine can.
    {
        size_t bcap = 150 * 5 + 64, bat = 0;
        char *bigs = malloc(bcap);
        bat += (size_t)snprintf(bigs, bcap, "<!doctype html>");
        for (int i = 0; i < 150; i++)
            bat += (size_t)snprintf(bigs + bat, bcap - bat, "<big>");
        snprintf(bigs + bat, bcap - bat, "x");
        char *got = layout_dump_of(bigs, 400);
        expect("a compounded font size is held, not wrapped negative",
               got != NULL && strstr(got, " serif 1000000\n") != NULL, NULL);
        free(got);
        free(bigs);
    }
    // Codex #147 round 4: a document libhtml stopped short of is not whole,
    // however well its prefix lays out.
    {
        os64_html_options_t opt = os64_html_options_default();
        opt.charset = "utf-8";
        opt.max_bytes = 32;
        os64_html_parser_t *p = os64_html_parser_new(&opt);
        const char *html = "<!doctype html><p>one two three four five six seven";
        os64_html_parser_feed(p, html, strlen(html));
        os64_html_document_t *doc = os64_html_parser_finish(p);
        os64_page_t *page = os64_page_build(doc, kPage, NULL);
        flow_tree_t *t = flow_layout(doc, page, 400, &kEnv);
        expect("a page the parser refused partway is incomplete",
               doc != NULL && doc->refusal != 0 && t != NULL && flow_incomplete(t), NULL);
        flow_free(t);
        os64_page_free(page);
        os64_html_document_free(doc);
    }
    // Codex #147 round 3: percentages of percentages, three tables deep,
    // overflowed int64_t (UBSan aborts this harness on it). Every length is
    // held to what a face can read, so the page is as wide as that allows.
    {
        os64_html_document_t *doc = parse("<!doctype html><table width=1000000%><tr><td>"
                                          "<table width=1000000%><tr><td>"
                                          "<table width=1000000%><tr><td>x</table></table></table>");
        os64_page_t *page = os64_page_build(doc, kPage, NULL);
        flow_tree_t *t = flow_layout(doc, page, 800, &kEnv);
        expect("nested percentages are held, not overflowed",
               t != NULL && !flow_incomplete(t) && flow_width(t) == INT32_MAX, NULL);
        flow_free(t);
        os64_page_free(page);
        os64_html_document_free(doc);
    }
    // Depth is libflow's own bound (F_DEPTH_MAX), whatever the parser was
    // told to allow: past it the build stops and says so, and inside it
    // the page is whole. html and body are two of the descents, so the
    // innermost of n divs opens descent 2 + n.
    expect("a page nested exactly to the depth bound is whole",
           nested_whole("<div>", "</div>", F_DEPTH_MAX - 2, 4096) == 1, NULL);
    expect("one level past the depth bound is incomplete",
           nested_whole("<div>", "</div>", F_DEPTH_MAX - 1, 4096) == 0, NULL);
    expect("a page nested past the depth bound is incomplete, not a crash",
           nested_whole("<div>", "</div>", 2000, 4096) == 0, NULL);
    // An inline-block's content is two descents: the innermost of j
    // marquees opens descent 2 + 2j.
    expect("inline-blocks nested exactly to the bound are whole",
           nested_whole("<marquee>", "</marquee>", (F_DEPTH_MAX - 2) / 2, 4096) == 1, NULL);
    expect("an inline-block level costs two descents",
           nested_whole("<marquee>", "</marquee>", (F_DEPTH_MAX - 2) / 2 + 1, 4096) == 0, NULL);
    expect("inline nesting counts against the same bound",
           nested_whole("<span>", "</span>", 2000, 4096) == 0, NULL);
    // Codex #147 round 5: display: contents is walked before any box is
    // made (the mixing scan), and that walk is charged too — a chain far
    // past the bound stops the build instead of the stack. The layout runs
    // on a thread with a small stack; the parser and libpage do not.
    {
        os64_html_document_t *doc = nested("<slot>", "</slot>", 20000, 40000);
        os64_page_t *page = os64_page_build(doc, kPage, NULL);
        Deep d = {doc, page, -1};
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 512 * 1024);
        pthread_t th;
        bool ran = pthread_create(&th, &attr, deep_layout, &d) == 0;
        if (ran)
            pthread_join(th, NULL);
        expect("a display-contents chain past the bound is incomplete, not a crash",
               ran && d.whole == 0, NULL);
        os64_page_free(page);
        os64_html_document_free(doc);
    }
    expect("tables nested past the bound are incomplete",
           nested_whole("<table><tr><td>", "</table>", 700, 4096) == 0, NULL);

    // A tab 34 million pixels along a pre line, past where the line's
    // stops fit the engine's 32-bit 26.6. At 48px the monospace space is
    // 24, so a stop every 192; the pen after the pictures and the x is
    // 34,000,024, the next stop 34,000,128, and the fragment that holds
    // the tab and the y is 34,000,128 - 34,000,024 + 24 = 128 wide.
    // (192 is not a power of two: at the default size the stops repeat
    // every 64px, 2^12 in 26.6, and a wrapped origin lands on them anyway.)
    size_t tcap = 34 * 48 + 128, tat = 0;
    char *tabs = malloc(tcap);
    tat += (size_t)snprintf(tabs, tcap, "<!doctype html><font size=7><pre>");
    for (int i = 0; i < 34; i++)
        tat += (size_t)snprintf(tabs + tat, tcap - tat, "<img src=known.png width=1000000 height=1>");
    snprintf(tabs + tat, tcap - tat, "<b>x</b>\ty</pre></font>");
    char *got = layout_dump_of(tabs, 800);
    expect("a tab far along a line still stops where the line's stops are",
           got != NULL && strstr(got, "text \"\\ty\" 34000032 54 128 48 mono 48\n") != NULL, NULL);
    free(got);
    free(tabs);

    // 2,200 pictures a million pixels tall, one a line: 2.2e9 px of page,
    // which flow_height holds to INT32_MAX rather than wrapping negative.
    size_t cap = 2200 * 48 + 64, at = 0;
    char *html = malloc(cap);
    at += (size_t)snprintf(html + at, cap - at, "<!doctype html>");
    for (int i = 0; i < 2200; i++)
        at += (size_t)snprintf(html + at, cap - at, "<img src=known.png height=1000000><br>");
    os64_html_document_t *doc = parse(html);
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    flow_tree_t *t = flow_layout(doc, page, 200, &kEnv);
    expect("a page past INT32_MAX is held to it",
           t != NULL && !flow_incomplete(t) && flow_height(t) == INT32_MAX, NULL);
    flow_free(t);
    os64_page_free(page);
    os64_html_document_free(doc);
    free(html);

    // A million M's at 48px (<font size=7>) are inside the engine's byte
    // cap and 36 million pixels wide: the window is halved until a run
    // fits, and the page lays out whole, as wide as the word.
    size_t n = 1000000;
    html = malloc(n + 64);
    at = (size_t)snprintf(html, 64, "<!doctype html><font size=7>");
    memset(html + at, 'M', n);
    html[at + n] = '\0';
    doc = parse(html);
    page = os64_page_build(doc, kPage, NULL);
    t = flow_layout(doc, page, 800, &kEnv);
    expect("a word wider than the engine's coordinates is still laid out",
           t != NULL && !flow_incomplete(t) && flow_width(t) > 1000000, NULL);
    flow_free(t);
    os64_page_free(page);
    os64_html_document_free(doc);
    free(html);
}

// ── The relation sweep (LAYOUT.md § Proof) ─────────────────────────────
//
// A laid-out tree flattened in the order it is made: each box, its lines,
// each line's fragments (an atom's content under its fragment) and spans,
// its child boxes, and its marker last.
typedef struct {
    int kind;                   // 1 box, 2 line, 3 fragment, 4 span, 5 marker
    const void *what;           // the node it stands for
    uint32_t begin, end;
    int64_t x, y, w, h;
    bool unfinished;
    int32_t parent;
} RelItem;

typedef struct {
    RelItem *v;
    size_t n, cap;
} Rel;

static int32_t rel_add(Rel *r, RelItem it)
{
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 64;
        r->v = realloc(r->v, r->cap * sizeof(*r->v));
    }
    r->v[r->n] = it;
    return (int32_t)r->n++;
}

static void rel_box(Rel *r, const FBox *b, int32_t parent);

static void rel_lines(Rel *r, const FBox *b, int32_t me)
{
    for (const FLine *ln = b->lines; ln != NULL; ln = ln->next) {
        int32_t li = rel_add(r, (RelItem){2, b->node, 0, 0, ln->x, ln->y, ln->w, ln->h, false, me});
        for (const FFrag *fr = ln->frags; fr != NULL; fr = fr->next) {
            int32_t fi = rel_add(r, (RelItem){3, fr->node, fr->begin, fr->end, fr->x, fr->y,
                                              fr->w, fr->h, false, li});
            if (fr->kind == FF_ATOMIC && fr->item->content != NULL)
                rel_box(r, fr->item->content, fi);
        }
        for (const FSpan *sp = ln->spans; sp != NULL; sp = sp->next)
            rel_add(r, (RelItem){4, sp->inl->node, 0, 0, sp->x0, sp->top, sp->x1 - sp->x0,
                                 sp->bottom - sp->top, false, li});
    }
}

static void rel_box(Rel *r, const FBox *b, int32_t parent)
{
    if (b == NULL || !b->placed)
        return;
    int32_t me = rel_add(r, (RelItem){1, b->node, 0, 0, b->x, b->y, b->w, b->h,
                                      b->unfinished, parent});
    rel_lines(r, b, me);
    for (const FBox *c = b->first; c != NULL; c = c->next)
        rel_box(r, c, me);
    // Last, as it is made: a marker is placed after its box's content.
    if (b->marker_frag != NULL)
        rel_add(r, (RelItem){5, b->node, 0, 0, b->marker_frag->x, b->marker_frag->y,
                             b->marker_frag->w, b->marker_frag->h, false, me});
}

static bool rel_under_unfinished(const Rel *r, size_t i)
{
    for (size_t k = i + 1; k < r->n; k++) {
        int32_t up = r->v[k].parent;
        while (up >= 0 && (size_t)up > i)
            up = r->v[up].parent;
        if (up != (int32_t)i)
            break;
        if (r->v[k].unfinished)
            return true;
    }
    return false;
}

// Every single allocation of pass 3 fails in turn, the rest succeeding.
// A partial tree is held to what the whole one says: (a) its pre-order is
// a prefix of the whole's, (b) everything finished is where the whole puts
// it, (c) the unfinished boxes are the path to the last thing it holds —
// and the page is as tall as what it holds.
static void layout_relation_sweep(const char *what, const char *html, int32_t width)
{
    os64_html_document_t *doc = parse(html);
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    FStyles *styles = f_style_build(doc, page, &kEnv);
    // Layout writes its geometry into the boxes, so every run gets its own,
    // built with nothing failing.
    FBoxes *boxes = f_boxes_build(doc, page, styles, &kEnv);
    FLayout *whole = f_layout(boxes, doc, page, &kEnv, width);
    Rel w = {0};
    rel_box(&w, boxes->root, -1);
    RelItem *wv = w.v;
    size_t wn = w.n;
    f_layout_free(whole);
    f_boxes_free(boxes);
    boxes = f_boxes_build(doc, page, styles, &kEnv);
    size_t base_live = live;
    allocations = 0;
    FLayout *probe = f_layout(boxes, doc, page, &kEnv, width);
    size_t count = allocations;
    f_layout_free(probe);
    f_boxes_free(boxes);
    boxes = NULL;
    size_t partial = 0, nulls = 0, compared = 0;
    bool ok_a = true, ok_b = true, ok_c = true, ok_h = true, ok_d = true;
    char detail[200] = "";
    fail_single = true;
    for (size_t at = 1; at <= count; at++) {
        boxes = f_boxes_build(doc, page, styles, &kEnv);
        base_live = live;
        allocations = 0;
        fail_at = at;
        FLayout *t = f_layout(boxes, doc, page, &kEnv, width);
        fail_at = 0;
        if (t == NULL) {
            nulls++;
        } else if (t->incomplete) {
            partial++;
            Rel p = {0};
            rel_box(&p, boxes->root, -1);
            bool prefix = p.n <= wn;
            for (size_t i = 0; prefix && i < p.n; i++)
                prefix = p.v[i].kind == wv[i].kind && p.v[i].what == wv[i].what &&
                         p.v[i].begin == wv[i].begin &&
                         (p.v[i].unfinished || wv[i].unfinished || p.v[i].end == wv[i].end);
            if (!prefix && ok_a) {
                size_t i = 0;
                while (i < p.n && i < wn && p.v[i].kind == wv[i].kind && p.v[i].what == wv[i].what &&
                       p.v[i].begin == wv[i].begin &&
                       (p.v[i].unfinished || wv[i].unfinished || p.v[i].end == wv[i].end))
                    i++;
                snprintf(detail, sizeof(detail),
                         "(a) at failure %zu: item %zu of %zu (whole %zu): kind %d +%u..%u, whole "
                         "kind %d +%u..%u", at, i, p.n, wn, i < p.n ? p.v[i].kind : -1,
                         i < p.n ? p.v[i].begin : 0, i < p.n ? p.v[i].end : 0,
                         i < wn ? wv[i].kind : -1, i < wn ? wv[i].begin : 0, i < wn ? wv[i].end : 0);
            }
            ok_a &= prefix;
            for (size_t i = 0; prefix && i < p.n; i++) {
                if (p.v[i].unfinished || rel_under_unfinished(&p, i))
                    continue;
                bool same = p.v[i].x == wv[i].x && p.v[i].y == wv[i].y &&
                            p.v[i].w == wv[i].w && p.v[i].h == wv[i].h;
                if (!same && ok_b)
                    snprintf(detail, sizeof(detail), "(b) at failure %zu: item %zu kind %d",
                             at, i, p.v[i].kind);
                ok_b &= same;
                compared++;
            }
            for (size_t i = 0; i < p.n; i++) {
                if (!p.v[i].unfinished)
                    continue;
                int32_t up = (int32_t)p.n - 1;
                while (up >= 0 && (size_t)up != i)
                    up = p.v[up].parent;
                if (up != (int32_t)i && ok_c)
                    snprintf(detail, sizeof(detail), "(c) at failure %zu", at);
                ok_c &= up == (int32_t)i;
            }
            int64_t bottom = 0;
            for (size_t i = 0; i < p.n; i++)
                if (p.v[i].kind == 1 && p.v[i].y + p.v[i].h > bottom)
                    bottom = p.v[i].y + p.v[i].h;
            if (t->height < bottom && ok_h)
                snprintf(detail, sizeof(detail), "height at failure %zu", at);
            ok_h &= t->height >= bottom;
            // (d) and no taller or wider than what it holds: nothing that is
            // not placed reaches the page's edges.
            int64_t reach_y = 0, reach_x = (int64_t)width * 64;
            for (size_t i = 0; i < p.n; i++) {
                reach_y = p.v[i].y + p.v[i].h > reach_y ? p.v[i].y + p.v[i].h : reach_y;
                reach_x = p.v[i].x + p.v[i].w > reach_x ? p.v[i].x + p.v[i].w : reach_x;
            }
            if ((t->height > reach_y || t->width > reach_x) && ok_d)
                snprintf(detail, sizeof(detail), "(d) at failure %zu: page %lld x %lld, holds %lld x %lld",
                         at, (long long)t->width, (long long)t->height, (long long)reach_x,
                         (long long)reach_y);
            ok_d &= t->height <= reach_y && t->width <= reach_x;
            free(p.v);
        }
        f_layout_free(t);
        bool leaked = live != base_live;
        f_boxes_free(boxes);
        boxes = NULL;
        if (leaked) {
            expect("relation sweep: nothing leaked", false, what);
            break;
        }
    }
    fail_single = false;
    char name[96];
    snprintf(name, sizeof(name), "relation sweep %s: (a) a prefix of the whole", what);
    expect(name, ok_a, detail);
    snprintf(name, sizeof(name), "relation sweep %s: (b) what finished is where it will be", what);
    expect(name, ok_b, detail);
    snprintf(name, sizeof(name), "relation sweep %s: (c) the unfinished lead to the last", what);
    expect(name, ok_c, detail);
    snprintf(name, sizeof(name), "relation sweep %s: as tall as what it holds", what);
    expect(name, ok_h, detail);
    snprintf(name, sizeof(name), "relation sweep %s: (d) nothing unplaced reaches an edge", what);
    expect(name, ok_d, detail);
    printf("libflow relation sweep, %s: %zu failures: %zu NULL, %zu partial trees held to "
           "(a)(b)(c), %zu finished items compared\n", what, count, nulls, partial, compared);
    free(wv);
    f_style_free(styles);
    os64_page_free(page);
    os64_html_document_free(doc);
}

// A face at its most hostile: every picture measured at an extreme — a
// sliver as tall or as wide as an int32_t holds, the largest square, or
// nothing — chosen by the node, so each page gets a mix.
static bool hostile_oracle(void *ctx, const os64_html_node_t *node, int32_t *w, int32_t *h)
{
    (void)ctx;
    switch (((uintptr_t)node >> 4) % 4) {
    case 0: *w = 1; *h = INT32_MAX; break;
    case 1: *w = INT32_MAX; *h = 1; break;
    case 2: *w = INT32_MAX; *h = INT32_MAX; break;
    default: *w = 0; *h = 0; break;
    }
    return true;
}

// Every corpus page under the hostile oracle, at a page's width and at
// none, under UBSan: whatever the face says, the geometry holds its range
// (the harness aborts on any overflow) and the page's size is a size.
static void hostile_corpus(void)
{
    flow_env_t env = kEnv;
    env.replaced_size = hostile_oracle;
    for (size_t i = 0; i < sizeof(kCorpus) / sizeof(kCorpus[0]); i++) {
        char path[256];
        snprintf(path, sizeof(path), "tools/html_corpus/%s.html", kCorpus[i]);
        size_t len = 0;
        char *html = slurp(path, &len);
        if (html == NULL)
            continue;
        os64_html_document_t *doc = parse(html);
        os64_page_t *page = os64_page_build(doc, kPage, NULL);
        static const int32_t widths[] = {800, 0};
        for (int k = 0; k < F_ARRAY(widths); k++) {
            flow_tree_t *t = flow_layout(doc, page, widths[k], &env);
            expect("a hostile face's measures are held", t != NULL && !flow_incomplete(t) &&
                   flow_height(t) >= 0 && flow_width(t) >= widths[k], path);
            flow_free(t);
        }
        os64_page_free(page);
        os64_html_document_free(doc);
        free(html);
    }
}

// Tables at the edges of their arithmetic: random nests of tables whose
// widths, heights, spacing and spans take the extremes a page may write,
// holding pictures the hostile face measures, laid out under UBSan. Any
// share, sum or product that overflows aborts the harness.
static uint64_t s_tfuzz = 0x7AB1E5F022ull;

static uint32_t tpick(uint32_t n)
{
    s_tfuzz ^= s_tfuzz << 13;
    s_tfuzz ^= s_tfuzz >> 7;
    s_tfuzz ^= s_tfuzz << 17;
    return (uint32_t)(s_tfuzz % n);
}

static void tfuzz_table(char *out, size_t cap, size_t *at, int depth)
{
    static const char *const widths[] = {"", " width=1", " width=1000000", " width=50%",
                                         " width=1000000%", " width=100%"};
    static const char *const heights[] = {"", " height=1000000", " height=1"};
    static const char *const spacing[] = {"", " cellspacing=1000000", " cellpadding=1000000"};
    *at += (size_t)snprintf(out + *at, cap - *at, "<table%s%s%s>", widths[tpick(6)],
                            heights[tpick(3)], spacing[tpick(3)]);
    int rows = 1 + (int)tpick(3);
    for (int r = 0; r < rows && *at < cap - 512; r++) {
        *at += (size_t)snprintf(out + *at, cap - *at, "<tr%s>", heights[tpick(3)]);
        int cells = 1 + (int)tpick(4);
        for (int c = 0; c < cells && *at < cap - 512; c++) {
            *at += (size_t)snprintf(out + *at, cap - *at, "<td%s colspan=%u rowspan=%u>",
                                    widths[tpick(6)], 1 + tpick(3), 1 + tpick(2));
            switch (tpick(4)) {
            case 0:
                if (depth < 3)
                    tfuzz_table(out, cap, at, depth + 1);
                break;
            case 1:
                *at += (size_t)snprintf(out + *at, cap - *at, "<img src=%s.png%s>",
                                        tpick(2) ? "tall" : "wide", widths[tpick(6)]);
                break;
            case 2:
                *at += (size_t)snprintf(out + *at, cap - *at, "words that wrap");
                break;
            default:
                break;
            }
        }
    }
    *at += (size_t)snprintf(out + *at, cap - *at, "</table>");
}

static void table_fuzz(size_t pages)
{
    flow_env_t env = kEnv;
    env.replaced_size = hostile_oracle;
    char *html = malloc(65536);
    for (size_t i = 0; i < pages; i++) {
        size_t at = (size_t)snprintf(html, 65536, "%s", tpick(2) ? "<!doctype html>" : "");
        tfuzz_table(html, 65536, &at, 0);
        os64_html_document_t *doc = parse(html);
        os64_page_t *page = os64_page_build(doc, kPage, NULL);
        static const int32_t widths[] = {800, 0};
        for (int k = 0; k < F_ARRAY(widths); k++) {
            flow_tree_t *t = flow_layout(doc, page, widths[k], &env);
            if (t == NULL || flow_incomplete(t) || flow_height(t) < 0 || flow_width(t) < widths[k])
                expect("table fuzz: laid out whole, sizes held", false, html);
            flow_free(t);
        }
        os64_page_free(page);
        os64_html_document_free(doc);
    }
    free(html);
    expect("table fuzz ran", true, NULL);
    printf("libflow table fuzz: %zu pages of extreme tables, laid out under UBSan\n", pages);
}

// Whether the laid-out page holds every one of `lines` (whole dump lines,
// indentation stripped) — for a case about a few boxes of a larger tree.
static bool has_lines(const char *html, int32_t width, const char *const *lines, int n)
{
    char *dump = layout_dump_of(html, width);
    bool all = dump != NULL;
    for (int i = 0; i < n && all; i++) {
        size_t len = strlen(lines[i]);
        bool found = false;
        for (const char *p = dump; *p != '\0' && !found;) {
            const char *e = strchr(p, '\n');
            const char *q = p;
            while (*q == ' ')
                q++;
            size_t have = e != NULL ? (size_t)(e - q) : strlen(q);
            found = have == len && strncmp(q, lines[i], len) == 0;
            p = e != NULL ? e + 1 : p + strlen(p);
        }
        all = found;
    }
    free(dump);
    return all;
}

// Codex, the table slice's first review, each worked by hand: a table 200
// wide with no spacing or padding, at the body's 8.
static void table_review_cases(void)
{
    static const char *const col_pct[] = {"cell td 8 8 150 20", "cell td 158 8 50 20"};
    expect("a col's percentage width reaches its column",
           has_lines("<!doctype html><table width=200 cellspacing=0 cellpadding=0>"
                     "<col width=75%><col><tr><td>a<td>b</table>", 400, col_pct, 2), NULL);
    static const char *const span_pct[] = {"cell td 8 8 150 20", "cell td 8 28 75 20",
                                           "cell td 83 28 75 20", "cell td 158 28 50 20"};
    expect("a spanning cell's percentage is shared by its columns",
           has_lines("<!doctype html><table width=200 cellspacing=0 cellpadding=0>"
                     "<tr><td colspan=2 width=75%>s<td>c<tr><td>a<td>b<td>c</table>", 400,
                     span_pct, 4), NULL);
    static const char *const group[] = {"cell td 8 8 50 20", "cell td 58 8 50 20",
                                        "cell td 108 8 100 20"};
    expect("an empty colgroup's width is each of its columns'",
           has_lines("<!doctype html><table width=200 cellspacing=0 cellpadding=0>"
                     "<colgroup span=2 width=50></colgroup><tr><td>a<td>b<td>c</table>", 400,
                     group, 3), NULL);
    // 30% of 200 is 60, the col's own 100 stands, and the third takes 40.
    static const char *const inherit[] = {"cell td 8 8 60 20", "cell td 68 8 100 20",
                                          "cell td 168 8 40 20"};
    expect("a col with no width takes its group's",
           has_lines("<!doctype html><table width=200 cellspacing=0 cellpadding=0>"
                     "<colgroup width=30%><col><col width=100></colgroup><tr><td>a<td>b<td>c"
                     "</table>", 400, inherit, 3), NULL);
    // A four-row span in column 1, crossed on row 2 by a two-row span: row
    // 4's second cell goes to column 2, not onto the span.
    static const char *const hold[] = {"cell td 16 8 8 60", "cell td 24 48 8 20"};
    expect("a rowspan's hold is never shortened by a span crossing it",
           has_lines("<!doctype html><table cellspacing=0 cellpadding=0><tr><td>a"
                     "<td rowspan=4>B<tr><td colspan=2 rowspan=2>C<tr><tr><td>d<td>e</table>",
                     400, hold, 2), NULL);
    // Baselines: the p's line sits 30 down (its 16px margin and 14), the
    // five lines' first 14 down; below the shared baseline the tall cell
    // needs 100 - 14 = 86, so the row is 30 + 86 = 116 and holds it moved.
    static const char *const row[] = {"row tr 8 8 20 116", "row tr 8 124 20 20"};
    expect("a baseline row is sized for its cells where the baseline puts them",
           has_lines("<!doctype html><table cellspacing=0 cellpadding=0><tr valign=baseline>"
                     "<td><p>x</p><td>a<br>b<br>c<br>d<br>e<tr><td>n<td>m</table>", 400, row, 2),
           NULL);
    // Two 100px preferences in a 44px containing block: each column gives
    // back toward its least (8), 8 + 28 x 92 / 184 = 22, and the grid is
    // the table's 44.
    static const char *const fixed[] = {"table table 8 8 44 20", "cell td 8 8 22 20",
                                        "cell td 30 8 22 20"};
    expect("set widths give way when the table cannot hold them",
           has_lines("<!doctype html><table cellspacing=0 cellpadding=0><tr><td width=100>a"
                     "<td width=100>b</table>", 60, fixed, 3), NULL);
    // A cell with no line: its baseline is the bottom of its content, 100,
    // so the x moves down 100 - 14 and the row is 100 + (20 - 14).
    static const char *const empty[] = {"row tr 8 8 8 106", "text \"x\" 8 96 8 16 serif 16"};
    expect("a cell with no line has its baseline at its content's bottom",
           has_lines("<!doctype html><table cellspacing=0 cellpadding=0><tr valign=baseline>"
                     "<td height=100></td><td>x</table>", 400, empty, 2), NULL);
    // A nested table's baseline is its first row as shown: the tbody, not
    // the tfoot written before it — the x shares the B's baseline, 50.
    static const char *const shown[] = {"text \"x\" 8 38 8 16 serif 16"};
    // Round 2. A nested table's baseline is its first row's as its layout
    // shared it: the 48px B's (50), not the top-aligned t's (22).
    static const char *const row_base[] = {"text \"x\" 8 38 8 16 serif 16"};
    expect("a nested table's baseline is its first row's shared one",
           has_lines("<!doctype html><table cellspacing=0 cellpadding=0><tr valign=baseline>"
                     "<td>x<td><table cellspacing=0 cellpadding=0><tr><td valign=top>t"
                     "<td valign=baseline><font size=7>B</font></table></table>", 400, row_base, 1),
           NULL);
    // An inline-block is measured from its content: the marquee's cell is
    // its text's 68 wide, and the next cell starts after it.
    static const char *const atom[] = {"cell td 8 8 68 26", "cell td 76 8 8 26"};
    expect("an inline-block's cell is as wide as its content",
           has_lines("<!doctype html><table cellspacing=0 cellpadding=0><tr><td>"
                     "<marquee>abcdefghij</marquee><td>x</table>", 400, atom, 2), NULL);
    // Quirks: an empty table is nothing — no width to scroll to, no margin
    // between the paragraphs it sits among.
    static const char *const empty_table[] = {"page 400 80", "table table 8 28 0 0",
                                              "block p 8 44 384 20"};
    expect("quirks: a table with no rows is nothing at all",
           has_lines("<p>a</p><table width=1000000 border=5><tbody></tbody></table><p>b</p>", 400,
                     empty_table, 3), NULL);
    // Round 3. An empty first row is still the first row: its bottom is
    // the nested table's baseline, so the x does not move down to the B.
    static const char *const empty_row[] = {"text \"x\" 8 10 8 16 serif 16",
                                            "row tr 16 22 24 0"};
    expect("a nested table whose first row is empty takes its baseline from it",
           has_lines("<!doctype html><table cellspacing=0 cellpadding=0><tr valign=baseline>"
                     "<td>x<td><table cellspacing=0 cellpadding=0><tr></tr><tr><td>"
                     "<font size=7>B</font></table></table>", 400, empty_row, 2), NULL);
    // A set height is a table's least with no rows to share it: the p comes
    // after 100 of it, 8 + 100 + 16.
    static const char *const tall_empty[] = {"table table 8 8 0 100", "block p 8 124 384 20"};
    expect("a table with no rows is as tall as it is set",
           has_lines("<!doctype html><table height=100></table><p>after</p>", 400, tall_empty, 2),
           NULL);
    expect("a table's baseline is its first row as shown",
           has_lines("<!doctype html><table cellspacing=0 cellpadding=0><tr valign=baseline>"
                     "<td>x<td><table cellspacing=0 cellpadding=0><tfoot><tr><td>f<br>f2"
                     "</tfoot><tbody><tr><td><font size=7>B</font></tbody></table></table>",
                     400, shown, 1), NULL);
}

static void layout_sweep(void)
{
    const char *html =
        "<!doctype html><h1>T</h1><p>one <b>two</b> three<br>four <img src=known.png>"
        "<ul><li>a<li>b</ul><pre>x\ty</pre><font face=arial><p>in</p>out</font>";
    os64_html_document_t *doc = parse(html);
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    size_t base_live = live;
    allocations = 0;
    flow_tree_t *probe = flow_layout(doc, page, 300, &kEnv);
    size_t count = allocations;
    flow_free(probe);
    size_t tried = 0, nulls = 0, partial = 0;
    for (size_t at = 1; at <= count; at++) {
        allocations = 0;
        fail_at = at;
        flow_tree_t *t = flow_layout(doc, page, 300, &kEnv);
        fail_at = 0;
        tried++;
        if (t == NULL) {
            nulls++;
        } else if (flow_incomplete(t)) {
            partial++;
            // Codex #147 round 4: what was laid out before the failure is
            // reachable — the root holds it, however late the failure came.
            int64_t need = flow_dump(t, NULL, 0);
            char *text = malloc((size_t)need + 1);
            flow_dump(t, text, (size_t)need + 1);
            if (strstr(text, "block html ") == NULL)
                expect("layout sweep: a partial tree keeps its root", false, NULL);
            free(text);
        }
        else
            expect("layout sweep: a failure is visible", false, NULL);
        flow_free(t);
        if (live != base_live) {
            expect("layout sweep: nothing leaked", false, NULL);
            break;
        }
    }
    printf("libflow layout sweep: %zu allocations failed in turn: %zu NULL, %zu incomplete, "
           "nothing leaked\n", tried, nulls, partial);
    os64_page_free(page);
    os64_html_document_free(doc);
}

int main(int argc, char **argv)
{
    // `--styles FILE` / `--boxes FILE`: print one page's dump, for reading.
    // `--layout FILE WIDTH`: the laid-out page.
    if (argc == 4 && strcmp(argv[1], "--layout") == 0) {
        size_t len = 0;
        char *html = slurp(argv[2], &len);
        if (html == NULL)
            return 2;
        text_setup();
        char *text = layout_dump_of(html, atoi(argv[3]));
        fputs(text != NULL ? text : "(null)\n", stdout);
        free(text);
        free(html);
        return 0;
    }
    if (argc == 3 && (strcmp(argv[1], "--styles") == 0 || strcmp(argv[1], "--boxes") == 0)) {
        size_t len = 0;
        char *html = slurp(argv[2], &len);
        if (html == NULL)
            return 2;
        text_setup();
        char *text = argv[1][2] == 's' ? style_dump_of(html) : boxes_dump_of(html);
        fputs(text != NULL ? text : "(null)\n", stdout);
        free(text);
        free(html);
        return 0;
    }
    text_setup();
    attrs_cases();
    style_cases();
    boxes_cases();
    layout_cases();
    table_cases();
    table_bounds();
    corpus();
    hostile_corpus();
    table_review_cases();
    table_fuzz(3000);
    allocation_sweep();
    boxes_sweep();
    layout_sweep();
    layout_relation_sweep("a page of every family",
        "<!doctype html><h1>T</h1><p>one <b>two</b> three<br>four <img src=known.png>"
        "<ul><li>a<li>b</ul><table border=1><tr><td>c1<td>c2</table><pre>x\ty</pre>"
        "<font face=arial><p>in</p>out</font><p>last words here <a href=x>link <i>it</i></a>"
        "<div align=justify>justified words across a line that wraps more than once</div>"
        "<marquee>m <b>q</b></marquee><ol><li><p>para</p><li>item</ol><q>quoted</q>", 300);
    // A tall first cell in a table the failure stops: its lines, not
    // placed, must not reach the page's edges.
    layout_relation_sweep("a tall cell in a table that stops",
        "<!doctype html><table><tr><td>a<br>b<br>c<br>d<br>e<br>f<br>g<br>h<br>i<br>j"
        "<td>x<br>y<br>z</table>", 300);
    {
        size_t len = 0;
        char *html = slurp("tools/html_corpus/floodgap.html", &len);
        if (html != NULL)
            layout_relation_sweep("floodgap", html, 800);
        expect("relation sweep: the corpus page is there", html != NULL, NULL);
        free(html);
    }
    paint_cases();
    decoration_colour_cases();
    limit_cases();
    text_teardown();
    printf("libflow: %d checks, %d failed%s\n", checks, failures,
           live != 0 ? " (AND LEAKED)" : "");
    return failures != 0 || live != 0 ? 1 : 0;
}
