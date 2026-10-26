#include "garb/cascade.h"
#include "garb/select.h"
#include "html/xml.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union {
    max_align_t align;
    size_t size;
} allocation;
static size_t calls, fail_at, live, live_bytes;
void *os64_malloc(size_t n)
{
    if (++calls == fail_at)
        return NULL;
    assert(n <= SIZE_MAX - sizeof(allocation));
    allocation *p = malloc(sizeof(*p) + n);
    if (!p)
        return NULL;
    p->size = n;
    ++live;
    live_bytes += n;
    return p + 1;
}
void os64_free(void *p)
{
    if (!p)
        return;
    allocation *a = (allocation *)p - 1;
    assert(live && live_bytes >= a->size);
    --live;
    live_bytes -= a->size;
    free(a);
}
void *os64_calloc(size_t n, size_t size)
{
    if (size && n > SIZE_MAX / size)
        return NULL;
    void *p = os64_malloc(n * size);
    if (p)
        memset(p, 0, n * size);
    return p;
}
void *os64_realloc(void *p, size_t size)
{
    if (!size) {
        os64_free(p);
        return NULL;
    }
    void *next = os64_malloc(size);
    if (!next)
        return NULL;
    if (p) {
        size_t old = ((allocation *)p - 1)->size;
        memcpy(next, p, old < size ? old : size);
        os64_free(p);
    }
    return next;
}
int64_t os64_write(int32_t fd, const void *s, size_t n)
{
    return (int64_t)fwrite(s, 1, n, fd == 2 ? stderr : stdout);
}
void os64_exit(int32_t code)
{
    fprintf(stderr, "fatal %d\n", code);
    abort();
}

static const char sample[] =
    "<?xml version='1.0'?><?outside data?><!--hello-->"
    "<!DOCTYPE s:svg SYSTEM 'must-not-fetch.dtd' [<!ENTITY ns "
    "'http://www.w3.org/2000/svg'><!--inside--><?hidden?>]>"
    "<s:svg xmlns:s='&ns;' xmlns:x='urn:attrs' id='root'>"
    "<s:style><![CDATA[:root { --ink: red; opacity: .8 } .shape { color: var(--ink) } ]]></s:style>"
    "<s:g><s:rect id='child' class='shape' x:Flag='yes' "
    "plain='v'>a<![CDATA[b]]>&amp;c</s:rect></s:g>"
    "<n xmlns='urn:custom' x:Flag='same'/><n xmlns='urn:other'/><n/>"
    "</s:svg><?after end?>";
static os64_html_document_t *parse(const char *text, size_t chunk,
                                   const os64_html_xml_options_t *opt, os64_html_xml_result_t *r)
{
    size_t baseline = live_bytes;
    os64_html_xml_parser_t *p = os64_html_xml_parser_new(opt, r);
    if (!p)
        return NULL;
    size_t n = strlen(text), pos = 0;
    while (pos < n && r->status == XML_NEED_INPUT) {
        size_t take = chunk ? chunk : 1 + (pos * 7) % 17;
        if (take > n - pos)
            take = n - pos;
        char *copy = malloc(take);
        assert(copy);
        memcpy(copy, text + pos, take);
        size_t consumed = SIZE_MAX;
        xml_status_t status = os64_html_xml_parser_feed(p, copy, take, &consumed);
        memset(copy, 0, take);
        free(copy);
        assert(consumed <= take);
        if (status == XML_NEED_INPUT)
            assert(consumed == take);
        pos += consumed;
        *r = os64_html_xml_parser_result(p);
        assert(r->status == status && r->arena_bytes == live_bytes - baseline);
        assert(r->parser.live_bytes <= r->arena_bytes && r->peak_arena_bytes >= r->arena_bytes);
    }
    os64_html_document_t *doc = os64_html_xml_parser_finish(p, r);
    assert((doc != NULL) == (r->status == XML_DONE));
    if (doc)
        assert(doc->arena_bytes == live_bytes - baseline);
    else
        assert(live_bytes == baseline);
    return doc;
}
static const os64_html_node_t *element(const os64_html_node_t *parent, size_t index)
{
    for (const os64_html_node_t *n = parent->first_child; n; n = n->next)
        if (n->kind == OS64_HTML_ELEMENT && !index--)
            return n;
    return NULL;
}
static void tree_checks(os64_html_document_t *d)
{
    assert(d->is_xml && !d->html && !d->head && !d->body && !d->refusal);
    const os64_html_node_t *root = os64_html_document_element(d);
    assert(root && root->parent == d->document && root->ns == OS64_HTML_NS_SVG);
    assert(!strcmp(root->name, "svg") && !strcmp(root->prefix, "s"));
    assert(!strcmp(os64_html_namespace_uri(root), "http://www.w3.org/2000/svg"));
    assert(root->xml_document && os64_html_owns_node(d, root));
    const os64_html_attr_t *ns = os64_html_attr(root, "xmlns:s");
    assert(ns && !strcmp(ns->ns, "http://www.w3.org/2000/xmlns/") &&
           !strcmp(ns->value, os64_html_namespace_uri(root)));
    assert(d->document->first_child->kind == OS64_HTML_PI &&
           !strcmp(d->document->first_child->name, "outside"));
    assert(!strcmp(d->document->first_child->text, "data"));
    size_t siblings = 0;
    for (const os64_html_node_t *n = d->document->first_child; n; n = n->next)
        ++siblings;
    assert(siblings == 5); /* PI, comment, DOCTYPE, element, PI; no DTD children. */
    const os64_html_node_t *rect = element(element(root, 1), 0);
    assert(!strcmp(rect->name, "rect") && rect->first_child == rect->last_child);
    assert(rect->first_child->kind == OS64_HTML_TEXT && !strcmp(rect->first_child->text, "ab&c"));
    assert(!strcmp(os64_html_attr(rect, "x:Flag")->ns, "urn:attrs"));
    assert(element(root, 2)->ns == OS64_HTML_NS_OTHER &&
           !strcmp(os64_html_namespace_uri(element(root, 2)), "urn:custom"));
    assert(element(root, 3)->ns == OS64_HTML_NS_OTHER &&
           !strcmp(os64_html_namespace_uri(element(root, 3)), "urn:other"));
    assert(element(root, 4)->ns == OS64_HTML_NS_NONE && !os64_html_namespace_uri(element(root, 4)));
    char serialized[2048];
    size_t length = os64_html_serialize(d->document, false, false, serialized, sizeof(serialized));
    assert(length < sizeof(serialized));
    /* The existing API serializes HTML, including its PI closing syntax. */
    assert(strstr(serialized, "<?outside data>") && strstr(serialized, "<s:svg ") &&
           strstr(serialized, "</s:svg>") && strstr(serialized, "x:Flag=\"yes\""));
}
static bool matches(const char *selector, const os64_html_node_t *n)
{
    garb_parsed_t p;
    assert(garb_parse_sheet_text(selector, strlen(selector), &p) == GARB_OK);
    assert(p.nitems == 1);
    const garb_rule_t *rule = p.items[0].rule;
    garb_selectors_t *sel =
        garb_selectors_parse(&p, rule->prelude, rule->nprelude, OS64_HTML_NO_QUIRKS);
    assert(sel && garb_selectors_count(sel) == 1);
    bool answer = garb_selector_matches(sel, 0, n);
    garb_free(&p);
    return answer;
}
static void cascade_checks(os64_html_document_t *d)
{
    const os64_html_node_t *root = os64_html_document_element(d), *style = element(root, 0),
                           *rect = element(element(root, 1), 0);
    assert(matches(":root {}", root) && !matches(":root {}", rect));
    assert(matches("svg {}", root) && !matches("SVG {}", root));
    assert(matches("[*|Flag] {}", rect) && !matches("[Flag] {}", rect));
    assert(!matches("[*|flag] {}", rect) && matches("[plain] {}", rect));
    assert(matches("|n {}", element(root, 4)) && !matches("|n {}", element(root, 2)));
    assert(matches("n:first-of-type {}", element(root, 2)) &&
           matches("n:first-of-type {}", element(root, 3)));
    assert(matches("|* {}", element(root, 4)) && !matches("|* {}", root));
    garb_parsed_t sheet;
    assert(garb_parse_style_element(style, &sheet) == GARB_OK);
    garb_sheet_in_t input = {.sheet = &sheet};
    garb_cascade_t *cascade = garb_cascade(&input, 1, d, (garb_env_t){.width = 640, .height = 480});
    assert(cascade && !garb_cascade_incomplete(cascade));
    char out[2048];
    garb_cascade_dump(cascade, d, out, sizeof(out));
    assert(strstr(out, "svg#root: opacity: 0.8") && strstr(out, "rect#child: color:"));
    garb_style_t child = garb_style_for(cascade, rect);
    assert(child.n == 1 && child.sets[0].prop == GARB_COLOR);
    const garb_val_t *ink = &child.sets[0].value;
    assert(ink->kind == GARB_V_COLOR && !ink->color.current && ink->color.r == 255 &&
           ink->color.g == 0 && ink->color.b == 0 && ink->color.a == 1);
    garb_cascade_free(cascade);
    garb_free(&sheet);
}
static void mutation_checks(os64_html_document_t *source)
{
    os64_html_xml_result_t result;
    os64_html_document_t *dest = parse("<r/>", 1, NULL, &result);
    assert(dest);
    int64_t why;
    os64_html_node_t *clone = os64_html_clone(dest, os64_html_document_element(source), true, &why);
    assert(clone && !why && !os64_html_owns_node(source, clone));
    os64_html_node_t *pi = os64_html_clone(dest, source->document->first_child, false, &why);
    assert(pi && !why);
    os64_html_document_free(source);
    assert(!strcmp(pi->name, "outside") && !strcmp(pi->text, "data"));
    const os64_html_node_t *rect = element(element(clone, 1), 0);
    assert(!strcmp(os64_html_attr(rect, "x:Flag")->ns, "urn:attrs"));
    assert(!strcmp(element(clone, 2)->namespace_uri, "urn:custom"));
    assert(!strcmp(clone->prefix, "s"));
    os64_html_node_t *old = (os64_html_node_t *)os64_html_document_element(dest);
    assert(os64_html_replace(dest, dest->document, clone, old) == OS64_HTML_OK);
    assert(os64_html_document_element(dest) == clone && !dest->html);
    assert(os64_html_insert(dest, dest->document, pi, clone) == OS64_HTML_OK);
    os64_html_hold(dest, clone);
    os64_html_pin_t pin = os64_html_pin(dest);
    assert(pin);
    assert(os64_html_remove(dest, clone) == OS64_HTML_OK && !os64_html_document_element(dest));
    assert(!strcmp(rect->name, "rect"));
    os64_html_unpin(dest, pin);
    assert(os64_html_insert(dest, dest->document, clone, NULL) == OS64_HTML_OK);
    os64_html_release(dest, clone);
    const os64_html_attr_t *before = os64_html_attr(rect, "x:Flag");
    pin = os64_html_pin(dest);
    assert(os64_html_set_attr(dest, (os64_html_node_t *)rect, "x:Flag", "new", 3) == OS64_HTML_OK);
    assert(!strcmp(before->value, "yes") && !strcmp(before->ns, "urn:attrs"));
    os64_html_unpin(dest, pin);
    assert(!strcmp(os64_html_attr(rect, "x:Flag")->ns, "urn:attrs"));
    os64_html_document_free(dest);
}
typedef struct {
    os64_html_xml_parser_t *parser;
    size_t polls, stop;
} cancel_state;
static bool stop(void *context)
{
    cancel_state *state = context;
    if (state->parser) {
        size_t consumed = 10;
        assert(os64_html_xml_parser_feed(state->parser, NULL, 0, &consumed) == XML_BAD_ARGUMENT &&
               !consumed);
        os64_html_xml_result_t r;
        assert(!os64_html_xml_parser_finish(state->parser, &r) && r.status == XML_BAD_ARGUMENT);
        os64_html_xml_parser_destroy(state->parser); /* Busy handle must stay owned. */
    }
    return ++state->polls >= state->stop;
}
static void api_checks(void)
{
    os64_html_xml_result_t r;
    os64_html_xml_parser_t *a = os64_html_xml_parser_new(NULL, &r),
                           *b = os64_html_xml_parser_new(NULL, NULL);
    assert(a && b);
    size_t consumed;
    assert(os64_html_xml_parser_feed(a, NULL, 1, &consumed) == XML_BAD_ARGUMENT && !consumed);
    assert(os64_html_xml_parser_feed(a, NULL, 0, &consumed) == XML_NEED_INPUT && !consumed);
    const char *sa = "<a xmlns='A'/>", *sb = "<b xmlns='B'/>";
    for (size_t i = 0; i < strlen(sa); ++i) {
        assert(os64_html_xml_parser_feed(a, sa + i, 1, &consumed) == XML_NEED_INPUT &&
               consumed == 1);
        assert(os64_html_xml_parser_feed(b, sb + i, 1, &consumed) == XML_NEED_INPUT &&
               consumed == 1);
    }
    os64_html_document_t *da = os64_html_xml_parser_finish(a, &r),
                         *db = os64_html_xml_parser_finish(b, NULL);
    assert(da && db && !os64_html_owns_node(da, os64_html_document_element(db)));
    os64_html_document_free(da);
    assert(!strcmp(os64_html_namespace_uri(os64_html_document_element(db)), "B"));
    os64_html_document_free(db);
    assert(!live);
    a = os64_html_xml_parser_new(NULL, &r);
    assert(a);
    const char *label = "<?xml version='1.0' encoding='Latin-1'?><r/>";
    assert(os64_html_xml_parser_feed(a, label, strlen(label), &consumed) ==
           XML_UNSUPPORTED_ENCODING);
    xml_slice_t encoding = os64_html_xml_parser_encoding(a);
    assert(encoding.size == 7 && !memcmp(encoding.data, "Latin-1", 7));
    assert(!os64_html_xml_parser_finish(a, &r) && r.status == XML_UNSUPPORTED_ENCODING && !live);
    a = os64_html_xml_parser_new(NULL, NULL);
    assert(a);
    assert(os64_html_xml_parser_feed(a, "<r>partial", 10, &consumed) == XML_NEED_INPUT);
    os64_html_xml_parser_destroy(a);
    assert(!live);
    for (size_t i = 1; i < 80; ++i) {
        cancel_state state = {.stop = i};
        os64_html_xml_options_t opt = os64_html_xml_options_default();
        opt.context = &state;
        opt.cancelled = stop;
        state.parser = os64_html_xml_parser_new(&opt, &r);
        assert(state.parser);
        assert(os64_html_xml_parser_feed(state.parser, sample, strlen(sample), &consumed) ==
               XML_CANCELLED);
        assert(!os64_html_xml_parser_finish(state.parser, &r) && r.status == XML_CANCELLED &&
               !live);
    }
    os64_html_xml_options_t opt = os64_html_xml_options_default();
    opt.parser_limits.depth = 2;
    da = parse("<a><b>text</b></a>", 1, &opt, &r);
    db = parse("<r/>", 1, &opt, &r);
    assert(da && db);
    int64_t why;
    os64_html_node_t *copy = os64_html_clone(db, os64_html_document_element(da), true, &why);
    assert(copy && !why);
    assert(os64_html_replace(db, db->document, copy,
                             (os64_html_node_t *)os64_html_document_element(db)) == OS64_HTML_OK);
    assert(!os64_html_parse_fragment(db, copy, "<x/>", 4, false, &why) &&
           why == OS64_HTML_BAD_ARGUMENT);
    os64_html_document_free(da);
    os64_html_document_free(db);
    assert(!live);
    da = parse("<h:Div xmlns:h='http://www.w3.org/1999/xhtml' xmlns:x='urn:attr' "
               "Case='x' type='A' x:Flag='yes'/>",
               1, NULL, &r);
    assert(da);
    const os64_html_node_t *root = os64_html_document_element(da);
    assert(matches("Div {}", root) && !matches("div {}", root));
    assert(matches("[Case] {}", root) && !matches("[case] {}", root));
    assert(!matches("[type=a] {}", root) && matches("[type=a i] {}", root) &&
           !matches("[type=a s] {}", root));
    os64_html_parser_t *html = os64_html_parser_new(NULL);
    assert(html);
    db = os64_html_parser_finish(html);
    assert(db && !db->refusal);
    copy = os64_html_clone(db, root, true, &why);
    assert(copy && !why && !copy->xml_document);
    assert(matches("[type=a] {}", copy) && matches("[type=a i] {}", copy) &&
           !matches("[type=a s] {}", copy));
    assert(matches("[*|Flag] {}", copy) && !matches("[*|flag] {}", copy));
    os64_html_document_free(db);
    os64_html_document_free(da);
    assert(!live);
}
int main(void)
{
    api_checks();
    os64_html_xml_result_t r;
    for (size_t chunk = 0; chunk < 3; ++chunk) {
        os64_html_document_t *d = parse(sample, chunk == 2 ? strlen(sample) : chunk, NULL, &r);
        assert(d);
        tree_checks(d);
        cascade_checks(d);
        mutation_checks(d);
        assert(!live);
    }
    calls = 0;
    os64_html_document_t *d = parse(sample, 1, NULL, &r);
    assert(d);
    size_t allocations = calls;
    os64_html_document_free(d);
    for (size_t i = 1; i <= allocations; ++i) {
        calls = 0;
        fail_at = i;
        d = parse(sample, 1, NULL, &r);
        assert(!d && r.status == XML_NO_MEMORY && !live);
    }
    fail_at = 0;
    const char *bad[] = {"", "<r>", "<r/><s/>", "<r><x></r>", "<r p:a='1'/>", "<r>abc</r>trailing"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        assert(!parse(bad[i], 1, NULL, &r) && r.status == XML_MALFORMED && !live);
    }
    os64_html_xml_options_t opt = os64_html_xml_options_default();
    opt.max_arena_bytes = 1;
    assert(!parse(sample, 1, &opt, &r) && r.status == XML_LIMIT && !live);
    opt = os64_html_xml_options_default();
    opt.max_tree_work = 20;
    assert(!parse(sample, 1, &opt, &r) && r.status == XML_LIMIT &&
           r.tree_status == OS64_HTML_WORK_EXHAUSTED && !live);
    opt = os64_html_xml_options_default();
    opt.parser_limits.depth = 1;
    assert(!parse(sample, 1, &opt, &r) && r.status == XML_LIMIT && !live);
    puts("XML shared tree: ownership, namespaces, CDATA, PI, cascade, mutation, limits, allocation "
         "sweep passed");
    return 0;
}
