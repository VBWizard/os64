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
// past a double's, and `--cost BYTES` and `--full` hold a parse and its
// cascade to what GARB.md says they cost, and a parse to keeping what it
// finished when the arena fills; `--cascades N` cascades one sheet N
// times, each whole and leaving the sheet as it was; `--read-budget` holds
// what a cascade reads of all its sheets to GARB_CASCADE_READ_MAX;
// `--keyframes-scale`
// holds @keyframes to a sort's cost, and
// `--faces-cap` the @font-face rules to GARB_FONT_FACES_MAX.

// open_memstream, for the sweep's answer.
#define _POSIX_C_SOURCE 200809L

#include <malloc.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "os64/arena.h"

#include "garb/garb.h"
#include "garb/select.h"
#include "garb/values.h"
#include "garb/cascade.h"
#include "html/html.h"

// ── Allocation, and failing it on purpose ───────────────────────────────

// How many allocations were asked for, which one fails, and how many are
// held now and in how many bytes — what a cascade holds while it lives is
// those bytes then less those before it (`--cost`, `--cascades`).
static size_t allocations, fail_at, live, live_bytes;
// Reallocs of nothing: how a parse's scratch starts (`--scratch`).
static size_t reallocs_from_nothing;
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
    if (at != NULL) {
        live++;
        live_bytes += malloc_usable_size(at);
    }
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
    reallocs_from_nothing += ptr == NULL;
    if (fail_now())
        return NULL;
    size_t was = ptr != NULL ? malloc_usable_size(ptr) : 0;
    void *at = realloc(ptr, size != 0 ? size : 1);
    if (at != NULL && ptr == NULL)
        live++;
    if (at != NULL)
        live_bytes += malloc_usable_size(at) - was;
    return at;
}

void os64_free(void *ptr)
{
    if (ptr != NULL) {
        live--;
        live_bytes -= malloc_usable_size(ptr);
    }
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
    int32_t n = garb_read_declaration(&r, &r.decl, (garb_reading_t){.quirks = quirks}, sets);
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
        garb_selectors_t *sel = garb_selectors_parse(&r, r.values, r.nvalues, doc->quirks, false);
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
    garb_env_t env = {.width = 0, .height = 0};
    int used = 0;
    if (sscanf(text, "%lfx%lf %n", &env.width, &env.height, &used) < 2) {
        puts("bad case");
        return;
    }
    // `WIDTHxHEIGHT dark ...`: the person's scheme is dark; `reduce ...`:
    // they asked for less motion; `scripting ...`: the document runs
    // scripts.
    if (strncmp(text + used, "dark ", 5) == 0) {
        env.dark = true;
        used += 5;
    }
    if (strncmp(text + used, "reduce ", 7) == 0) {
        env.reduced_motion = true;
        used += 7;
    }
    if (strncmp(text + used, "scripting ", 10) == 0) {
        env.scripting = true;
        used += 10;
    }
    // `device WxH ...`: the screen, which the device-* features answer.
    int more = 0;
    if (sscanf(text + used, "device %lfx%lf %n", &env.device_width, &env.device_height, &more) == 2)
        used += more;
    // `resolution N ...`: the zoom, in dppx, which `resolution` answers.
    more = 0;
    if (sscanf(text + used, "resolution %lf %n", &env.resolution, &more) == 1)
        used += more;
    puts(garb_media_text_matches(text + used, env) ? "true" : "false");
}

// A media query list as CSSOM serialises it (garb_media_text_serialize),
// whole, and again into each smaller buffer: what fits is the whole text's
// prefix, and the length answered is the whole text's every time.
static void media_text(const char *text)
{
    char whole[512];
    size_t n = garb_media_text_serialize(text, whole, sizeof(whole));
    if (n >= sizeof(whole)) {
        puts("too long for the case");
        return;
    }
    for (size_t cap = 0; cap <= n; cap++) {
        char cut[512];
        memset(cut, 'x', sizeof(cut));
        size_t again = garb_media_text_serialize(text, cut, cap);
        bool fits = cap == 0 ? cut[0] == 'x'
                             : strlen(cut) == (cap - 1 < n ? cap - 1 : n) && memcmp(cut, whole, strlen(cut)) == 0;
        if (again != n || !fits) {
            printf("cut at %zu: %zu, %s\n", cap, again, fits ? "fits" : "spills");
            return;
        }
    }
    printf("[%s]\n", whole);
}

// The faces mode: each face garb_cascade_font_faces answers, in order —
// `"FAMILY" WEIGHTS STYLE [RANGES] sheet N` and then its sources, each
// `url(TEXT) [format(F)]` or `local(NAME)` — and how many were lost.
static void print_faces(const garb_cascade_t *c, FILE *to)
{
    static const char *const kStyles[] = {"normal", "italic", "oblique"};
    const garb_font_face_t *f;
    uint32_t lost;
    int32_t n = garb_cascade_font_faces(c, &f, &lost);
    for (int32_t i = 0; i < n; i++) {
        fprintf(to, "\"%.*s\" %g", (int)f[i].family_len, f[i].family, f[i].weight_min);
        if (f[i].weight_max != f[i].weight_min)
            fprintf(to, "-%g", f[i].weight_max);
        fprintf(to, " %s", kStyles[f[i].style]);
        for (int32_t r = 0; r < f[i].nranges; r++)
            fprintf(to, "%s U+%X-%X", r ? "," : "", f[i].ranges[r].first, f[i].ranges[r].last);
        fprintf(to, " sheet %d\n", f[i].sheet);
        for (int32_t s = 0; s < f[i].nsrc; s++) {
            const garb_face_src_t *src = &f[i].src[s];
            if (src->kind == GARB_FACE_LOCAL)
                fprintf(to, "  local(%.*s)\n", (int)src->len, src->text);
            else if (src->format != NULL)
                fprintf(to, "  url(%.*s) format(%.*s)\n", (int)src->len, src->text, (int)src->format_len,
                       src->format);
            else
                fprintf(to, "  url(%.*s)\n", (int)src->len, src->text);
        }
    }
    if (lost > 0)
        fprintf(to, "(%u lost)\n", lost);
}

// The keyframes mode: for each element whose winners name animations, in
// document order, each name and its keyframes as garb_keyframes answers
// them — `  OFFSET% [EASING]: prop: value; ...` — or `(no rule)`.
static void print_keyframes(const garb_cascade_t *c, const os64_html_document_t *doc)
{
    for (const os64_html_node_t *x = doc->document; x != NULL;) {
        garb_style_t st = x->kind == OS64_HTML_ELEMENT ? garb_style_for(c, x)
                                                       : (garb_style_t){NULL, NULL, NULL, 0};
        for (int32_t k = 0; k < st.n; k++) {
            if (st.sets[k].prop != GARB_ANIMATION_NAME)
                continue;
            const garb_val_t *v = &st.sets[k].value;
            const garb_val_t *items = v->kind == GARB_V_LAYERS ? v->items : v;
            int32_t n = v->kind == GARB_V_LAYERS ? v->nitems : 1;
            for (int32_t i = 0; i < n; i++) {
                if (items[i].kind != GARB_V_STRING)
                    continue;
                const os64_html_attr_t *id = os64_html_attr(x, "id");
                printf("%s%s%s: %.*s\n", x->name, id != NULL ? "#" : "",
                       id != NULL ? id->value : "", (int)items[i].len, items[i].text);
                const garb_keyframe_t *f;
                int32_t nf = garb_keyframes(c, x, items[i].text, items[i].len, &f);
                if (nf < 0)
                    puts("  (no rule)");
                for (int32_t j = 0; j < nf; j++) {
                    char e[256] = "";
                    if (f[j].easing != NULL)
                        garb_val_dump(f[j].easing, e, sizeof(e));
                    printf("  %g%%%s%s:", f[j].offset * 100, f[j].easing != NULL ? " " : "", e);
                    for (int32_t s = 0; s < f[j].nsets; s++) {
                        char val[512];
                        garb_val_dump(&f[j].sets[s].value, val, sizeof(val));
                        printf("%s %s: %s", s ? ";" : "", garb_prop_name(f[j].sets[s].prop), val);
                    }
                    putchar('\n');
                }
            }
        }
        if (x->first_child != NULL) {
            x = x->first_child;
            continue;
        }
        while (x != NULL && x->next == NULL)
            x = x->parent;
        x = x != NULL ? x->next : NULL;
    }
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

// A page's cascade as the skips mode prints it: what the build passed
// over (garb_cascade_skips), one `kind name count` a line.
static void print_skips(const garb_cascade_t *c)
{
    static const char *const kinds[] = {"property", "at-rule", "function", "value"};
    garb_skip_t skips[GARB_SKIPS_MAX];
    uint32_t lost;
    int32_t n = garb_cascade_skips(c, skips, GARB_SKIPS_MAX, &lost);
    for (int32_t i = 0; i < n && i < GARB_SKIPS_MAX; i++)
        printf("%s %s %u\n", kinds[skips[i].kind], skips[i].name, (unsigned)skips[i].count);
    if (lost != 0)
        printf("lost %u\n", (unsigned)lost);
}

// A page and "WIDTHxHEIGHT": every element's author winners, or with
// `skips`, what the cascade passed over, or with `keyframes`, the
// animations its elements name.
typedef enum { CASCADE_WINNERS, CASCADE_SKIPS, CASCADE_KEYFRAMES, CASCADE_FACES } CascadeMode;

static void cascade(const char *html, size_t hlen, const char *viewport, CascadeMode mode)
{
    garb_env_t env = {.width = 800, .height = 600};
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
    if (mode == CASCADE_SKIPS) {
        print_skips(c);
        puts(garb_cascade_incomplete(c) ? "(incomplete)" : "(end)");
    } else if (mode == CASCADE_KEYFRAMES) {
        print_keyframes(c, doc);
        puts(garb_cascade_incomplete(c) ? "(incomplete)" : "(end)");
    } else if (mode == CASCADE_FACES) {
        print_faces(c, stdout);
        puts(garb_cascade_incomplete(c) ? "(incomplete)" : "(end)");
    } else {
        size_t need = garb_cascade_dump(c, doc, NULL, 0) + 1;
        char *out = malloc(need);
        garb_cascade_dump(c, doc, out, need);
        fputs(out, stdout);
        puts(garb_cascade_incomplete(c) ? "(incomplete)" : "(end)");
        free(out);
    }
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
    garb_env_t env = {.width = 800, .height = 600};
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
        (void)garb_selectors_parse(&r, rule->prelude, rule->nprelude, OS64_HTML_NO_QUIRKS, false);
        garb_set_t sets[GARB_SETS_MAX];
        for (int32_t k = 0; k < n; k++)
            if (items[k].kind == GARB_ITEM_DECL)
                (void)garb_read_declaration(&r, &items[k].decl, (garb_reading_t){0}, sets);
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
    // A media query list's serialization is the whole list's or none: a
    // parse that ran short would serialize another list.
    const char *media_list = "screen and (min-width: 600px), (color) or (hover), print";
    char whole[256], part[256];
    allocations = 0;
    fail_at = 0;
    size_t wanted = garb_media_text_serialize(media_list, whole, sizeof(whole));
    size_t worst = allocations;
    int media_failures = 0;
    for (size_t at = 1; at <= worst; at++) {
        allocations = 0;
        fail_at = at;
        size_t got = garb_media_text_serialize(media_list, part, sizeof(part));
        fail_at = 0;
        if (!(got == wanted && strcmp(part, whole) == 0) && !(got == (size_t)-1 && part[0] == '\0'))
            media_failures++;
        if (live != s_live_base) {
            media_failures++;
            live = s_live_base;
        }
    }
    printf("libgarb sweep, media text: each of %zu allocations failed in turn, %s\n", worst,
           media_failures == 0 ? "each answer the whole list or none, nothing leaked" : "FAILED");
    // The same of the grown form, whose buffer is one allocation more.
    allocations = 0;
    size_t grown_length = 0;
    char *grown = garb_media_text_serialized(media_list, &grown_length, NULL, NULL);
    size_t grown_worst = allocations;
    int grown_failures = grown == NULL || strcmp(grown, whole) != 0 || grown_length != wanted;
    os64_free(grown);
    live = s_live_base;
    for (size_t at = 1; at <= grown_worst; at++) {
        allocations = 0;
        fail_at = at;
        grown = garb_media_text_serialized(media_list, &grown_length, NULL, NULL);
        fail_at = 0;
        if (grown != NULL ? strcmp(grown, whole) != 0 || grown_length != wanted : grown_length != 0)
            grown_failures++;
        os64_free(grown);
        if (live != s_live_base) {
            grown_failures++;
            live = s_live_base;
        }
    }
    printf("libgarb sweep, grown media text: each of %zu allocations failed in turn, %s\n", grown_worst,
           grown_failures == 0 ? "each answer the whole list or none, nothing leaked" : "FAILED");
    media_failures += grown_failures;
    // Brackets nested past GARB_DEPTH_MAX lose their contents: none either;
    // nested within it, the list is read.
    static char deep[4 * GARB_DEPTH_MAX + 1], shallow_text[GARB_DEPTH_MAX + 1];
    for (int i = 0; i < 2 * GARB_DEPTH_MAX; i++) {
        deep[i] = '(';
        deep[4 * GARB_DEPTH_MAX - 1 - i] = ')';
    }
    for (int i = 0; i < GARB_DEPTH_MAX / 2; i++) {
        shallow_text[i] = '(';
        shallow_text[GARB_DEPTH_MAX - 1 - i] = ')';
    }
    size_t nested = garb_media_text_serialize(deep, part, sizeof(part));
    size_t shallow = garb_media_text_serialize(shallow_text, part, sizeof(part));
    bool depth_ok = nested == (size_t)-1 && shallow != (size_t)-1;
    printf("libgarb media text nested past GARB_DEPTH_MAX: %s\n", depth_ok ? "none" : "FAILED");
    return failures + media_failures + !depth_ok;
}

// A selector list with every allocation failing in turn: an answer that
// differs from the whole parse's — NULL where it was valid, a list where it
// was invalid, fewer selectors, other weights — must come with the owner
// marked incomplete. A failure is never a verdict on the selector.
static void selector_desc(const char *text, char *out, size_t cap, bool *incomplete)
{
    garb_parsed_t r;
    (void)garb_parse_values(text, strlen(text), &r);
    garb_selectors_t *sel = garb_selectors_parse(&r, r.values, r.nvalues, OS64_HTML_NO_QUIRKS, false);
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
    garb_env_t env = {.width = 800, .height = 600};
    garb_cascade_t *c = garb_cascade(&in, 1, s_sweep_doc, env);
    // The winners, and then the faces, which no dump of winners holds.
    char *out = NULL;
    size_t size = 0;
    FILE *text_out = open_memstream(&out, &size);
    size_t need = garb_cascade_dump(c, s_sweep_doc, NULL, 0) + 1;
    char *dump = malloc(need);
    garb_cascade_dump(c, s_sweep_doc, dump, need);
    fputs(dump, text_out);
    free(dump);
    print_faces(c, text_out);
    fclose(text_out);
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
// with the sheet calling itself whole (Quinn, #203). A transform's lists
// are kept in the arena, and are swept beside them as the other lists a
// declaration builds of its own.
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
                                        "left 3px bottom",
                                        "transform: translate(1px, 2px) rotate(3deg) scale(2) "
                                        "matrix(1, 0, 0, 1, 0, 0)",
                                        "transform-origin: left 10px 2px", "rotate: 1 1 0 10deg",
                                        "translate: 1px 2%",
                                        "animation: a 1s, b 2s steps(2, end), c 3s ease-in 2",
                                        "animation-timing-function: cubic-bezier(0, 0, 1, 1), "
                                        "steps(3), ease",
                                        "will-change: transform, opacity, left"};
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
        int32_t whole = garb_read_declaration(&r, &r.decl, (garb_reading_t){0}, sets);
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
            int32_t n = garb_read_declaration(&r, &r.decl, (garb_reading_t){0}, sets);
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
    printf("libgarb sweep, shadow, layer and transform lists: every allocation of the read failed alone, %s\n",
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
    // A number as a script reads it (garb_css_number): six significant
    // digits, no trailing zeros, no exponent, no negative zero; every
    // double, the largest included, fits its own length.
    static const struct { double v; const char *want; } css[] = {
        {0, "0"}, {-0.0, "0"}, {1, "1"}, {-1, "-1"}, {0.5, "0.5"}, {33.333333333, "33.3333"},
        {1234.5678, "1234.57"}, {123456.7, "123457"}, {1e15, "1000000000000000"},
        {0.000001234, "0.000001234"}, {-0.0000001, "-0.0000001"}, {-0.00000000000000000001, "-0.00000000000000000001"}, {-1e-25, "0"},
        {1e-30, "0"}, {0.1, "0.1"}, {2.675, "2.675"}, {100, "100"}, {99.9999999, "100"},
        {1.5e20, "150000000000000000000"}, {-2.5e19, "-25000000000000000000"},
        {1.0 / 0.0, "0"}, {0.0 / 0.0, "0"}, {9007199254740993.0, "9007199254740992"},
    };
    int css_failed = 0;
    for (size_t i = 0; i < sizeof(css) / sizeof(css[0]); i++) {
        char b[8];
        size_t n = garb_css_number(css[i].v, b, sizeof(b));
        char whole[400];
        size_t again = garb_css_number(css[i].v, whole, sizeof(whole));
        bool cut = strlen(b) == (n < sizeof(b) ? n : sizeof(b) - 1) && strncmp(b, whole, strlen(b)) == 0;
        if (strcmp(whole, css[i].want) != 0 || again != n || n != strlen(whole) || !cut) {
            printf("FAIL css number %.17g: got %s (%zu), want %s\n", css[i].v, whole, n, css[i].want);
            css_failed++;
        }
    }
    char largest[400];
    size_t n_largest = garb_css_number(1.7976931348623157e308, largest, sizeof(largest));
    if (n_largest != strlen(largest) || n_largest != 309 || strncmp(largest, "179769", 6) != 0) {
        printf("FAIL css number: the largest double came out %zu long\n", n_largest);
        css_failed++;
    }
    printf("libgarb css numbers: %s\n", css_failed ? "FAILED" : "as a script reads them");
    return ok && css_failed == 0 ? 0 : 1;
}

// What a parse and a cascade COST, GARB.md § The cost: a sheet shaped like
// the web's biggest (Bootstrap's long selector lists, many short
// declarations), parsed, then cascaded over a page its rules reach.
// `--cost BYTES`: the parse must come back whole, at under GARB_COST_MAX
// arena bytes a byte of text, the ratio GARB.md gives, and so must the
// cascade, holding under GARB_CASCADE_COST_MAX bytes a byte of the sheet
// while it lives — its reading of the sheet and its own rules and winners
// — and leaving the sheet's arena as it found it. `--full`: a sheet at the
// input's own cap fills the arena, and must come back incomplete with the
// rules it finished kept — never with nothing because the list that held
// them could not be kept.
#define GARB_COST_MAX 17
#define GARB_CASCADE_COST_MAX 30
static const char kShaped[] =
    ".btn-outline-%d:not(:disabled):not(.disabled).active,"
    ".btn-outline-%d:not(:disabled):not(.disabled):active,"
    ".show>.btn-outline-%d.dropdown-toggle{color:#fff;background-color:#007bff;"
    "border-color:#007bff;box-shadow:0 0 0 .2rem rgba(0,123,255,.5)}\n"
    "@media (min-width:%dpx){.col-md-%d{flex:0 0 8.333333%%;max-width:8.333333%%}}\n";

// The shaped sheet, as near `bytes` long as its rules come, malloc'd; its
// length in *len, and how many times kShaped was written in *copies.
static char *shaped_sheet(size_t bytes, size_t *len, int *copies)
{
    char *text = malloc(bytes + 512);
    *len = 0;
    int i = 0;
    for (; *len + 512 < bytes; i++)
        *len += (size_t)snprintf(text + *len, 512, kShaped, i, i, i, 540 + i % 900, i % 12 + 1);
    *copies = i;
    return text;
}

// A document from `html`, finished.
static os64_html_document_t *page_of(const char *html, size_t len)
{
    os64_html_options_t opt = os64_html_options_default();
    os64_html_parser_t *hp = os64_html_parser_new(&opt);
    os64_html_parser_feed(hp, html, len);
    return os64_html_parser_finish(hp);
}

static size_t arena_used(const garb_parsed_t *r)
{
    return os64_arena_stats(r->arena).used_bytes;
}

static int cost(size_t bytes, bool full)
{
    size_t len;
    int copies;
    char *text = shaped_sheet(bytes, &len, &copies);
    garb_parsed_t r;
    garb_status_t st = garb_parse_sheet_text(text, len, &r);
    int32_t rules = 0;
    for (int32_t i = 0; i < r.nitems; i++)
        rules += r.items[i].kind == GARB_ITEM_RULE && r.items[i].rule->has_block;
    size_t parsed = arena_used(&r);
    double ratio = (double)parsed / (double)len;
    int ok = full ? st == GARB_OK && r.incomplete && rules > 1000
                  : st == GARB_OK && !r.incomplete && ratio < GARB_COST_MAX;
    printf("libgarb %s: a %zu KiB sheet, %d rules: %.1f arena bytes a byte%s%s\n",
           full ? "full" : "cost", len >> 10, rules, ratio, r.incomplete ? ", incomplete" : "",
           ok ? "" : " - FAILED");
    if (!full) {
        static const char kPage[] = "<!doctype html><a class=\"btn-outline-5 active\">x</a>"
                                    "<div class=col-md-3>y</div>";
        os64_html_document_t *doc = page_of(kPage, sizeof(kPage) - 1);
        garb_sheet_in_t in = {.sheet = &r};
        size_t before = live_bytes;
        garb_cascade_t *c = garb_cascade(&in, 1, doc, (garb_env_t){.width = 1200, .height = 900});
        double held = (double)(live_bytes - before) / (double)len;
        bool whole = c != NULL && !garb_cascade_incomplete(c) && !r.incomplete &&
                     arena_used(&r) == parsed && held < GARB_CASCADE_COST_MAX;
        printf("libgarb cost: its cascade holds %.1f bytes a byte, %s%s\n", held,
               c != NULL && !garb_cascade_incomplete(c) ? "whole" : "incomplete",
               whole ? ", the sheet as it was" : " - FAILED");
        ok &= whole;
        garb_cascade_free(c);
        os64_html_document_free(doc);
    }
    garb_free(&r);
    free(text);
    return ok ? 0 : 1;
}

// ONE READ BUDGET A CASCADE (GARB.md § The cost): what a cascade reads of
// all its sheets together is held to GARB_CASCADE_READ_MAX. A 1.5 MiB sheet
// given four times is read into more than one parse's arena and still
// whole, every element styled; given eight times, past the budget, the
// cascade comes up short and never holds more than the budget and its own
// arena's cap, while the sheet stays as it was parsed.
static int read_budget(void)
{
    size_t len;
    int copies;
    char *text = shaped_sheet((size_t)3 << 19, &len, &copies);
    garb_parsed_t r;
    garb_status_t st = garb_parse_sheet_text(text, len, &r);
    char page[8192];
    size_t at = (size_t)snprintf(page, sizeof(page), "<!doctype html><body>");
    int elements = 0;
    for (int i = 0; i < copies; i += copies / 32 + 1, elements++)
        at += (size_t)snprintf(page + at, sizeof(page) - at, "<a class=\"btn-outline-%d active\">%d</a>", i, i);
    os64_html_document_t *doc = page_of(page, at);
    garb_sheet_in_t in[8];
    for (int k = 0; k < 8; k++)
        in[k] = (garb_sheet_in_t){.sheet = &r, .parent = -1};
    size_t parsed = arena_used(&r);
    int failures = st != GARB_OK || r.incomplete;
    static const int kGiven[] = {4, 8};
    for (int g = 0; g < 2 && failures == 0; g++) {
        size_t before = live_bytes;
        garb_cascade_t *c = garb_cascade(in, kGiven[g], doc, (garb_env_t){.width = 1200, .height = 900});
        size_t held = live_bytes - before;
        int styled = 0;
        for (const os64_html_node_t *x = doc->body != NULL ? doc->body->first_child : NULL; x != NULL; x = x->next)
            styled += x->kind == OS64_HTML_ELEMENT && garb_style_for(c, x).n > 0;
        bool past = kGiven[g] == 8;
        bool ok = c != NULL && garb_cascade_incomplete(c) == past && (past || styled == elements) &&
                  held <= GARB_CASCADE_READ_MAX + GARB_ARENA_MAX + ((size_t)4 << 20) &&
                  (past || held > GARB_ARENA_MAX) && !r.incomplete && arena_used(&r) == parsed;
        printf("libgarb read budget: a %zu KiB sheet given %d times: %s, %d of %d elements styled, %.1f MiB held%s\n",
               len >> 10, kGiven[g], c != NULL && !garb_cascade_incomplete(c) ? "whole" : "short", styled,
               elements, held / 1048576.0, ok ? "" : " - FAILED");
        failures += !ok;
        garb_cascade_free(c);
    }
    os64_html_document_free(doc);
    garb_free(&r);
    free(text);
    return failures != 0;
}

// A PAGE CASCADED AGAIN AND AGAIN (GARB.md § The cost): a script that
// measures, a window resized, cascade the page's sheets once a layout.
// Every cascade reads the sheet into arenas of its own, freed with it, so
// across `--cascades N` of a 1 MiB sheet — 16 would read some 340 MiB, three
// and a half times GARB_ARENA_MAX, were the reads still the sheet's — the
// sheet is never marked incomplete and its arena never grows, every cascade
// is whole and answers exactly what the first did, every element its rules
// reach styled, and the heap holds after each what it held before.
static int cascades(int times)
{
    size_t len;
    int copies;
    char *text = shaped_sheet((size_t)1 << 20, &len, &copies);
    garb_parsed_t r;
    garb_status_t st = garb_parse_sheet_text(text, len, &r);
    // Elements across the whole sheet, its first rules and its last: the
    // buttons' rules hold everywhere, and every column's at some width.
    char page[16384];
    size_t at = (size_t)snprintf(page, sizeof(page), "<!doctype html><body>");
    int elements = 0;
    for (int i = 0; i < copies; i += copies / 64 + 1, elements++)
        at += (size_t)snprintf(page + at, sizeof(page) - at, "<a class=\"btn-outline-%d active\">%d</a>", i, i);
    for (int k = 1; k <= 12; k++, elements++)
        at += (size_t)snprintf(page + at, sizeof(page) - at, "<div class=col-md-%d>%d</div>", k, k);
    os64_html_document_t *doc = page_of(page, at);
    garb_sheet_in_t in = {.sheet = &r};
    garb_env_t env = {.width = 1200, .height = 900};
    size_t parsed = arena_used(&r), base_live = live, base_bytes = live_bytes;
    char *first = NULL;
    int failures = st != GARB_OK || r.incomplete;
    for (int t = 1; t <= times && st == GARB_OK; t++) {
        garb_cascade_t *c = garb_cascade(&in, 1, doc, env);
        if (c == NULL) {
            fprintf(stderr, "FAIL cascades: cascade %d came back with nothing\n", t);
            failures++;
            break;
        }
        size_t need = garb_cascade_dump(c, doc, NULL, 0) + 1;
        char *dump = malloc(need);
        garb_cascade_dump(c, doc, dump, need);
        int styled = 0;
        for (const os64_html_node_t *x = doc->body != NULL ? doc->body->first_child : NULL; x != NULL; x = x->next)
            styled += x->kind == OS64_HTML_ELEMENT && garb_style_for(c, x).n > 0;
        if (garb_cascade_incomplete(c) || styled != elements ||
            (first != NULL && strcmp(dump, first) != 0)) {
            fprintf(stderr, "FAIL cascades: cascade %d is %s, %d of %d elements styled%s\n", t,
                    !garb_cascade_incomplete(c) ? "whole" : "incomplete", styled,
                    elements, first != NULL && strcmp(dump, first) != 0 ? ", not the first's answer" : "");
            failures++;
        }
        garb_cascade_free(c);
        if (first == NULL)
            first = dump;
        else
            free(dump);
        if (r.incomplete || arena_used(&r) != parsed || live != base_live || live_bytes != base_bytes) {
            fprintf(stderr, "FAIL cascades: after cascade %d the sheet is %s at %zu bytes (parsed at %zu), "
                    "the heap %zu blocks and %zu bytes (from %zu and %zu)\n", t,
                    r.incomplete ? "incomplete" : "whole", arena_used(&r), parsed, live, live_bytes,
                    base_live, base_bytes);
            failures++;
        }
    }
    printf("libgarb cascades: a %zu KiB sheet cascaded %d times over %d elements: %s\n", len >> 10,
           times, elements, failures == 0 ? "each whole and the first's answer, the sheet and the heap as they were"
                                          : "FAILED");
    free(first);
    os64_html_document_free(doc);
    garb_free(&r);
    free(text);
    return failures != 0;
}

// A CASCADE STARTS ITS PARSE SCRATCH ONCE (HEAP_BINS.md § Part A): every
// rule's block is read on the scratch the cascade keeps, so a sheet of
// twice the rules — style rules and `@media` blocks of them — starts no
// more scratches than the first, and only a few; were each read to start
// its own, there would be one a rule. Every element is still styled, and
// the heap holds after each cascade what it held before.
static int scratch(void)
{
    static const int kRules[] = {1000, 2000};
    size_t started[2] = {0, 0};
    int failures = 0;
    const char *page = "<!doctype html><body><p class=r7>a</p><p class=m7>b</p>";
    os64_html_document_t *doc = page_of(page, strlen(page));
    for (int k = 0; k < 2; k++) {
        size_t cap = (size_t)kRules[k] * 96, len = 0;
        char *text = malloc(cap);
        for (int i = 0; i < kRules[k]; i += 2) {
            len += (size_t)snprintf(text + len, cap - len, ".r%d { color: red; margin: %dpx }\n", i + 1, i);
            len += (size_t)snprintf(text + len, cap - len,
                                    "@media (min-width: 10px) { .m%d { padding: %dpx } }\n", i + 1, i);
        }
        garb_parsed_t r;
        garb_status_t st = garb_parse_sheet_text(text, len, &r);
        garb_sheet_in_t in = {.sheet = &r, .parent = -1};
        size_t base_live = live, base_bytes = live_bytes, before = reallocs_from_nothing;
        garb_cascade_t *c = garb_cascade(&in, 1, doc, (garb_env_t){.width = 1200, .height = 900});
        started[k] = reallocs_from_nothing - before;
        int styled = 0;
        for (const os64_html_node_t *x = doc->body->first_child; x != NULL; x = x->next)
            styled += x->kind == OS64_HTML_ELEMENT && garb_style_for(c, x).n > 0;
        bool whole = c != NULL && !garb_cascade_incomplete(c);
        garb_cascade_free(c);
        if (st != GARB_OK || !whole || styled != 2 || live != base_live || live_bytes != base_bytes) {
            fprintf(stderr, "FAIL scratch: %d rules: %s, %d of 2 elements styled, the heap %zu blocks "
                    "(from %zu)\n", kRules[k], whole ? "whole" : "short", styled, live, base_live);
            failures++;
        }
        garb_free(&r);
        free(text);
    }
    bool ok = failures == 0 && started[1] == started[0] && started[1] <= 4;
    printf("libgarb scratch: scratches started by a cascade of %d rules: %zu, of %d rules: %zu%s\n",
           kRules[0], started[0], kRules[1], started[1], ok ? "" : " - FAILED");
    os64_html_document_free(doc);
    return !ok;
}

static const os64_html_node_t *by_id(const os64_html_node_t *n, const char *id)
{
    for (; n != NULL; n = n->next) {
        const os64_html_attr_t *a = n->kind == OS64_HTML_ELEMENT ? os64_html_attr(n, "id") : NULL;
        if (a != NULL && a->value != NULL && strcmp(a->value, id) == 0)
            return n;
        const os64_html_node_t *in = by_id(n->first_child, id);
        if (in != NULL)
            return in;
    }
    return NULL;
}

// What @keyframes cost at scale (PILE3.md § Animations): 4,000 rules of
// different names, then one of 40,000 offsets, every one different. The
// element's keyframes are every offset, in order, and the cascade and the
// answer take a sort's work, whichever order the offsets are written in.
// Two bounds, since a quadratic can hide behind either: under the sanitizers
// the whole costs under a second of CPU where grouping each offset against
// those gathered before it took 45 (KEYFRAMES_SCALE_SECONDS sits between),
// and written last to first it costs what it does written first to last,
// where sorting one at a time made it five times as much
// (KEYFRAMES_SCALE_BACKWARDS).
#define KEYFRAMES_SCALE_SECONDS 10.0
#define KEYFRAMES_SCALE_BACKWARDS 2.0

// One run of it, the offsets written `backwards` or not: true when the
// keyframes came back whole and in order, the CPU it took in *took.
static bool keyframes_at_scale(bool backwards, int *nkeyframes, double *took)
{
    enum { kNames = 4000, kOffsets = 40000 };
    size_t cap = (size_t)kNames * 40 + (size_t)kOffsets * 32 + 64, len = 0;
    char *text = malloc(cap);
    for (int i = 0; i < kNames; i++)
        len += (size_t)snprintf(text + len, cap - len, "@keyframes n%d{to{opacity:0}}", i);
    len += (size_t)snprintf(text + len, cap - len, "@keyframes big{");
    for (int k = 0; k < kOffsets; k++) {
        int i = backwards ? kOffsets - 1 - k : k;
        len += (size_t)snprintf(text + len, cap - len, "%.4f%%{opacity:.5}", i * 100.0 / kOffsets);
    }
    len += (size_t)snprintf(text + len, cap - len, "}");
    static const char kPage[] = "<!doctype html><div id=x style='animation-name:big'></div>";
    os64_html_options_t opt = os64_html_options_default();
    os64_html_parser_t *hp = os64_html_parser_new(&opt);
    os64_html_parser_feed(hp, kPage, sizeof(kPage) - 1);
    os64_html_document_t *doc = os64_html_parser_finish(hp);
    garb_parsed_t r;
    garb_status_t st = garb_parse_sheet_text(text, len, &r);
    clock_t t0 = clock();
    garb_sheet_in_t in = {.sheet = &r};
    garb_cascade_t *c = garb_cascade(&in, 1, doc, (garb_env_t){.width = 800, .height = 600});
    const garb_keyframe_t *f = NULL;
    int32_t nf = c != NULL ? garb_keyframes(c, by_id(doc->document, "x"), "big", 3, &f) : -1;
    *took = (double)(clock() - t0) / CLOCKS_PER_SEC;
    bool ordered = st == GARB_OK && !r.incomplete && nf == kOffsets;
    for (int32_t i = 1; ordered && i < nf; i++)
        ordered = f[i].offset > f[i - 1].offset;
    *nkeyframes = nf;
    garb_cascade_free(c);
    garb_free(&r);
    os64_html_document_free(doc);
    free(text);
    return ordered;
}

// The faces kept: a sheet of 300 @font-face rules and one more of the
// first face again keeps the first GARB_FONT_FACES_MAX and counts the rest
// lost, the cascade whole — the repeat among them, since a repeat is a face
// of its own (it joins the first in a composite), not a replacement.
static int faces_cap(void)
{
    enum { kFaces = 300 };
    size_t cap = (size_t)kFaces * 64 + 128, len = 0;
    char *text = malloc(cap);
    for (int i = 0; i < kFaces; i++)
        len += (size_t)snprintf(text + len, cap - len, "@font-face{font-family:f%d;src:url(a%d)}", i, i);
    len += (size_t)snprintf(text + len, cap - len, "@font-face{font-family:F0;src:url(again)}");
    static const char kPage[] = "<!doctype html><p>x";
    os64_html_options_t opt = os64_html_options_default();
    os64_html_parser_t *hp = os64_html_parser_new(&opt);
    os64_html_parser_feed(hp, kPage, sizeof(kPage) - 1);
    os64_html_document_t *doc = os64_html_parser_finish(hp);
    garb_parsed_t r;
    (void)garb_parse_sheet_text(text, len, &r);
    garb_sheet_in_t in = {.sheet = &r};
    garb_cascade_t *c = garb_cascade(&in, 1, doc, (garb_env_t){.width = 800, .height = 600});
    const garb_font_face_t *f = NULL;
    uint32_t lost = 0;
    int32_t n = garb_cascade_font_faces(c, &f, &lost);
    bool ok = c != NULL && !garb_cascade_incomplete(c) && n == GARB_FONT_FACES_MAX &&
              lost == kFaces + 1 - GARB_FONT_FACES_MAX && f[0].src[0].len == 2 &&
              memcmp(f[0].src[0].text, "a0", 2) == 0;
    printf("libgarb faces: %d rules, %d kept, %u lost, a repeat past the cap among them%s\n", kFaces + 1,
           n, lost, ok ? "" : " - FAILED");
    garb_cascade_free(c);
    garb_free(&r);
    os64_html_document_free(doc);
    free(text);
    return ok ? 0 : 1;
}

// What a cascade allocates for GARB_FONT_FACES_MAX small faces and then
// one rule of `extra` sources.
static size_t faces_past_cap_cost(int extra)
{
    size_t cap = (size_t)GARB_FONT_FACES_MAX * 64 + (size_t)extra * 16 + 128, len = 0;
    char *text = malloc(cap);
    for (int i = 0; i < GARB_FONT_FACES_MAX; i++)
        len += (size_t)snprintf(text + len, cap - len, "@font-face{font-family:f%d;src:url(a%d)}", i, i);
    len += (size_t)snprintf(text + len, cap - len, "@font-face{font-family:late;src:url(z)");
    for (int i = 0; i < extra; i++)
        len += (size_t)snprintf(text + len, cap - len, ",url(z%d)", i);
    len += (size_t)snprintf(text + len, cap - len, "}");
    static const char kPage[] = "<!doctype html><p>x";
    os64_html_options_t opt = os64_html_options_default();
    os64_html_parser_t *hp = os64_html_parser_new(&opt);
    os64_html_parser_feed(hp, kPage, sizeof(kPage) - 1);
    os64_html_document_t *doc = os64_html_parser_finish(hp);
    garb_parsed_t r;
    fail_at = 0;
    (void)garb_parse_sheet_text(text, len, &r);
    garb_sheet_in_t in = {.sheet = &r};
    allocations = 0;
    garb_cascade_free(garb_cascade(&in, 1, doc, (garb_env_t){.width = 800, .height = 600}));
    size_t cost = allocations;
    garb_free(&r);
    os64_html_document_free(doc);
    free(text);
    return cost;
}

// A rule past the cap costs the cascade nothing however long it is: it is
// counted, not read.
static int faces_past_cap(void)
{
    size_t small = faces_past_cap_cost(0), large = faces_past_cap_cost(2000);
    bool ok = small == large;
    printf("libgarb faces: a rule past the cap costs %zu allocations with 1 source and %zu with 2001%s\n",
           small, large, ok ? "" : " - FAILED");
    return ok ? 0 : 1;
}

// The faces with each allocation failing alone: a face is kept whole —
// every one of its sources — or counted lost, never kept in part.
static int faces_sweep(void)
{
    // The three faces many times over, so the faces outgrow the arena's
    // chunks and a failure lands inside a face's own reading. Each
    // allocation is failed alone, and then it and every one after it —
    // the arena retries a chunk it could not get at a smaller size, so
    // only the second way fails the arena's own allocations. The sheet is
    // parsed once: a failure marks the cascade it lands in, never the
    // sheet the cascade reads (garb/cascade.h), which every step checks.
    static const char kFaces[] =
        "@font-face{font-family:\"One Face\";src:url(a.woff2) format(woff2),url(a.ttf),local(A)}"
        "@font-face{font-family:Two;src:url(b1),url(b2) format(truetype);unicode-range:U+0-7F,U+100-17F}"
        "@font-face{font-family:Three;src:local(\"Three Sans\"),url(c);font-weight:300 500}";
    static const int32_t kSources[] = {3, 2, 2};
    enum { COPIES = 80 };
    static char sheet[COPIES * sizeof(kFaces)];
    size_t len = 0;
    for (int i = 0; i < COPIES; i++)
        len += (size_t)snprintf(sheet + len, sizeof(sheet) - len, "%s", kFaces);
    static const char kPage[] = "<!doctype html><p>x";
    os64_html_options_t opt = os64_html_options_default();
    os64_html_parser_t *hp = os64_html_parser_new(&opt);
    os64_html_parser_feed(hp, kPage, sizeof(kPage) - 1);
    os64_html_document_t *doc = os64_html_parser_finish(hp);
    garb_parsed_t r;
    garb_sheet_in_t in = {.sheet = &r};
    fail_at = 0;
    (void)garb_parse_sheet_text(sheet, len, &r);
    allocations = 0;
    garb_cascade_t *whole = garb_cascade(&in, 1, doc, (garb_env_t){.width = 800, .height = 600});
    size_t count = allocations;
    const garb_font_face_t *f;
    int failures = whole == NULL || garb_cascade_incomplete(whole) ||
                   garb_cascade_font_faces(whole, &f, NULL) != 3 * COPIES;
    if (failures)
        fprintf(stderr, "FAIL faces sweep: the cascade with no failure is not every face\n");
    garb_cascade_free(whole);
    for (size_t step = 0; step < 2 * count; step++) {
        size_t at = step % count + 1;
        fail_single = step < count;
        allocations = 0;
        fail_at = at;
        garb_cascade_t *c = garb_cascade(&in, 1, doc, (garb_env_t){.width = 800, .height = 600});
        fail_at = 0;
        uint32_t lost = 0;
        int32_t n = c != NULL ? garb_cascade_font_faces(c, &f, &lost) : 3 * COPIES;
        for (int32_t i = 0; i < n && c != NULL; i++) {
            int32_t which = f[i].family_len == 8 ? 0 : f[i].family_len == 3 ? 1 : 2;
            if (f[i].nsrc != kSources[which]) {
                fprintf(stderr, "FAIL faces sweep: failure at %zu%s kept %.*s with %d of its %d sources\n",
                        at, fail_single ? "" : " on", (int)f[i].family_len, f[i].family, f[i].nsrc,
                        kSources[which]);
                failures++;
            }
        }
        if (n + (int32_t)lost != 3 * COPIES) {
            fprintf(stderr, "FAIL faces sweep: failure at %zu%s kept %d and lost %u of %d\n", at,
                    fail_single ? "" : " on", n, lost, 3 * COPIES);
            failures++;
        }
        if (r.incomplete) {
            fprintf(stderr, "FAIL faces sweep: failure at %zu%s marked the sheet incomplete\n", at,
                    fail_single ? "" : " on");
            failures++;
            r.incomplete = false;
        }
        garb_cascade_free(c);
    }
    garb_free(&r);
    fail_single = false;
    os64_html_document_free(doc);
    printf("libgarb faces sweep: each of %zu allocations failed alone and on, %s\n", count,
           failures == 0 ? "every face kept whole or counted lost" : "FAILED");
    return failures != 0;
}

static int keyframes_scale(void)
{
    int nf = 0, nb = 0;
    double forwards = 0, backwards = 0;
    bool whole_forwards = keyframes_at_scale(false, &nf, &forwards);
    bool whole_backwards = keyframes_at_scale(true, &nb, &backwards);
    bool whole = whole_forwards && whole_backwards;
    int ok = whole && forwards < KEYFRAMES_SCALE_SECONDS && backwards < KEYFRAMES_SCALE_SECONDS &&
             backwards < KEYFRAMES_SCALE_BACKWARDS * forwards;
    printf("libgarb keyframes: 4000 names and 40000 offsets, %d and %d keyframes in order%s, "
           "in %.3f s of CPU written forwards and %.3f backwards%s\n",
           nf, nb, whole ? "" : " - NOT", forwards, backwards, ok ? "" : " - FAILED");
    return ok ? 0 : 1;
}

// THE MODERN MARK (values.h's garb_prop_modern and garb_reading_t): with
// `modern_off`, a marked name reads as one libgarb does not read, a marked
// longhand leaves no shorthand's answer, a shorthand reads no part of its
// value that only a marked longhand takes, and a cascade built with it in
// its environment neither applies nor supports what is marked, and tallies
// it as passed over. tools/test_garb_host.sh builds this driver with
// GARB_MODERN_PROBE marking a longhand (animation-name), a shorthand
// (grid-gap) and an alias (-webkit-transform), since the arc may not have
// marked one of each, and a longhand of every other shorthand, so each
// shorthand's reader is held to the rule.
static int modern_reads(const char *decl, garb_reading_t how, garb_prop_t *props, int32_t cap)
{
    garb_parsed_t r;
    if (garb_parse_one_declaration(decl, strlen(decl), &r) != GARB_OK) {
        garb_free(&r);
        return -100;
    }
    garb_set_t sets[GARB_SETS_MAX];
    int32_t n = garb_read_declaration(&r, &r.decl, how, sets);
    for (int32_t k = 0; k < n && k < cap; k++)
        props[k] = sets[k].prop;
    garb_free(&r);
    return n;
}

static bool modern_has(const garb_prop_t *props, int32_t n, garb_prop_t p)
{
    for (int32_t k = 0; k < n; k++)
        if (props[k] == p)
            return true;
    return false;
}

// `#x`'s winners and what the cascade passed over, with the environment
// knowing the marked names or not.
static void modern_cascade(bool off, garb_prop_t *props, int32_t *n, bool *skipped_name, bool *skipped_gap)
{
    static const char kPage[] = "<!doctype html><div id=x style='color:blue;-webkit-transform:none'></div>";
    static const char kSheet[] =
        "#x{animation-name:spin;grid-gap:4px;margin:1px}"
        "@supports (animation-name: spin){#x{display:flex}}"
        "@supports (animation-name: var(--a)){#x{float:left}}"
        "@supports (animation: spin){#x{visibility:hidden}}"
        "@supports not (animation-name: spin){#x{z-index:1}}";
    os64_html_options_t opt = os64_html_options_default();
    os64_html_parser_t *hp = os64_html_parser_new(&opt);
    os64_html_parser_feed(hp, kPage, sizeof(kPage) - 1);
    os64_html_document_t *doc = os64_html_parser_finish(hp);
    garb_parsed_t r;
    (void)garb_parse_sheet_text(kSheet, sizeof(kSheet) - 1, &r);
    garb_sheet_in_t in = {.sheet = &r};
    garb_cascade_t *c = garb_cascade(&in, 1, doc, (garb_env_t){.width = 800, .height = 600, .modern_off = off});
    garb_style_t st = c != NULL ? garb_style_for(c, by_id(doc->document, "x")) : (garb_style_t){0};
    *n = st.n;
    for (int32_t k = 0; k < st.n && k < 64; k++)
        props[k] = st.sets[k].prop;
    garb_skip_t skips[GARB_SKIPS_MAX];
    int32_t ns = c != NULL ? garb_cascade_skips(c, skips, GARB_SKIPS_MAX, NULL) : 0;
    *skipped_name = *skipped_gap = false;
    for (int32_t k = 0; k < ns && k < GARB_SKIPS_MAX; k++) {
        if (skips[k].kind != GARB_SKIP_PROPERTY)
            continue;
        *skipped_name |= skips[k].len == 14 && memcmp(skips[k].name, "animation-name", 14) == 0;
        *skipped_gap |= skips[k].len == 8 && memcmp(skips[k].name, "grid-gap", 8) == 0;
    }
    garb_cascade_free(c);
    garb_free(&r);
    os64_html_document_free(doc);
}

// :defined (CUSTOM_ELEMENTS.md). With the switch on, an HTML element with a
// custom name is not defined while it waits for a definition, nor once its
// constructor has failed; with it off every element is, as before the arc.
// Read through a selector list, which takes the switch as it is parsed, and
// through a cascade, which takes it from its environment.
static int defined(void)
{
    static const char kPage[] = "<!doctype html><body><x-wait id=w></x-wait><x-made id=m></x-made>"
                                "<x-fail id=f></x-fail><div id=d></div><font-face id=r></font-face>"
                                "<svg><x-svg id=s /></svg>";
    static const struct { const char *id; bool on; } kOn[] = {
        {"w", false}, {"m", true}, {"f", false}, {"d", true}, {"r", true}, {"s", true}};
    os64_html_options_t opt = os64_html_options_default();
    os64_html_parser_t *hp = os64_html_parser_new(&opt);
    os64_html_parser_feed(hp, kPage, sizeof(kPage) - 1);
    os64_html_document_t *doc = os64_html_parser_finish(hp);
    os64_html_set_ce(doc, (os64_html_node_t *)by_id(doc->document, "m"), OS64_HTML_CE_CUSTOM);
    os64_html_set_ce(doc, (os64_html_node_t *)by_id(doc->document, "f"), OS64_HTML_CE_FAILED);
    static const char kSheet[] = ":not(:defined) { display: none }";
    int failed = 0;
    for (int off = 0; off < 2; off++) {
        garb_parsed_t r, sheet;
        garb_parse_values(":defined", 8, &r);
        garb_selectors_t *sel = garb_selectors_parse(&r, r.values, r.nvalues, OS64_HTML_NO_QUIRKS, off);
        garb_parse_sheet_text(kSheet, sizeof(kSheet) - 1, &sheet);
        garb_sheet_in_t in = {.sheet = &sheet};
        garb_cascade_t *c = garb_cascade(&in, 1, doc, (garb_env_t){.width = 800, .height = 600,
                                                                   .modern_off = off});
        for (size_t k = 0; k < sizeof(kOn) / sizeof(kOn[0]); k++) {
            const os64_html_node_t *el = by_id(doc->document, kOn[k].id);
            bool want = off || kOn[k].on;
            if (sel == NULL || el == NULL || garb_selector_matches(sel, 0, el) != want ||
                c == NULL || (garb_style_for(c, el).n == 0) != want) {
                printf("libgarb :defined: #%s with the switch %s is %s - FAILED\n", kOn[k].id,
                       off ? "off" : "on", want ? "defined" : "not defined");
                failed++;
            }
        }
        garb_cascade_free(c);
        garb_free(&sheet);
        garb_free(&r);
    }
    os64_html_document_free(doc);
    printf("libgarb :defined: %s\n", failed ? "FAILED" : "each state, the switch on and off");
    return failed;
}

static int modern_marks(void)
{
    int failed = 0;
#define MODERN_CHECK(cond, what) \
    do { if (!(cond)) { printf("libgarb modern: %s - FAILED\n", what); failed++; } } while (0)
    garb_reading_t on = {0}, off = {.modern_off = true};
    garb_prop_t p[GARB_SETS_MAX];
    MODERN_CHECK(garb_prop_modern(GARB_ANIMATION_NAME) && !garb_prop_modern(GARB_DISPLAY) &&
                 !garb_prop_modern(GARB_NPROPS), "the mark is read off the table, the probe's included");
    MODERN_CHECK(modern_reads("animation-name: spin", on, p, GARB_SETS_MAX) == 1 &&
                 p[0] == GARB_ANIMATION_NAME, "a marked longhand is read when marked names are known");
    MODERN_CHECK(modern_reads("animation-name: spin", off, p, GARB_SETS_MAX) == 0,
                 "a marked longhand is a name libgarb does not read with modern_off");
    MODERN_CHECK(modern_reads("-webkit-animation-name: spin", off, p, GARB_SETS_MAX) == 0,
                 "an alias of a marked longhand goes with it");
    MODERN_CHECK(modern_reads("animation: spin 1s", on, p, GARB_SETS_MAX) == 8 &&
                 modern_has(p, 8, GARB_ANIMATION_NAME), "a shorthand sets all of its longhands");
    int32_t n = modern_reads("animation: 1s", off, p, GARB_SETS_MAX);
    MODERN_CHECK(n == 7 && !modern_has(p, n, GARB_ANIMATION_NAME),
                 "an unmarked shorthand sets every longhand but the marked one with modern_off");
    // Each shorthand, with modern_off, as an engine that never had its
    // marked longhand reads it: a part only that longhand takes is invalid,
    // and the rest reads without it.
    static const struct { const char *decl; int32_t off; } kParts[] = {
        {"animation: spin", GARB_DECL_INVALID}, {"animation: spin 1s", GARB_DECL_INVALID},
        {"animation: ease ease", GARB_DECL_INVALID}, {"animation: ease", 7},
        {"font: bold 12px serif", GARB_DECL_INVALID}, {"font: italic 12px serif", 5},
        {"text-decoration: underline", GARB_DECL_INVALID}, {"text-decoration: dotted", 0},
        {"list-style: url(a.png)", GARB_DECL_INVALID}, {"list-style: none none", GARB_DECL_INVALID},
        {"list-style: none", 2}, {"list-style: square inside", 2},
        {"flex: 1 2", GARB_DECL_INVALID}, {"flex: 1 10px", 2},
        {"flex-flow: wrap", GARB_DECL_INVALID}, {"flex-flow: column", 1},
        {"place-items: center start", GARB_DECL_INVALID}, {"place-items: center", 1},
        {"overflow: hidden auto", GARB_DECL_INVALID}, {"overflow: hidden", 1},
        {"grid-row: 1 / 3", GARB_DECL_INVALID}, {"grid-row: 1", 1},
        {"grid-area: 1 / 2 / 3", GARB_DECL_INVALID}, {"grid-area: 1 / 2", 3},
        {"margin: 1px 2px 3px 4px", GARB_DECL_INVALID}, {"margin: 1px 2px 3px", 3},
        {"border-radius: 1px 2px 3px 4px", GARB_DECL_INVALID}, {"border-radius: 1px / 2px", 3},
        {"border-radius: 1px / 1px 2px 3px 4px", GARB_DECL_INVALID},
        {"border-top: red", GARB_DECL_INVALID}, {"border-top: 1px solid", 2}, {"border: red", 11},
        {"background: url(a.png) content-box padding-box", GARB_DECL_INVALID},
        {"background: content-box", 6}, {"background: 0 0 / 10px", GARB_DECL_INVALID},
        {"background: 0 0 red", 6},
    };
    for (size_t k = 0; k < sizeof(kParts) / sizeof(kParts[0]); k++) {
        int32_t got = modern_reads(kParts[k].decl, off, p, GARB_SETS_MAX);
        if (got != kParts[k].off) {
            printf("libgarb modern: %s read %d with modern_off, not %d - FAILED\n", kParts[k].decl,
                   (int)got, (int)kParts[k].off);
            failed++;
        }
        if (modern_reads(kParts[k].decl, on, p, GARB_SETS_MAX) <= 0) {
            printf("libgarb modern: %s is not read with the marked names known - FAILED\n",
                   kParts[k].decl);
            failed++;
        }
    }
    n = modern_reads("animation: inherit", off, p, GARB_SETS_MAX);
    MODERN_CHECK(n == 7 && !modern_has(p, n, GARB_ANIMATION_NAME),
                 "a CSS-wide keyword reaches every longhand but the marked one with modern_off");
    MODERN_CHECK(modern_reads("grid-gap: 4px", off, p, GARB_SETS_MAX) == 0 &&
                 modern_reads("gap: 4px", off, p, GARB_SETS_MAX) == 2 &&
                 modern_reads("grid-gap: 4px", on, p, GARB_SETS_MAX) == 2,
                 "a marked shorthand is unknown with modern_off, its unmarked twin is not");
    MODERN_CHECK(modern_reads("-webkit-transform: none", off, p, GARB_SETS_MAX) == 0 &&
                 modern_reads("transform: none", off, p, GARB_SETS_MAX) == 1 &&
                 modern_reads("-webkit-transform: none", on, p, GARB_SETS_MAX) == 1,
                 "a marked alias is unknown with modern_off, the name it stands for is not");
    MODERN_CHECK(modern_reads("width: 10", (garb_reading_t){.quirks = true, .modern_off = true}, p,
                              GARB_SETS_MAX) == 1, "quirks still reads beside modern_off");
    MODERN_CHECK(modern_reads("animation-name: var(--a)", off, p, GARB_SETS_MAX) == 0,
                 "a marked name holding var() is unknown, not held, with modern_off");
    // The list of names a style object makes members of: each once, every
    // one read with the marked names known, the marked ones unknown without.
    int32_t names = 0;
    bool once = true, all_known = true;
    for (const char *nm; (nm = garb_prop_name_at(names)) != NULL; names++) {
        for (int32_t j = 0; j < names; j++)
            once &= strcmp(garb_prop_name_at(j), nm) != 0;
        all_known &= garb_prop_known(nm, strlen(nm), false);
    }
    MODERN_CHECK(names > GARB_NPROPS && once && all_known && garb_prop_name_at(-1) == NULL &&
                 strcmp(garb_prop_name_at(0), garb_prop_name(0)) == 0,
                 "the names are the longhands' and the rest, each once, each read");
    // Every name the switch takes away, read by name with it off and on: a
    // CSS-wide keyword, which every property takes, is no declaration at
    // all with it off and sets at least one longhand with it on.
    int32_t marked = 0;
    for (int32_t k = 0; garb_prop_name_at(k) != NULL; k++) {
        const char *nm = garb_prop_name_at(k);
        if (garb_prop_known(nm, strlen(nm), true))
            continue;
        char decl[96];
        snprintf(decl, sizeof(decl), "%s: inherit", nm);
        marked++;
        if (modern_reads(decl, off, p, GARB_SETS_MAX) != 0 ||
            modern_reads(decl, on, p, GARB_SETS_MAX) <= 0) {
            printf("libgarb modern: %s is read with modern_off, or not without it - FAILED\n", nm);
            failed++;
        }
    }
    MODERN_CHECK(marked >= 3, "the names the switch takes away are read by name");
    MODERN_CHECK(!garb_prop_known("animation-name", 14, true) &&
                 !garb_prop_known("grid-gap", 8, true) &&
                 !garb_prop_known("-webkit-transform", 17, true) &&
                 garb_prop_known("Animation", 9, true) && garb_prop_known("margin", 6, true) &&
                 !garb_prop_known("marginx", 7, false),
                 "a marked name is not known with modern_off, a shorthand with a longhand left is");

    garb_prop_t w[64];
    bool name, gap;
    modern_cascade(false, w, &n, &name, &gap);
    MODERN_CHECK(modern_has(w, n, GARB_ANIMATION_NAME) && modern_has(w, n, GARB_ROW_GAP) &&
                 modern_has(w, n, GARB_DISPLAY) && modern_has(w, n, GARB_FLOAT) &&
                 modern_has(w, n, GARB_TRANSFORM) && modern_has(w, n, GARB_VISIBILITY) &&
                 !modern_has(w, n, GARB_Z_INDEX) && !name && !gap,
                 "a cascade that knows the marked names applies and supports them");
    modern_cascade(true, w, &n, &name, &gap);
    MODERN_CHECK(!modern_has(w, n, GARB_ANIMATION_NAME) && !modern_has(w, n, GARB_ROW_GAP) &&
                 !modern_has(w, n, GARB_DISPLAY) && !modern_has(w, n, GARB_FLOAT) &&
                 !modern_has(w, n, GARB_TRANSFORM) && modern_has(w, n, GARB_COLOR) &&
                 modern_has(w, n, GARB_MARGIN_TOP),
                 "a cascade with modern_off applies no marked name, in a sheet or a style attribute, "
                 "and supports none");
    MODERN_CHECK(name && gap, "a cascade with modern_off tallies the marked names it passed over");
    MODERN_CHECK(!modern_has(w, n, GARB_VISIBILITY) && modern_has(w, n, GARB_Z_INDEX),
                 "with modern_off, @supports finds a shorthand whose value only a marked longhand "
                 "takes unsupported, and the marked longhand itself unsupported, in one reading");
#undef MODERN_CHECK
    printf("libgarb modern: %s\n", failed ? "FAILED" : "the mark holds for longhands, shorthands, aliases and the cascade");
    return failed ? 1 : 0;
}

// A media query's cost against its depth, by the steps media.c counts
// (GARB_MEDIA_COUNT, which the build defines): `(width)` in d brackets,
// matched, serialised and serialised into a grown buffer. Each bracket
// level must add the same steps as the one before; a reading that judged
// a level and then wrote it doubled them at every level.
extern size_t garb_media_visits;
static int media_depth(void)
{
    int failed = 0;
    size_t last[3] = {0, 0, 0}, step[3] = {0, 0, 0};
    garb_env_t env = {.width = 800, .height = 600};
    // It stops at the first level that costs more than the one before: a
    // cost that doubles would otherwise take days to reach the bound.
    for (int d = 0; d < GARB_DEPTH_MAX - 2 && !failed; d++) {
        char q[2 * GARB_DEPTH_MAX + 16];
        int k = 0;
        for (int i = 0; i < d; i++) q[k++] = '(';
        k += snprintf(q + k, sizeof(q) - (size_t)k, "(width)");
        for (int i = 0; i < d; i++) q[k++] = ')';
        q[k] = '\0';
        size_t visits[3];
        garb_media_visits = 0;
        bool matched = garb_media_text_matches(q, env);
        visits[0] = garb_media_visits;
        char out[256];
        garb_media_visits = 0;
        size_t n = garb_media_text_serialize(q, out, sizeof(out));
        visits[1] = garb_media_visits;
        size_t grown_length = 0;
        garb_media_visits = 0;
        char *grown = garb_media_text_serialized(q, &grown_length, NULL, NULL);
        visits[2] = garb_media_visits;
        if (!matched || n != (size_t)k || strcmp(out, q) != 0 || grown == NULL ||
            grown_length != n || strcmp(grown, out) != 0) {
            printf("media depth %d: matched %d, [%s] %zu, grown [%s]\n", d, matched, out, n, grown ? grown : "NULL");
            failed = 1;
        }
        os64_free(grown);
        for (int f = 0; f < 3; f++) {
            if (d >= 2 && visits[f] - last[f] != step[f]) {
                printf("media depth %d: %s took %zu steps, %zu more than at %d (the level before added %zu)\n", d,
                       f == 0 ? "a match" : f == 1 ? "a serialization" : "a grown serialization", visits[f],
                       visits[f] - last[f], d - 1, step[f]);
                failed = 1;
            }
            step[f] = visits[f] - last[f];
            last[f] = visits[f];
        }
    }
    if (step[0] == 0 || step[1] == 0 || step[2] == 0) {
        printf("media depth: a level cost no steps: the count is not counting\n");
        failed = 1;
    }
    printf("libgarb media depth: %s (a level costs %zu, %zu and %zu steps)\n",
           failed ? "FAILED" : "each bracket level adds the same steps", step[0], step[1], step[2]);
    return failed;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--media-depth") == 0)
        return media_depth();
    if (argc == 2 && strcmp(argv[1], "--modern") == 0)
        return modern_marks();
    if (argc == 2 && strcmp(argv[1], "--defined") == 0)
        return defined();
    if (argc == 2 && strcmp(argv[1], "--keyframes-scale") == 0)
        return keyframes_scale();
    if (argc == 2 && strcmp(argv[1], "--faces-cap") == 0)
        return faces_cap() | faces_past_cap() | faces_sweep();
    if (argc == 3 && strcmp(argv[1], "--cost") == 0)
        return cost((size_t)strtoul(argv[2], NULL, 10), false);
    if (argc == 2 && strcmp(argv[1], "--full") == 0)
        return cost(GARB_SHEET_MAX, true);
    if (argc == 3 && strcmp(argv[1], "--cascades") == 0)
        return cascades(atoi(argv[2]));
    if (argc == 2 && strcmp(argv[1], "--read-budget") == 0)
        return read_budget();
    if (argc == 2 && strcmp(argv[1], "--scratch") == 0)
        return scratch();
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
        else if (strcmp(argv[1], "media-text") == 0)
            media_text(text);
        else if (strcmp(argv[1], "cascade") == 0)
            cascade(text, len, pabsent ? NULL : protocol, CASCADE_WINNERS);
        else if (strcmp(argv[1], "skips") == 0)
            cascade(text, len, pabsent ? NULL : protocol, CASCADE_SKIPS);
        else if (strcmp(argv[1], "keyframes") == 0)
            cascade(text, len, pabsent ? NULL : protocol, CASCADE_KEYFRAMES);
        else if (strcmp(argv[1], "faces") == 0)
            cascade(text, len, pabsent ? NULL : protocol, CASCADE_FACES);
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
