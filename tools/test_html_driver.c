#define _POSIX_C_SOURCE 200809L
#include "../userland/libhtml/internal.h"
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
                safety_fail(name);
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
