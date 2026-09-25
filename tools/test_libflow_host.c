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

// The theme a face would hand in, in colours no page case uses, so the
// dump's `ink`, `link` and `paper` can only mean the environment's.
static const flow_env_t kEnv = {
    .viewport_font_px = 16,
    .default_generic = FLOW_GENERIC_SERIF,
    .ink = 0x101010,
    .link_ink = 0x1010ee,
    .paper = 0xfefefe,
};

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
        printf("corpus %-22s %zu styled elements\n", kCorpus[i], lines);
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

int main(void)
{
    attrs_cases();
    style_cases();
    corpus();
    allocation_sweep();
    printf("libflow: %d checks, %d failed%s\n", checks, failures,
           live != 0 ? " (AND LEAKED)" : "");
    return failures != 0 || live != 0 ? 1 : 0;
}
