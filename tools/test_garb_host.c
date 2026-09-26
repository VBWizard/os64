// test_garb_host.c — libgarb's parser on the host, under the sanitizers.
//
// Two jobs. As a DRIVER (`garb_driver MODE`) it reads records on stdin —
// css-parsing-tests' inputs, framed by tools/test_garb_suite.py — and prints
// one JSON line per record, in the suite's representation, for the runner to
// compare. As a TEST (`garb_driver --sweep FILE`) it parses a sheet with
// every allocation failing in turn and asserts nothing leaks and nothing
// crashes: a result that came out short says so.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "garb/garb.h"
#include "garb/select.h"
#include "garb/values.h"
#include "html/html.h"

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

int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    (void)handle;
    (void)buf;
    return (int64_t)len;
}

// ── Records ─────────────────────────────────────────────────────────────

// A field: its length in decimal and a newline, then that many bytes; -1
// for a field that is absent.
static char *field(size_t *len, bool *absent)
{
    long n;
    if (scanf("%ld", &n) != 1)
        return NULL;
    getchar();                          // the newline
    *absent = n < 0;
    if (n < 0)
        n = 0;
    char *b = malloc((size_t)n + 1);
    if (b == NULL || fread(b, 1, (size_t)n, stdin) != (size_t)n)
        exit(2);
    b[n] = '\0';
    *len = (size_t)n;
    return b;
}

static char s_out[4 << 20];

static const char *status_name(garb_status_t st)
{
    return st == GARB_EMPTY ? "empty" : st == GARB_EXTRA_INPUT ? "extra-input" : "invalid";
}

static void one(const char *mode, const char *text, size_t len, const char *protocol,
                const char *environment)
{
    garb_parsed_t r;
    garb_status_t st;
    if (strcmp(mode, "component_value_list") == 0) {
        st = garb_parse_values(text, len, &r);
        garb_dump_values(r.values, r.nvalues, s_out, sizeof(s_out));
    } else if (strcmp(mode, "one_component_value") == 0) {
        st = garb_parse_one_value(text, len, &r);
        if (st == GARB_OK) {
            // One value, without the list around it.
            size_t n = garb_dump_values(r.values, r.nvalues, s_out, sizeof(s_out));
            memmove(s_out, s_out + 1, n - 2);
            s_out[n - 2] = '\0';
        } else
            snprintf(s_out, sizeof(s_out), "[\"error\", \"%s\"]", status_name(st));
    } else if (strcmp(mode, "declaration_list") == 0) {
        st = garb_parse_declaration_list(text, len, &r);
        garb_dump_items(r.items, r.nitems, s_out, sizeof(s_out));
    } else if (strcmp(mode, "blocks_contents") == 0) {
        st = garb_parse_block(text, len, &r);
        garb_dump_items(r.items, r.nitems, s_out, sizeof(s_out));
    } else if (strcmp(mode, "one_declaration") == 0) {
        st = garb_parse_one_declaration(text, len, &r);
        if (st == GARB_OK)
            garb_dump_decl(&r.decl, s_out, sizeof(s_out));
        else
            snprintf(s_out, sizeof(s_out), "[\"error\", \"%s\"]", status_name(st));
    } else if (strcmp(mode, "one_rule") == 0) {
        st = garb_parse_one_rule(text, len, &r);
        if (st == GARB_OK)
            garb_dump_rule(r.rule, s_out, sizeof(s_out));
        else
            snprintf(s_out, sizeof(s_out), "[\"error\", \"%s\"]", status_name(st));
    } else if (strcmp(mode, "rule_list") == 0) {
        st = garb_parse_rules(text, len, &r);
        garb_dump_items(r.items, r.nitems, s_out, sizeof(s_out));
    } else if (strcmp(mode, "stylesheet") == 0) {
        st = garb_parse_sheet_text(text, len, &r);
        garb_dump_items(r.items, r.nitems, s_out, sizeof(s_out));
    } else if (strcmp(mode, "stylesheet_bytes") == 0) {
        st = garb_parse_sheet((const uint8_t *)text, len, protocol, environment, &r);
        size_t n = garb_dump_items(r.items, r.nitems, s_out + 1, sizeof(s_out) - 64);
        s_out[0] = '[';
        snprintf(s_out + 1 + n, 64, ", \"%s\"]", r.encoding != NULL ? r.encoding : "?");
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        exit(2);
    }
    (void)st;
    puts(s_out);
    garb_free(&r);
}

// ── Selectors ───────────────────────────────────────────────────────────

// §6 An+B, for the suite's An+B.json.
static void an_plus_b(const char *text, size_t len)
{
    garb_parsed_t r;
    int32_t a, b;
    if (garb_parse_values(text, len, &r) == GARB_OK && garb_an_plus_b(r.values, r.nvalues, &a, &b))
        printf("[%d, %d]\n", (int)a, (int)b);
    else
        puts("null");
    garb_free(&r);
}

// ── Values ──────────────────────────────────────────────────────────────

// A colour as Color 4 serializes it, or null: the suite's color_*.json.
static void color(const char *text, size_t len)
{
    garb_parsed_t r;
    garb_color_t c;
    garb_parse_values(text, len, &r);
    if (!garb_read_color(r.values, r.nvalues, &c)) {
        puts("null");
    } else if (c.current) {
        puts("\"currentcolor\"");
    } else {
        garb_val_t v = {0};
        v.kind = GARB_V_COLOR;
        v.color = c;
        char b[128];
        garb_val_dump(&v, b, sizeof(b));
        printf("\"%s\"\n", b);
    }
    garb_free(&r);
}

// One declaration: the longhands it sets, `invalid`, or `unknown`; with
// `quirks` first on the line, read as quirks mode reads it.
static void declaration(const char *text, size_t len)
{
    bool quirks = len > 7 && memcmp(text, "quirks ", 7) == 0;
    if (quirks) {
        text += 7;
        len -= 7;
    }
    garb_parsed_t r;
    if (garb_parse_one_declaration(text, len, &r) != GARB_OK) {
        puts("unparsable");
        garb_free(&r);
        return;
    }
    garb_set_t sets[GARB_SETS_MAX];
    int32_t n = garb_read_declaration(&r, &r.decl, quirks, sets);
    if (n < 0) {
        puts("invalid");
    } else if (n == 0) {
        puts("unknown");
    } else {
        for (int32_t i = 0; i < n; i++) {
            char b[512];
            garb_val_dump(&sets[i].value, b, sizeof(b));
            printf("%s%s: %s", i ? "; " : "", garb_prop_name(sets[i].prop), b);
        }
        putchar('\n');
    }
    garb_free(&r);
}

// Every element in document order, template contents aside.
static const os64_html_node_t **s_elements;
static size_t s_nelements, s_elcap;

static void collect(const os64_html_node_t *n)
{
    for (; n != NULL; n = n->next) {
        if (n->kind == OS64_HTML_ELEMENT) {
            if (s_nelements == s_elcap) {
                s_elcap = s_elcap ? s_elcap * 2 : 256;
                s_elements = realloc(s_elements, s_elcap * sizeof(*s_elements));
            }
            s_elements[s_nelements++] = n;
        }
        collect(n->first_child);
    }
}

// A page, and a selector list per line: for each list, null when it is
// invalid, else each selector's [a, b, c, pseudo, [indices it matches]].
static void select_page(const char *html, size_t hlen, const char *lists)
{
    os64_html_options_t opt = os64_html_options_default();
    os64_html_parser_t *hp = os64_html_parser_new(&opt);
    os64_html_parser_feed(hp, html, hlen);
    os64_html_document_t *doc = os64_html_parser_finish(hp);
    s_nelements = 0;
    collect(doc->document->first_child);
    for (const char *line = lists; *line != '\0';) {
        const char *end = strchr(line, '\n');
        size_t n = end != NULL ? (size_t)(end - line) : strlen(line);
        garb_parsed_t r;
        garb_parse_values(line, n, &r);
        garb_selectors_t *sel = garb_selectors_parse(&r, r.values, r.nvalues, doc->quirks);
        if (sel == NULL) {
            puts("null");
        } else {
            fputs("[", stdout);
            for (int32_t i = 0; i < garb_selectors_count(sel); i++) {
                uint32_t sp = garb_selector_specificity(sel, i);
                printf("%s[%u, %u, %u, %d, [", i ? ", " : "", sp >> 20, (sp >> 10) & 1023,
                       sp & 1023, (int)garb_selector_pseudo(sel, i));
                bool first = true;
                for (size_t k = 0; k < s_nelements; k++)
                    if (garb_selector_matches(sel, i, s_elements[k])) {
                        printf("%s%zu", first ? "" : ", ", k);
                        first = false;
                    }
                fputs("]]", stdout);
            }
            puts("]");
        }
        garb_free(&r);
        line = end != NULL ? end + 1 : line + n;
    }
    os64_html_document_free(doc);
}

// ── The allocation sweep ────────────────────────────────────────────────

static void parse_all(const char *text, size_t len)
{
    garb_parsed_t r;
    (void)garb_parse_sheet((const uint8_t *)text, len, NULL, NULL, &r);
    for (int32_t i = 0; i < r.nitems; i++) {
        const garb_rule_t *rule = r.items[i].rule;
        if (r.items[i].kind != GARB_ITEM_RULE || rule == NULL || !rule->has_block)
            continue;
        garb_item_t *items;
        int32_t n;
        if (rule->at) {
            (void)garb_rules_of(&r, rule->block, rule->nblock, &items, &n);
            continue;
        }
        (void)garb_items_of(&r, rule->block, rule->nblock, &items, &n);
        (void)garb_selectors_parse(&r, rule->prelude, rule->nprelude, OS64_HTML_NO_QUIRKS);
        garb_set_t sets[GARB_SETS_MAX];
        for (int32_t k = 0; k < n; k++)
            if (items[k].kind == GARB_ITEM_DECL)
                (void)garb_read_declaration(&r, &items[k].decl, false, sets);
    }
    garb_free(&r);
}

static int sweep(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 2;
    static char text[1 << 20];
    size_t len = fread(text, 1, sizeof(text), f);
    fclose(f);
    allocations = 0;
    fail_at = 0;
    parse_all(text, len);
    size_t worst = allocations;
    int failures = 0;
    for (size_t at = 1; at <= worst; at++) {
        allocations = 0;
        fail_at = at;
        parse_all(text, len);
        fail_at = 0;
        if (live != 0) {
            fprintf(stderr, "FAIL sweep: %zu leaked with failure at %zu\n", live, at);
            failures++;
            live = 0;
        }
    }
    printf("libgarb sweep: each of %zu allocations failed in turn, %s\n", worst,
           failures == 0 ? "nothing leaked" : "LEAKS");
    return failures == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--sweep") == 0)
        return sweep(argv[2]);
    if (argc != 2) {
        fprintf(stderr, "usage: garb_driver MODE < records | --sweep FILE\n");
        return 2;
    }
    for (;;) {
        size_t len, plen, elen;
        bool absent, pabsent, eabsent;
        char *text = field(&len, &absent);
        if (text == NULL)
            break;
        char *protocol = field(&plen, &pabsent);
        char *environment = field(&elen, &eabsent);
        if (strcmp(argv[1], "anplusb") == 0)
            an_plus_b(text, len);
        else if (strcmp(argv[1], "color") == 0)
            color(text, len);
        else if (strcmp(argv[1], "decl") == 0)
            declaration(text, len);
        else if (strcmp(argv[1], "select") == 0)
            select_page(text, len, protocol);
        else
            one(argv[1], text, len, pabsent ? NULL : protocol, eabsent ? NULL : environment);
        fflush(stdout);
        free(text);
        free(protocol);
        free(environment);
    }
    return 0;
}
