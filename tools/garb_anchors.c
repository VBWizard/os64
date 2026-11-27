/* What a style sheet's anchored :has() compounds cost libgarb's cascade
 * (select.h § AN ANCHORED :has()): for each sheet file named, every rule
 * holding one, and each anchor's reach (down, siblings) and verdict:
 * filtered by its own compound, filtered by an enclosing one, or BARE
 * (nothing to turn most elements away before its :has() is tried, so every
 * ask of it is a walk: cascade.c has_visit). Then the sheet's
 * counts, and with more than one sheet their sum, which is what a cascade
 * applying them all holds. `-q` prints the counts alone.
 *
 * `-p page.html` measures what that costs on a page: the sheets named, and
 * the page's own <style> elements, cascaded over the page, then 200 class
 * changes on elements drawn by a fixed seed, each followed by an update
 * (or, refused, a fresh cascade), and printed: the first build's time,
 * the updates taken and their mean, the refusals by reason and the mean of
 * the fresh cascades they cost. GARB_ANCHORS_BEFORE builds it against a
 * libgarb that cannot count anchors, to measure the tree before them.
 *
 * Built and run by tools/garb_anchors.sh. It reads the files it is given
 * and names nothing else: a page under study is the caller's. */
#define _POSIX_C_SOURCE 200809L     /* clock_gettime, under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "garb/cascade.h"

#include "garb/garb.h"
#include "garb/select.h"

/* What libgarb asks of the system. */
void os64_exit(int32_t code) { exit(code); }
void *os64_malloc(size_t size) { return malloc(size ? size : 1); }
void *os64_calloc(size_t count, size_t size) { return calloc(count ? count : 1, size ? size : 1); }
void *os64_realloc(void *ptr, size_t size) { return realloc(ptr, size ? size : 1); }
void os64_free(void *ptr) { free(ptr); }
int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    return (int64_t)fwrite(buf, 1, len, handle == 2 ? stderr : stdout);
}

typedef struct {
    long rules, anchored, anchors, bare, by_enclosing;
} Counts;

static int quiet;

static void walk(garb_parsed_t *p, const garb_item_t *items, int32_t n, int depth, Counts *k)
{
    for (int32_t i = 0; i < n; i++) {
        if (items[i].kind != GARB_ITEM_RULE) continue;
        const garb_rule_t *r = items[i].rule;
        if (r->at) {
            garb_item_t *inner = NULL;
            int32_t ninner = 0;
            if (r->has_block && depth < 8 && garb_rules_of(p, r->block, r->nblock, &inner, &ninner))
                walk(p, inner, ninner, depth + 1, k);
            continue;
        }
        k->rules++;
        garb_selectors_t *sel = garb_selectors_parse(p, r->prelude, r->nprelude, OS64_HTML_NO_QUIRKS, false);
        int32_t held = sel ? garb_selectors_anchors(sel, NULL, 0) : 0;
        if (held > 0) {
            garb_anchor_t *anchors = calloc((size_t)held, sizeof(*anchors));
            garb_selectors_anchors(sel, anchors, held);
            k->anchored++;
            k->anchors += held;
            if (!quiet) {
                char text[4096];
                garb_dump_values(r->prelude, r->nprelude, text, sizeof(text));
                printf("  %s\n", text);
            }
            for (int32_t a = 0; a < held; a++) {
                k->bare += anchors[a].bare;
#ifdef GARB_ANCHORS_BEFORE
                bool own = !anchors[a].bare;
#else
                bool own = !anchors[a].bare && anchors[a].nouter == 0;
#endif
                k->by_enclosing += !anchors[a].bare && !own;
                if (!quiet)
                    printf("    anchor %d: %s%s%s, %s\n", a + 1, anchors[a].down ? "down" : "",
                           anchors[a].down && anchors[a].siblings ? "+" : "", anchors[a].siblings ? "siblings" : "",
                           anchors[a].bare ? "BARE" : own ? "filtered by its own compound"
                                                          : "filtered by an enclosing compound");
            }
            free(anchors);
        }
        /* Nested style rules (CSS Nesting) inside the block. */
        garb_item_t *inner = NULL;
        int32_t ninner = 0;
        if (r->has_block && depth < 8 && garb_items_of(p, r->block, r->nblock, &inner, &ninner))
            walk(p, inner, ninner, depth + 1, k);
    }
}

static void counts(const char *name, const Counts *k)
{
    printf("%s: %ld rules, %ld with an anchored :has(), %ld anchors: %ld bare, %ld filtered by an enclosing "
           "compound\n", name, k->rules, k->anchored, k->anchors, k->bare, k->by_enclosing);
}

static char *slurp(const char *path, size_t *len)
{
    FILE *in = fopen(path, "rb");
    if (!in) return NULL;
    fseek(in, 0, SEEK_END);
    long n = ftell(in);
    fseek(in, 0, SEEK_SET);
    char *text = malloc((size_t)n + 1);
    size_t got = fread(text, 1, (size_t)n, in);
    fclose(in);
    if (got != (size_t)n) { free(text); return NULL; }
    text[n] = '\0';
    *len = (size_t)n;
    return text;
}
static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

#define SHEETS_MAX 512
static garb_parsed_t s_sheets[SHEETS_MAX];
static int s_nsheets;

// The page's own <style> elements, each a sheet.
static void page_styles(const os64_html_node_t *root)
{
    for (const os64_html_node_t *x = root; x != NULL; x = x->next) {
        if (x->kind == OS64_HTML_ELEMENT && x->name && !strcmp(x->name, "style") && s_nsheets < SHEETS_MAX) {
            size_t len = 0;
            for (const os64_html_node_t *t = x->first_child; t; t = t->next) len += t->kind == OS64_HTML_TEXT ? t->text_len : 0;
            char *text = malloc(len + 1), *at = text;
            for (const os64_html_node_t *t = x->first_child; t; t = t->next)
                if (t->kind == OS64_HTML_TEXT) { memcpy(at, t->text, t->text_len); at += t->text_len; }
            if (garb_parse_sheet((const uint8_t *)text, len, NULL, NULL, &s_sheets[s_nsheets]) == GARB_OK) s_nsheets++;
            free(text);
        }
        if (x->first_child) page_styles(x->first_child);
    }
}
static int elements_of(os64_html_node_t *root, os64_html_node_t **out, int cap, int n)
{
    for (os64_html_node_t *x = root; x != NULL; x = x->next) {
        if (x->kind == OS64_HTML_ELEMENT && n < cap) out[n++] = x;
        if (x->first_child) n = elements_of(x->first_child, out, cap, n);
    }
    return n;
}

static int measure(const char *page)
{
    size_t len = 0;
    char *bytes = slurp(page, &len);
    if (bytes == NULL) { fprintf(stderr, "garb_anchors: cannot read %s\n", page); return 1; }
    os64_html_options_t opt = os64_html_options_default();
    os64_html_parser_t *parser = os64_html_parser_new(&opt);
    os64_html_parser_feed(parser, bytes, len);
    os64_html_document_t *doc = os64_html_parser_finish(parser);
    free(bytes);
    if (doc == NULL) { fprintf(stderr, "garb_anchors: %s did not parse\n", page); return 1; }
    page_styles(doc->document);
    garb_sheet_in_t in[SHEETS_MAX];
    for (int i = 0; i < s_nsheets; i++) in[i] = (garb_sheet_in_t){.sheet = &s_sheets[i]};
    garb_env_t env = {.width = 1280, .height = 800};
    double t0 = now_ms();
    garb_cascade_t *c = garb_cascade(in, s_nsheets, doc, env);
    double built = now_ms() - t0;
    int32_t anchors = -1, bare = -1;
#ifndef GARB_ANCHORS_BEFORE
    garb_cascade_anchors(c, &anchors, &bare);
#endif
    static os64_html_node_t *els[200000];
    int n = elements_of(doc->document, els, 200000, 0);
    printf("%s: %d sheets, %d elements; cascade built in %.1f ms, tracks changes %s, anchors %d, bare %d\n", page,
           s_nsheets, n, built, garb_cascade_tracks_changes(c) ? "yes" : "no", anchors, bare);
    uint64_t seed = 0x2545F4914F6CDD1Dull;
    unsigned taken = 0, refused[GARB_UPDATE_REASONS] = {0};
    double update_ms = 0, fresh_ms = 0;
    for (int step = 0; step < 200; step++) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        os64_html_node_t *e = els[seed % (uint64_t)n];
        const os64_html_attr_t *was = os64_html_attr(e, "class");
        char cls[1024];
        snprintf(cls, sizeof(cls), "%s", was && was->value ? was->value : "");
        size_t l = strlen(cls);
        if (l >= 3 && !strcmp(cls + l - 3, " gc")) cls[l - 3] = '\0';
        else if (l + 4 < sizeof(cls)) strcat(cls, " gc");
        os64_html_set_attr(doc, e, "class", cls, strlen(cls));
        double u0 = now_ms();
        if (garb_cascade_update(c)) {
            update_ms += now_ms() - u0;
            taken++;
            continue;
        }
        refused[garb_cascade_update_refusal(c)]++;
        double f0 = now_ms();
        garb_cascade_t *fresh = garb_cascade(in, s_nsheets, doc, env);
        fresh_ms += now_ms() - f0;
        garb_cascade_free(c);
        c = fresh;
    }
    unsigned fresh = 200 - taken;
    printf("  200 class changes: %u updates, mean %.2f ms; %u fresh cascades, mean %.1f ms", taken,
           taken ? update_ms / taken : 0.0, fresh, fresh ? fresh_ms / fresh : 0.0);
    for (int i = 1; i < GARB_UPDATE_REASONS; i++)
        if (refused[i]) printf("; %u refused %s", refused[i], garb_update_refusal_name((garb_update_refusal_t)i));
    printf("\n");
    garb_cascade_free(c);
    os64_html_document_free(doc);
    return 0;
}

int main(int argc, char **argv)
{
    Counts all = {0};
    const char *page = NULL;
    int sheets = 0;
    for (int f = 1; f < argc; f++) {
        if (!strcmp(argv[f], "-q")) { quiet = 1; continue; }
        if (!strcmp(argv[f], "-p") && f + 1 < argc) { page = argv[++f]; quiet = 1; continue; }
        size_t len = 0;
        char *text = slurp(argv[f], &len);
        if (!text) { fprintf(stderr, "garb_anchors: cannot read %s\n", argv[f]); continue; }
        garb_parsed_t *p = &s_sheets[s_nsheets];
        Counts k = {0};
        if (s_nsheets < SHEETS_MAX && garb_parse_sheet((const uint8_t *)text, len, NULL, NULL, p) == GARB_OK) {
            if (!quiet) printf("== %s\n", argv[f]);
            walk(p, p->items, p->nitems, 0, &k);
            if (!quiet || (k.anchors && !page)) counts(argv[f], &k);
            all.rules += k.rules; all.anchored += k.anchored; all.anchors += k.anchors;
            all.bare += k.bare; all.by_enclosing += k.by_enclosing;
            sheets++;
            s_nsheets++;
        }
        free(text);
    }
    if (sheets > 1) counts("all", &all);
    int rc = page ? measure(page) : 0;
    for (int i = 0; i < s_nsheets; i++) garb_free(&s_sheets[i]);
    return rc;
}
