#define _POSIX_C_SOURCE 200809L
#include "../userland/libhtml/internal.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Added to a tree record's kind: parse it as a host that runs scripts does. */
#define SCRIPTING 0x100u
static size_t allocations, fail_at, live;
void *os64_malloc(size_t n)
{
    allocations++;
    if (fail_at && allocations == fail_at)
        return NULL;
    void *p = malloc(n);
    if (p)
        live++;
    return p;
}
void *os64_calloc(size_t n, size_t s)
{
    if (s && n > SIZE_MAX / s)
        return NULL;
    void *p = os64_malloc(n * s);
    if (p)
        memset(p, 0, n * s);
    return p;
}
void os64_free(void *p)
{
    if (p) {
        live--;
        free(p);
    }
}
/* The library ends the program on a broken pin promise; nothing here makes one. */
int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    return (int64_t)fwrite(buf, 1, len, handle == 2 ? stderr : stdout);
}
void os64_exit(int32_t code)
{
    exit(code ? 3 : 0);
}
#ifdef HTML_TOKENIZER_ONLY
void h_tree(os64_html_parser_t *p, HToken *t)
{
    (void)p;
    (void)t;
}
#endif
static void json_string_n(const char *s, size_t n)
{
    putchar('"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            putchar('\\');
            putchar(c);
        } else if (c < 32)
            printf("\\u%04x", c);
        else
            putchar(c);
    }
    putchar('"');
}
static void json_string(const char *s)
{
    if (!s)
        fputs("null", stdout);
    else
        json_string_n(s, strlen(s));
}
static bool comma;
static void token_sink(os64_html_parser_t *p, const HToken *t, void *ctx)
{
    (void)p;
    (void)ctx;
    if (t->type == H_END_INPUT)
        return;
    if (comma)
        putchar(',');
    comma = true;
    if (t->type == H_CHAR) {
        fputs("[\"Codepoint\",", stdout);
        printf("%u]", t->ch);
        return;
    }
    const char *kind = t->type == H_START     ? "StartTag"
                       : t->type == H_END     ? "EndTag"
                       : t->type == H_COMMENT ? "Comment"
                                              : "DOCTYPE";
    putchar('[');
    json_string(kind);
    putchar(',');
    if (t->type == H_COMMENT) {
        json_string(t->data.s ? t->data.s : "");
        putchar(']');
        return;
    }
    json_string(t->type == H_DOCTYPE && !t->name.len ? NULL : (t->name.s ? t->name.s : ""));
    if (t->type == H_START) {
        fputs(",{", stdout);
        bool first = true;
        for (HAttr *a = t->attrs; a; a = a->next) {
            if (!first)
                putchar(',');
            first = false;
            json_string(a->name);
            putchar(':');
            json_string(a->value);
        }
        putchar('}');
        if (t->self_closing)
            fputs(",true", stdout);
    } else if (t->type == H_DOCTYPE) {
        putchar(',');
        json_string(t->has_public ? (t->public_id.s ? t->public_id.s : "") : NULL);
        putchar(',');
        json_string(t->has_system ? (t->system_id.s ? t->system_id.s : "") : NULL);
        printf(",%s", t->force_quirks ? "false" : "true");
    }
    putchar(']');
}
static const char *attr_prefix(const HAttr *a)
{
    if (!a->ns)
        return "";
    return strstr(a->ns, "xlink") ? "xlink " : strstr(a->ns, "xmlns") ? "xmlns " : "xml ";
}
static const char *attr_local(const HAttr *a)
{
    const char *colon = a->ns ? strchr(a->name, ':') : NULL;
    return colon ? colon + 1 : a->name;
}
static int attr_compare(const void *av, const void *bv)
{
    const HAttr *a = *(const HAttr *const *)av, *b = *(const HAttr *const *)bv;
    const char *ap = attr_prefix(a), *bp = attr_prefix(b), *an = attr_local(a), *bn = attr_local(b);
    size_t al = strlen(ap), bl = strlen(bp);
    for (size_t i = 0;; i++) {
        unsigned char ac = i < al ? ap[i] : an[i - al], bc = i < bl ? bp[i] : bn[i - bl];
        if (ac != bc)
            return (int)ac - (int)bc;
        if (!ac)
            return 0;
    }
}
static void dump_nodes(FILE *out, HNode *node, unsigned depth, size_t *visited, size_t limit)
{
    for (HNode *n = node; n; n = n->next) {
        if (++*visited > limit || depth > 10000) {
            fputs("invalid tree topology\n", stderr);
            exit(3);
        }
        fprintf(out, "| %*s", (int)(depth * 2), "");
        if (n->kind == OS64_HTML_ELEMENT) {
            fprintf(out, "<%s%s>\n",
                    n->ns == OS64_HTML_NS_SVG      ? "svg "
                    : n->ns == OS64_HTML_NS_MATHML ? "math "
                                                   : "",
                    n->name);
            size_t count = 0;
            for (HAttr *a = n->attrs; a; a = a->next)
                count++;
            HAttr **attrs = malloc((count ? count : 1) * sizeof(*attrs));
            if (!attrs)
                exit(2);
            size_t i = 0;
            for (HAttr *a = n->attrs; a; a = a->next)
                attrs[i++] = a;
            qsort(attrs, count, sizeof(*attrs), attr_compare);
            for (i = 0; i < count; i++) {
                HAttr *a = attrs[i];
                const char *name = a->name, *prefix = "";
                if (a->ns) {
                    prefix = strstr(a->ns, "xlink")   ? "xlink "
                             : strstr(a->ns, "xmlns") ? "xmlns "
                                                      : "xml ";
                    const char *colon = strchr(name, ':');
                    if (colon)
                        name = colon + 1;
                }
                fprintf(out, "| %*s%s%s=\"%s\"\n", (int)((depth + 1) * 2), "", prefix, name,
                        a->value);
            }
            free(attrs);
            if (n->template_contents) {
                fprintf(out, "| %*scontent\n", (int)((depth + 1) * 2), "");
                dump_nodes(out, n->template_contents->first_child, depth + 2, visited, limit);
            }
            dump_nodes(out, n->first_child, depth + 1, visited, limit);
        } else if (n->kind == OS64_HTML_TEXT) {
            fputc('"', out);
            fwrite(n->text, 1, n->text_len, out);
            fputs("\"\n", out);
        } else if (n->kind == OS64_HTML_COMMENT) {
            fprintf(out, "<!-- %s -->\n", n->text ? n->text : "");
        } else if (n->kind == OS64_HTML_DOCTYPE) {
            fprintf(out, "<!DOCTYPE %s", n->name);
            if ((n->public_id && *n->public_id) || (n->system_id && *n->system_id))
                fprintf(out, " \"%s\" \"%s\"", n->public_id ? n->public_id : "",
                        n->system_id ? n->system_id : "");
            fputs(">\n", out);
        }
    }
}
/* Which random walk is running, for a failure to name (random_walks). */
static long walk_now = -1;
static void safety_fail(const char *reason)
{
    fprintf(stderr, "html safety: %s (allocation %zu, fail_at %zu)\n", reason, allocations,
            fail_at);
    if (walk_now >= 0)
        fprintf(stderr, "in random walk %ld: --checks 1 %ld runs it alone\n", walk_now, walk_now);
    exit(3);
}
static uint64_t hash_bytes(uint64_t h, const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ull;
    }
    return h;
}
static uint64_t hash_string(uint64_t h, const char *s)
{
    unsigned char present = s != NULL;
    h = hash_bytes(h, (const char *)&present, 1);
    if (s)
        h = hash_bytes(h, s, strlen(s) + 1);
    return h;
}
static void valid_utf8(const char *string)
{
    if (!string)
        return;
    const unsigned char *s = (const unsigned char *)string;
    while (*s) {
        unsigned char lead = *s++;
        if (lead < 0x80)
            continue;
        unsigned need = lead >= 0xc2 && lead <= 0xdf   ? 1
                        : lead >= 0xe0 && lead <= 0xef ? 2
                        : lead >= 0xf0 && lead <= 0xf4 ? 3
                                                       : 0;
        if (!need)
            safety_fail("invalid UTF-8 lead");
        for (unsigned i = 0; i < need; i++) {
            unsigned char b = *s++;
            unsigned lo = i == 0 && lead == 0xe0 ? 0xa0 : i == 0 && lead == 0xf0 ? 0x90 : 0x80;
            unsigned hi = i == 0 && lead == 0xed ? 0x9f : i == 0 && lead == 0xf4 ? 0x8f : 0xbf;
            if (b < lo || b > hi)
                safety_fail("invalid UTF-8 continuation");
        }
    }
}
static uint64_t validate(os64_html_document_t *doc)
{
    if (!doc || !doc->document || !doc->html || doc->html->parent != doc->document)
        safety_fail("missing document skeleton");
    HNode **stack = malloc((doc->node_count + 1) * sizeof(*stack));
    if (!stack)
        exit(2);
    size_t top = 0, visited = 0, records = 0;
    const HDoc *d = (const HDoc *)doc;
    uint64_t hash = 1469598103934665603ull;
    hash = hash_bytes(hash, (const char *)&doc->quirks, sizeof(doc->quirks));
    hash = hash_string(hash, doc->charset);
    hash = hash_string(hash, doc->charset_unsupported);
    hash = hash_string(hash, doc->charset_late_meta);
    hash = hash_bytes(hash, (const char *)&doc->parse_errors, sizeof(doc->parse_errors));
    for (size_t i = 0; i < doc->parse_errors && i < 16; i++) {
        hash = hash_string(hash, doc->first_errors[i].name);
        hash = hash_bytes(hash, (const char *)&doc->first_errors[i].byte_offset,
                          sizeof(doc->first_errors[i].byte_offset));
    }
    stack[top++] = doc->document;
    while (top) {
        HNode *n = stack[--top];
        if (++visited > doc->node_count)
            safety_fail("cycle or multiply linked node");
        hash = hash_bytes(hash, (const char *)&n->kind, sizeof(n->kind));
        hash = hash_bytes(hash, (const char *)&n->ns, sizeof(n->ns));
        valid_utf8(n->name);
        valid_utf8(n->text);
        valid_utf8(n->public_id);
        valid_utf8(n->system_id);
        hash = hash_string(hash, n->name);
        hash = hash_string(hash, n->text);
        hash = hash_string(hash, n->public_id);
        hash = hash_string(hash, n->system_id);
        if ((n->kind == OS64_HTML_TEXT || n->kind == OS64_HTML_COMMENT) &&
            (!n->text || strlen(n->text) != n->text_len))
            safety_fail("invalid text string");
        size_t attrs = 0;
        for (HAttr *a = n->attrs; a; a = a->next) {
            if (++attrs > doc->arena_bytes / sizeof(*a))
                safety_fail("attribute cycle");
            if (!a->name || !a->value)
                safety_fail("incomplete attribute");
            valid_utf8(a->name);
            valid_utf8(a->value);
            hash = hash_string(hash, a->name);
            hash = hash_string(hash, a->value);
            hash = hash_string(hash, a->ns);
        }
        HNode *next = NULL;
        size_t children = 0;
        for (HNode *child = n->last_child; child; child = child->prev) {
            if (++children > doc->node_count || top >= doc->node_count)
                safety_fail("child cycle");
            if (child->parent != n || child->next != next)
                safety_fail("broken parent/sibling link");
            stack[top++] = child;
            next = child;
        }
        if (next != n->first_child)
            safety_fail("first/last child disagree");
        /* Preorder payload alone cannot distinguish siblings from nesting. */
        hash = hash_bytes(hash, (const char *)&children, sizeof(children));
        unsigned char associated = n->form_owner != NULL;
        hash = hash_bytes(hash, (const char *)&associated, 1);
        records += associated;
        /* What the verbs hold a tree to, the parser must already deliver
         * (internal.h, h_depth_limit). */
        size_t depth = 0, limit = h_depth_limit(d);
        for (const HNode *a = n; depth <= limit;) {
            if (a->parent) {
                a = a->parent;
                depth++;
            } else if (a->kind == OS64_HTML_FRAGMENT && *h_word(a))
                a = (const HNode *)*h_word(a);
            else
                break;
        }
        if (depth > limit)
            safety_fail("node deeper than the parse's limit allows");
        if (n->form_owner &&
            (n->kind != OS64_HTML_ELEMENT || n->ns != OS64_HTML_NS_HTML ||
             n->form_owner->kind != OS64_HTML_ELEMENT ||
             n->form_owner->ns != OS64_HTML_NS_HTML ||
             n->form_owner->tag != OS64_HTML_TAG_FORM))
            safety_fail("invalid parser form owner");
        /* Reached from the document, so in its tree; a template's contents
         * hold no control with an owner. Its form is in that tree too,
         * however the parse ended. */
        if (n->form_owner) {
            const HNode *top = n->form_owner;
            while (top->parent)
                top = top->parent;
            if (top != doc->document)
                safety_fail("a control in the document whose form is not");
        }
        unsigned char fragment = n->template_contents != NULL;
        hash = hash_bytes(hash, (const char *)&fragment, 1);
        if (n->template_contents) {
            if (n->first_child || n->template_contents->parent ||
                n->template_contents->kind != OS64_HTML_FRAGMENT)
                safety_fail("invalid template fragment");
            if (top >= doc->node_count)
                safety_fail("fragment cycle");
            stack[top++] = n->template_contents;
        }
    }
    free(stack);
    if (records != d->records)
        safety_fail("form-owner record count disagrees with the tree");
    if (!d->version || d->retired || d->retired_bytes)
        safety_fail("a parse left mutation state behind");
    return hash;
}
static uint32_t random_next(uint32_t *seed)
{
    *seed ^= *seed << 13;
    *seed ^= *seed >> 17;
    *seed ^= *seed << 5;
    return *seed;
}
/* ── A host that runs scripts ────────────────────────────────────────────
 *
 * Each stop is taken, checked, noted and resumed. `stops` and `stops_hash`
 * are what a parse stopped at, in order, so that two ways of driving one
 * document can be asked whether they stopped at the same scripts. `at_stop`
 * is what a test does there, in the script's place. */
static size_t stops;
static uint64_t stops_hash;
static void (*at_stop)(os64_html_parser_t *);
static void stops_reset(void)
{
    stops = 0;
    stops_hash = 1469598103934665603ull;
}
static int64_t run_scripts(os64_html_parser_t *p, int64_t status)
{
    while (status == OS64_HTML_SCRIPT) {
        HNode *script = os64_html_parser_script(p);
        os64_html_document_t *doc = os64_html_parser_document(p);
        if (!script || script->kind != OS64_HTML_ELEMENT || strcmp(script->name, "script") ||
            script->ns == OS64_HTML_NS_MATHML)
            safety_fail("a stop that names no script");
        if (doc->refusal)
            safety_fail("a stop in a refused parse");
        stops++;
        stops_hash = hash_bytes(stops_hash, (const char *)&script->ns, sizeof(script->ns));
        for (HNode *c = script->first_child; c; c = c->next)
            stops_hash = hash_string(stops_hash, c->text);
        if (at_stop)
            at_stop(p);
        else
            validate(doc);
        status = os64_html_parser_resume(p);
    }
    if (os64_html_parser_script(p))
        safety_fail("a script named by a parse that is not stopped");
    return status;
}
/* `chunk`: 0 feeds the document whole, SIZE_MAX in random pieces, anything
 * else in pieces of that size. PILE feeds random pieces and resumes nothing
 * until the input has ended, so every stop but the first is met in the hold. */
#define PILE (SIZE_MAX - 1)
static os64_html_document_t *parse_raw(const unsigned char *data, size_t len, size_t chunk,
                                       os64_html_options_t *options)
{
    os64_html_parser_t *p = os64_html_parser_new(options);
    stops_reset();
    if (!p)
        return NULL;
    uint32_t seed = 0x64a11u;
    int64_t status = OS64_HTML_OK;
    for (size_t at = 0; at < len;) {
        size_t amount = chunk >= PILE ? 1 + random_next(&seed) % 37 : chunk ? chunk : len;
        if (amount > len - at)
            amount = len - at;
        status = os64_html_parser_feed(p, data + at, amount);
        if (chunk != PILE)
            status = run_scripts(p, status);
        at += amount;
        if (status < 0) {
            if (os64_html_parser_feed(p, data + at, len - at) != status)
                safety_fail("refusal resumed");
            break;
        }
    }
    /* A host that runs scripts says the input has ended before it finishes,
     * so that a script still waiting gets its stop. */
    if (options->scripting && status >= 0)
        run_scripts(p, os64_html_parser_end(p));
    return os64_html_parser_finish(p);
}
static size_t safety_prefixes, safety_failures, safety_cases, safety_stops;
static void safety_case(const unsigned char *data, size_t len, bool scripting)
{
    /* Byte at a time, random pieces, and for a host that runs scripts the
     * pile, which is the way of driving that fills the hold. */
    static const size_t chunkings[] = {1, SIZE_MAX, PILE};
    size_t ways = scripting ? 3 : 2;
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = "utf-8";
    opt.scripting = scripting;
    fail_at = 0;
    os64_html_document_t *doc = parse_raw(data, len, 0, &opt);
    if (!doc || doc->refusal)
        safety_fail("ordinary reference refused");
    uint64_t hash = validate(doc);
    uint64_t work = doc->work;
    size_t stopped = stops;
    uint64_t stopped_hash = stops_hash;
    safety_stops += stops;
    os64_html_document_free(doc);
    if (live)
        safety_fail("baseline leak");
    /* How many allocations each way of driving makes: the hold is one the
     * others do not have. */
    size_t count[3] = {0};
    for (size_t c = 0; c < ways; c++) {
        allocations = 0;
        doc = parse_raw(data, len, chunkings[c], &opt);
        count[c] = allocations;
        if (!doc || doc->refusal || validate(doc) != hash || doc->work != work)
            safety_fail("feed chunk changed document/work");
        if (stops != stopped || stops_hash != stopped_hash)
            safety_fail("feed chunk changed where the parse stopped");
        os64_html_document_free(doc);
        if (live)
            safety_fail("chunk leak");
    }
    for (size_t c = 0; c < ways; c += 2) {
        for (size_t n = 1; n <= count[c]; n++) {
            allocations = 0;
            fail_at = n;
            doc = parse_raw(data, len, chunkings[c], &opt);
            if (doc) {
                validate(doc);
                if (doc->refusal != OS64_HTML_NO_MEMORY)
                    safety_fail("lost heap refusal");
                os64_html_document_free(doc);
            }
            if (live)
                safety_fail("allocation-failure leak");
            safety_failures++;
        }
    }
    fail_at = 0;
    for (size_t cut = 0; cut < len; cut++) {
        opt.max_bytes = cut;
        doc = parse_raw(data, cut, 0, &opt);
        if (!doc || doc->refusal)
            safety_fail("standalone prefix refused");
        hash = validate(doc);
        stopped = stops;
        stopped_hash = stops_hash;
        os64_html_document_free(doc);
        for (unsigned c = 0; c < 3 + (unsigned)scripting; c++) {
            doc = parse_raw(data, len, c == 0 ? 0 : chunkings[c - 1], &opt);
            if (!doc || doc->refusal != OS64_HTML_TOO_LARGE || !doc->truncated ||
                doc->input_bytes != cut || validate(doc) != hash || stops != stopped ||
                stops_hash != stopped_hash)
                safety_fail("byte-limit prefix mismatch");
            os64_html_document_free(doc);
            if (live)
                safety_fail("prefix leak");
            safety_prefixes++;
        }
    }
    safety_cases++;
}

static void bounded_case(const unsigned char *data, size_t len, os64_html_options_t *opt,
                         int64_t expected, const char *name)
{
    uint64_t hash = 0, work = 0, stopped_hash = 0;
    size_t stopped = 0;
    int64_t refusal = 0;
    for (unsigned c = 0; c < 4; c++) {
        struct timespec begin, finish;
        clock_gettime(CLOCK_MONOTONIC, &begin);
        os64_html_document_t *doc =
            parse_raw(data, len, c == 0 ? 0 : c == 1 ? 1 : c == 2 ? SIZE_MAX : PILE, opt);
        clock_gettime(CLOCK_MONOTONIC, &finish);
        if (!doc)
            safety_fail("bounded constructor failed");
        uint64_t current = validate(doc);
        if (doc->work > opt->max_work || doc->peak_arena_bytes > opt->max_arena_bytes)
            safety_fail("budget overrun");
        if (c && (current != hash || doc->work != work || doc->refusal != refusal ||
                  stops != stopped || stops_hash != stopped_hash))
            safety_fail("bounded feed changed result");
        if (!c) {
            hash = current;
            work = doc->work;
            refusal = doc->refusal;
            stopped = stops;
            stopped_hash = stops_hash;
            if (expected != INT64_MAX && refusal != expected)
                safety_fail(name ? name : "unexpected bounded refusal");
            if (name)
                printf(
                    "Adversarial %s: bytes=%zu work=%llu peak_arena=%zu refusal=%s seconds=%.4f\n",
                    name, len, (unsigned long long)work, doc->peak_arena_bytes,
                    os64_html_status_name(refusal),
                    finish.tv_sec - begin.tv_sec + (finish.tv_nsec - begin.tv_nsec) / 1e9);
        }
        os64_html_document_free(doc);
        if (live)
            safety_fail("bounded leak");
    }
}
/* ── The encoding table's two readers ────────────────────────────────────
 *
 * index single-byte windows-1252, transcribed from the Encoding Standard. A
 * test that borrowed the table under test could not catch a wrong table, so
 * these 32 pointers are written out here and BOTH directions are asked to
 * agree with them: the encoder directly, the decoder through a document. */
static const struct {
    unsigned char byte;
    uint32_t cp;
} windows_1252_index[] = {
    {0x80, 0x20ac}, {0x81, 0x0081}, {0x82, 0x201a}, {0x83, 0x0192}, {0x84, 0x201e}, {0x85, 0x2026},
    {0x86, 0x2020}, {0x87, 0x2021}, {0x88, 0x02c6}, {0x89, 0x2030}, {0x8a, 0x0160}, {0x8b, 0x2039},
    {0x8c, 0x0152}, {0x8d, 0x008d}, {0x8e, 0x017d}, {0x8f, 0x008f}, {0x90, 0x0090}, {0x91, 0x2018},
    {0x92, 0x2019}, {0x93, 0x201c}, {0x94, 0x201d}, {0x95, 0x2022}, {0x96, 0x2013}, {0x97, 0x2014},
    {0x98, 0x02dc}, {0x99, 0x2122}, {0x9a, 0x0161}, {0x9b, 0x203a}, {0x9c, 0x0153}, {0x9d, 0x009d},
    {0x9e, 0x017e}, {0x9f, 0x0178}};
static size_t checks_run, checks_failed;
/* Set while a case is run again with an allocation failed: what the case
 * asserts of a parse that had its memory is then not asked. */
static bool quiet;
static void check(bool ok, const char *fmt, ...)
{
    if (quiet)
        return;
    checks_run++;
    if (ok)
        return;
    checks_failed++;
    va_list ap;
    va_start(ap, fmt);
    fputs("FAIL ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
static size_t utf8_put(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    out[0] = (char)(0xe0 | cp >> 12);
    out[1] = (char)(0x80 | (cp >> 6 & 0x3f));
    out[2] = (char)(0x80 | (cp & 0x3f));
    return 3;
}
static void collect_text(const HNode *n, char *out, size_t cap, size_t *len)
{
    for (; n; n = n->next) {
        if (n->kind == OS64_HTML_TEXT && n->text) {
            size_t take = n->text_len < cap - *len ? n->text_len : cap - *len;
            memcpy(out + *len, n->text, take);
            *len += take;
        }
        collect_text(n->first_child, out, cap, len);
    }
}
static bool index_holds(uint32_t cp)
{
    for (size_t i = 0; i < H_ARRAY(windows_1252_index); i++)
        if (windows_1252_index[i].cp == cp)
            return true;
    return false;
}
static void encoding_exports(void)
{
    static const struct {
        const char *label, *want;
    } labels[] = {{"utf-8", "utf-8"},
                  {"UTF-8", "utf-8"},
                  {"utf8", "utf-8"},
                  {"unicode-1-1-utf-8", "utf-8"},
                  {"unicode11utf8", "utf-8"},
                  {"unicode20utf8", "utf-8"},
                  {"x-unicode20utf8", "utf-8"},
                  {"ansi_x3.4-1968", "windows-1252"},
                  {"ascii", "windows-1252"},
                  {"cp1252", "windows-1252"},
                  {"cp819", "windows-1252"},
                  {"csisolatin1", "windows-1252"},
                  {"ibm819", "windows-1252"},
                  {"iso-8859-1", "windows-1252"},
                  {"iso-ir-100", "windows-1252"},
                  {"iso8859-1", "windows-1252"},
                  {"iso88591", "windows-1252"},
                  {"iso_8859-1", "windows-1252"},
                  {"iso_8859-1:1987", "windows-1252"},
                  {"l1", "windows-1252"},
                  {"latin1", "windows-1252"},
                  {"LATIN1", "windows-1252"},
                  {"us-ascii", "windows-1252"},
                  {"windows-1252", "windows-1252"},
                  {"x-cp1252", "windows-1252"},
                  {"utf-16", "utf-16le"},
                  {"utf-16le", "utf-16le"},
                  {"UTF-16BE", "utf-16be"},
                  /* ASCII whitespace at either end is not part of a label. */
                  {" \t\r\n\futf-8 \t\r\n\f", "utf-8"},
                  /* Nothing else names an encoding this parser decodes. */
                  {"", NULL},
                  {"shift_jis", NULL},
                  {"euc-jp", NULL},
                  {"utf-32", NULL},
                  {"latin", NULL},
                  {"latin12", NULL},
                  {"iso-8859-2", NULL},
                  {"utf-8 or else", NULL}};
    for (size_t i = 0; i < H_ARRAY(labels); i++) {
        const char *got = os64_html_encoding_for_label(labels[i].label, strlen(labels[i].label));
        check(labels[i].want ? got && strcmp(got, labels[i].want) == 0 : got == NULL,
              "label |%s| named %s, want %s", labels[i].label, got ? got : "(nothing)",
              labels[i].want ? labels[i].want : "(nothing)");
    }
    /* A label is a span of bytes and not a C string: the caller splitting an
     * accept-charset list hands over one name out of the middle of one. */
    const char *spanned = os64_html_encoding_for_label("utf-8 or else", 5);
    check(spanned && strcmp(spanned, "utf-8") == 0, "label by length named %s",
          spanned ? spanned : "(nothing)");
    check(os64_html_encoding_for_label(NULL, 0) == NULL, "no label at all");

    /* The encoder, over the transcribed index. */
    for (size_t i = 0; i < H_ARRAY(windows_1252_index); i++) {
        uint8_t b = 0;
        bool got = os64_html_encode_windows_1252(windows_1252_index[i].cp, &b);
        check(got && b == windows_1252_index[i].byte, "encode U+%04X gave %s0x%02X, want 0x%02X",
              windows_1252_index[i].cp, got ? "" : "nothing, ", b, windows_1252_index[i].byte);
    }
    for (uint32_t cp = 0; cp < 0x80; cp++) {
        uint8_t b = 0;
        check(os64_html_encode_windows_1252(cp, &b) && b == cp, "encode ASCII U+%04X", cp);
    }
    for (uint32_t cp = 0xa0; cp <= 0xff; cp++) {
        uint8_t b = 0;
        check(os64_html_encode_windows_1252(cp, &b) && b == cp, "encode Latin-1 U+%04X", cp);
    }
    /* The C1 block is where the index is SPARSE, and a code point it has no
     * pointer for has no byte — the five it does hold are the ones the
     * decoder maps to themselves. */
    for (uint32_t cp = 0x80; cp <= 0x9f; cp++) {
        uint8_t b = 0;
        bool got = os64_html_encode_windows_1252(cp, &b);
        check(got == index_holds(cp), "encode C1 U+%04X %s", cp, got ? "gave a byte" : "refused");
    }
    static const uint32_t absent[] = {0x100, 0x2028, 0x20a0, 0x1f600, 0x10ffff};
    for (size_t i = 0; i < H_ARRAY(absent); i++) {
        uint8_t b = 0;
        check(!os64_html_encode_windows_1252(absent[i], &b), "encode U+%04X refused", absent[i]);
    }

    /* And the decoder, asked the same question the other way round: the 32
     * high bytes through a windows-1252 document come out as those 32 code
     * points, so the two readers of one table cannot drift apart. */
    unsigned char markup[3 + H_ARRAY(windows_1252_index)];
    memcpy(markup, "<p>", 3);
    for (size_t i = 0; i < H_ARRAY(windows_1252_index); i++)
        markup[3 + i] = windows_1252_index[i].byte;
    char want[4 * H_ARRAY(windows_1252_index)];
    size_t want_len = 0;
    for (size_t i = 0; i < H_ARRAY(windows_1252_index); i++)
        want_len += utf8_put(windows_1252_index[i].cp, want + want_len);
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = "windows-1252";
    allocations = 0;
    fail_at = 0;
    os64_html_document_t *doc = parse_raw(markup, sizeof(markup), 0, &opt);
    if (!doc || doc->refusal) {
        check(false, "windows-1252 document refused");
    } else {
        char got[sizeof(want)];
        size_t got_len = 0;
        collect_text(doc->document, got, sizeof(got), &got_len);
        check(got_len == want_len && memcmp(got, want, want_len) == 0,
              "windows-1252 text decoded to %zu bytes, want %zu", got_len, want_len);
        check(doc->charset && strcmp(doc->charset, "windows-1252") == 0,
              "document charset %s", doc->charset ? doc->charset : "(none)");
    }
    os64_html_document_free(doc);
}
/* These small ownership fixtures include template fragments in their walk. */
static HNode *node_with_id(HNode *n, const char *id)
{
    for (; n; n = n->next) {
        const HAttr *a = os64_html_attr(n, "id");
        if (a && strcmp(a->value, id) == 0)
            return n;
        HNode *found = node_with_id(n->first_child, id);
        if (!found && n->template_contents)
            found = node_with_id(n->template_contents->first_child, id);
        if (found)
            return found;
    }
    return NULL;
}
static void ownership_case(const char *markup, bool associated, bool empty_form)
{
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = "utf-8";
    for (unsigned chunk = 0; chunk < 3; chunk++) {
        os64_html_document_t *doc = parse_raw((const unsigned char *)markup, strlen(markup),
                                             chunk == 0 ? 0 : chunk == 1 ? 1 : SIZE_MAX, &opt);
        check(doc && !doc->refusal, "owner fixture parsed, chunk %u: %s", chunk, markup);
        if (doc && !doc->refusal) {
            HNode *control = node_with_id(doc->document, "q");
            HNode *form = node_with_id(doc->document, "f");
            check(control && (associated ? form && control->form_owner == form
                                         : control->form_owner == NULL),
                  "parser association, chunk %u: %s", chunk, markup);
            if (empty_form)
                check(form && !form->first_child, "table form stays empty: %s", markup);
            validate(doc);
        }
        os64_html_document_free(doc);
        check(live == 0, "ownership fixture freed");
    }
    safety_case((const unsigned char *)markup, strlen(markup), false);
    safety_case((const unsigned char *)markup, strlen(markup), true);
}
static void form_owners(void)
{
    ownership_case("<table><form id=f action=/s><tr><td><input id=q name=q>"
                   "</td></tr></form></table>", true, true);
    ownership_case("<form id=f><input id=q>", true, false);
    ownership_case("<form id=f></form><input id=q form=f>", false, false);
    ownership_case("<input id=q>", false, false);
    ownership_case("<form id=f></form><input id=q>", false, false);
    ownership_case("<form id=f><form id=ignored><input id=q>", true, false);
    ownership_case("<form id=f><div id=q></div>", false, false);
    ownership_case("<form id=f><template><input id=q></template>", false, false);
    ownership_case("<template><form id=f><input id=q></form></template>", false, false);
    ownership_case("<form id=f><template><template></template></template><input id=q>",
                   true, false);
    ownership_case("<form id=f><svg><output id=q /></svg>", false, false);
    ownership_case("<form id=f><math><output id=q /></math>", false, false);
    ownership_case("<form id=f><svg><foreignObject><input id=q></foreignObject></svg>",
                   true, false);
    ownership_case("<table><form id=f><input id=q></table>", true, true);
    ownership_case("<table><form id=f><input type=hidden id=q></table>", true, true);
    /* The adoption algorithm changes ancestry without changing this insertion record. */
    ownership_case("<b><form id=f></b><input id=q>", true, false);
    /* The control is the seventeenth open element, so the push that follows
     * its association has to grow the stack: the allocation sweep fails that
     * growth, and the control that never reached the tree has no record. */
    ownership_case("<form id=f><div><div><div><div><div><div><div><div><div><div><div><div><div>"
                   "<button id=q>", true, false);
    static const char tags[][9] = {
        "button", "fieldset", "input", "object", "output", "select", "textarea", "img"};
    for (size_t i = 0; i < H_ARRAY(tags); i++) {
        char markup[160];
        snprintf(markup, sizeof(markup), "<form id=f><%s id=q>", tags[i]);
        ownership_case(markup, true, false);
        snprintf(markup, sizeof(markup), "<form id=f><%s id=q form=f>", tags[i]);
        ownership_case(markup, strcmp(tags[i], "img") == 0, false);
        snprintf(markup, sizeof(markup), "<form id=f><%s id=q form=''>", tags[i]);
        ownership_case(markup, strcmp(tags[i], "img") == 0, false);
    }
    /* A detached form exercises the same-tree gate independently of templates.
     * The public parser cannot run scripts that detach nodes during a feed. */
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = "utf-8";
    os64_html_parser_t *p = os64_html_parser_new(&opt);
    if (!p) {
        check(false, "detached-form parser allocation");
        return;
    }
    const char *prefix = "<form id=f></form>";
    os64_html_parser_feed(p, prefix, strlen(prefix));
    h_encoding_start(p);
    HNode *form = node_with_id(p->d->pub.document, "f");
    check(form != NULL, "detached form fixture created");
    if (form) {
        h_detach(form);
        p->form = form;
        const char *input = "<input id=q>";
        os64_html_parser_feed(p, input, strlen(input));
    }
    os64_html_document_t *doc = os64_html_parser_finish(p);
    HNode *control = node_with_id(doc->document, "q");
    check(!doc->refusal && control && !control->form_owner, "different trees exclude owner");
    os64_html_document_free(doc);
    check(live == 0, "detached form freed with document");

    /* Interrupt at each charged step, including the association's stack,
     * attribute and root walks; refusal must leave a freeable partial tree.
     * The second is the adoption agency with controls in the block it moves:
     * stopped at any step, no control is left tied to a form in another
     * tree, or counted and out of the document. */
    static const char *const swept[] = {
        "<form id=f><div><span><input id=q a=1 b=2 c=3>",
        "<form id=f><b><i><u><div><input id=q><input id=r>x<input id=s></b>y"};
    for (size_t i = 0; i < H_ARRAY(swept); i++) {
        const char *markup = swept[i];
        opt.max_work = os64_html_options_default().max_work;
        doc = parse_raw((const unsigned char *)markup, strlen(markup), 0, &opt);
        check(doc && !doc->refusal, "work sweep baseline");
        uint64_t work = doc ? doc->work : 0;
        os64_html_document_free(doc);
        for (uint64_t budget = 0; budget <= work; budget++) {
            opt.max_work = budget;
            bounded_case((const unsigned char *)markup, strlen(markup), &opt,
                         budget < work ? OS64_HTML_WORK_EXHAUSTED : OS64_HTML_OK, NULL);
        }
    }
}
/* ── The parse that stops (html.h) ───────────────────────────────────── */
static os64_html_options_t scripted(void)
{
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = "utf-8";
    opt.scripting = true;
    return opt;
}
static char *tree_text(os64_html_document_t *doc)
{
    char *dump = NULL;
    size_t len = 0, visited = 0;
    FILE *out = open_memstream(&dump, &len);
    if (!out)
        exit(2);
    dump_nodes(out, doc->document->first_child, 0, &visited, doc->node_count);
    fclose(out);
    return dump;
}
static const char *script_source(const HNode *script)
{
    return script && script->first_child && script->first_child->text ? script->first_child->text
                                                                      : "";
}
static int64_t feed_text(os64_html_parser_t *p, const char *s)
{
    return os64_html_parser_feed(p, s, strlen(s));
}
static HNode *find_named(HNode *n, const char *name)
{
    for (; n; n = n->next) {
        if (n->kind == OS64_HTML_ELEMENT && strcmp(n->name, name) == 0)
            return n;
        HNode *found = find_named(n->first_child, name);
        if (!found && n->template_contents)
            found = find_named(n->template_contents->first_child, name);
        if (found)
            return found;
    }
    return NULL;
}
/* The sniff window: a parser reads its first 1024 bytes for an encoding
 * before it parses any of them. Spaces ahead of a document change nothing in
 * its tree, and put the cases below past the window. */
static const char *window(void)
{
    static char pad[1025];
    memset(pad, ' ', 1024);
    return pad;
}
/* A parser abandoned at each of its stops, and one abandoned part way through
 * its input, leaves a document that walks and frees. */
static size_t abandoned;
static void abandon_everywhere(const unsigned char *data, size_t len, os64_html_options_t *opt)
{
    os64_html_document_t *doc = parse_raw(data, len, 0, opt);
    size_t total = stops;
    os64_html_document_free(doc);
    for (size_t at = 0; at <= total + 1; at++) {
        for (unsigned way = 0; way < 2; way++) {
            /* `at` 1 to total: abandon at that stop. 0: before any byte.
             * total + 1: with half the input fed, wherever that leaves it. */
            os64_html_parser_t *p = os64_html_parser_new(opt);
            if (!p)
                safety_fail("abandon parser allocation");
            size_t seen = 0, fed = 0, limit = at > total ? len / 2 : len;
            size_t piece = way ? 4096 : limit;
            int64_t status = OS64_HTML_OK;
            bool ended = false;
            while (at && status >= 0 && seen < at) {
                if (status == OS64_HTML_SCRIPT) {
                    if (++seen < at)
                        status = os64_html_parser_resume(p);
                } else if (fed < limit) {
                    size_t amount = piece < limit - fed ? piece : limit - fed;
                    status = os64_html_parser_feed(p, data + fed, amount);
                    fed += amount;
                } else if (at <= total && !ended) {
                    /* A script inside the sniff window stops at the end. */
                    ended = true;
                    status = os64_html_parser_end(p);
                } else
                    break;
            }
            if (at && at <= total && seen != at)
                safety_fail("abandon never reached its stop");
            doc = os64_html_parser_abandon(p);
            validate(doc);
            os64_html_document_free(doc);
            if (live)
                safety_fail("abandon leak");
            abandoned++;
        }
    }
}
/* `</p>` and `</br>` met in foreign content are handed to the HTML rules. At
 * an integration point there is nothing foreign left to pop first, and the
 * token must still be handed over: these once went round until the work
 * budget ran out. The trees are Chrome's. */
static void integration_end_tags(void)
{
    static const struct {
        const char *markup, *tree;
    } cases[] = {
        {"<svg><foreignObject></p>x",
         "| <html>\n|   <head>\n|   <body>\n|     <svg svg>\n|       <svg foreignObject>\n"
         "|         <p>\n|         \"x\"\n"},
        {"<svg><desc></br>x",
         "| <html>\n|   <head>\n|   <body>\n|     <svg svg>\n|       <svg desc>\n"
         "|         <br>\n|         \"x\"\n"},
        {"<svg><title></p>y",
         "| <html>\n|   <head>\n|   <body>\n|     <svg svg>\n|       <svg title>\n"
         "|         <p>\n|         \"y\"\n"},
        {"<math><mi></p>x",
         "| <html>\n|   <head>\n|   <body>\n|     <math math>\n|       <math mi>\n"
         "|         <p>\n|         \"x\"\n"},
    };
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = "utf-8";
    for (size_t i = 0; i < H_ARRAY(cases); i++) {
        const unsigned char *data = (const unsigned char *)cases[i].markup;
        os64_html_document_t *doc = parse_raw(data, strlen(cases[i].markup), 0, &opt);
        char *got = tree_text(doc);
        check(doc && !doc->refusal && doc->work < 1000 && strcmp(got, cases[i].tree) == 0,
              "an end tag at an integration point: %s", cases[i].markup);
        free(got);
        os64_html_document_free(doc);
        safety_case(data, strlen(cases[i].markup), false);
        safety_case(data, strlen(cases[i].markup), true);
    }
}
/* A byte order mark chooses the encoding and is no part of the text: the
 * window is replayed from the byte after it. */
static void byte_order_marks(void)
{
    static const struct {
        const char *bytes;
        size_t len;
        const char *charset;
    } marks[] = {{"\xef\xbb\xbf<p>x", 7, "utf-8"},
                 {"\xff\xfe<\0p\0>\0x\0", 10, "utf-16le"},
                 {"\xfe\xff\0<\0p\0>\0x", 10, "utf-16be"}};
    os64_html_options_t opt = os64_html_options_default();
    for (size_t i = 0; i < H_ARRAY(marks); i++) {
        for (unsigned way = 0; way < 2; way++) {
            os64_html_document_t *doc =
                parse_raw((const unsigned char *)marks[i].bytes, marks[i].len, way, &opt);
            check(doc && !doc->refusal && doc->charset && strcmp(doc->charset, marks[i].charset) == 0 &&
                      doc->body && doc->body->first_child == doc->body->last_child &&
                      doc->body->first_child->kind == OS64_HTML_ELEMENT &&
                      doc->body->first_child->first_child &&
                      strcmp(doc->body->first_child->first_child->text, "x") == 0,
                  "a %s byte order mark is not text", marks[i].charset);
            os64_html_document_free(doc);
        }
    }
}
static void stop_checks(void)
{
    os64_html_options_t on = scripted(), off = scripted();
    off.scripting = false;
    const char *page =
        "<p>a<script>one</script>b<noscript><i>n</i></noscript><script>two</script>c";
    char whole[2048];

    /* Scripting off: a script is text like any other and nothing stops. */
    os64_html_parser_t *p = os64_html_parser_new(&off);
    if (!p)
        safety_fail("stop fixture allocation");
    check(feed_text(p, window()) == OS64_HTML_OK && feed_text(p, page) == OS64_HTML_OK &&
              !os64_html_parser_script(p) && os64_html_parser_end(p) == OS64_HTML_OK,
          "scripting off: nothing stops");
    os64_html_document_t *doc = os64_html_parser_finish(p);
    check(find_named(doc->document, "i") != NULL, "scripting off: noscript holds elements");
    os64_html_document_free(doc);

    /* Scripting on, call by call. */
    p = os64_html_parser_new(&on);
    if (!p)
        safety_fail("stop fixture allocation");
    doc = os64_html_parser_document(p);
    check(doc && doc->html && !doc->html->parent,
          "before the first tag the html element is not yet in the document");
    check(feed_text(p, window()) == OS64_HTML_OK, "the window fed");
    uint64_t version = os64_html_version(doc);
    int64_t status = feed_text(p, page);
    HNode *one = os64_html_parser_script(p);
    check(status == OS64_HTML_SCRIPT && one && strcmp(script_source(one), "one") == 0,
          "the first script stops the parse, whole");
    check(os64_html_version(doc) > version, "a call that parses moves the version");
    check(doc->html->parent == doc->document && doc->body && one->parent &&
              one->parent->parent == doc->body && !one->next && !doc->body->next,
          "at a stop the tree ends with the script");
    version = os64_html_version(doc);
    size_t bytes = doc->input_bytes;
    check(feed_text(p, "<p>later") == OS64_HTML_SCRIPT && os64_html_parser_script(p) == one &&
              os64_html_version(doc) == version && doc->input_bytes == bytes + 8 && !one->next,
          "a feed while stopped is kept and counted, and parses nothing");
    status = os64_html_parser_resume(p);
    HNode *two = os64_html_parser_script(p);
    check(status == OS64_HTML_SCRIPT && two && two != one &&
              strcmp(script_source(two), "two") == 0,
          "resume stops at the next script");
    check(!find_named(doc->document, "i"), "scripting on: noscript holds text");
    check(os64_html_parser_resume(p) == OS64_HTML_OK && !os64_html_parser_script(p) &&
              !p->hold,
          "the last resume parses what was kept, and gives the hold back");
    version = os64_html_version(doc);
    check(os64_html_parser_resume(p) == OS64_HTML_BAD_ARGUMENT && !doc->refusal &&
              os64_html_version(doc) == version,
          "a resume of a parse that is not stopped changes nothing");
    check(os64_html_parser_end(p) == OS64_HTML_OK &&
              feed_text(p, "x") == OS64_HTML_BAD_ARGUMENT && !doc->refusal,
          "no byte is fed after the end");
    os64_html_document_t *done = os64_html_parser_finish(p);
    check(done == doc, "finish hands over the document that was being read");
    char *got = tree_text(done);
    uint64_t work = done->work;
    os64_html_document_free(done);
    snprintf(whole, sizeof(whole), "%s%s<p>later", window(), page);
    doc = parse_raw((const unsigned char *)whole, strlen(whole), 0, &on);
    char *want = tree_text(doc);
    check(strcmp(got, want) == 0 && stops == 2 && doc->work == work,
          "driven by hand and whole: one tree, for the same work");
    free(got);
    os64_html_document_free(doc);

    /* finish does not stop: what is held is parsed straight through. */
    p = os64_html_parser_new(&on);
    if (!p)
        safety_fail("stop fixture allocation");
    feed_text(p, window());
    check(feed_text(p, page) == OS64_HTML_SCRIPT && feed_text(p, "<p>later") == OS64_HTML_SCRIPT,
          "stopped, with input held");
    doc = os64_html_parser_finish(p);
    got = tree_text(doc);
    check(!doc->refusal && strcmp(got, want) == 0 && doc->work == work,
          "finish while stopped parses the rest, for the same work");
    free(got);
    free(want);
    os64_html_document_free(doc);

    /* Where the parse stops and where it does not, however it is driven. */
    static const struct {
        const char *markup;
        size_t want;
    } places[] = {
        {"<script>a</script><script>b</script><script>c</script>", 3},
        {"<table><script>a</script></table>", 1},
        {"<select><script>a</script></select>", 1},
        {"<svg><script>a</script><script/>z</svg>", 2},
        /* The end tag closes an SVG script that is not the current node:
         * the standard pops to it and runs nothing. */
        {"<svg><script><g>a</script>z</svg>", 0},
        {"<script>a</script><template><script>b</script></template><script>c</script>", 2},
        {"<template><script>inert</script></template>y", 0},
        {"<template><svg><script>inert</script><script/></svg></template>", 0},
        {"<script>never closed", 0},
        {"<math><script>not a script</script></math>", 0},
        {"<textarea><script>text</script></textarea>", 0},
        {"<noscript><script>text</script></noscript>", 0},
    };
    for (size_t i = 0; i < H_ARRAY(places); i++) {
        for (unsigned padded = 0; padded < 2; padded++) {
            snprintf(whole, sizeof(whole), "%s%s", padded ? window() : "", places[i].markup);
            bounded_case((const unsigned char *)whole, strlen(whole), &on, OS64_HTML_OK, NULL);
            check(stops == places[i].want, "%zu stops, want %zu: %s", stops, places[i].want,
                  places[i].markup);
            bounded_case((const unsigned char *)whole, strlen(whole), &off, OS64_HTML_OK, NULL);
            check(stops == 0, "scripting off stopped: %s", places[i].markup);
            abandon_everywhere((const unsigned char *)whole, strlen(whole), &on);
        }
    }

    /* The end of the input is parsed too: a document of nothing but spaces
     * gets its elements when the host says it has ended. */
    p = os64_html_parser_new(&on);
    if (!p)
        safety_fail("stop fixture allocation");
    doc = os64_html_parser_document(p);
    check(feed_text(p, window()) == OS64_HTML_OK && !doc->body, "spaces build nothing");
    version = os64_html_version(doc);
    check(os64_html_parser_end(p) == OS64_HTML_OK && doc->body && os64_html_version(doc) > version,
          "the end-of-input steps move the version");
    os64_html_document_free(os64_html_parser_finish(p));

    /* A script inside the sniff window stops when the window is parsed: at
     * the end of a short document, or when the window fills. */
    p = os64_html_parser_new(&on);
    if (!p)
        safety_fail("stop fixture allocation");
    check(feed_text(p, "<script>s</script>") == OS64_HTML_OK && !os64_html_parser_script(p),
          "a short document is not parsed until it ends");
    check(os64_html_parser_end(p) == OS64_HTML_SCRIPT &&
              strcmp(script_source(os64_html_parser_script(p)), "s") == 0 &&
              os64_html_parser_resume(p) == OS64_HTML_OK,
          "end stops at the script, and resume reaches the end");
    doc = os64_html_parser_finish(p);
    check(!doc->refusal && find_named(doc->document, "body") != NULL,
          "the end-of-input steps ran once the script was passed");
    os64_html_document_free(doc);
    snprintf(whole, sizeof(whole), "<script>a</script>%s<script>b</script>", window());
    p = os64_html_parser_new(&on);
    if (!p)
        safety_fail("stop fixture allocation");
    check(feed_text(p, whole) == OS64_HTML_SCRIPT &&
              strcmp(script_source(os64_html_parser_script(p)), "a") == 0 &&
              os64_html_parser_resume(p) == OS64_HTML_SCRIPT &&
              strcmp(script_source(os64_html_parser_script(p)), "b") == 0 &&
              os64_html_parser_resume(p) == OS64_HTML_OK,
          "a stop inside the window, and one past it out of the hold");
    os64_html_parser_destroy(p);
    p = os64_html_parser_new(&on);
    if (!p)
        safety_fail("stop fixture allocation");
    check(feed_text(p, whole) == OS64_HTML_SCRIPT, "stopped with input held");
    os64_html_parser_destroy(p);
    check(live == 0, "destroy frees a parser that holds input");

    /* The byte limit met while stopped: the refusal waits for the parse. */
    os64_html_options_t small = on;
    small.max_bytes = 1024 + 30;
    p = os64_html_parser_new(&small);
    if (!p)
        safety_fail("stop fixture allocation");
    feed_text(p, window());
    doc = os64_html_parser_document(p);
    check(feed_text(p, "<script>1</script>abc<script>2</script>and a tail past the limit") ==
                  OS64_HTML_SCRIPT &&
              !doc->refusal && doc->input_bytes == small.max_bytes,
          "stopped at the limit: not yet refused");
    check(os64_html_parser_resume(p) == OS64_HTML_TOO_LARGE && doc->truncated &&
              !os64_html_parser_script(p),
          "the limit is the answer once the parse reaches it");
    doc = os64_html_parser_finish(p);
    os64_html_document_free(doc);

    /* Abandon: with nothing fed, in the window, and after a refusal. */
    p = os64_html_parser_new(&on);
    version = os64_html_version(os64_html_parser_document(p));
    doc = os64_html_parser_abandon(p);
    check(doc && doc->html && doc->html->parent == doc->document && !doc->html->first_child &&
              os64_html_version(doc) > version,
          "an abandoned parser's document is given its html element, and its version says so");
    validate(doc);
    os64_html_document_free(doc);
    small = on;
    small.max_depth = 2;
    p = os64_html_parser_new(&small);
    feed_text(p, window());
    check(feed_text(p, "<div><div><div>") == OS64_HTML_TOO_DEEP, "a refused parse");
    doc = os64_html_parser_abandon(p);
    check(doc->refusal == OS64_HTML_TOO_DEEP, "abandon keeps what the parse said");
    validate(doc);
    os64_html_document_free(doc);
    check(os64_html_parser_abandon(NULL) == NULL && os64_html_parser_document(NULL) == NULL &&
              os64_html_parser_script(NULL) == NULL,
          "no parser, no answer");
    check(live == 0, "stop fixtures freed");

    /* A snapshot taken at a stop keeps what it pointed at. The script takes
     * itself out, so the text after it joins the text before it. */
    p = os64_html_parser_new(&on);
    feed_text(p, window());
    doc = os64_html_parser_document(p);
    check(feed_text(p, "<p>before<script>s</script>") == OS64_HTML_SCRIPT, "stopped after text");
    HNode *script = os64_html_parser_script(p), *text = script->prev;
    os64_html_pin_t pin = os64_html_pin(doc);
    const char *held = text->text;
    check(pin && os64_html_remove(doc, script) == OS64_HTML_OK, "the script removes itself");
    check(os64_html_parser_resume(p) == OS64_HTML_OK && feed_text(p, " after") == OS64_HTML_OK,
          "text joins the node a snapshot can see");
    check(text->text != held && strcmp(held, "before") == 0 &&
              strcmp(text->text, "before after") == 0 && os64_html_retired_bytes(doc) > 0,
          "the pinned bytes stay; the longer text is written elsewhere");
    size_t retired = os64_html_retired_bytes(doc);
    check(feed_text(p, " and more text than the block had room for, by a margin") ==
                  OS64_HTML_OK &&
              os64_html_retired_bytes(doc) == retired && strcmp(held, "before") == 0 &&
              strncmp(text->text, "before after and more", 21) == 0,
          "one copy for one snapshot: the block the copy made is the parser's alone");
    os64_html_unpin(doc, pin);
    check(os64_html_retired_bytes(doc) == 0, "letting go reclaims");
    /* A pin older than a text node never saw its bytes. */
    check(feed_text(p, "<script>t</script>") == OS64_HTML_SCRIPT, "stopped again");
    pin = os64_html_pin(doc);
    check(os64_html_parser_resume(p) == OS64_HTML_OK &&
              feed_text(p, "<p>text made after the pin, long enough to outgrow its first block") ==
                  OS64_HTML_OK &&
              os64_html_retired_bytes(doc) == 0,
          "text newer than every pin grows in place and retires nothing");
    os64_html_unpin(doc, pin);
    doc = os64_html_parser_finish(p);
    validate(doc);
    os64_html_document_free(doc);
    check(live == 0, "pinned fixtures freed");
}

/* ── A tree changed under the parser ─────────────────────────────────────
 *
 * Every node a document ever had, found by following every link from every
 * node already known: a script can detach what the parser built, and the
 * parser goes on building under it, out of the document's sight. */
static struct {
    HNode **list, **slots;
    size_t n, cap, size;
} known;
static os64_html_document_t *known_doc;
static void known_reset(void)
{
    free(known.list);
    free(known.slots);
    memset(&known, 0, sizeof(known));
    known_doc = NULL;
}
static void known_add(HNode *n)
{
    if (!n)
        return;
    if (known.n * 2 >= known.size) {
        size_t size = known.size ? known.size * 2 : 256;
        HNode **slots = calloc(size, sizeof(*slots));
        if (!slots)
            exit(2);
        for (size_t i = 0; i < known.n; i++) {
            size_t at = ((uintptr_t)known.list[i] >> 4) & (size - 1);
            while (slots[at])
                at = (at + 1) & (size - 1);
            slots[at] = known.list[i];
        }
        free(known.slots);
        known.slots = slots;
        known.size = size;
    }
    size_t at = ((uintptr_t)n >> 4) & (known.size - 1);
    for (; known.slots[at]; at = (at + 1) & (known.size - 1))
        if (known.slots[at] == n)
            return;
    known.slots[at] = n;
    if (known.n == known.cap) {
        known.cap = known.cap ? known.cap * 2 : 256;
        known.list = realloc(known.list, known.cap * sizeof(*known.list));
        if (!known.list)
            exit(2);
    }
    known.list[known.n++] = n;
    // This oracle caches nodes through later actions and parser calls.
    if (known_doc) os64_html_hold(known_doc, n);
}
static HNode *host_of(const HNode *n)
{
    return n->kind == OS64_HTML_FRAGMENT ? (HNode *)*h_word(n) : NULL;
}
static void known_close(void)
{
    for (size_t i = 0; i < known.n; i++) {
        HNode *n = known.list[i];
        known_add(n->parent);
        known_add(host_of(n));
        known_add(n->template_contents);
        known_add(n->form_owner);
        size_t children = 0;
        for (HNode *c = n->first_child; c; c = c->next) {
            if (++children > 10000000)
                safety_fail("a child list that does not end");
            known_add(c);
        }
    }
}
static void known_scan(os64_html_parser_t *p)
{
    known_doc = &p->d->pub;
    known_add(&p->d->root);
    known_add(&p->d->html);
    for (size_t i = 0; i < p->stack.n; i++)
        known_add(p->stack.v[i]);
    for (size_t i = 0; i < p->formatting.n; i++)
        known_add(p->formatting.v[i]);
    known_add(p->form);
    known_add(p->head);
    known_add(p->script);
    known_close();
}
static HNode *top_of(HNode *n)
{
    for (size_t steps = 0; n->parent; n = n->parent)
        if (++steps > 10000000)
            safety_fail("a parent chain that does not end");
    return n;
}
/* What every reader of a document relies on, asked of every node it has. */
static void everything_holds(os64_html_document_t *doc)
{
    known_doc = doc;
    const HDoc *d = (const HDoc *)doc;
    size_t limit = h_depth_limit(d), records = 0;
    known_add(doc->document);
    known_close();
    if (doc->document != &d->root || d->root.parent || d->root.kind != OS64_HTML_DOCUMENT)
        safety_fail("the document node");
    for (size_t i = 0; i < known.n; i++) {
        HNode *n = known.list[i];
        if (n->document_id != d->id)
            safety_fail("a node of another document");
        size_t depth = 0;
        for (const HNode *a = n; depth <= limit;) {
            if (a->parent) {
                a = a->parent;
                depth++;
            } else if (host_of(a))
                a = host_of(a);
            else
                break;
        }
        if (depth > limit)
            safety_fail("a cycle, or a node deeper than the limit");
        if (n->parent) {
            HNode *up = n->parent;
            if (up->kind != OS64_HTML_DOCUMENT && up->kind != OS64_HTML_FRAGMENT &&
                up->kind != OS64_HTML_ELEMENT)
                safety_fail("a parent that cannot have children");
            if ((n->prev ? n->prev->next != n || n->prev->parent != up : up->first_child != n) ||
                (n->next ? n->next->prev != n || n->next->parent != up : up->last_child != n))
                safety_fail("sibling links disagree");
            if (n->kind == OS64_HTML_DOCUMENT || n->kind == OS64_HTML_FRAGMENT)
                safety_fail("a document or fragment with a parent");
            if (n->kind == OS64_HTML_DOCTYPE && up->kind != OS64_HTML_DOCUMENT)
                safety_fail("a doctype outside the document");
            if (n->kind == OS64_HTML_TEXT && up->kind == OS64_HTML_DOCUMENT)
                safety_fail("text under the document");
        } else if (n->prev || n->next)
            safety_fail("a root with siblings");
        if ((n->kind == OS64_HTML_TEXT || n->kind == OS64_HTML_COMMENT ||
             n->kind == OS64_HTML_DOCTYPE) &&
            (n->first_child || n->last_child))
            safety_fail("a leaf with children");
        if (!n->first_child != !n->last_child)
            safety_fail("first and last child disagree");
        if ((n->kind == OS64_HTML_TEXT || n->kind == OS64_HTML_COMMENT) &&
            (!n->text || strlen(n->text) != n->text_len))
            safety_fail("text and its length disagree");
        valid_utf8(n->name);
        valid_utf8(n->text);
        size_t attrs = 0;
        for (HAttr *a = n->attrs; a; a = a->next) {
            if (++attrs > 100000 || !a->name || !a->value)
                safety_fail("an attribute list");
            valid_utf8(a->name);
            valid_utf8(a->value);
        }
        if (n->template_contents &&
            (n->template_contents->kind != OS64_HTML_FRAGMENT || n->template_contents->parent ||
             host_of(n->template_contents) != n))
            safety_fail("a template and its contents disagree");
        if (n->form_owner) {
            records++;
            if (n->form_owner->kind != OS64_HTML_ELEMENT ||
                n->form_owner->tag != OS64_HTML_TAG_FORM)
                safety_fail("a form owner that is no form");
            if (top_of(n) != top_of(n->form_owner))
                safety_fail("a control and its form in different trees");
        }
    }
    if (records != d->records)
        safety_fail("form-owner record count disagrees with the nodes");
    /* The document: one element, which is `html`; a doctype only before it;
     * the landmarks by their definitions. */
    HNode *element = NULL, *head = NULL, *body = NULL;
    bool doctype = false;
    for (HNode *c = d->root.first_child; c; c = c->next) {
        if (c->kind == OS64_HTML_ELEMENT) {
            if (element)
                safety_fail("two elements under the document");
            element = c;
        } else if (c->kind == OS64_HTML_DOCTYPE) {
            if (doctype || element)
                safety_fail("a second doctype, or one after the element");
            doctype = true;
        }
    }
    if (element && (element != doc->html || element->tag != OS64_HTML_TAG_HTML))
        safety_fail("the document's element is not its html landmark");
    for (HNode *c = doc->html->first_child; c; c = c->next) {
        if (!head && c->kind == OS64_HTML_ELEMENT && c->ns == OS64_HTML_NS_HTML &&
            c->tag == OS64_HTML_TAG_HEAD)
            head = c;
        if (!body && c->kind == OS64_HTML_ELEMENT && c->ns == OS64_HTML_NS_HTML &&
            c->tag == OS64_HTML_TAG_BODY)
            body = c;
    }
    if (head != doc->head || body != doc->body)
        safety_fail("head or body is not the html element's first");
}
/* Parse as a host that runs scripts, in pieces of `chunk` (0: whole), doing
 * `act` at each stop in the script's place. The document comes back
 * finished, or abandoned at stop `abandon_at` (0: never), with `known`
 * holding every node it has. */
typedef void (*Act)(os64_html_parser_t *p, os64_html_document_t *doc, HNode *script);
static os64_html_document_t *disturbed_parse(const char *markup, size_t len, size_t chunk,
                                             os64_html_options_t *opt, Act act, size_t abandon_at)
{
    known_reset();
    stops_reset();
    os64_html_parser_t *p = os64_html_parser_new(opt);
    if (!p)
        return NULL;
    os64_html_document_t *doc = os64_html_parser_document(p);
    int64_t status = OS64_HTML_OK;
    bool ended = false;
    for (size_t fed = 0; status >= 0;) {
        if (status == OS64_HTML_SCRIPT) {
            stops++;
            known_scan(p);
            everything_holds(doc);
            if (stops == abandon_at)
                break;
            act(p, doc, os64_html_parser_script(p));
            known_close();
            everything_holds(doc);
            status = os64_html_parser_resume(p);
        } else if (fed < len) {
            size_t amount = chunk && chunk < len - fed ? chunk : len - fed;
            status = os64_html_parser_feed(p, markup + fed, amount);
            fed += amount;
        } else if (!ended) {
            ended = true;
            status = os64_html_parser_end(p);
        } else
            break;
    }
    known_scan(p);
    doc = stops == abandon_at && abandon_at ? os64_html_parser_abandon(p)
                                            : os64_html_parser_finish(p);
    everything_holds(doc);
    return doc;
}
static HNode *by_id(os64_html_document_t *doc, const char *id)
{
    (void)doc;
    for (size_t i = 0; i < known.n; i++) {
        const HAttr *a = os64_html_attr(known.list[i], "id");
        if (a && strcmp(a->value, id) == 0)
            return known.list[i];
    }
    return NULL;
}
static void disturbed_free(os64_html_document_t *doc)
{
    os64_html_document_free(doc);
    known_reset();
    if (live)
        safety_fail("a disturbed parse leaked");
}

/* The hand cases: one script each, doing the one thing the case is about. */
static HNode *made;
static void act_nest_the_other_way(os64_html_parser_t *p, os64_html_document_t *doc, HNode *script)
{
    (void)p;
    (void)script;
    /* <div><a><p>: the p is taken out and the div put inside it, so the
     * place the adoption agency moves the p to is now under the p. */
    HNode *div = by_id(doc, "d"), *para = by_id(doc, "p");
    check(os64_html_remove(doc, para) == OS64_HTML_OK &&
              os64_html_insert(doc, para, div, NULL) == OS64_HTML_OK,
          "the open elements nested the other way");
}
static void act_sink(os64_html_parser_t *p, os64_html_document_t *doc, HNode *script)
{
    (void)p;
    /* The innermost open element is moved to the foot of a chain that leaves
     * it and the script just inside the limit. */
    HNode *open = script->parent, *foot = doc->body;
    int64_t why = OS64_HTML_OK;
    for (size_t depth = 2; depth + 5 < h_depth_limit((HDoc *)doc); depth++) {
        HNode *link = os64_html_create_element(doc, OS64_HTML_NS_HTML, "span", &why);
        if (!link || os64_html_insert(doc, foot, link, NULL))
            break;
        foot = link;
    }
    check(os64_html_insert(doc, foot, open, NULL) == OS64_HTML_OK, "an open element sunk");
}
static void act_replace_html(os64_html_parser_t *p, os64_html_document_t *doc, HNode *script)
{
    (void)p;
    (void)script;
    made = os64_html_create_element(doc, OS64_HTML_NS_HTML, "html", NULL);
    check(made && os64_html_replace(doc, doc->document, made, doc->html) == OS64_HTML_OK &&
              doc->html == made,
          "the document element replaced");
}
static void act_body_first(os64_html_parser_t *p, os64_html_document_t *doc, HNode *script)
{
    (void)p;
    (void)script;
    made = os64_html_create_element(doc, OS64_HTML_NS_HTML, "body", NULL);
    check(made && os64_html_insert(doc, doc->html, made, NULL) == OS64_HTML_OK &&
              doc->body == made,
          "a body before the parser's");
}
static void act_own_attrs(os64_html_parser_t *p, os64_html_document_t *doc, HNode *script)
{
    (void)p;
    (void)script;
    check(os64_html_set_attr(doc, by_id(doc, "x"), "class", "k", 1) == OS64_HTML_OK,
          "an open formatting element given an attribute");
}
static void act_clone_body(os64_html_parser_t *p, os64_html_document_t *doc, HNode *script)
{
    (void)p;
    (void)script;
    made = os64_html_clone(doc, doc->body, false, NULL);
    check(made != NULL, "the body cloned");
    known_add(made);
}
static void act_detach_b(os64_html_parser_t *p, os64_html_document_t *doc, HNode *script)
{
    (void)p;
    (void)script;
    HNode *control = by_id(doc, "q");
    check(control && control->form_owner == by_id(doc, "f") &&
              os64_html_remove(doc, by_id(doc, "b")) == OS64_HTML_OK &&
              control->form_owner == by_id(doc, "f"),
          "a control and its form detached together stay tied");
}
static void act_table_out(os64_html_parser_t *p, os64_html_document_t *doc, HNode *script)
{
    (void)p;
    (void)script;
    /* The open table goes out of the form's block to sit under the `b`, and
     * the `b` out of the document: what the parser fosters out of the table
     * next lands beside it, outside the block the form is in. */
    check(os64_html_insert(doc, by_id(doc, "b"), by_id(doc, "t"), NULL) == OS64_HTML_OK &&
              os64_html_remove(doc, by_id(doc, "b")) == OS64_HTML_OK,
          "the table moved out of the block, and the lot out of the document");
}
static void act_beside_body(os64_html_parser_t *p, os64_html_document_t *doc, HNode *script)
{
    (void)p;
    /* The open div goes to sit beside the body: the control the parser puts
     * in it next is tied to a form that is in the body. */
    check(os64_html_insert(doc, doc->html, script->parent, NULL) == OS64_HTML_OK,
          "an open element moved beside the body");
}
static void act_nothing(os64_html_parser_t *p, os64_html_document_t *doc, HNode *script)
{
    (void)p;
    (void)doc;
    (void)script;
}
static void act_elsewhere(os64_html_parser_t *p, os64_html_document_t *doc, HNode *script)
{
    (void)p;
    (void)script;
    /* A node put in and taken out again: the tree is as it was, and the
     * parser has been told that a verb moved something. */
    HNode *span = os64_html_create_element(doc, OS64_HTML_NS_HTML, "span", NULL);
    check(span && os64_html_insert(doc, doc->body, span, NULL) == OS64_HTML_OK &&
              os64_html_remove(doc, span) == OS64_HTML_OK,
          "a move that leaves the tree as it was");
}
static void disturbed_checks(void)
{
    os64_html_options_t on = scripted();
    char whole[4096];
    for (size_t chunk = 0; chunk < 2; chunk++) {
        /* The checks change nothing a parser alone would build: the same
         * tree, the control still tied to its form across the adoption
         * agency's move inside one tree, and the walks charged as work. */
        snprintf(whole, sizeof(whole),
                 "%s<form id=f><b id=b><div id=blk><input id=q><script>s</script></b>"
                 "<table><tr><td>cell</td>fostered<p>x</table>",
                 window());
        os64_html_document_t *plain =
            disturbed_parse(whole, strlen(whole), chunk, &on, act_nothing, 0);
        char *want = tree_text(plain);
        uint64_t work = plain->work;
        disturbed_free(plain);
        os64_html_document_t *checked =
            disturbed_parse(whole, strlen(whole), chunk, &on, act_elsewhere, 0);
        char *got = tree_text(checked);
        check(strcmp(got, want) == 0 && !checked->refusal, "a checked parse builds the same tree");
        check(by_id(checked, "q") && by_id(checked, "q")->form_owner == by_id(checked, "f"),
              "a move inside one tree parts nobody");
        check(checked->work > work, "the checks' walks are charged: %llu against %llu",
              (unsigned long long)checked->work, (unsigned long long)work);
        free(got);
        free(want);
        disturbed_free(checked);
        /* The same of plain links, with no adoption to charge for. */
        snprintf(whole, sizeof(whole), "%s<p>a<script>s</script>b<div>c<span>d", window());
        plain = disturbed_parse(whole, strlen(whole), chunk, &on, act_nothing, 0);
        work = plain->work;
        disturbed_free(plain);
        checked = disturbed_parse(whole, strlen(whole), chunk, &on, act_elsewhere, 0);
        check(checked->work > work, "a checked link is charged: %llu against %llu",
              (unsigned long long)checked->work, (unsigned long long)work);
        disturbed_free(checked);

        /* A frameset takes the body out of the document: its landmark goes. */
        snprintf(whole, sizeof(whole), "%s<div><script>s</script></div><frameset><frame>",
                 window());
        checked = disturbed_parse(whole, strlen(whole), chunk, &on, act_elsewhere, 0);
        check(checked && !checked->refusal && !checked->body &&
                  find_named(checked->html->first_child, "frameset"),
              "the body a frameset replaced is no landmark");
        disturbed_free(checked);


        /* A link that would make a cycle is not made. */
        snprintf(whole, sizeof(whole), "%s<div id=d><a id=a><p id=p><script>s</script></a>tail",
                 window());
        os64_html_document_t *doc =
            disturbed_parse(whole, strlen(whole), chunk, &on, act_nest_the_other_way, 0);
        check(doc && !doc->refusal && stops == 1 && !by_id(doc, "p")->parent &&
                  top_of(by_id(doc, "d")) == by_id(doc, "p"),
              "the adoption agency's move under its own subtree is left out");
        disturbed_free(doc);

        /* The stack's bound on depth is void once a verb has moved an open
         * element: the parser measures, and refuses by name. */
        os64_html_options_t shallow = on;
        shallow.max_depth = 8;
        snprintf(whole, sizeof(whole), "%s<div><div><script>s</script><p><i><b><u>deeper",
                 window());
        doc = parse_raw((const unsigned char *)whole, strlen(whole), chunk, &shallow);
        check(doc && !doc->refusal, "eight open elements are within the stack's limit");
        os64_html_document_free(doc);
        doc = disturbed_parse(whole, strlen(whole), chunk, &shallow, act_sink, 0);
        check(doc && doc->refusal == OS64_HTML_TOO_DEEP,
              "a node past the depth limit refuses the parse");
        disturbed_free(doc);
        /* The node refused is a control already tied to its form: a control
         * that did not reach the tree is tied to nothing. */
        shallow.max_depth = 12;
        snprintf(whole, sizeof(whole),
                 "%s<form id=f><div><div><script>s</script><p><i><b><u><button id=q>", window());
        doc = disturbed_parse(whole, strlen(whole), chunk, &shallow, act_sink, 0);
        check(doc && doc->refusal == OS64_HTML_TOO_DEEP && by_id(doc, "q") &&
                  !by_id(doc, "q")->parent && !by_id(doc, "q")->form_owner,
              "a control refused for depth keeps no form owner");
        disturbed_free(doc);

        /* The document element replaced: the parser goes on in its own, out
         * of the document, and finish does not seat a second. */
        snprintf(whole, sizeof(whole), "%s<p id=p><script>s</script>text<p id=after>",
                 window());
        doc = disturbed_parse(whole, strlen(whole), chunk, &on, act_replace_html, 0);
        check(doc && !doc->refusal && doc->html == made && made->parent == doc->document &&
                  !made->next && !made->prev && by_id(doc, "after") &&
                  top_of(by_id(doc, "after")) != doc->document && !doc->body && !doc->head,
              "the parser's tree stays out of the document it was replaced in");
        disturbed_free(doc);

        /* body is the html element's first body, whoever made it. */
        snprintf(whole, sizeof(whole), "%s<head><script>s</script></head><body id=theirs>x",
                 window());
        doc = disturbed_parse(whole, strlen(whole), chunk, &on, act_body_first, 0);
        check(doc && !doc->refusal && doc->body == made && by_id(doc, "theirs") &&
                  by_id(doc, "theirs")->prev == made,
              "the parser's body comes second and is not the landmark");
        disturbed_free(doc);

        /* A formatting element whose attributes a verb made its own: the
         * parser's later clone of it gets records of its own. */
        snprintf(whole, sizeof(whole), "%s<p><b id=x><script>s</script></p><p id=second>t",
                 window());
        doc = disturbed_parse(whole, strlen(whole), chunk, &on, act_own_attrs, 0);
        HNode *first = doc ? by_id(doc, "x") : NULL;
        HNode *clone = doc && by_id(doc, "second") ? by_id(doc, "second")->first_child : NULL;
        check(first && clone && clone != first && os64_html_attr(clone, "class") &&
                  os64_html_set_attr(doc, first, "class", "changed", 7) == OS64_HTML_OK &&
                  os64_html_remove_attr(doc, first, "id") == OS64_HTML_OK &&
                  strcmp(os64_html_attr(clone, "class")->value, "k") == 0 &&
                  strcmp(os64_html_attr(clone, "id")->value, "x") == 0 &&
                  os64_html_remove_attr(doc, clone, "class") == OS64_HTML_OK,
              "the clone's attributes are not the original's records");
        disturbed_free(doc);

        /* A second body tag adds to the body's own list: a clone that shared
         * the parser's list does not gain the attribute. */
        snprintf(whole, sizeof(whole), "%s<body id=b><script>s</script><body class=late>x",
                 window());
        doc = disturbed_parse(whole, strlen(whole), chunk, &on, act_clone_body, 0);
        check(doc && os64_html_attr(doc->body, "class") && !os64_html_attr(made, "class") &&
                  os64_html_attr(made, "id") &&
                  os64_html_remove_attr(doc, doc->body, "class") == OS64_HTML_OK &&
                  os64_html_remove_attr(doc, doc->body, "id") == OS64_HTML_OK &&
                  os64_html_attr(made, "id") != NULL,
              "merged attributes go on the element's own list");
        disturbed_free(doc);

        /* A control the parser moves out of its form's tree loses the tie. */
        snprintf(whole, sizeof(whole),
                 "%s<b id=b><table><form id=f></table><div id=blk><input id=q>"
                 "<script>s</script></b>",
                 window());
        doc = disturbed_parse(whole, strlen(whole), chunk, &on, act_detach_b, 0);
        check(doc && !doc->refusal && by_id(doc, "q") && !by_id(doc, "q")->form_owner &&
                  top_of(by_id(doc, "q")) == doc->document &&
                  top_of(by_id(doc, "f")) != doc->document,
              "the adoption agency carried the control out of its form's tree");
        disturbed_free(doc);

        /* And the other way about: the form goes with the block, and the
         * control it left behind is tied to it no longer. */
        snprintf(whole, sizeof(whole),
                 "%s<b id=b><div id=blk><form id=f><table id=t><script>s</script><input id=q>"
                 "</table></b>",
                 window());
        doc = disturbed_parse(whole, strlen(whole), chunk, &on, act_table_out, 0);
        check(doc && !doc->refusal && by_id(doc, "q") && !by_id(doc, "q")->form_owner &&
                  top_of(by_id(doc, "f")) == doc->document &&
                  top_of(by_id(doc, "q")) != doc->document,
              "the adoption agency carried the form away from its control");
        disturbed_free(doc);
    }

    /* A frameset takes the body out, and with it the form a control beside
     * the body was tied to. */
    for (size_t chunk = 0; chunk < 2; chunk++) {
        snprintf(whole, sizeof(whole),
                 "%s<form id=f><div><script>s</script><input type=hidden id=q></div><frameset>",
                 window());
        os64_html_document_t *doc =
            disturbed_parse(whole, strlen(whole), chunk, &on, act_beside_body, 0);
        check(doc && !doc->refusal && !doc->body && by_id(doc, "q") &&
                  !by_id(doc, "q")->form_owner && top_of(by_id(doc, "q")) == doc->document &&
                  top_of(by_id(doc, "f")) != doc->document,
              "the body a frameset took out carried the form away from its control");
        disturbed_free(doc);
    }

    /* Each case again with every allocation failed in turn, and then with
     * the work running out at every step: the verbs fail, the parse is
     * refused part way, and what a reader relies on still holds of every
     * node (disturbed_parse asks). */
    static const struct {
        const char *markup;
        Act act;
    } swept[] = {
        {"<div id=d><a id=a><p id=p><script>s</script></a>tail", act_nest_the_other_way},
        {"<p id=p><script>s</script>text<p id=after>", act_replace_html},
        {"<head><script>s</script></head><body id=theirs>x", act_body_first},
        {"<p><b id=x><script>s</script></p><p id=second>t", act_own_attrs},
        {"<body id=b><script>s</script><body class=late>x", act_clone_body},
        {"<b id=b><table><form id=f></table><div id=blk><input id=q><script>s</script></b>",
         act_detach_b},
        {"<b id=b><div id=blk><form id=f><table id=t><script>s</script><input id=q></table></b>",
         act_table_out},
        {"<div><script>s</script></div><frameset><frame>", act_elsewhere},
        {"<form id=f><div><script>s</script><input type=hidden id=q></div><frameset>",
         act_beside_body},
        {"<form id=f><b id=b><div id=blk><input id=q><script>s</script></b>"
         "<table><tr><td>cell</td>fostered<p>x</table>",
         act_elsewhere},
    };
    size_t failures = 0;
    for (size_t i = 0; i < H_ARRAY(swept); i++) {
        snprintf(whole, sizeof(whole), "%s%s", window(), swept[i].markup);
        allocations = 0;
        os64_html_document_t *whole_doc =
            disturbed_parse(whole, strlen(whole), 0, &on, swept[i].act, 0);
        size_t count = allocations;
        uint64_t work = whole_doc->work;
        disturbed_free(whole_doc);
        quiet = true;
        for (uint64_t budget = 0; budget < work; budget++) {
            os64_html_options_t opt = on;
            opt.max_work = budget;
            os64_html_document_t *doc =
                disturbed_parse(whole, strlen(whole), 0, &opt, swept[i].act, 0);
            if (!doc || doc->refusal != OS64_HTML_WORK_EXHAUSTED)
                safety_fail("a disturbed parse outran its work budget");
            disturbed_free(doc);
            failures++;
        }
        for (size_t n = 1; n <= count; n++) {
            allocations = 0;
            fail_at = n;
            os64_html_document_t *doc =
                disturbed_parse(whole, strlen(whole), 0, &on, swept[i].act, 0);
            fail_at = 0;
            if (doc)
                disturbed_free(doc);
            failures++;
        }
        quiet = false;
    }
    check(failures > 100, "disturbed sweeps ran: %zu", failures);


    /* head is the html element's first head, whoever made it: here a verb,
     * between two feeds, before the parser reached its own. */
    os64_html_parser_t *early = os64_html_parser_new(&on);
    os64_html_document_t *open = os64_html_parser_document(early);
    feed_text(early, window());
    check(feed_text(early, "<html>") == OS64_HTML_OK && open->html->parent == open->document,
          "the html element seated");
    HNode *theirs = os64_html_create_element(open, OS64_HTML_NS_HTML, "head", NULL);
    check(theirs && os64_html_insert(open, open->html, theirs, NULL) == OS64_HTML_OK &&
              open->head == theirs,
          "a head before the parser's");
    check(feed_text(early, "<head>") == OS64_HTML_OK && open->head == theirs && theirs->next &&
              theirs->next->tag == OS64_HTML_TAG_HEAD,
          "the parser's head comes second and is not the landmark");
    feed_text(early, "<title>t</title></head><body>x");
    open = os64_html_parser_finish(early);
    check(!open->refusal && open->head == theirs && open->body, "nor once the body is in");
    os64_html_document_free(open);

    /* Attributes a verb gave the html element before its tag was parsed. */
    os64_html_parser_t *p = os64_html_parser_new(&on);
    os64_html_document_t *doc = os64_html_parser_document(p);
    check(os64_html_set_attr(doc, doc->html, "data-x", "1", 1) == OS64_HTML_OK &&
              os64_html_set_attr(doc, doc->html, "lang", "fr", 2) == OS64_HTML_OK,
          "attributes on an html element not yet in the document");
    feed_text(p, "<html lang=en class=c><p>x");
    doc = os64_html_parser_finish(p);
    check(!doc->refusal && strcmp(os64_html_attr(doc->html, "lang")->value, "fr") == 0 &&
              os64_html_attr(doc->html, "data-x") && os64_html_attr(doc->html, "class") &&
              os64_html_remove_attr(doc, doc->html, "class") == OS64_HTML_OK &&
              os64_html_remove_attr(doc, doc->html, "lang") == OS64_HTML_OK &&
              os64_html_remove_attr(doc, doc->html, "data-x") == OS64_HTML_OK && !doc->html->attrs,
          "the tag adds what the element lacks, and every record can be retired");
    validate(doc);
    os64_html_document_free(doc);
    check(live == 0, "disturbed fixtures freed");
}

/* ── Random scripts ──────────────────────────────────────────────────────
 *
 * A document built from pieces that set the parser's machinery going
 * (formatting to adopt, tables to foster out of, forms, templates, foreign
 * content), with a script at every turn; and at each script a run of random
 * verbs on random nodes, the parser's open elements among them. Nothing is
 * predicted. What is asked is what a reader relies on: everything_holds
 * after every stop and at the end, what a pin was holding still reads as it
 * did, and the document frees. */
static uint32_t walk_seed;
static uint32_t roll(uint32_t n)
{
    return random_next(&walk_seed) % n;
}
static struct {
    os64_html_pin_t pin;
    struct {
        const char *at;
        char *copy;
    } held[12];
    size_t n;
} holds[3];
static void holds_check(void)
{
    for (size_t i = 0; i < H_ARRAY(holds); i++)
        for (size_t j = 0; holds[i].pin && j < holds[i].n; j++)
            if (strcmp(holds[i].held[j].at, holds[i].held[j].copy) != 0)
                safety_fail("bytes a pinned snapshot points at changed");
}
static void holds_drop(os64_html_document_t *doc, size_t i)
{
    if (!holds[i].pin)
        return;
    for (size_t j = 0; j < holds[i].n; j++)
        free(holds[i].held[j].copy);
    os64_html_unpin(doc, holds[i].pin);
    memset(&holds[i], 0, sizeof(holds[i]));
}
static void holds_take(os64_html_document_t *doc, size_t i)
{
    holds[i].pin = os64_html_pin(doc);
    for (size_t tries = 0; holds[i].pin && tries < 40 && holds[i].n < H_ARRAY(holds[i].held);
         tries++) {
        HNode *n = known.list[roll((uint32_t)known.n)];
        const char *at = n->text;
        if (!at && n->attrs)
            at = n->attrs->value;
        if (!at)
            continue;
        holds[i].held[holds[i].n].at = at;
        holds[i].held[holds[i].n].copy = strdup(at);
        if (!holds[i].held[holds[i].n++].copy)
            exit(2);
    }
}
static HNode *any_node(void)
{
    return known.list[roll((uint32_t)known.n)];
}
static HNode *any_child(HNode *parent)
{
    size_t count = 0;
    for (HNode *c = parent->first_child; c; c = c->next)
        count++;
    if (!count || !roll(3))
        return NULL;
    HNode *c = parent->first_child;
    for (size_t skip = roll((uint32_t)count); skip; skip--)
        c = c->next;
    return c;
}
static void act_at_random(os64_html_parser_t *p, os64_html_document_t *doc, HNode *script)
{
    static const char *const names[] = {"div", "p",     "b",    "form",     "input", "table", "tr",
                                        "td",  "html",  "body", "head",     "span",  "a",     "select",
                                        "svg", "title", "li",   "template", "i"};
    static const char *const attrs[] = {"id", "class", "form", "href", "x"};
    static const char *const values[] = {"", "v", "a longer value than the others", "f", "\xc3\xa9"};
    holds_check();
    /* The script and what is open are picked more often than chance gives. */
    for (unsigned turn = 2 + roll(12); turn; turn--) {
        HNode *a = roll(4) ? any_node() : roll(2) || !p->stack.n ? script
                                                                : p->stack.v[roll((uint32_t)p->stack.n)];
        HNode *b = roll(5) ? any_node() : &p->d->root;
        int64_t why = OS64_HTML_OK;
        HNode *fresh = NULL;
        switch (roll(12)) {
        case 0:
            os64_html_remove(doc, a);
            break;
        case 1:
        case 2:
            os64_html_insert(doc, b, a, any_child(b));
            break;
        case 3:
            os64_html_replace(doc, b, a, any_child(b));
            break;
        case 4: {
            const char *value = values[roll(H_ARRAY(values))];
            os64_html_set_attr(doc, a, attrs[roll(H_ARRAY(attrs))], value, strlen(value));
            break;
        }
        case 5:
            os64_html_remove_attr(doc, a, attrs[roll(H_ARRAY(attrs))]);
            break;
        case 6:
            os64_html_set_text(doc, a, "set by a script", roll(16));
            break;
        case 7:
            fresh = os64_html_create_element(
                doc, roll(8) ? OS64_HTML_NS_HTML : OS64_HTML_NS_SVG, names[roll(H_ARRAY(names))],
                &why);
            break;
        case 8:
            fresh = roll(3) ? os64_html_create_text(doc, "made by a script", roll(17), &why)
                    : roll(2) ? os64_html_create_comment(doc, "note", 4, &why)
                              : os64_html_create_fragment(doc, &why);
            break;
        case 9:
            fresh = os64_html_clone(doc, a, roll(2), &why);
            break;
        case 10: {
            size_t i = roll(H_ARRAY(holds));
            if (!holds[i].pin)
                holds_take(doc, i);
            break;
        }
        default:
            holds_drop(doc, roll(H_ARRAY(holds)));
            break;
        }
        if (fresh) {
            known_add(fresh);
            known_close();
            if (roll(4))
                os64_html_insert(doc, b, fresh, any_child(b));
        }
    }
    holds_check();
}
static size_t walk_stops, walk_refusals, walk_abandoned;
static void random_walks(unsigned walks, unsigned first)
{
    static const char *const pieces[] = {
        "<script>s</script>", "<script>s</script>", "<script>s</script>", "text ",
        "<p>",        "</p>",       "<b>",        "</b>",       "<i>",      "</i>",
        "<a href=x>", "</a>",       "<div>",      "</div>",     "<table>",  "</table>",
        "<tr>",       "<td>",       "</td>",      "<form id=f>", "</form>", "<input name=n>",
        "<select>",   "<option>",   "</select>",  "<template>", "</template>",
        "<svg>",      "</svg>",     "<math>",     "<mi>",       "</math>",  "<title>t</title>",
        "<body class=again>",       "<html lang=late>",         "<frameset>", "<!-- c -->",
        "<button>",   "</button>",  "<textarea>x</textarea>",   "<li>",     "<h1>", "</h1>",
        "<nobr>",     "<font size=1>", "</font>",  "<caption>",  "<tbody>",  "<br>", "</body>",
        "</html>",    "<head>",     "</head>",    "<noscript>n</noscript>", "<svg><script/></svg>",
        "<output form=f>", "\xc3\xa9", "&amp;",   "<img>",      "<object>", "</object>"};
    char *markup = malloc(64 * 1024);
    if (!markup)
        exit(2);
    for (unsigned walk = first; walk < first + walks; walk++) {
        walk_now = walk;
        walk_seed = (0x5c417u + walk * 2654435761u) | 1;
        os64_html_options_t opt = scripted();
        /* Half the walks under limits tight enough to keep meeting them. */
        if (roll(2)) {
            opt.max_depth = 3 + roll(30);
            if (!roll(3))
                opt.max_arena_bytes = 12000 + roll(60000);
            if (!roll(3))
                opt.max_work = 500 + roll(20000);
        }
        size_t len = roll(2) ? (size_t)snprintf(markup, 2048, "%s", window()) : 0;
        if (!roll(4))
            len += (size_t)sprintf(markup + len, "<!DOCTYPE html>");
        for (unsigned n = 20 + roll(120); n; n--)
            len += (size_t)sprintf(markup + len, "%s", pieces[roll(H_ARRAY(pieces))]);
        size_t chunk = roll(3) ? 1 + roll(64) : 0;
        size_t abandon_at = roll(5) ? 0 : 1 + roll(6);
        os64_html_document_t *doc =
            disturbed_parse(markup, len, chunk, &opt, act_at_random, abandon_at);
        if (!doc)
            safety_fail("walk parser allocation");
        holds_check();
        /* A few kilobytes cannot need the default budget: spending it all is
         * a loop that only the budget ended. */
        if (doc->refusal == OS64_HTML_WORK_EXHAUSTED &&
            opt.max_work == os64_html_options_default().max_work)
            safety_fail("a small document spent the whole work budget");
        walk_stops += stops;
        walk_refusals += doc->refusal != 0;
        walk_abandoned += abandon_at && stops == abandon_at;
        for (size_t i = 0; i < H_ARRAY(holds); i++)
            holds_drop(doc, i);
        if (os64_html_retired_bytes(doc))
            safety_fail("retired bytes outlived every pin");
        everything_holds(doc);
        disturbed_free(doc);
    }
    walk_now = -1;
    free(markup);
}
static void stress(void)
{
    char *data = malloc(1024 * 1024);
    if (!data)
        exit(2);
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = "utf-8";
    size_t n = 0;
    for (unsigned i = 0; i < 1000; i++)
        n += sprintf(data + n, "<div>");
    bounded_case((unsigned char *)data, n, &opt, OS64_HTML_TOO_DEEP, "deep-nest");
    /* html and body remain open when div reaches depth three. */
    opt.max_depth = 3;
    bounded_case((unsigned char *)"<div></div>", 11, &opt, 0, "depth-at-limit");
    bounded_case((unsigned char *)"<div><div>", 10, &opt, OS64_HTML_TOO_DEEP, "depth-crossing");
    opt.max_depth = 100000;
    opt.max_work = 100000;
    n = 0;
    for (unsigned i = 0; i < 3000; i++)
        n += sprintf(data + n, "<p><b id='%u'>x</p>", i);
    bounded_case((unsigned char *)data, n, &opt, OS64_HTML_WORK_EXHAUSTED, "formatting-flood");
    n = 0;
    for (unsigned i = 0; i < 100; i++)
        n += sprintf(data + n, "<b id='%u'>", i);
    for (unsigned i = 0; i < 3000; i++)
        n += sprintf(data + n, "<p>x</p>");
    bounded_case((unsigned char *)data, n, &opt, OS64_HTML_WORK_EXHAUSTED,
                 "reconstruct-per-paragraph");
    opt.max_work = 100000000;
    opt.max_arena_bytes = 65536;
    n = sprintf(data, "<table>");
    for (unsigned i = 0; i < 10000; i++)
        n += sprintf(data + n, "x<span></span>");
    bounded_case((unsigned char *)data, n, &opt, OS64_HTML_ARENA_EXHAUSTED, "foster-parent-flood");
    opt = os64_html_options_default();
    opt.charset = "utf-8";
    opt.max_work = 100000;
    n = sprintf(data, "<div");
    for (unsigned i = 0; i < 3000; i++)
        n += sprintf(data + n, " long_attribute_prefix_%u=x", i);
    data[n++] = '>';
    bounded_case((unsigned char *)data, n, &opt, OS64_HTML_WORK_EXHAUSTED, "attribute-flood");
    opt = os64_html_options_default();
    opt.charset = "utf-8";
    n = sprintf(data, "<svg><");
    memset(data + n, 'x', 200000);
    n += 200000;
    data[n++] = '>';
    memset(data + n, 'a', 200000);
    n += 200000;
    bounded_case((unsigned char *)data, n, &opt, 0, "long-foreign-name");
    opt.max_work = 1000000;
    n = sprintf(data, "<math><annotation-xml");
    for (unsigned i = 0; i < 100; i++)
        n += sprintf(data + n, " a%u=x", i);
    data[n++] = '>';
    memset(data + n, 'a', 100000);
    n += 100000;
    bounded_case((unsigned char *)data, n, &opt, OS64_HTML_WORK_EXHAUSTED,
                 "integration-attribute-walk");
    free(data);
}
static void read_exact(void *p, size_t n)
{
    if (fread(p, 1, n, stdin) != n) {
        fputs("short harness record\n", stderr);
        exit(2);
    }
}
#ifndef HTML_TOKENIZER_ONLY
#include "test_html_fragment.inc"
#endif
int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--fragments")) {
        fragment_checks();
        printf("html fragments: %zu run, %zu failed\n", checks_run, checks_failed);
        return checks_failed ? 1 : 0;
    }
    bool safety = argc > 1 && strcmp(argv[1], "--safety") == 0;
    bool fuzz = argc > 1 && strcmp(argv[1], "--fuzz") == 0;
    bool abandon = argc > 1 && strcmp(argv[1], "--abandon") == 0;
    if (argc > 1 && strcmp(argv[1], "--stress") == 0) {
        stress();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--checks") == 0) {
        fragment_checks();
        encoding_exports();
        form_owners();
        integration_end_tags();
        byte_order_marks();
        stop_checks();
        disturbed_checks();
        random_walks(argc > 2 ? (unsigned)atoi(argv[2]) : 20000,
                     argc > 3 ? (unsigned)atoi(argv[3]) : 0);
        printf("html stops: abandoned parsers=%zu; random walks: stops=%zu refused=%zu "
               "abandoned=%zu\n",
               abandoned, walk_stops, walk_refusals, walk_abandoned);
        printf("html checks: %zu run, %zu failed\n", checks_run, checks_failed);
        return checks_failed ? 1 : 0;
    }
    uint32_t fuzz_seed = 0x64f022u;
    size_t fuzz_cases = 0, fuzz_walks = 0;
    uint32_t hdr[4];
    while (fread(hdr, sizeof(hdr), 1, stdin) == 1) {
        unsigned char *data = malloc(hdr[2] + 1);
        char *last = malloc(hdr[3] + 1);
        if (!data || !last)
            return 2;
        read_exact(data, hdr[2]);
        data[hdr[2]] = 0;
        read_exact(last, hdr[3]);
        last[hdr[3]] = 0;
        unsigned kind = hdr[0] & ~SCRIPTING;
        if (kind == 3) {
            if (fuzz) {
                uint64_t budget = 100 + (random_next(&fuzz_seed) >> 16) % 20000;
                fragment_safety(data, hdr[2], last, hdr[0] & SCRIPTING, budget, false);
                fuzz_cases++;
            } else if (safety) {
                fragment_safety(data, hdr[2], last, hdr[0] & SCRIPTING,
                                4096 + 256 * (uint64_t)hdr[2], true);
            } else {
                fragment_record(data, hdr[2], last, hdr[0] & SCRIPTING);
            }
            free(last);
            free(data);
            continue;
        }
        if (fuzz) {
            os64_html_options_t opt = os64_html_options_default();
            opt.charset = (random_next(&fuzz_seed) >> 16) & 1 ? "utf-8" : NULL;
            opt.max_work = 100 + (random_next(&fuzz_seed) >> 16) % 20000;
            opt.max_depth = 1 + (random_next(&fuzz_seed) >> 16) % 100;
            opt.max_arena_bytes = 16384 + (random_next(&fuzz_seed) >> 16) % 131072;
            opt.scripting = (random_next(&fuzz_seed) >> 16) & 1;
            bounded_case(data, hdr[2], &opt, INT64_MAX, NULL);
            /* And once more with a random script at each stop. */
            if (opt.scripting) {
                walk_seed = random_next(&fuzz_seed) | 1;
                os64_html_document_t *doc = disturbed_parse((const char *)data, hdr[2],
                                                            roll(3) ? 1 + roll(64) : 0, &opt,
                                                            act_at_random, 0);
                if (doc) {
                    for (size_t i = 0; i < H_ARRAY(holds); i++)
                        holds_drop(doc, i);
                    everything_holds(doc);
                    disturbed_free(doc);
                    fuzz_walks += stops != 0;
                }
            }
            fuzz_cases++;
            free(last);
            free(data);
            continue;
        }
        if (abandon) {
            os64_html_options_t opt = scripted();
            opt.charset = hdr[3] ? last : NULL;
            abandon_everywhere(data, hdr[2], &opt);
            free(last);
            free(data);
            continue;
        }
        if (safety) {
            safety_case(data, hdr[2], hdr[0] & SCRIPTING);
            free(last);
            free(data);
            continue;
        }
        allocations = 0;
        fail_at = 0;
        struct timespec begin, finish;
        clock_gettime(CLOCK_MONOTONIC, &begin);
        os64_html_options_t options = os64_html_options_default();
        options.charset = kind == 1 ? "utf-8" : hdr[3] ? last : NULL;
        options.scripting = hdr[0] & SCRIPTING;
        os64_html_parser_t *p = os64_html_parser_new(&options);
        if (!p)
            return 2;
        stops_reset();
        if (kind == 0) {
            p->token_sink = token_sink;
            const HState states[] = {T_DATA, T_RCDATA, T_RAWTEXT, T_SCRIPT, T_PLAIN, T_CDATA};
            p->state = states[hdr[1]];
            p->started = true;
            if (hdr[3])
                h_buf_bytes(p, &p->last_start, last, hdr[3]);
            fputs("{\"tokens\":[", stdout);
            comma = false;
            for (size_t i = 0; i < hdr[2]; i += 4) {
                uint32_t c;
                memcpy(&c, data + i, 4);
                p->offset = i / 4;
                h_codepoint(p, c);
            }
            p->offset = hdr[2] / 4;
            h_codepoint(p, H_EOF);
            p->eof_sent = true;
            fputs("],", stdout);
        } else {
            uint32_t seed = 0x64a11u;
            for (size_t i = 0; i < hdr[2];) {
                size_t chunk = hdr[1] == UINT32_MAX ? 1 + random_next(&seed) % 37
                               : hdr[1]             ? hdr[1]
                                                    : hdr[2];
                if (chunk > hdr[2] - i)
                    chunk = hdr[2] - i;
                run_scripts(p, os64_html_parser_feed(p, data + i, chunk));
                i += chunk;
            }
            if (options.scripting)
                run_scripts(p, os64_html_parser_end(p));
            fputs("{", stdout);
        }
        os64_html_document_t *doc = os64_html_parser_finish(p);
        clock_gettime(CLOCK_MONOTONIC, &finish);
        if (kind != 0) {
            validate(doc);
            char *dump = NULL;
            size_t len = 0, visited = 0;
            FILE *out = open_memstream(&dump, &len);
            if (!out)
                return 2;
            dump_nodes(out, doc->document->first_child, 0, &visited, doc->node_count);
            fclose(out);
            if (len && dump[len - 1] == '\n')
                len--;
            fputs("\"tree\":", stdout);
            json_string_n(dump, len);
            putchar(',');
            free(dump);
            char *serialized = serialization_check(doc->document, true, options.scripting);
            fputs("\"serialized\":", stdout);
            json_string(serialized);
            putchar(',');
            free(serialized);
        }
        double elapsed = finish.tv_sec - begin.tv_sec + (finish.tv_nsec - begin.tv_nsec) / 1e9;
        printf("\"seconds\":%.9f,\"work\":%llu,\"arena\":%zu,\"nodes\":%zu,\"stops\":%zu,", elapsed,
               (unsigned long long)doc->work, doc->peak_arena_bytes, doc->node_count, stops);
        fputs("\"charset\":", stdout);
        json_string(doc->charset);
        fputs(",\"unsupported\":", stdout);
        json_string(doc->charset_unsupported);
        fputs(",\"late_meta\":", stdout);
        json_string(doc->charset_late_meta);
        printf(",\"refusal\":%lld,\"errors\":[", (long long)doc->refusal);
        size_t errors = doc->parse_errors < 16 ? doc->parse_errors : 16;
        for (size_t i = 0; i < errors; i++) {
            if (i)
                putchar(',');
            json_string(doc->first_errors[i].name);
        }
        printf("],\"parse_errors\":%zu}\n", doc->parse_errors);
        os64_html_document_free(doc);
        free(last);
        free(data);
        if (live) {
            fprintf(stderr, "live allocations: %zu\n", live);
            return 3;
        }
    }
    if (safety)
        printf("Fragments: cases=%zu allocation-failures=%zu work-cuts=%zu\n",
               fragment_cases, fragment_failures, fragment_work_cuts);
    if (fuzz)
        printf("Fuzz: cases=%zu chunkings=4 budget-seed=0x64f022 scripted-walks=%zu\n", fuzz_cases,
               fuzz_walks);
    if (abandon)
        printf("Abandon: parsers=%zu\n", abandoned);
    if (safety)
        printf("Safety: cases=%zu stops=%zu allocation-failures=%zu prefix/chunk checks=%zu "
               "seed=0x64a11\n",
               safety_cases, safety_stops, safety_failures, safety_prefixes);
    return ferror(stdin) ? 2 : 0;
}
