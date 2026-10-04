// test_garb_host.c — libgarb's parser on the host, under the sanitizers.
//
// Its jobs. As a DRIVER (`garb_driver MODE`) it reads records on stdin —
// css-parsing-tests' inputs, framed by tools/test_garb_suite.py — and prints
// one JSON line per record, in the suite's representation, for the runner to
// compare. As a TEST (`garb_driver --sweep FILE`) it runs every entry
// point — the sheet, each list, each single item, a selector list, the
// cascade — with allocations failing in turn, and asserts each parse ends,
// nothing leaks, and what comes back is true of the whole text or says it
// is not. And probes: `--deep` parses a million nested `(` on a thread
// with a small stack (past the depth bound the parser must find the end
// without recursing), `--numbers` dumps numbers past int64_t's reach and
// past a double's, and `--cost BYTES` and `--full` hold a parse to what
// GARB.md says it costs, and to keeping what it finished when the arena
// fills.

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "os64/arena.h"

#include "garb/garb.h"
#include "garb/select.h"
#include "garb/values.h"
#include "garb/cascade.h"
#include "html/html.h"

// ── Allocation, and failing it on purpose ───────────────────────────────

static size_t allocations, fail_at, live;
// Fail only allocation fail_at, the rest succeeding: the failure that a
// later success would otherwise paper over.
static bool fail_single;

static bool fail_now(void)
{
    return fail_at != 0 && (fail_single ? allocations == fail_at : allocations >= fail_at);
}

// libhtml ends the program when a pinned document is freed. A harness that
// reached this has found that, so it fails.
void os64_exit(int32_t code)
{
    (void)code;
    exit(3);
}
void *os64_malloc(size_t size)
{
    allocations++;
    if (fail_now())
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
    if (fail_now())
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
    if (n == GARB_DECL_HELD) {
        puts("held");
    } else if (n < 0) {
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

// ── The cascade ─────────────────────────────────────────────────────────

// "WIDTHxHEIGHT query…": whether the query holds in that viewport.
static void media(const char *text)
{
    garb_env_t env = {0, 0};
    int used = 0;
    if (sscanf(text, "%lfx%lf %n", &env.width, &env.height, &used) < 2) {
        puts("bad case");
        return;
    }
    puts(garb_media_text_matches(text + used, env) ? "true" : "false");
}

// The sheets a case supplies beside its page, for its @imports: the page's
// text ends where the first "\n@@SHEET <url>\n" begins, and each such line
// starts a sheet that runs to the next.
typedef struct {
    const char *url;
    size_t ulen;
    const char *text;
    size_t len;
} Supplied;

static Supplied s_supplied[16];
static int32_t s_nsupplied;

static size_t split_supplied(const char *html, size_t hlen)
{
    static const char kMark[] = "\n@@SHEET ";
    s_nsupplied = 0;
    const char *end = html + hlen, *at = strstr(html, kMark);
    if (at == NULL)
        return hlen;
    size_t page = (size_t)(at - html);
    while (at != NULL && s_nsupplied < 16) {
        const char *url = at + sizeof(kMark) - 1;
        const char *nl = memchr(url, '\n', (size_t)(end - url));
        if (nl == NULL)
            break;
        const char *next = strstr(nl, kMark);
        const char *stop = next != NULL ? next : end;
        s_supplied[s_nsupplied++] = (Supplied){url, (size_t)(nl - url), nl + 1,
                                               (size_t)(stop - nl - 1)};
        at = next;
    }
    return page;
}

// The cascade's input, built as a browser builds it: each sheet's @imports
// (found by address among the supplied sheets, never twice on one chain)
// stand before it, and name it as their `parent`.
#define INPUT_MAX 64
typedef struct {
    garb_parsed_t parsed[INPUT_MAX];
    garb_import_t imports[INPUT_MAX][8];
    int32_t nparsed;
    garb_sheet_in_t in[INPUT_MAX];
    int32_t n;
    const char *chain[16];
    int32_t depth;
} Input;

static int32_t emit(Input *in, int32_t p, const char *media, const garb_import_t *via,
                    const char *url)
{
    int32_t kids[8], nkids = 0;
    garb_import_t *imps = in->imports[p];
    int32_t nimp = garb_sheet_imports(&in->parsed[p], imps, 8);
    in->chain[in->depth++] = url;
    for (int32_t i = 0; i < nimp && i < 8; i++) {
        const Supplied *found = NULL;
        for (int32_t k = 0; k < s_nsupplied; k++)
            if (s_supplied[k].ulen == imps[i].len &&
                memcmp(s_supplied[k].url, imps[i].url, imps[i].len) == 0)
                found = &s_supplied[k];
        bool looping = false;
        for (int32_t k = 0; found != NULL && k < in->depth; k++)
            looping |= in->chain[k] == found->url;
        if (found == NULL || looping || in->nparsed >= INPUT_MAX || in->depth >= 16)
            continue;
        int32_t child = in->nparsed++;
        garb_parse_sheet_text(found->text, found->len, &in->parsed[child]);
        kids[nkids++] = emit(in, child, NULL, &imps[i], found->url);
    }
    in->depth--;
    int32_t at = in->n++;
    in->in[at] = (garb_sheet_in_t){.sheet = &in->parsed[p], .media = media, .via = via};
    for (int32_t k = 0; k < nkids; k++)
        in->in[kids[k]].parent = at;
    return at;
}

// The page's style elements as sheets, in document order — libpage's list,
// done by hand here so the harness needs no libpage.
static void collect_styles(const os64_html_node_t *n, Input *in)
{
    for (; n != NULL && in->nparsed < INPUT_MAX; n = n->next) {
        if (n->kind == OS64_HTML_ELEMENT && n->ns == OS64_HTML_NS_HTML && n->name != NULL &&
            strcmp(n->name, "style") == 0) {
            int32_t p = in->nparsed++;
            garb_parse_style_element(n, &in->parsed[p]);
            const os64_html_attr_t *m = os64_html_attr(n, "media");
            emit(in, p, m != NULL ? m->value : NULL, NULL, NULL);
        }
        collect_styles(n->first_child, in);
    }
}

// A page and "WIDTHxHEIGHT": every element's author winners.
static void cascade(const char *html, size_t hlen, const char *viewport)
{
    garb_env_t env = {800, 600};
    if (viewport != NULL)
        sscanf(viewport, "%lfx%lf", &env.width, &env.height);
    hlen = split_supplied(html, hlen);
    os64_html_options_t opt = os64_html_options_default();
    os64_html_parser_t *hp = os64_html_parser_new(&opt);
    os64_html_parser_feed(hp, html, hlen);
    os64_html_document_t *doc = os64_html_parser_finish(hp);
    static Input in;
    memset(&in, 0, sizeof(in));
    collect_styles(doc->document, &in);
    garb_cascade_t *c = garb_cascade(in.in, in.n, doc, env);
    size_t need = garb_cascade_dump(c, doc, NULL, 0) + 1;
    char *out = malloc(need);
    garb_cascade_dump(c, doc, out, need);
    fputs(out, stdout);
    puts(garb_cascade_incomplete(c) ? "(incomplete)" : "(end)");
    free(out);
    garb_cascade_free(c);
    for (int32_t k = 0; k < in.nparsed; k++)
        garb_free(&in.parsed[k]);
    os64_html_document_free(doc);
}

// ── The allocation sweep ────────────────────────────────────────────────

// The page parse_all cascades over, built once before the sweep: what it
// holds is live throughout, and every leak check counts from there.
static os64_html_document_t *s_sweep_doc;
static size_t s_live_base;

static void parse_all(const char *text, size_t len)
{
    garb_parsed_t r;
    (void)garb_parse_sheet((const uint8_t *)text, len, NULL, NULL, &r);
    // The whole sheet cascaded over a small page, custom properties and a
    // style attribute included.
    garb_sheet_in_t in = {.sheet = &r};
    garb_env_t env = {800, 600};
    garb_cascade_free(garb_cascade(&in, 1, s_sweep_doc, env));
    for (int32_t i = 0; i < r.nitems; i++) {
        const garb_rule_t *rule = r.items[i].rule;
        if (r.items[i].kind != GARB_ITEM_RULE || rule == NULL || !rule->has_block)
            continue;
        garb_item_t *items;
        int32_t n;
        bool whole = rule->at ? garb_rules_of(&r, rule->block, rule->nblock, &items, &n)
                              : garb_items_of(&r, rule->block, rule->nblock, &items, &n);
        // A short reparse is never silent: its owner says so too.
        if (!whole && !r.incomplete) {
            fprintf(stderr, "FAIL sweep: a reparse came out short and the sheet says whole\n");
            exit(1);
        }
        if (rule->at)
            continue;
        (void)garb_selectors_parse(&r, rule->prelude, rule->nprelude, OS64_HTML_NO_QUIRKS);
        garb_set_t sets[GARB_SETS_MAX];
        for (int32_t k = 0; k < n; k++)
            if (items[k].kind == GARB_ITEM_DECL)
                (void)garb_read_declaration(&r, &items[k].decl, false, sets);
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
        if (live != s_live_base) {
            fprintf(stderr, "FAIL sweep %s: %zu leaked with failure at %zu\n", what,
                    live - s_live_base, at);
            failures++;
            live = s_live_base;
        }
    }
    free_dumps(whole, nwhole);
    printf("libgarb sweep, %s: each of %zu allocations failed in turn, %s\n", what, worst,
           failures == 0 ? "every result a prefix, nothing leaked" : "FAILED");
    return failures;
}

// A single item has no prefix to be: under every allocation failure the
// answer is the whole parse's — its status and, when OK, its item dumped
// the same — or NO_MEMORY with nothing published. A status reached from a
// parse that ran out is a claim about text the parser never read.
typedef garb_status_t (*OneParse)(const char *, size_t, garb_parsed_t *);

static garb_status_t one_dump(OneParse parse, const char *text, char **dump, garb_parsed_t *r)
{
    garb_status_t st = parse(text, strlen(text), r);
    *dump = NULL;
    if (st != GARB_OK)
        return st;
    size_t n = parse == garb_parse_one_rule ? garb_dump_rule(r->rule, NULL, 0)
             : parse == garb_parse_one_declaration ? garb_dump_decl(&r->decl, NULL, 0)
             : garb_dump_values(r->values, r->nvalues, NULL, 0);
    *dump = malloc(n + 1);
    if (parse == garb_parse_one_rule)
        garb_dump_rule(r->rule, *dump, n + 1);
    else if (parse == garb_parse_one_declaration)
        garb_dump_decl(&r->decl, *dump, n + 1);
    else
        garb_dump_values(r->values, r->nvalues, *dump, n + 1);
    return st;
}

static int sweep_one(const char *what, OneParse parse, const char *text)
{
    allocations = 0;
    fail_at = 0;
    garb_parsed_t r;
    char *whole;
    garb_status_t whole_st = one_dump(parse, text, &whole, &r);
    garb_free(&r);
    size_t worst = allocations;
    int failures = 0;
    for (size_t at = 1; at <= worst; at++) {
        allocations = 0;
        fail_at = at;
        char *part;
        garb_status_t st = one_dump(parse, text, &part, &r);
        fail_at = 0;
        bool nothing = r.rule == NULL && r.decl.name == NULL && r.nvalues == 0;
        bool ok = st == GARB_NO_MEMORY ? nothing
                : st == whole_st && (st != GARB_OK || strcmp(part, whole) == 0);
        if (!ok) {
            fprintf(stderr, "FAIL sweep %s \"%s\": failure at %zu answered %d, the whole %d\n",
                    what, text, at, (int)st, (int)whole_st);
            failures++;
        }
        free(part);
        garb_free(&r);
        if (live != s_live_base) {
            fprintf(stderr, "FAIL sweep %s: %zu leaked with failure at %zu\n", what,
                    live - s_live_base, at);
            failures++;
            live = s_live_base;
        }
    }
    free(whole);
    return failures;
}

static int sweep_ones(void)
{
    // Each shape of answer: OK, EMPTY, EXTRA_INPUT, INVALID, and items
    // whose values nest, since a nested list is where a push fails late.
    static const char *const rules[] = {"a { b: c }", "@media x { a { b: c } }", "@import x;",
                                        "", "  ", "a {} b {}", "@x", "a", "a { (b [c {d}]) }"};
    static const char *const decls[] = {"b: c d e", "b: f(1, 2) !important", "", " ", "b",
                                        "b: (c [d {e}])", "1: 2"};
    static const char *const values[] = {"a", "(a b c)", "f(1, [2 {3}])", "", " ", "a b",
                                         "{a: (b)}", "\"str\""};
    int failures = 0;
    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++)
        failures += sweep_one("one rule", garb_parse_one_rule, rules[i]);
    for (size_t i = 0; i < sizeof(decls) / sizeof(decls[0]); i++)
        failures += sweep_one("one declaration", garb_parse_one_declaration, decls[i]);
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
        failures += sweep_one("one value", garb_parse_one_value, values[i]);
    printf("libgarb sweep, single items: every allocation failed in turn, %s\n",
           failures == 0 ? "each answer whole or NO_MEMORY, nothing leaked" : "FAILED");
    return failures;
}

// Every list entry point, every allocation failing in turn, over texts
// where a token is PEEKED and the next one outgrows the tokenizer's first
// buffer: the growth fails with a lookahead cached, and recovery restores
// it. The parse must end — a spin is caught by the alarm — publish a prefix
// of the whole parse's list, and leak nothing.
typedef garb_status_t (*ListParse)(const char *, size_t, garb_parsed_t *);

static int32_t list_dumps(ListParse parse, const char *text, char ***dumps)
{
    garb_parsed_t r;
    (void)parse(text, strlen(text), &r);
    int32_t n = parse == garb_parse_values ? r.nvalues : r.nitems;
    *dumps = calloc((size_t)(n > 0 ? n : 1), sizeof(char *));
    for (int32_t i = 0; i < n; i++) {
        size_t need = parse == garb_parse_values ? garb_dump_values(&r.values[i], 1, NULL, 0)
                                                 : garb_dump_items(&r.items[i], 1, NULL, 0);
        (*dumps)[i] = malloc(need + 1);
        if (parse == garb_parse_values)
            garb_dump_values(&r.values[i], 1, (*dumps)[i], need + 1);
        else
            garb_dump_items(&r.items[i], 1, (*dumps)[i], need + 1);
    }
    garb_free(&r);
    return n;
}

static void sweep_hung(int sig)
{
    (void)sig;
    static const char msg[] = "FAIL sweep: a parse under allocation failure never ended\n";
    (void)!write(2, msg, sizeof(msg) - 1);
    _exit(1);
}

static int sweep_lists(void)
{
    static const struct { const char *what; ListParse parse; } modes[] = {
        {"sheet", garb_parse_sheet_text}, {"rules", garb_parse_rules},
        {"block", garb_parse_block}, {"declarations", garb_parse_declaration_list},
        {"values", garb_parse_values},
    };
#define LONG "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    static const char *const texts[] = {
        "a " LONG, "a:b; c " LONG " d:e", "@x a " LONG ";", "a { b " LONG " }",
        "a:b; " LONG ": c", "(a " LONG ")", "a b; c:d; @e " LONG " { f:g }",
    };
#undef LONG
    signal(SIGALRM, sweep_hung);
    int failures = 0;
    for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        for (size_t t = 0; t < sizeof(texts) / sizeof(texts[0]); t++) {
            allocations = 0;
            fail_at = 0;
            char **whole;
            int32_t nwhole = list_dumps(modes[m].parse, texts[t], &whole);
            size_t worst = allocations;
            for (size_t at = 1; at <= worst; at++) {
                allocations = 0;
                fail_at = at;
                alarm(10);
                char **part;
                int32_t npart = list_dumps(modes[m].parse, texts[t], &part);
                alarm(0);
                fail_at = 0;
                bool prefix = npart <= nwhole;
                for (int32_t i = 0; prefix && i < npart; i++)
                    prefix = strcmp(part[i], whole[i]) == 0;
                if (!prefix) {
                    fprintf(stderr, "FAIL sweep %s \"%s\": failure at %zu is not a prefix\n",
                            modes[m].what, texts[t], at);
                    failures++;
                }
                free_dumps(part, npart);
                if (live != s_live_base) {
                    fprintf(stderr, "FAIL sweep %s: %zu leaked with failure at %zu\n",
                            modes[m].what, live - s_live_base, at);
                    failures++;
                    live = s_live_base;
                }
            }
            free_dumps(whole, nwhole);
        }
    }
    printf("libgarb sweep, every list parse: every allocation failed in turn, %s\n",
           failures == 0 ? "each ended with a prefix, nothing leaked" : "FAILED");
    return failures;
}

// A selector list with every allocation failing in turn: an answer that
// differs from the whole parse's — NULL where it was valid, a list where it
// was invalid, fewer selectors, other weights — must come with the owner
// marked incomplete. A failure is never a verdict on the selector.
static void selector_desc(const char *text, char *out, size_t cap, bool *incomplete)
{
    garb_parsed_t r;
    (void)garb_parse_values(text, strlen(text), &r);
    garb_selectors_t *sel = garb_selectors_parse(&r, r.values, r.nvalues, OS64_HTML_NO_QUIRKS);
    size_t at = (size_t)snprintf(out, cap, "%s", sel == NULL ? "null" : "list");
    for (int32_t i = 0; sel != NULL && i < garb_selectors_count(sel) && at < cap; i++)
        at += (size_t)snprintf(out + at, cap - at, " %u/%d", garb_selector_specificity(sel, i),
                               (int)garb_selector_pseudo(sel, i));
    *incomplete = r.incomplete;
    garb_free(&r);
}

static int sweep_selectors(void)
{
    // Each shape, many times over, so the selectors outgrow the arena's
    // first chunk and a failure lands inside the selector parse: a long
    // LIST (its array of selectors grows), and one long SELECTOR of
    // compounds (its array of compounds grows), bare and in :is().
    static const struct { const char *before, *unit, *sep, *after; } shapes[] = {
        {"", "a, b.c, #d > e + f ~ g", ", ", ""},
        {"", ":is(a, b, :not(.c)) d", ", ", ""},
        {"", "[x=y i], [z|=w], a[b]", ", ", ""},
        {"", ":where(a, :is(b, [c])), :has(> d)", ", ", ""},
        {"", "li:nth-child(2n + 1 of .x)", ", ", ""},
        {"", "a::before", ", ", ""},
        {"", ":is(a, $$)", ", ", ""},
        {"", "a, $$", ", ", ""},
        {"", "a.b[c]", " ", ""},
        {"x, :is(", "a.b", " > ", ")"},
        {"", "a", " ", "::before b"},
    };
    enum { COPIES = 2000 };
    static char text[COPIES * 64], whole[COPIES * 32], part[COPIES * 32];
    int failures = 0;
    for (size_t k = 0; k < sizeof(shapes) / sizeof(shapes[0]); k++) {
        size_t len = (size_t)snprintf(text, sizeof(text), "%s", shapes[k].before);
        for (int i = 0; i < COPIES; i++)
            len += (size_t)snprintf(text + len, sizeof(text) - len, "%s%s", i ? shapes[k].sep : "",
                                    shapes[k].unit);
        snprintf(text + len, sizeof(text) - len, "%s", shapes[k].after);
        bool inc;
        allocations = 0;
        fail_at = 0;
        selector_desc(text, whole, sizeof(whole), &inc);
        size_t worst = allocations;
        for (size_t at = 1; at <= worst; at++) {
            allocations = 0;
            fail_at = at;
            selector_desc(text, part, sizeof(part), &inc);
            fail_at = 0;
            if (strcmp(part, whole) != 0 && !inc) {
                fprintf(stderr, "FAIL sweep selectors \"%s\"%s: failure at %zu answered "
                        "\"%.60s\", the whole \"%.60s\", and nothing says it is short\n",
                        shapes[k].unit, shapes[k].sep, at, part, whole);
                failures++;
            }
            if (live != s_live_base) {
                fprintf(stderr, "FAIL sweep selectors: %zu leaked with failure at %zu\n",
                        live - s_live_base, at);
                failures++;
                live = s_live_base;
            }
        }
    }
    printf("libgarb sweep, selectors: every allocation failed in turn, %s\n",
           failures == 0 ? "each answer the whole one or marked incomplete" : "FAILED");
    return failures;
}

// The cascade with each single allocation failing, the rest succeeding —
// in the sheet's parse, a rule block read again, a selector list, a value,
// a style attribute: its answer is the whole cascade's, or it says it is
// incomplete. A short read that a later success papers over is the case.
static char *cascade_answer(const char *text, size_t len, bool *incomplete)
{
    garb_parsed_t r;
    (void)garb_parse_sheet((const uint8_t *)text, len, NULL, NULL, &r);
    garb_sheet_in_t in = {.sheet = &r};
    garb_env_t env = {800, 600};
    garb_cascade_t *c = garb_cascade(&in, 1, s_sweep_doc, env);
    size_t need = garb_cascade_dump(c, s_sweep_doc, NULL, 0) + 1;
    char *out = malloc(need);
    garb_cascade_dump(c, s_sweep_doc, out, need);
    *incomplete = garb_cascade_incomplete(c);
    garb_cascade_free(c);
    garb_free(&r);
    return out;
}

static int sweep_cascade(const char *what, const char *text, size_t len)
{
    allocations = 0;
    fail_at = 0;
    bool inc;
    char *whole = cascade_answer(text, len, &inc);
    size_t worst = allocations;
    int failures = 0;
    fail_single = true;
    for (size_t at = 1; at <= worst; at++) {
        allocations = 0;
        fail_at = at;
        char *part = cascade_answer(text, len, &inc);
        fail_at = 0;
        if (strcmp(part, whole) != 0 && !inc) {
            fprintf(stderr, "FAIL sweep cascade %s: failure at %zu changed the answer and it "
                    "says it is whole\n", what, at);
            failures++;
        }
        free(part);
        if (live != s_live_base) {
            fprintf(stderr, "FAIL sweep cascade %s: %zu leaked with failure at %zu\n", what,
                    live - s_live_base, at);
            failures++;
            live = s_live_base;
        }
    }
    fail_single = false;
    free(whole);
    printf("libgarb sweep, cascade over %s: each of %zu allocations failed alone, %s\n", what,
           worst, failures == 0 ? "each answer the whole one or marked incomplete" : "FAILED");
    return failures;
}

// A shadow list and a background's layers grow outside the arena
// (props.c's shadow_list and Layers), so their failure is said by hand:
// each allocation of reading such a declaration failed alone, the read must
// be the whole one or say it is incomplete — never an invalid declaration
// with the sheet calling itself whole (Quinn, #203).
static int sweep_grown_lists(void)
{
    static const char *const decls[] = {"box-shadow: 1px 1px red",
                                        "text-shadow: 1px 1px red, 2px 2px blue",
                                        "box-shadow: 1px 1px red, 2px 2px blue, 3px 3px green, "
                                        "4px 4px, 5px 5px, 6px 6px, 7px 7px, 8px 8px, 9px 9px",
                                        "background-image: url(a), url(b), url(c), url(d), url(e)",
                                        "background: url(a) 0 0 / 2px, url(b), url(c) content-box, "
                                        "url(d), linear-gradient(red, blue) red",
                                        "background-position: 0 0, 1px 2px, center, right, top, "
                                        "left 3px bottom"};
    int failures = 0;
    for (size_t d = 0; d < sizeof(decls) / sizeof(decls[0]); d++) {
        garb_set_t sets[GARB_SETS_MAX];
        garb_parsed_t r;
        allocations = 0;
        fail_at = 0;
        if (garb_parse_one_declaration(decls[d], strlen(decls[d]), &r) != GARB_OK) {
            garb_free(&r);
            failures++;
            continue;
        }
        size_t before = allocations;
        int32_t whole = garb_read_declaration(&r, &r.decl, false, sets);
        size_t worst = allocations - before;
        garb_free(&r);
        for (size_t at = 1; at <= worst; at++) {
            allocations = 0;
            fail_at = 0;
            if (garb_parse_one_declaration(decls[d], strlen(decls[d]), &r) != GARB_OK) {
                garb_free(&r);
                continue;
            }
            fail_single = true;
            fail_at = allocations + at;
            int32_t n = garb_read_declaration(&r, &r.decl, false, sets);
            fail_at = 0;
            fail_single = false;
            if (n != whole && !r.incomplete) {
                fprintf(stderr, "FAIL sweep lists \"%s\": failure %zu read %d, sheet whole\n",
                        decls[d], at, (int)n);
                failures++;
            }
            garb_free(&r);
        }
    }
    printf("libgarb sweep, shadow and layer lists: every allocation of the read failed alone, %s\n",
           failures == 0 ? "each the whole read or marked incomplete" : "FAILED");
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
    static const char kPage[] =
        "<!doctype html><html><body class=panel><div id=main class='item x' data-state=open>"
        "<div class=title style='color: red; --pad: 3px; margin: var(--pad)'>t</div>"
        "<a href=/>link</a><div class=grid>g</div><p class=card>c</div>";
    os64_html_options_t opt = os64_html_options_default();
    os64_html_parser_t *hp = os64_html_parser_new(&opt);
    os64_html_parser_feed(hp, kPage, sizeof(kPage) - 1);
    s_sweep_doc = os64_html_parser_finish(hp);
    s_live_base = live;
    int failures = sweep_text(path, text, len);
    // A token that outgrows the tokenizer's first buffer (64 bytes) with a
    // two-byte character across the boundary: the growth fails between the
    // character's bytes, and that token must never be published.
    static const char long_ident[] =
        "a { b: c } xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\xc3\xa9 { d: e }";
    failures += sweep_text("a long identifier", long_ident, sizeof(long_ident) - 1);
    failures += sweep_ones();
    failures += sweep_lists();
    failures += sweep_grown_lists();
    failures += sweep_selectors();
    failures += sweep_cascade(path, text, len);
    os64_html_document_free(s_sweep_doc);
    s_live_base = 0;
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

// What a parse COSTS, GARB.md § The cost: a sheet shaped like the web's
// biggest (Bootstrap's long selector lists, many short declarations),
// parsed, then every rule's block read as its items — what the cascade
// does. `--cost BYTES`: it must come back whole, at under GARB_COST_MAX
// arena bytes a byte of text, the ratio GARB.md gives. `--full`: a sheet at
// the input's own cap fills the arena, and must come back incomplete with
// the rules it finished kept — never with nothing because the list that
// held them could not be kept.
#define GARB_COST_MAX 22
static const char kShaped[] =
    ".btn-outline-%d:not(:disabled):not(.disabled).active,"
    ".btn-outline-%d:not(:disabled):not(.disabled):active,"
    ".show>.btn-outline-%d.dropdown-toggle{color:#fff;background-color:#007bff;"
    "border-color:#007bff;box-shadow:0 0 0 .2rem rgba(0,123,255,.5)}\n"
    "@media (min-width:%dpx){.col-md-%d{flex:0 0 8.333333%%;max-width:8.333333%%}}\n";

static int cost(size_t bytes, bool full)
{
    char *text = malloc(bytes + 512);
    size_t len = 0;
    for (int i = 0; len + 512 < bytes; i++)
        len += (size_t)snprintf(text + len, 512, kShaped, i, i, i, 540 + i % 900, i % 12 + 1);
    garb_parsed_t r;
    garb_status_t st = garb_parse_sheet_text(text, len, &r);
    int32_t rules = 0, decls = 0;
    bool whole = st == GARB_OK && !r.incomplete;
    for (int32_t i = 0; i < r.nitems; i++) {
        const garb_rule_t *rule = r.items[i].rule;
        if (r.items[i].kind != GARB_ITEM_RULE || !rule->has_block)
            continue;
        rules++;
        garb_item_t *items;
        int32_t n;
        whole &= garb_items_of(&r, rule->block, rule->nblock, &items, &n);
        for (int32_t k = 0; k < n; k++)
            decls += items[k].kind == GARB_ITEM_DECL;
    }
    double ratio = (double)os64_arena_stats(r.arena).used_bytes / (double)len;
    int ok = full ? st == GARB_OK && r.incomplete && rules > 1000
                  : whole && !r.incomplete && ratio < GARB_COST_MAX;
    printf("libgarb %s: a %zu KiB sheet, %d rules, %d declarations read: %.1f arena bytes a "
           "byte%s%s\n", full ? "full" : "cost", len >> 10, rules, decls, ratio,
           r.incomplete ? ", incomplete" : "", ok ? "" : " - FAILED");
    garb_free(&r);
    free(text);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--cost") == 0)
        return cost((size_t)strtoul(argv[2], NULL, 10), false);
    if (argc == 2 && strcmp(argv[1], "--full") == 0)
        return cost(GARB_SHEET_MAX, true);
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
        if (strcmp(argv[1], "anplusb") == 0)
            an_plus_b(text, len);
        else if (strcmp(argv[1], "color") == 0)
            color(text, len);
        else if (strcmp(argv[1], "decl") == 0)
            declaration(text, len);
        else if (strcmp(argv[1], "media") == 0)
            media(text);
        else if (strcmp(argv[1], "cascade") == 0)
            cascade(text, len, pabsent ? NULL : protocol);
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
