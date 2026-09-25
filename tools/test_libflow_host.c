// test_libflow_host.c — markup in, the page's styles and boxes out, checked
// on the host against dumps computed by hand from the standard.
//
// THIS FILE IS THE DURABLE ARTEFACT (LAYOUT.md § Proof before integration).
// Every expected dump here was worked out from the Rendering chapter and
// CSS 2.1 with a pencil, not captured from the engine: a case that passes
// says the engine agrees with the rule.

#include <stdarg.h>
#include <stdint.h>
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

void *os64_malloc(size_t size)
{
    allocations++;
    if (fail_at != 0 && allocations >= fail_at)
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
    if (fail_at != 0 && allocations >= fail_at)
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

// The face's measure of a picture: one whose src says "known" is 40x30;
// everything else has not arrived.
static bool test_oracle(void *ctx, const os64_html_node_t *node, int32_t *w, int32_t *h)
{
    (void)ctx;
    const os64_html_attr_t *src = os64_html_attr(node, "src");
    if (src == NULL || src->value == NULL || strstr(src->value, "known") == NULL)
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
        if (t == NULL)
            nulls++;
        else if (flow_incomplete(t))
            partial++;
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
    allocation_sweep();
    boxes_sweep();
    layout_sweep();
    text_teardown();
    printf("libflow: %d checks, %d failed%s\n", checks, failures,
           live != 0 ? " (AND LEAKED)" : "");
    return failures != 0 || live != 0 ? 1 : 0;
}
