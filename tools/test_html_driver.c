#define _POSIX_C_SOURCE 200809L
#include "../userland/libhtml/internal.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
static void safety_fail(const char *reason)
{
    fprintf(stderr, "html safety: %s (allocation %zu, fail_at %zu)\n", reason, allocations,
            fail_at);
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
    size_t top = 0, visited = 0;
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
        if (n->form_owner &&
            (n->kind != OS64_HTML_ELEMENT || n->ns != OS64_HTML_NS_HTML ||
             n->form_owner->kind != OS64_HTML_ELEMENT ||
             n->form_owner->ns != OS64_HTML_NS_HTML ||
             n->form_owner->tag != OS64_HTML_TAG_FORM))
            safety_fail("invalid parser form owner");
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
    return hash;
}
static uint32_t random_next(uint32_t *seed)
{
    *seed ^= *seed << 13;
    *seed ^= *seed >> 17;
    *seed ^= *seed << 5;
    return *seed;
}
static os64_html_document_t *parse_raw(const unsigned char *data, size_t len, size_t chunk,
                                       os64_html_options_t *options)
{
    os64_html_parser_t *p = os64_html_parser_new(options);
    if (!p)
        return NULL;
    uint32_t seed = 0x64a11u;
    for (size_t at = 0; at < len;) {
        size_t amount = chunk == SIZE_MAX ? 1 + random_next(&seed) % 37 : chunk ? chunk : len;
        if (amount > len - at)
            amount = len - at;
        int64_t status = os64_html_parser_feed(p, data + at, amount);
        at += amount;
        if (status) {
            if (os64_html_parser_feed(p, data + at, len - at) != status)
                safety_fail("refusal resumed");
            break;
        }
    }
    return os64_html_parser_finish(p);
}
static size_t safety_prefixes, safety_failures, safety_cases;
static void safety_case(const unsigned char *data, size_t len)
{
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = "utf-8";
    allocations = 0;
    fail_at = 0;
    os64_html_document_t *doc = parse_raw(data, len, 0, &opt);
    if (!doc || doc->refusal)
        safety_fail("ordinary reference refused");
    uint64_t hash = validate(doc);
    size_t count = allocations;
    uint64_t work = doc->work;
    os64_html_document_free(doc);
    if (live)
        safety_fail("baseline leak");
    for (unsigned c = 0; c < 2; c++) {
        doc = parse_raw(data, len, c ? SIZE_MAX : 1, &opt);
        if (!doc || doc->refusal || validate(doc) != hash || doc->work != work)
            safety_fail("feed chunk changed document/work");
        os64_html_document_free(doc);
        if (live)
            safety_fail("chunk leak");
    }
    for (size_t n = 1; n <= count; n++) {
        allocations = 0;
        fail_at = n;
        doc = parse_raw(data, len, 1, &opt);
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
    fail_at = 0;
    for (size_t cut = 0; cut < len; cut++) {
        opt.max_bytes = cut;
        doc = parse_raw(data, cut, 0, &opt);
        if (!doc || doc->refusal)
            safety_fail("standalone prefix refused");
        hash = validate(doc);
        os64_html_document_free(doc);
        for (unsigned c = 0; c < 3; c++) {
            doc = parse_raw(data, len, c == 0 ? 0 : c == 1 ? 1 : SIZE_MAX, &opt);
            if (!doc || doc->refusal != OS64_HTML_TOO_LARGE || !doc->truncated ||
                doc->input_bytes != cut || validate(doc) != hash)
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
    uint64_t hash = 0, work = 0;
    int64_t refusal = 0;
    for (unsigned c = 0; c < 3; c++) {
        struct timespec begin, finish;
        clock_gettime(CLOCK_MONOTONIC, &begin);
        os64_html_document_t *doc = parse_raw(data, len, c == 0 ? 0 : c == 1 ? 1 : SIZE_MAX, opt);
        clock_gettime(CLOCK_MONOTONIC, &finish);
        if (!doc)
            safety_fail("bounded constructor failed");
        uint64_t current = validate(doc);
        if (doc->work > opt->max_work || doc->peak_arena_bytes > opt->max_arena_bytes)
            safety_fail("budget overrun");
        if (c && (current != hash || doc->work != work || doc->refusal != refusal))
            safety_fail("bounded feed changed result");
        if (!c) {
            hash = current;
            work = doc->work;
            refusal = doc->refusal;
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
static void check(bool ok, const char *fmt, ...)
{
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
    safety_case((const unsigned char *)markup, strlen(markup));
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
     * attribute and root walks; refusal must leave a freeable partial tree. */
    const char *markup = "<form id=f><div><span><input id=q a=1 b=2 c=3>";
    doc = parse_raw((const unsigned char *)markup, strlen(markup), 0, &opt);
    check(doc && !doc->refusal, "work sweep baseline");
    uint64_t work = doc ? doc->work : 0;
    os64_html_document_free(doc);
    for (uint64_t budget = 0; budget <= work; budget++) {
        opt.max_work = budget;
        bounded_case((const unsigned char *)markup, strlen(markup), &opt,
                     budget < work ? OS64_HTML_WORK_EXHAUSTED : OS64_HTML_OK,
                     NULL);
    }
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
int main(int argc, char **argv)
{
    bool safety = argc > 1 && strcmp(argv[1], "--safety") == 0;
    bool fuzz = argc > 1 && strcmp(argv[1], "--fuzz") == 0;
    if (argc > 1 && strcmp(argv[1], "--stress") == 0) {
        stress();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--checks") == 0) {
        encoding_exports();
        form_owners();
        printf("html checks: %zu run, %zu failed\n", checks_run, checks_failed);
        return checks_failed ? 1 : 0;
    }
    uint32_t fuzz_seed = 0x64f022u;
    size_t fuzz_cases = 0;
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
        if (fuzz) {
            os64_html_options_t opt = os64_html_options_default();
            opt.charset = random_next(&fuzz_seed) & 1 ? "utf-8" : NULL;
            opt.max_work = 100 + random_next(&fuzz_seed) % 20000;
            opt.max_depth = 1 + random_next(&fuzz_seed) % 100;
            opt.max_arena_bytes = 16384 + random_next(&fuzz_seed) % 131072;
            bounded_case(data, hdr[2], &opt, INT64_MAX, NULL);
            fuzz_cases++;
            free(last);
            free(data);
            continue;
        }
        if (safety) {
            safety_case(data, hdr[2]);
            free(last);
            free(data);
            continue;
        }
        allocations = 0;
        fail_at = 0;
        struct timespec begin, finish;
        clock_gettime(CLOCK_MONOTONIC, &begin);
        os64_html_options_t options = os64_html_options_default();
        options.charset = hdr[0] == 1 ? "utf-8" : hdr[3] ? last : NULL;
        os64_html_parser_t *p = os64_html_parser_new(&options);
        if (!p)
            return 2;
        if (hdr[0] == 0) {
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
                os64_html_parser_feed(p, data + i, chunk);
                i += chunk;
            }
            fputs("{", stdout);
        }
        os64_html_document_t *doc = os64_html_parser_finish(p);
        clock_gettime(CLOCK_MONOTONIC, &finish);
        if (hdr[0] != 0) {
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
        }
        double elapsed = finish.tv_sec - begin.tv_sec + (finish.tv_nsec - begin.tv_nsec) / 1e9;
        printf("\"seconds\":%.9f,\"work\":%llu,\"arena\":%zu,\"nodes\":%zu,", elapsed,
               (unsigned long long)doc->work, doc->peak_arena_bytes, doc->node_count);
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
    if (fuzz)
        printf("Fuzz: cases=%zu chunkings=3 budget-seed=0x64f022\n", fuzz_cases);
    if (safety)
        printf("Safety: cases=%zu allocation-failures=%zu prefix/chunk checks=%zu seed=0x64a11\n",
               safety_cases, safety_failures, safety_prefixes);
    return ferror(stdin) ? 2 : 0;
}
