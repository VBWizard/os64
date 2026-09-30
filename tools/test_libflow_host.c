// test_libflow_host.c — markup in, what libflow makes of it out, checked on
// the host against dumps computed by hand from the standard.
//
// THIS FILE IS THE DURABLE ARTEFACT (LAYOUT.md § Proof before integration).
// Every expected dump here was worked out from the Rendering chapter and
// CSS 2.1 with a pencil, not captured from the engine: a case that passes
// says the engine agrees with the rule.

#include <stdarg.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "html/html.h"
#include "page/page.h"
#include "flow/flow.h"
#include "garb/cascade.h"
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

// A page not scrolled: every box's coordinates are the document's then.
static const flow_point_t kNoScroll = {0, 0};

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

// A page with its `style` sheets through libgarb, cascaded at `width` x
// `height` — what yonder does for a page. The sheets and the cascade
// outlive what is styled or laid out with them.
typedef struct {
    os64_html_document_t *doc;
    os64_page_t *page;
    garb_parsed_t sheets[16];
    int32_t n;
    garb_cascade_t *cascade;
    flow_env_t env;
} Sheeted;

static void sheeted_open(Sheeted *p, const char *html, double width, double height)
{
    memset(p, 0, sizeof(*p));
    p->doc = parse(html);
    p->page = os64_page_build(p->doc, kPage, NULL);
    garb_sheet_in_t in[16];
    for (int32_t i = 0; i < os64_page_nsheets(p->page) && p->n < 16; i++) {
        const os64_page_sheet_t *sh = os64_page_sheet(p->page, i);
        if (sh->linked)
            continue;
        garb_parse_style_element(sh->node, &p->sheets[p->n]);
        in[p->n] = (garb_sheet_in_t){.sheet = &p->sheets[p->n], .media = sh->media};
        p->n++;
    }
    p->cascade = garb_cascade(in, p->n, p->doc, (garb_env_t){width, height});
    p->env = kEnv;
    p->env.cascade = p->cascade;
    p->env.viewport_height = (int32_t)height;
}

static void sheeted_close(Sheeted *p)
{
    garb_cascade_free(p->cascade);
    for (int32_t k = 0; k < p->n; k++)
        garb_free(&p->sheets[k]);
    os64_page_free(p->page);
    os64_html_document_free(p->doc);
}

static char *cascade_dump_of(const char *html, double width, double height)
{
    Sheeted p;
    sheeted_open(&p, html, width, height);
    FStyles *styles = f_style_build(p.doc, p.page, &p.env);
    char *text = NULL;
    if (styles != NULL) {
        int64_t need = f_style_dump(styles, NULL, 0);
        text = malloc((size_t)need + 1);
        f_style_dump(styles, text, (size_t)need + 1);
    }
    f_style_free(styles);
    sheeted_close(&p);
    return text;
}

static char *cascade_layout_of(const char *html, int32_t width)
{
    Sheeted p;
    sheeted_open(&p, html, width, 600);
    flow_tree_t *tree = flow_layout(p.doc, p.page, width, &p.env);
    char *text = NULL;
    if (tree != NULL) {
        int64_t need = flow_dump(tree, NULL, 0);
        text = malloc((size_t)need + 1);
        flow_dump(tree, text, (size_t)need + 1);
    }
    flow_free(tree);
    sheeted_close(&p);
    return text;
}

static void cascade_layout_case(const char *name, const char *html, int32_t width,
                                const char *expected)
{
    char *got = cascade_layout_of(html, width);
    bool ok = got != NULL && strcmp(got, expected) == 0;
    expect(name, ok, NULL);
    if (!ok)
        fprintf(stderr, "---- expected\n%s---- got\n%s----\n", expected, got ? got : "(null)\n");
    free(got);
}

static void cascade_case(const char *name, double width, const char *html,
                         const char *expected)
{
    char *got = cascade_dump_of(html, width, 600);
    bool ok = got != NULL && strcmp(got, expected) == 0;
    expect(name, ok, NULL);
    if (!ok)
        fprintf(stderr, "---- expected\n%s---- got\n%s----\n", expected, got ? got : "(null)\n");
    free(got);
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
#include "test_libflow_cascade.inc"

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

// The same over a page whose sheets name families, which the style pass
// allocates for: the cascade is built once, outside the sweep, as yonder
// builds it before any layout.
static void cascade_sweep(void)
{
    static const char kHtml[] =
        "<!doctype html><style>body { font-family: Verdana, sans-serif; margin: 1em }"
        " p { font-family: 'Courier New', monospace; color: inherit } .x { width: calc(50% + 1em) }"
        "</style><p>a<p class=x>b<span style='font-family: Georgia'>c</span>";
    os64_html_document_t *doc = parse(kHtml);
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    const os64_page_sheet_t *sh = os64_page_sheet(page, 0);
    garb_parsed_t sheet;
    garb_parse_style_element(sh->node, &sheet);
    garb_sheet_in_t in = {.sheet = &sheet};
    garb_cascade_t *c = garb_cascade(&in, 1, doc, (garb_env_t){800, 600});
    flow_env_t env = kEnv;
    env.cascade = c;
    env.viewport_height = 600;
    char *whole = cascade_dump_of(kHtml, 800, 600);
    size_t base_live = live;
    allocations = 0;
    FStyles *st = f_style_build(doc, page, &env);
    size_t count = allocations;
    f_style_free(st);
    size_t tried = 0;
    for (size_t at = 1; at <= count; at++) {
        allocations = 0;
        fail_at = at;
        st = f_style_build(doc, page, &env);
        fail_at = 0;
        tried++;
        if (st != NULL) {
            int64_t need = f_style_dump(st, NULL, 0);
            char *got = malloc((size_t)need + 1);
            f_style_dump(st, got, (size_t)need + 1);
            expect("cascade sweep: a build that succeeds is whole", strcmp(got, whole) == 0, NULL);
            free(got);
        }
        f_style_free(st);
        if (live != base_live) {
            expect("cascade sweep: nothing leaked", false, NULL);
            break;
        }
    }
    printf("libflow cascade sweep: each of %zu allocations failed in turn, nothing leaked\n",
           tried);
    free(whole);
    garb_cascade_free(c);
    garb_free(&sheet);
    os64_page_free(page);
    os64_html_document_free(doc);
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

// The first public TEXT box whose text starts with `text`, or NULL.
static const flow_box_t *public_text(const flow_box_t *b, const char *text)
{
    if (b == NULL)
        return NULL;
    if (b->kind == FLOW_BOX_TEXT && b->length >= strlen(text) &&
        strncmp(b->text, text, strlen(text)) == 0)
        return b;
    for (const flow_box_t *c = b->first; c != NULL; c = c->next) {
        const flow_box_t *found = public_text(c, text);
        if (found != NULL)
            return found;
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
    // And the public box a painter reads says the same.
    flow_tree_t *t = flow_layout(doc, page, 400, &kEnv);
    const flow_box_t *x = t != NULL ? public_text(flow_root(t), "x") : NULL;
    expect("the public box carries each decoration's colour",
           x != NULL && x->underline_color == kEnv.ink && x->line_through_color == 0xFF0000,
           NULL);
    flow_free(t);
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

static int32_t lowest_y(const flow_box_t *b)
{
    int32_t y = b != NULL ? b->rect.y : 0;
    for (const flow_box_t *c = b != NULL ? b->first : NULL; c != NULL; c = c->next) {
        int32_t k = lowest_y(c);
        y = k < y ? k : y;
    }
    return y;
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

// What the alarm is guarding, named in its failure.
static const char *s_cost_case = "";

static void cost_hung(int sig)
{
    (void)sig;
    static const char msg[] = "FAIL the build never ended: ";
    (void)!write(2, msg, sizeof(msg) - 1);
    (void)!write(2, s_cost_case, strlen(s_cost_case));
    (void)!write(2, "\n", 1);
    _exit(1);
}

// A question about an element's ancestors or siblings is answered from its
// parent's record, never by a walk: a walk per element is quadratic. Here a
// ruby of a hundred thousand annotations (each rt asking whether an rp is
// among its siblings) and quotes nested to the build's depth bound, under
// an alarm a walk could not beat.
static void cost_cases(void)
{
    size_t n = 100000;
    char *html = malloc(n * 12 + 4096);
    size_t at = (size_t)sprintf(html, "<!doctype html><p><ruby>");
    for (size_t i = 0; i < n; i++)
        at += (size_t)sprintf(html + at, "a<rt>b");
    at += (size_t)sprintf(html + at, "</ruby><p>");
    for (int i = 0; i < 400; i++)
        at += (size_t)sprintf(html + at, "<q>");
    sprintf(html + at, "x");
    // The question here is time, not memory: a budget the page cannot
    // reach, so a walk would show as the alarm and not as a stop.
    flow_env_t roomy = kEnv;
    roomy.max_arena_bytes = (size_t)1 << 30;
    signal(SIGALRM, cost_hung);
    s_cost_case = "a descendant or sibling question walks";
    alarm(60);
    os64_html_document_t *doc = parse(html);
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    FStyles *styles = f_style_build(doc, page, &roomy);
    FBoxes *boxes = styles != NULL ? f_boxes_build(doc, page, styles, &roomy) : NULL;
    alarm(0);
    expect("a ruby of 100000 annotations and 400 nested quotes build whole",
           boxes != NULL && !boxes->incomplete, NULL);
    f_boxes_free(boxes);
    f_style_free(styles);
    os64_page_free(page);
    os64_html_document_free(doc);
    free(html);

    // A page that multiplies its records: 400 inlines open, then a block
    // interrupting the run 60000 times, each reopening all 400 — 3.7 GB of
    // boxes unbounded. At the budget the box build stops, incomplete, having
    // reserved no more than the budget, and the layout of what it holds
    // stays inside its own. Pass 1 is not budgeted: one record an element.
    n = 60000;
    html = malloc(n * 16 + 4096);
    at = (size_t)sprintf(html, "<!doctype html>");
    for (int i = 0; i < 400; i++)
        at += (size_t)sprintf(html + at, "<b>");
    for (size_t i = 0; i < n; i++)
        at += (size_t)sprintf(html + at, "x<div></div>");
    flow_env_t tight = kEnv;
    tight.max_arena_bytes = (size_t)16 << 20;
    s_cost_case = "a page that multiplies its boxes met no budget";
    alarm(60);
    doc = parse(html);
    page = os64_page_build(doc, kPage, NULL);
    styles = f_style_build(doc, page, &tight);
    boxes = styles != NULL ? f_boxes_build(doc, page, styles, &tight) : NULL;
    FLayout *lay = boxes != NULL ? f_layout(boxes, doc, page, &tight, 800) : NULL;
    alarm(0);
    expect("a page that multiplies its boxes stops at the budget, incomplete",
           boxes != NULL && boxes->incomplete && boxes->arena.reserved <= tight.max_arena_bytes,
           NULL);
    expect("and what it holds lays out inside the budget too",
           lay != NULL && lay->incomplete && lay->arena.reserved <= tight.max_arena_bytes, NULL);
    f_layout_free(lay);
    f_boxes_free(boxes);
    f_style_free(styles);
    os64_page_free(page);
    os64_html_document_free(doc);
    free(html);

    // A table's working memory is declared, not written — ten
    // `<col span=1000>`s are ten thousand columns in a few bytes — and a
    // cell lays the next table out while its table's columns wait, so 120
    // tables nested that way held 56 MB of it from a 20 KB page. It shares
    // the arena's budget: at 8 MiB the layout stops, incomplete, and never
    // held more than the budget between them.
    html = malloc(120 * 200 + 64);
    at = (size_t)sprintf(html, "<!doctype html>");
    for (int i = 0; i < 120; i++) {
        at += (size_t)sprintf(html + at, "<table>");
        for (int k = 0; k < 10; k++)
            at += (size_t)sprintf(html + at, "<col span=1000>");
        at += (size_t)sprintf(html + at, "<tr><td>");
    }
    sprintf(html + at, "x");
    flow_env_t eight = kEnv;
    eight.max_arena_bytes = (size_t)8 << 20;
    s_cost_case = "a chain of declared columns met no budget";
    alarm(60);
    doc = parse(html);
    page = os64_page_build(doc, kPage, NULL);
    styles = f_style_build(doc, page, &eight);
    boxes = styles != NULL ? f_boxes_build(doc, page, styles, &eight) : NULL;
    lay = boxes != NULL ? f_layout(boxes, doc, page, &eight, 800) : NULL;
    alarm(0);
    expect("a chain of declared columns stops at the budget, incomplete",
           boxes != NULL && !boxes->incomplete && lay != NULL && lay->incomplete &&
           lay->scratch_peak <= eight.max_arena_bytes &&
           lay->arena.reserved <= eight.max_arena_bytes, NULL);
    f_layout_free(lay);
    f_boxes_free(boxes);
    f_style_free(styles);
    os64_page_free(page);
    os64_html_document_free(doc);
    free(html);
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
               got != NULL && strstr(got, "      block li 48 48 244 60\n") != NULL &&
                   strstr(got, "        marker \"\xe2\x80\xa2 \" 12 54 36 48\n") != NULL,
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
    // So is an absolute box's level (an open dialog is one with no sheet):
    // it is laid out from inside its containing block's frame.
    expect("absolute boxes nested exactly to the bound are whole",
           nested_whole("<dialog open>", "</dialog>", (F_DEPTH_MAX - 2) / 2, 4096) == 1, NULL);
    expect("an absolute level costs two descents",
           nested_whole("<dialog open>", "</dialog>", (F_DEPTH_MAX - 2) / 2 + 1, 4096) == 0, NULL);
    expect("an absolute chain past the bound is incomplete, not a crash",
           nested_whole("<dialog open>", "</dialog>", 2000, 4096) == 0, NULL);
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
    // And so is every box a face reads: the last picture sits on the last
    // pixel, and nothing wraps round to a negative y.
    const flow_box_t *last = t != NULL ? flow_image(t, flow_nimages(t) - 1) : NULL;
    expect("a box past INT32_MAX sits on the last pixel",
           last != NULL && flow_nimages(t) == 2200 && last->rect.y == INT32_MAX &&
               last->rect.h == 0, NULL);
    expect("no box of a page past INT32_MAX wraps above it",
           t != NULL && lowest_y(flow_root(t)) >= 0, NULL);
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

// Every table in a laid-out tree holds its grid: no row reaches past the
// table's own box. Returns the first offending table's node, or NULL.
static const FBox *grid_outgrows(const FBox *b)
{
    if (b == NULL || !b->placed)
        return NULL;
    if (b->kind == FB_TABLE)
        for (const FBox *g = b->first; g != NULL; g = g->next) {
            const FBox *rows = g->kind == FB_ROW_GROUP ? g->first : g;
            for (const FBox *r = rows; r != NULL; r = g->kind == FB_ROW_GROUP ? r->next : NULL)
                if (r->kind == FB_ROW && r->placed && r->x + r->w > b->x + b->w)
                    return b;
        }
    for (const FLine *ln = b->lines; ln != NULL; ln = ln->next)
        for (const FFrag *fr = ln->frags; fr != NULL; fr = fr->next)
            if (fr->kind == FF_ATOMIC && fr->item->content != NULL) {
                const FBox *hit = grid_outgrows(fr->item->content);
                if (hit != NULL)
                    return hit;
            }
    for (const FBox *c = b->first; c != NULL; c = c->next) {
        const FBox *hit = grid_outgrows(c);
        if (hit != NULL)
            return hit;
    }
    return NULL;
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
            // And the invariant the table review kept finding the edges of:
            // whatever its columns are, no table's grid outgrows it.
            FStyles *st = f_style_build(doc, page, &env);
            FBoxes *bx = st != NULL ? f_boxes_build(doc, page, st, &env) : NULL;
            FLayout *lay = bx != NULL ? f_layout(bx, doc, page, &env, widths[k]) : NULL;
            if (lay != NULL && grid_outgrows(bx->root) != NULL)
                expect("table fuzz: no table's grid outgrows it", false, html);
            f_layout_free(lay);
            f_boxes_free(bx);
            f_style_free(st);
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
    // its text's 68 wide, and the next cell starts after it. The marquee's
    // overflow is hidden, so it sits on its bottom edge (§ 10.8.1) and the
    // strut hangs 6 below: the row is 26.
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
    // Round 4. A 75% column beside a 64px least: the table is its least,
    // 128, and the percentage gives back so the row is the table's 128.
    static const char *const pct_room[] = {"table table 8 8 128 20", "row tr 8 8 128 20",
                                           "cell td 8 8 64 20", "cell td 72 8 64 20"};
    expect("a percentage column leaves the others their least",
           has_lines("<!doctype html><table width=120 cellspacing=0 cellpadding=0><tr>"
                     "<td width=75%><img src=known.png width=64 height=1><td>"
                     "<img src=known.png width=64 height=1></table>", 400, pct_room, 4), NULL);
    // Round 6. A row group's set height is its rows' least together: they
    // need 20 and 40, the group asks 100, and the 40 more goes 13 and 27.
    static const char *const group_h[] = {"row-group tbody 8 8 16 100", "row tr 8 8 16 33",
                                          "row tr 8 41 16 67", "block p 8 124 384 20"};
    expect("a row group's set height reaches its rows",
           has_lines("<!doctype html><table cellspacing=0 cellpadding=0><tbody height=100>"
                     "<tr><td>x<tr><td>yy<br>y</tbody></table><p>after</p>", 400, group_h, 4),
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

#include "test_libflow_door.inc"
#include "test_libflow_position.inc"
#include "test_libflow_flex.inc"
#include "test_libflow_grid.inc"

int main(int argc, char **argv)
{
    // `--write-corpus`: regenerate tools/html_corpus/*.boxes from the engine.
    if (argc == 2 && strcmp(argv[1], "--write-corpus") == 0) {
        text_setup();
        corpus_dumps(true);
        text_teardown();
        return 0;
    }
    bool full_fuzz = argc >= 2 && strcmp(argv[1], "--fuzz") == 0;
    // `--fuzz PAGE K N`: only the full fuzz, of one corpus page, and only
    // its Kth of N shares — the whole thing is hours in one process.
    if (full_fuzz && argc == 5) {
        text_setup();
        fuzz_share(argv[2], (size_t)atoi(argv[3]), (size_t)atoi(argv[4]));
        text_teardown();
        printf("libflow: %d checks, %d failed%s\n", checks, failures,
               live != 0 ? " (AND LEAKED)" : "");
        return failures != 0 || live != 0 ? 1 : 0;
    }
    // `--styles FILE` / `--boxes FILE`: print one page's dump, for reading.
    // `--layout FILE WIDTH`: the laid-out page; `--cascade FILE WIDTH`, the
    // same with its style sheets, at 600 high.
    if (argc == 4 && (strcmp(argv[1], "--layout") == 0 || strcmp(argv[1], "--cascade") == 0)) {
        size_t len = 0;
        char *html = slurp(argv[2], &len);
        if (html == NULL)
            return 2;
        text_setup();
        char *text = argv[1][2] == 'l' ? layout_dump_of(html, atoi(argv[3]))
                                       : cascade_layout_of(html, atoi(argv[3]));
        fputs(text != NULL ? text : "(null)\n", stdout);
        free(text);
        free(html);
        return 0;
    }
    // `--sheet-styles FILE`: the styles with the page's sheets, at 800 x 600.
    if (argc == 3 && (strcmp(argv[1], "--styles") == 0 || strcmp(argv[1], "--boxes") == 0 ||
                      strcmp(argv[1], "--sheet-styles") == 0)) {
        size_t len = 0;
        char *html = slurp(argv[2], &len);
        if (html == NULL)
            return 2;
        text_setup();
        char *text = argv[1][2] == 's' && argv[1][3] == 't' ? style_dump_of(html)
                   : argv[1][2] == 's' ? cascade_dump_of(html, 800, 600)
                   : boxes_dump_of(html);
        fputs(text != NULL ? text : "(null)\n", stdout);
        free(text);
        free(html);
        return 0;
    }
    text_setup();
    attrs_cases();
    style_cases();
    cascade_cases();
    boxes_cases();
    layout_cases();
    table_cases();
    table_bounds();
    corpus();
    hostile_corpus();
    table_review_cases();
    table_fuzz(3000);
    allocation_sweep();
    cascade_sweep();
    boxes_sweep();
    layout_sweep();
    door_cases();
    position_cases();
    flex_cases();
    grid_cases();
    fixed_cases();
    paint_cases();
    decoration_colour_cases();
    limit_cases();
    cost_cases();
    layout_relation_sweep("a page of every family", kSweepPage, 300);
    layout_relation_sweep("a positioned page", kPositionedSweepPage, 300);
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
    corpus_dumps(false);
    fuzz_share(NULL, 0, full_fuzz ? 1 : 0);
    text_teardown();
    printf("libflow: %d checks, %d failed%s\n", checks, failures,
           live != 0 ? " (AND LEAKED)" : "");
    return failures != 0 || live != 0 ? 1 : 0;
}
