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
        if (rule->at)
            (void)garb_rules_of(&r, rule->block, rule->nblock, &items, &n);
        else
            (void)garb_items_of(&r, rule->block, rule->nblock, &items, &n);
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
        one(argv[1], text, len, pabsent ? NULL : protocol, eabsent ? NULL : environment);
        fflush(stdout);
        free(text);
        free(protocol);
        free(environment);
    }
    return 0;
}
