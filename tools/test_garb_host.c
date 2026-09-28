// test_garb_host.c — libgarb's parser on the host, under the sanitizers.
//
// Its jobs. As a DRIVER (`garb_driver MODE`) it reads records on stdin —
// css-parsing-tests' inputs, framed by tools/test_garb_suite.py — and prints
// one JSON line per record, in the suite's representation, for the runner to
// compare. As a TEST (`garb_driver --sweep FILE`) it parses a sheet with
// every allocation failing in turn and asserts nothing leaks and nothing
// crashes: a result that came out short says so. And two probes: `--deep`
// parses a million nested `(` on a thread with a small stack (past the
// depth bound the parser must find the end without recursing), and
// `--numbers` dumps numbers past int64_t's reach and past a double's.

#include <pthread.h>
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

// The sheet's top-level items, each dumped on its own, into `dumps`: what
// the prefix check compares. Returns how many.
static int32_t item_dumps(const char *text, size_t len, char ***dumps)
{
    garb_parsed_t r;
    (void)garb_parse_sheet((const uint8_t *)text, len, NULL, NULL, &r);
    *dumps = calloc((size_t)(r.nitems > 0 ? r.nitems : 1), sizeof(char *));
    for (int32_t i = 0; i < r.nitems; i++) {
        size_t n = garb_dump_items(&r.items[i], 1, NULL, 0);
        (*dumps)[i] = malloc(n + 1);
        garb_dump_items(&r.items[i], 1, (*dumps)[i], n + 1);
    }
    int32_t n = r.nitems;
    garb_free(&r);
    return n;
}

static void free_dumps(char **dumps, int32_t n)
{
    for (int32_t i = 0; i < n; i++)
        free(dumps[i]);
    free(dumps);
}

// Every allocation fails in turn: nothing leaks, nothing crashes, and what
// comes back is a PREFIX of the whole result — each item it has is the
// whole parse's item, dumped the same. The first failure ends the parse,
// so nothing cut short is published (a token half-built, a declaration
// missing a value).
static int sweep_text(const char *what, const char *text, size_t len)
{
    allocations = 0;
    fail_at = 0;
    parse_all(text, len);
    size_t worst = allocations;
    char **whole;
    int32_t nwhole = item_dumps(text, len, &whole);
    int failures = 0;
    for (size_t at = 1; at <= worst; at++) {
        allocations = 0;
        fail_at = at;
        parse_all(text, len);
        allocations = 0;
        char **part;
        int32_t npart = item_dumps(text, len, &part);
        fail_at = 0;
        bool prefix = npart <= nwhole;
        for (int32_t i = 0; prefix && i < npart; i++)
            prefix = strcmp(part[i], whole[i]) == 0;
        if (!prefix) {
            fprintf(stderr, "FAIL sweep %s: failure at %zu is not a prefix of the whole\n", what,
                    at);
            failures++;
        }
        free_dumps(part, npart);
        if (live != 0) {
            fprintf(stderr, "FAIL sweep %s: %zu leaked with failure at %zu\n", what, live, at);
            failures++;
            live = 0;
        }
    }
    free_dumps(whole, nwhole);
    printf("libgarb sweep, %s: each of %zu allocations failed in turn, %s\n", what, worst,
           failures == 0 ? "every result a prefix, nothing leaked" : "FAILED");
    return failures;
}

static int sweep(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 2;
    static char text[1 << 20];
    size_t len = fread(text, 1, sizeof(text), f);
    fclose(f);
    int failures = sweep_text(path, text, len);
    // A token that outgrows the tokenizer's first buffer (64 bytes) with a
    // two-byte character across the boundary: the growth fails between the
    // character's bytes, and that token must never be published.
    static const char long_ident[] =
        "a { b: c } xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\xc3\xa9 { d: e }";
    failures += sweep_text("a long identifier", long_ident, sizeof(long_ident) - 1);
    return failures == 0 ? 0 : 1;
}

typedef struct {
    const char *text;
    size_t len;
    garb_status_t st;
    bool incomplete;
} Deep;

static void *deep_parse(void *arg)
{
    Deep *d = arg;
    garb_parsed_t r = {0};
    d->st = garb_parse_sheet_text(d->text, d->len, &r);
    d->incomplete = r.incomplete;
    garb_free(&r);
    return NULL;
}

static int deep(void)
{
    size_t n = 1000000, cap = n + 64, at = 0;
    char *text = malloc(cap);
    at += (size_t)snprintf(text, cap, "a { b: ");
    memset(text + at, '(', n);
    at += n;
    at += (size_t)snprintf(text + at, cap - at, " }");
    Deep d = {text, at, GARB_OK, false};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    pthread_t th;
    int ok = pthread_create(&th, &attr, deep_parse, &d) == 0;
    if (ok)
        pthread_join(th, NULL);
    ok = ok && d.st == GARB_OK && d.incomplete;
    printf("libgarb deep: a million nested ( on a 512K stack: %s\n",
           ok ? "parsed, and says it is incomplete" : "FAILED");
    free(text);
    return ok ? 0 : 1;
}

// Numbers between int64_t's reach and a double's: the dump must test the
// range before it casts (float-cast-overflow is on in this harness).
static int numbers(void)
{
    const char *text = "1e19 -1e19 99999999999999999999 1e300 -1e300 1e999 5e-324 1.71e308";
    garb_parsed_t r;
    garb_status_t st = garb_parse_values(text, strlen(text), &r);
    size_t n = garb_dump_values(r.values, r.nvalues, s_out, sizeof(s_out));
    // 1e999 is an infinity and dumps as one; 1.71e308 is finite (below
    // DBL_MAX) and must not: exactly one Infinity in the dump.
    const char *inf = strstr(s_out, "Infinity");
    int ok = st == GARB_OK && n > 0 && inf != NULL && strstr(inf + 1, "Infinity") == NULL;
    printf("libgarb numbers: past int64_t and past a double: %s\n", ok ? "dumped" : "FAILED");
    garb_free(&r);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--deep") == 0)
        return deep();
    if (argc == 2 && strcmp(argv[1], "--numbers") == 0)
        return numbers();
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
