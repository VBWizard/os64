// test_html_dom_host.c — libhtml's mutation verbs, on the host (DOM.md, D1).
//
// Four kinds of proof, in the order main runs them:
//   - cases written by hand, for each rule a verb enforces;
//   - what a verb does when memory runs out at each allocation it makes;
//   - pins and retirement, by what stays readable and what is freed;
//   - a long random walk checked, step by step, against a second tree kept
//     in this file by different code (the R model), which predicts every
//     verb's answer and every link before the library is asked.
#define _POSIX_C_SOURCE 200809L
#include "../userland/libhtml/internal.h"
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ── What the library asks of the system ─────────────────────────────────

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
static jmp_buf death_landing;
static bool expecting_death;
static int32_t death_code;
static char death_said[256];
int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    if (handle == 2 && expecting_death) {
        size_t take = len < sizeof(death_said) - 1 ? len : sizeof(death_said) - 1;
        memcpy(death_said, buf, take);
        death_said[take] = 0;
        return (int64_t)len;
    }
    return (int64_t)fwrite(buf, 1, len, handle == 2 ? stderr : stdout);
}
void os64_exit(int32_t code)
{
    if (expecting_death) {
        death_code = code;
        longjmp(death_landing, 1);
    }
    exit(3);
}

static size_t checks_run, checks_failed;
__attribute__((format(printf, 2, 3))) static void check(bool ok, const char *fmt, ...)
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
    if (checks_failed > 40) {
        fputs("too many failures\n", stderr);
        exit(1);
    }
}

// ── Reading a real tree ─────────────────────────────────────────────────

static os64_html_document_t *parse_with(const char *markup, const os64_html_options_t *options)
{
    os64_html_options_t opt = options ? *options : os64_html_options_default();
    opt.charset = "utf-8";
    os64_html_parser_t *p = os64_html_parser_new(&opt);
    if (!p)
        return NULL;
    os64_html_parser_feed(p, markup, strlen(markup));
    return os64_html_parser_finish(p);
}
static os64_html_document_t *parse(const char *markup)
{
    return parse_with(markup, NULL);
}
static HNode *by_id(HNode *n, const char *id)
{
    for (; n; n = n->next) {
        const HAttr *a = os64_html_attr(n, "id");
        if (a && strcmp(a->value, id) == 0)
            return n;
        HNode *found = by_id(n->first_child, id);
        if (!found && n->template_contents)
            found = by_id(n->template_contents->first_child, id);
        if (found)
            return found;
    }
    return NULL;
}
static HNode *id_in(os64_html_document_t *doc, const char *id)
{
    return by_id(doc->document, id);
}
// A subtree as text: names, attributes in LIST order, text, a template's
// contents, and for a control the id of the form it is tied to.
static void spell(FILE *out, const HNode *n)
{
    switch (n->kind) {
    case OS64_HTML_DOCUMENT:
        fputs("#document", out);
        break;
    case OS64_HTML_FRAGMENT:
        fputs("#fragment", out);
        break;
    case OS64_HTML_DOCTYPE:
        fprintf(out, "<!%s>", n->name ? n->name : "");
        return;
    case OS64_HTML_TEXT:
        fprintf(out, "\"%s\"", n->text);
        return;
    case OS64_HTML_COMMENT:
        fprintf(out, "<!--%s-->", n->text);
        return;
    case OS64_HTML_ELEMENT:
        fprintf(out, "<%s%s", n->ns == OS64_HTML_NS_SVG ? "svg:" : n->ns ? "math:" : "", n->name);
        for (const HAttr *a = n->attrs; a; a = a->next)
            fprintf(out, " %s='%s'", a->name, a->value);
        if (n->form_owner) {
            const HAttr *id = os64_html_attr(n->form_owner, "id");
            fprintf(out, " ^%s", id ? id->value : "?");
        }
        fputc('>', out);
        break;
    }
    if (n->template_contents) {
        fputc('{', out);
        for (const HNode *c = n->template_contents->first_child; c; c = c->next)
            spell(out, c);
        fputc('}', out);
    }
    if (n->first_child) {
        fputc('[', out);
        for (const HNode *c = n->first_child; c; c = c->next)
            spell(out, c);
        fputc(']', out);
    }
}
static char *spelled(const HNode *n)
{
    char *text = NULL;
    size_t len = 0;
    FILE *out = open_memstream(&text, &len);
    if (!out)
        exit(2);
    spell(out, n);
    fclose(out);
    return text;
}
static void expect_tree(const HNode *n, const char *want, const char *what)
{
    char *got = spelled(n);
    check(strcmp(got, want) == 0, "%s:\n   got  %s\n   want %s", what, got, want);
    free(got);
}
// Every link under `n` agrees with its neighbours, and no node is deeper
// than the document allows. Returns how many records the subtree holds.
static size_t links_ok(const HDoc *d, const HNode *n, size_t depth)
{
    size_t records = n->form_owner != NULL;
    if (depth > h_depth_limit(d))
        check(false, "node at depth %zu, limit %zu", depth, h_depth_limit(d));
    if ((n->kind == OS64_HTML_TEXT || n->kind == OS64_HTML_COMMENT) &&
        (!n->text || strlen(n->text) != n->text_len))
        check(false, "text length disagrees with its bytes");
    const HNode *prev = NULL;
    for (const HNode *c = n->first_child; c; prev = c, c = c->next) {
        if (c->parent != n || c->prev != prev)
            check(false, "broken parent or sibling link under <%s>", n->name ? n->name : "#");
        records += links_ok(d, c, depth + 1);
    }
    if (n->last_child != prev)
        check(false, "last_child disagrees under <%s>", n->name ? n->name : "#");
    if (n->template_contents) {
        const HNode *f = n->template_contents;
        if (f->kind != OS64_HTML_FRAGMENT || f->parent || (const HNode *)*h_word(f) != n)
            check(false, "template contents do not know their template");
        records += links_ok(d, f, depth);
    }
    return records;
}

// ── Cases by hand ───────────────────────────────────────────────────────

#define PAGE "<!doctype html><html><head><title>t</title></head><body>" \
             "<div id=a><span id=b>one</span></div><p id=c>two</p><!--note--></body></html>"

static void t_validity(void)
{
    os64_html_document_t *doc = parse(PAGE);
    HNode *root = doc->document, *html = doc->html, *a = id_in(doc, "a"), *b = id_in(doc, "b");
    HNode *c = id_in(doc, "c"), *doctype = root->first_child;
    int64_t st = 99;
    HNode *text = os64_html_create_text(doc, "x", 1, &st);
    HNode *comment = os64_html_create_comment(doc, "k", 1, NULL);
    HNode *div = os64_html_create_element(doc, OS64_HTML_NS_HTML, "div", NULL);
    HNode *html2 = os64_html_create_element(doc, OS64_HTML_NS_HTML, "html", NULL);
    HNode *svg_html = os64_html_create_element(doc, OS64_HTML_NS_SVG, "html", NULL);
    HNode *doctype2 = os64_html_clone(doc, doctype, false, NULL);
    check(st == OS64_HTML_OK && text && comment && div && html2 && svg_html && doctype2,
          "detached nodes made");
    // These pointers are reused after successful detachments below.
    os64_html_hold(doc, html); os64_html_hold(doc, text); os64_html_hold(doc, doctype);
    char *before = spelled(root);
    uint64_t version = os64_html_version(doc);

    struct {
        const char *what;
        int64_t got, want;
    } refused[] = {
        {"text under the document", os64_html_insert(doc, root, text, NULL), OS64_HTML_HIERARCHY},
        {"a doctype under an element", os64_html_insert(doc, a, doctype2, NULL), OS64_HTML_HIERARCHY},
        {"a node under itself", os64_html_insert(doc, a, a, NULL), OS64_HTML_HIERARCHY},
        {"an ancestor under its descendant", os64_html_insert(doc, b, a, NULL), OS64_HTML_HIERARCHY},
        {"the document under an element", os64_html_insert(doc, a, root, NULL), OS64_HTML_HIERARCHY},
        {"a child under a text node", os64_html_insert(doc, b->first_child, div, NULL), OS64_HTML_HIERARCHY},
        {"before a node that is not a child", os64_html_insert(doc, a, div, c), OS64_HTML_NOT_FOUND},
        {"a second element under the document", os64_html_insert(doc, root, html2, NULL), OS64_HTML_HIERARCHY},
        {"a second doctype", os64_html_insert(doc, root, doctype2, doctype), OS64_HTML_HIERARCHY},
        {"removing the html element", os64_html_remove(doc, html), OS64_HTML_ROOT_REQUIRED},
        {"moving the html element into a new div", os64_html_insert(doc, div, html, NULL), OS64_HTML_ROOT_REQUIRED},
        {"the html element under its own body", os64_html_insert(doc, doc->body, html, NULL), OS64_HTML_HIERARCHY},
        {"replacing html with a comment", os64_html_replace(doc, root, comment, html), OS64_HTML_ROOT_REQUIRED},
        {"replacing html with a div", os64_html_replace(doc, root, div, html), OS64_HTML_ROOT_REQUIRED},
        {"replacing html with an SVG element called html", os64_html_replace(doc, root, svg_html, html), OS64_HTML_ROOT_REQUIRED},
        {"replacing a node that is not a child", os64_html_replace(doc, a, div, c), OS64_HTML_NOT_FOUND},
        {"replacing the doctype with a second element", os64_html_replace(doc, root, html2, doctype), OS64_HTML_HIERARCHY},
        {"no parent", os64_html_insert(doc, NULL, div, NULL), OS64_HTML_BAD_ARGUMENT},
        {"no node", os64_html_insert(doc, a, NULL, NULL), OS64_HTML_BAD_ARGUMENT},
        {"no document", os64_html_insert(NULL, a, div, NULL), OS64_HTML_BAD_ARGUMENT},
        {"replace with nothing to replace", os64_html_replace(doc, a, div, NULL), OS64_HTML_BAD_ARGUMENT},
        {"an attribute on a text node", os64_html_set_attr(doc, text, "x", "y", 1), OS64_HTML_BAD_ARGUMENT},
        {"an attribute with no name", os64_html_set_attr(doc, a, "", "y", 1), OS64_HTML_BAD_ARGUMENT},
        {"an attribute value with a NUL", os64_html_set_attr(doc, a, "x", "a\0b", 3), OS64_HTML_BAD_TEXT},
        {"an attribute value cut mid-character", os64_html_set_attr(doc, a, "x", "\xc3", 1), OS64_HTML_BAD_TEXT},
        {"an attribute name that is not UTF-8", os64_html_set_attr(doc, a, "\xff", "v", 1), OS64_HTML_BAD_TEXT},
        {"text with a surrogate", os64_html_set_text(doc, text, "\xed\xa0\x80", 3), OS64_HTML_BAD_TEXT},
        {"text with an overlong form", os64_html_set_text(doc, text, "\xc0\xaf", 2), OS64_HTML_BAD_TEXT},
        {"text beyond U+10FFFF", os64_html_set_text(doc, text, "\xf4\x90\x80\x80", 4), OS64_HTML_BAD_TEXT},
        {"text on an element", os64_html_set_text(doc, a, "x", 1), OS64_HTML_BAD_ARGUMENT},
    };
    for (size_t i = 0; i < H_ARRAY(refused); i++)
        check(refused[i].got == refused[i].want, "%s: %s, want %s", refused[i].what,
              os64_html_status_name(refused[i].got), os64_html_status_name(refused[i].want));
    st = 0;
    check(!os64_html_create_text(doc, "a\0b", 3, &st) && st == OS64_HTML_BAD_TEXT, "text node with a NUL");
    check(!os64_html_create_element(doc, OS64_HTML_NS_HTML, "", &st) && st == OS64_HTML_BAD_ARGUMENT,
          "element with no name");
    check(!os64_html_create_element(doc, OS64_HTML_NS_HTML, "\x80", &st) && st == OS64_HTML_BAD_TEXT,
          "element name that is not UTF-8");
    check(!os64_html_clone(doc, root, true, &st) && st == OS64_HTML_BAD_ARGUMENT, "cloning the document");

    // The document's own element and doctype, put under it again. Each is
    // what Chrome answers: appendChild and insertBefore throw "Only one
    // element (doctype) on document allowed", replaceChild with itself is
    // quiet.
    struct {
        const char *what;
        int64_t got, want;
    } again[] = {
        {"the html element appended to its document", os64_html_insert(doc, root, html, NULL), OS64_HTML_HIERARCHY},
        {"the html element before itself", os64_html_insert(doc, root, html, html), OS64_HTML_HIERARCHY},
        {"the doctype before the html element", os64_html_insert(doc, root, doctype, html), OS64_HTML_HIERARCHY},
        {"the doctype before itself", os64_html_insert(doc, root, doctype, doctype), OS64_HTML_HIERARCHY},
        {"the html element in place of itself", os64_html_replace(doc, root, html, html), OS64_HTML_OK},
        {"the doctype in place of itself", os64_html_replace(doc, root, doctype, doctype), OS64_HTML_OK},
    };
    for (size_t i = 0; i < H_ARRAY(again); i++)
        check(again[i].got == again[i].want, "%s: %s, want %s", again[i].what,
              os64_html_status_name(again[i].got), os64_html_status_name(again[i].want));

    char *after = spelled(root);
    check(strcmp(before, after) == 0, "refused verbs changed the tree:\n   %s\n   %s", before, after);
    check(os64_html_version(doc) == version, "refused verbs moved the version");
    check(doc->refusal == 0, "a refused verb touched the parse's refusal");
    free(before);
    free(after);

    // Fragments under the document, by what they hold.
    HNode *frag = os64_html_create_fragment(doc, NULL);
    os64_html_hold(doc, frag);
    os64_html_insert(doc, frag, os64_html_create_comment(doc, "1", 1, NULL), NULL);
    os64_html_insert(doc, frag, os64_html_create_comment(doc, "2", 1, NULL), NULL);
    check(os64_html_insert(doc, root, frag, html) == OS64_HTML_OK && !frag->first_child,
          "a fragment of comments goes under the document and is left empty");
    os64_html_insert(doc, frag, text, NULL);
    check(os64_html_insert(doc, root, frag, NULL) == OS64_HTML_HIERARCHY, "a fragment holding text");
    os64_html_remove(doc, text);
    os64_html_insert(doc, frag, html2, NULL);
    check(os64_html_insert(doc, root, frag, NULL) == OS64_HTML_HIERARCHY,
          "a fragment holding an element, when the document has one");
    check(os64_html_replace(doc, root, frag, html) == OS64_HTML_OK && doc->html == html2,
          "a fragment holding one html element replaces the html element");
    check(doc->head == NULL && doc->body == NULL && html->parent == NULL,
          "the landmarks follow the new html element");
    check(os64_html_replace(doc, root, html, html2) == OS64_HTML_OK && doc->html == html &&
              doc->head && doc->body && doc->body->parent == html,
          "and follow the old one back");
    expect_tree(root,
                "#document[<!html><!--1--><!--2--><html>[<head>[<title>[\"t\"]]<body>[<div id='a'>"
                "[<span id='b'>[\"one\"]]<p id='c'>[\"two\"]<!--note-->]]]",
                "the document after the fragment cases");

    // A doctype may only come before the element.
    os64_html_remove(doc, doctype);
    check(os64_html_insert(doc, root, doctype, NULL) == OS64_HTML_HIERARCHY, "a doctype after the element");
    check(os64_html_insert(doc, root, doctype, root->first_child) == OS64_HTML_OK, "a doctype first");
    check(os64_html_insert(doc, root, comment, doctype) == OS64_HTML_OK, "a comment before it");
    check(os64_html_replace(doc, root, doctype2, doctype) == OS64_HTML_OK, "one doctype for another");
    check(os64_html_replace(doc, root, doctype, html->prev) == OS64_HTML_HIERARCHY,
          "a second doctype in place of a comment");
    // With no doctype left, one may still not go anywhere after the element,
    // by insert or by replace, however the place is named.
    os64_html_remove(doc, doctype2);
    HNode *trailing = os64_html_create_comment(doc, "end", 3, NULL), *last = os64_html_create_comment(doc, "z", 1, NULL);
    os64_html_insert(doc, root, trailing, NULL);
    os64_html_insert(doc, root, last, NULL);
    check(os64_html_insert(doc, root, doctype, last) == OS64_HTML_HIERARCHY,
          "a doctype before a comment that follows the element");
    check(os64_html_replace(doc, root, doctype, trailing) == OS64_HTML_HIERARCHY,
          "a doctype in place of a comment that follows the element");
    check(os64_html_insert(doc, root, doctype, html) == OS64_HTML_OK && doctype->next == html,
          "a doctype just before the element");

    HDoc *d = (HDoc *)doc;
    check(links_ok(d, root, 0) == d->records, "links after the validity cases");
    os64_html_release(doc, frag); os64_html_release(doc, html);
    os64_html_release(doc, text); os64_html_release(doc, doctype);
    os64_html_document_free(doc);
    check(live == 0, "validity cases freed");
}

static void t_structure(void)
{
    os64_html_document_t *doc = parse(PAGE);
    HNode *a = id_in(doc, "a"), *b = id_in(doc, "b"), *c = id_in(doc, "c"), *body = doc->body;
    uint64_t v = os64_html_version(doc);
    check(v != 0, "a document's version is never zero");

    // A move within one parent, and its edge: before the node itself.
    check(os64_html_insert(doc, body, c, a) == OS64_HTML_OK, "move p before div");
    expect_tree(body, "<body>[<p id='c'>[\"two\"]<div id='a'>[<span id='b'>[\"one\"]]<!--note-->]", "moved");
    check(os64_html_insert(doc, body, c, c) == OS64_HTML_OK, "a node before itself stays put");
    expect_tree(body, "<body>[<p id='c'>[\"two\"]<div id='a'>[<span id='b'>[\"one\"]]<!--note-->]", "stayed");
    check(os64_html_version(doc) > v, "a change moved the version");
    // An empty fragment is allowed anywhere a fragment is and brings nothing.
    v = os64_html_version(doc);
    HNode *nothing = os64_html_create_fragment(doc, NULL);
    check(os64_html_insert(doc, body, nothing, a) == OS64_HTML_OK && os64_html_version(doc) == v,
          "inserting an empty fragment changes nothing, the version included");
    // Nor does a node put where it already sits, however the place is named.
    check(os64_html_insert(doc, body, c, c) == OS64_HTML_OK && os64_html_insert(doc, body, c, a) == OS64_HTML_OK &&
              os64_html_insert(doc, body, body->last_child, NULL) == OS64_HTML_OK &&
              os64_html_insert(doc, a, b, b) == OS64_HTML_OK && os64_html_version(doc) == v,
          "a node inserted where it is leaves the version");
    expect_tree(body, "<body>[<p id='c'>[\"two\"]<div id='a'>[<span id='b'>[\"one\"]]<!--note-->]", "as it was");
    check(os64_html_insert(doc, body, a, c) == OS64_HTML_OK && os64_html_version(doc) == v + 1 &&
              os64_html_insert(doc, body, c, a) == OS64_HTML_OK && os64_html_version(doc) == v + 2,
          "and one that moves by a single place moves it");

    // A move across parents; text is not merged with its new neighbour.
    HNode *t1 = os64_html_create_text(doc, "A", 1, NULL), *t2 = os64_html_create_text(doc, "B", 1, NULL);
    os64_html_insert(doc, b, t1, NULL);
    os64_html_insert(doc, b, t2, b->first_child);
    expect_tree(b, "<span id='b'>[\"B\"\"one\"\"A\"]", "three text nodes side by side");
    os64_html_insert(doc, c, b, c->first_child);
    expect_tree(body, "<body>[<p id='c'>[<span id='b'>[\"B\"\"one\"\"A\"]\"two\"]<div id='a'><!--note-->]",
                "span moved under p");

    // A fragment gives up its children in order, at the place asked for.
    HNode *frag = os64_html_create_fragment(doc, NULL);
    os64_html_hold(doc, frag);
    for (int i = 0; i < 3; i++) {
        char name[2] = {(char)('x' + i), 0};
        os64_html_insert(doc, frag, os64_html_create_element(doc, OS64_HTML_NS_HTML, name, NULL), NULL);
    }
    check(os64_html_insert(doc, body, frag, a) == OS64_HTML_OK && !frag->first_child && !frag->last_child,
          "fragment inserted and emptied");
    expect_tree(body, "<body>[<p id='c'>[<span id='b'>[\"B\"\"one\"\"A\"]\"two\"]<x><y><z><div id='a'><!--note-->]",
                "fragment children in order");
    check(os64_html_insert(doc, body, frag, NULL) == OS64_HTML_OK, "an empty fragment inserts nothing");

    // Replace: by a node, by a sibling, by itself, by a fragment.
    HNode *x = a->prev->prev->prev, *z = a->prev;
    os64_html_hold(doc, x); os64_html_hold(doc, c);
    check(os64_html_replace(doc, body, a, x) == OS64_HTML_OK && !x->parent, "div replaces x");
    expect_tree(body, "<body>[<p id='c'>[<span id='b'>[\"B\"\"one\"\"A\"]\"two\"]<div id='a'><y><z><!--note-->]",
                "replaced by a later sibling");
    check(os64_html_replace(doc, body, z, z) == OS64_HTML_OK, "a node replaces itself");
    os64_html_insert(doc, frag, x, NULL);
    os64_html_insert(doc, frag, os64_html_create_text(doc, "!", 1, NULL), NULL);
    check(os64_html_replace(doc, body, frag, z) == OS64_HTML_OK, "a fragment replaces z");
    expect_tree(body, "<body>[<p id='c'>[<span id='b'>[\"B\"\"one\"\"A\"]\"two\"]<div id='a'><y><x>\"!\"<!--note-->]",
                "replaced by a fragment");

    // A held removed node stays whole and can come back.
    check(os64_html_remove(doc, c) == OS64_HTML_OK && !c->parent && !c->prev && !c->next, "p removed");
    expect_tree(c, "<p id='c'>[<span id='b'>[\"B\"\"one\"\"A\"]\"two\"]", "the removed subtree is whole");
    v = os64_html_version(doc);
    check(os64_html_remove(doc, c) == OS64_HTML_OK && os64_html_version(doc) == v,
          "removing a node with no parent is a no-op");
    os64_html_insert(doc, a, c, NULL);
    expect_tree(body, "<body>[<div id='a'>[<p id='c'>[<span id='b'>[\"B\"\"one\"\"A\"]\"two\"]]<y><x>\"!\"<!--note-->]",
                "and comes back");

    // Clone: shallow, deep, and through a template.
    HNode *shallow = os64_html_clone(doc, a, false, NULL), *deep = os64_html_clone(doc, a, true, NULL);
    expect_tree(shallow, "<div id='a'>", "a shallow clone has its attributes and no children");
    expect_tree(deep, "<div id='a'>[<p id='c'>[<span id='b'>[\"B\"\"one\"\"A\"]\"two\"]]", "a deep clone has the subtree");
    check(deep->first_child != a->first_child && deep->first_child->first_child->first_child->text !=
                                                      b->first_child->text,
          "a deep clone shares no node and no text buffer");
    os64_html_set_text(doc, deep->first_child->first_child->first_child, "changed", 7);
    expect_tree(b, "<span id='b'>[\"B\"\"one\"\"A\"]", "changing the clone's text leaves the original");

    HDoc *d = (HDoc *)doc;
    check(links_ok(d, doc->document, 0) + links_ok(d, deep, 0) + links_ok(d, shallow, 0) == d->records,
          "links after the structure cases");
    os64_html_release(doc, frag); os64_html_release(doc, x); os64_html_release(doc, c);
    os64_html_document_free(doc);
    check(live == 0, "structure cases freed");
}

static void t_templates(void)
{
    os64_html_document_t *doc = parse("<body><template id=t><b id=in>x</b><template id=u><i></i></template>"
                                      "</template><div id=d></div>");
    HNode *t = id_in(doc, "t"), *in = id_in(doc, "in"), *u = id_in(doc, "u"), *dv = id_in(doc, "d");
    check(t && in && u && in->parent == t->template_contents, "the parser put the contents on the fragment");
    check(os64_html_insert(doc, t->template_contents, t, NULL) == OS64_HTML_HIERARCHY,
          "a template under its own contents");
    check(os64_html_insert(doc, u->template_contents, t, NULL) == OS64_HTML_HIERARCHY,
          "a template under the contents of a template inside it");
    check(os64_html_insert(doc, in, u->template_contents, NULL) == OS64_HTML_OK,
          "a template's contents inserted elsewhere move their children");
    expect_tree(t, "<template id='t'>{<b id='in'>[\"x\"<i>]<template id='u'>{}}", "after moving the inner contents");
    check(os64_html_insert(doc, t->template_contents, dv, in) == OS64_HTML_OK, "a node into a template's contents");
    check(os64_html_insert(doc, t, os64_html_create_text(doc, "own", 3, NULL), NULL) == OS64_HTML_OK,
          "a template may be given children of its own");

    HNode *made = os64_html_create_element(doc, OS64_HTML_NS_HTML, "template", NULL);
    check(made && made->template_contents && made->template_contents->kind == OS64_HTML_FRAGMENT &&
              (HNode *)*h_word(made->template_contents) == made,
          "a created template has contents that know it");
    HNode *svg_template = os64_html_create_element(doc, OS64_HTML_NS_SVG, "template", NULL);
    check(svg_template && !svg_template->template_contents, "an SVG element called template has none");

    HNode *copy = os64_html_clone(doc, t, true, NULL), *bare = os64_html_clone(doc, t, false, NULL);
    expect_tree(copy, "<template id='t'>{<div id='d'><b id='in'>[\"x\"<i>]<template id='u'>{}}[\"own\"]",
                "a deep clone copies the contents and the children");
    expect_tree(bare, "<template id='t'>{}", "a shallow clone has empty contents");
    check(bare->template_contents && !bare->template_contents->first_child, "but it has them");
    check(os64_html_insert(doc, copy->template_contents, copy, NULL) == OS64_HTML_HIERARCHY,
          "the clone's contents know the clone");
    HDoc *d = (HDoc *)doc;
    check(links_ok(d, doc->document, 0) + links_ok(d, copy, 0) + links_ok(d, bare, 0) == d->records,
          "links after the template cases");
    os64_html_document_free(doc);
    check(live == 0, "template cases freed");
}

static void t_attributes(void)
{
    // Two <b> elements that the parser made from one start tag share one
    // list of attribute records. Changing one must not change the other.
    os64_html_document_t *doc = parse("<body><p id=p1><b class=x title=y>a<p id=p2>b");
    HNode *b1 = id_in(doc, "p1")->first_child, *b2 = id_in(doc, "p2")->first_child;
    check(b1 != b2 && b1->attrs && b1->attrs == b2->attrs, "the fixture: two elements, one attribute list");
    const HAttr *borrowed = os64_html_attr(b1, "class");
    check(os64_html_set_attr(doc, b1, "class", "z", 1) == OS64_HTML_OK, "set on the first");
    expect_tree(b1, "<b class='z' title='y'>[\"a\"]", "the first changed, order kept");
    expect_tree(b2, "<b class='x' title='y'>[\"b\"]", "the second did not");
    check(strcmp(borrowed->value, "x") == 0, "a record borrowed before the change still reads");
    check(os64_html_remove_attr(doc, b2, "class") == OS64_HTML_OK, "remove on the second");
    expect_tree(b1, "<b class='z' title='y'>[\"a\"]", "the first is untouched by it");
    expect_tree(b2, "<b title='y'>[\"b\"]", "the second lost it");

    HNode *e = b1;
    os64_html_set_attr(doc, e, "new", "1", 1);
    os64_html_set_attr(doc, e, "title", "", 0);
    os64_html_set_attr(doc, e, "new", "2", 1);
    expect_tree(e, "<b class='z' title='' new='2'>[\"a\"]", "a new attribute goes last; a set one keeps its place");
    uint64_t v = os64_html_version(doc);
    check(os64_html_remove_attr(doc, e, "absent") == OS64_HTML_OK && os64_html_version(doc) == v,
          "removing an attribute that is not there is a no-op");
    // The value an attribute already has: nothing is replaced, on a list the
    // element owns or on one it still shares.
    const HAttr *held = os64_html_attr(e, "new"), *shared = os64_html_attr(b2, "title");
    const char *held_value = held->value;
    size_t arena = doc->arena_bytes;
    check(os64_html_set_attr(doc, e, "new", "2", 1) == OS64_HTML_OK &&
              os64_html_set_attr(doc, e, "title", "", 0) == OS64_HTML_OK &&
              os64_html_set_attr(doc, b2, "title", "y", 1) == OS64_HTML_OK,
          "setting the value it has");
    check(os64_html_version(doc) == v && doc->arena_bytes == arena && os64_html_attr(e, "new") == held &&
              held->value == held_value && os64_html_attr(b2, "title") == shared,
          "replaces no record, takes no memory and leaves the version");
    check(os64_html_set_attr(doc, e, "new", "20", 2) == OS64_HTML_OK && os64_html_version(doc) > v &&
              os64_html_set_attr(doc, e, "new", "2", 1) == OS64_HTML_OK,
          "a value that only starts the same is a change");
    v = os64_html_version(doc);
    os64_html_remove_attr(doc, e, "class");
    os64_html_remove_attr(doc, e, "new");
    os64_html_remove_attr(doc, e, "title");
    check(e->attrs == NULL, "every attribute removed");
    os64_html_set_attr(doc, e, "caf\xc3\xa9", "\xe6\x97\xa5", 3);
    expect_tree(e, "<b caf\xc3\xa9='\xe6\x97\xa5'>[\"a\"]", "names and values past ASCII");

    // A clone of an element whose list is its own gets its own, too.
    HNode *copy = os64_html_clone(doc, e, false, NULL);
    os64_html_set_attr(doc, e, "caf\xc3\xa9", "one", 3);
    os64_html_set_attr(doc, copy, "caf\xc3\xa9", "two", 3);
    expect_tree(e, "<b caf\xc3\xa9='one'>[\"a\"]", "the original after both sets");
    expect_tree(copy, "<b caf\xc3\xa9='two'>", "the clone after both sets");
    // And a clone of one whose list is still the parser's shares it until either changes.
    HNode *p2 = id_in(doc, "p2"), *p2copy = os64_html_clone(doc, p2, false, NULL);
    check(p2copy->attrs == p2->attrs, "an unchanged list is shared by a clone");
    os64_html_set_attr(doc, p2copy, "id", "other", 5);
    expect_tree(p2, "<p id='p2'>[<b title='y'>[\"b\"]]", "and the original keeps its own on a change");

    // A foreign attribute found by its qualified name keeps its namespace.
    os64_html_document_free(doc);
    doc = parse("<body><svg id=s xlink:href=a></svg>");
    HNode *s = id_in(doc, "s");
    const char *ns = os64_html_attr(s, "xlink:href")->ns;
    os64_html_set_attr(doc, s, "xlink:href", "b", 1);
    check(ns && os64_html_attr(s, "xlink:href")->ns == ns && strcmp(os64_html_attr(s, "xlink:href")->value, "b") == 0,
          "a namespaced attribute keeps its namespace when set");
    os64_html_document_free(doc);
    check(live == 0, "attribute cases freed");
}

static void t_text(void)
{
    os64_html_document_t *doc = parse("<body><p id=p>parsed</p><!--said-->");
    HNode *text = id_in(doc, "p")->first_child, *comment = doc->body->last_child;
    check(comment->kind == OS64_HTML_COMMENT, "the fixture's comment");
    const char *old = text->text;
    check(os64_html_set_text(doc, text, "", 0) == OS64_HTML_OK && text->text_len == 0 && text->text[0] == 0 &&
              text->text != old,
          "text set to nothing is a fresh empty string");
    check(os64_html_set_text(doc, text, "na\xc3\xafve", 6) == OS64_HTML_OK && text->text_len == 6 &&
              strcmp(text->text, "na\xc3\xafve") == 0,
          "text set");
    check(os64_html_set_text(doc, comment, "new", 3) == OS64_HTML_OK && strcmp(comment->text, "new") == 0,
          "a parsed comment's data set");
    check(os64_html_set_text(doc, comment, "newer", 5) == OS64_HTML_OK && strcmp(comment->text, "newer") == 0,
          "and set again");
    char big[5000];
    memset(big, 'q', sizeof(big));
    check(os64_html_set_text(doc, text, big, sizeof(big)) == OS64_HTML_OK && text->text_len == sizeof(big) &&
              text->text[sizeof(big)] == 0,
          "a long text");
    check(os64_html_retired_bytes(doc) == 0, "with no pin, nothing is kept");
    // The text a node already has: nothing is replaced.
    uint64_t v = os64_html_version(doc);
    const char *kept = comment->text;
    check(os64_html_set_text(doc, comment, "newer", 5) == OS64_HTML_OK && comment->text == kept &&
              os64_html_version(doc) == v,
          "setting the text it has keeps the bytes and the version");
    check(os64_html_set_text(doc, comment, "new", 3) == OS64_HTML_OK && os64_html_version(doc) == v + 1 &&
              os64_html_set_text(doc, comment, "newer!", 6) == OS64_HTML_OK && os64_html_version(doc) == v + 2,
          "a shorter or longer text that starts the same is a change");
    os64_html_set_text(doc, text, "", 0);
    v = os64_html_version(doc);
    check(os64_html_set_text(doc, text, "", 0) == OS64_HTML_OK && os64_html_version(doc) == v,
          "nothing set to nothing");
    os64_html_document_free(doc);
    check(live == 0, "text cases freed");
}

static void t_form_owners(void)
{
    HDoc *d;
    // The table case: the parser ties q to f, which is not its ancestor.
    os64_html_document_t *doc = parse("<body><table id=t><form id=f><tr><td><input id=q></table><div id=x></div>");
    HNode *q = id_in(doc, "q"), *f = id_in(doc, "f"), *x = id_in(doc, "x"), *t = id_in(doc, "t");
    d = (HDoc *)doc;
    check(q->form_owner == f && d->records == 1, "the fixture: a record to a form that is not an ancestor");
    check(os64_html_set_attr(doc, q, "form", "missing", 7) == OS64_HTML_OK && !q->form_owner && d->records == 0,
          "setting the control's form attribute voids the record");
    check(os64_html_remove_attr(doc, q, "form") == OS64_HTML_OK && !q->form_owner,
          "and removing it does not bring the record back");
    os64_html_document_free(doc);

    doc = parse("<body><table id=t><form id=f><tr><td><input id=q></table><div id=x></div>");
    q = id_in(doc, "q"), f = id_in(doc, "f"), x = id_in(doc, "x"), t = id_in(doc, "t");
    d = (HDoc *)doc;
    os64_html_set_attr(doc, q, "class", "c", 1);
    check(q->form_owner == f, "another attribute leaves the record");
    // A node put where it already sits is still taken out and put back. That
    // parts nobody when the pair goes together, and the version stays; it
    // parts a control whose form is outside what moved, and the version says
    // so though no link changed.
    uint64_t same = os64_html_version(doc);
    check(os64_html_insert(doc, t->parent, t, t) == OS64_HTML_OK && q->form_owner == f &&
              os64_html_version(doc) == same,
          "the pair reinserted in place: tied, and the version stays");
    os64_html_document_t *twin = parse("<body><table id=t><form id=f><tr><td><input id=q></table>");
    HNode *q2 = id_in(twin, "q");
    same = os64_html_version(twin);
    check(q2->form_owner && os64_html_insert(twin, q2->parent, q2, q2) == OS64_HTML_OK && !q2->form_owner &&
              os64_html_version(twin) > same,
          "the control alone reinserted in place: parted, and the version moves");
    os64_html_document_free(twin);
    check(os64_html_remove_attr(doc, q, "form") == OS64_HTML_OK && q->form_owner == f,
          "removing a form attribute that was never there leaves it too");
    // The control and its form move together: still tied.
    check(os64_html_insert(doc, x, t, NULL) == OS64_HTML_OK && q->form_owner == f && d->records == 1,
          "a move that carries both keeps the record");
    // The control moves away from its form, inside one document.
    check(os64_html_insert(doc, x, q, NULL) == OS64_HTML_OK && !q->form_owner && d->records == 0,
          "a move that parts them clears it, though both stay in the document");
    os64_html_document_free(doc);

    // The form leaves; the control stays where it is.
    doc = parse("<body><table id=t><form id=f><tr><td><input id=q><input id=r></table>");
    q = id_in(doc, "q"), f = id_in(doc, "f");
    HNode *r = id_in(doc, "r");
    d = (HDoc *)doc;
    check(q->form_owner == f && r->form_owner == f && d->records == 2, "the fixture: two records to one form");
    os64_html_hold(doc, f);
    check(os64_html_remove(doc, f) == OS64_HTML_OK && !q->form_owner && !r->form_owner && d->records == 0,
          "removing the form clears the records of the controls left behind");
    os64_html_insert(doc, id_in(doc, "t"), f, NULL);
    check(!q->form_owner, "and putting the form back does not restore them");
    os64_html_release(doc, f);
    os64_html_document_free(doc);

    // A pair inside a template's contents is in the tree those contents are
    // the root of. Nothing done to the template takes either out of it.
    doc = parse("<body><table id=t><form id=f><tr><td><input id=q></table><template id=m></template>"
                "<div id=x></div>");
    q = id_in(doc, "q"), f = id_in(doc, "f"), x = id_in(doc, "x"), t = id_in(doc, "t");
    HNode *m = id_in(doc, "m");
    d = (HDoc *)doc;
    check(os64_html_insert(doc, m->template_contents, t, NULL) == OS64_HTML_OK && q->form_owner == f,
          "a pair moved into a template's contents together stays tied");
    check(os64_html_insert(doc, x, m, NULL) == OS64_HTML_OK && q->form_owner == f && d->records == 1,
          "moving the template leaves a pair in its contents tied");
    os64_html_hold(doc, x);
    check(os64_html_remove(doc, x) == OS64_HTML_OK && q->form_owner == f && d->records == 1,
          "and so does removing what holds the template");
    HNode *outer = os64_html_clone(doc, m, false, NULL);
    check(os64_html_insert(doc, outer->template_contents, x, NULL) == OS64_HTML_OK && q->form_owner == f,
          "and nesting it in another template's contents");
    check(os64_html_insert(doc, doc->body, f, NULL) == OS64_HTML_OK && !q->form_owner && d->records == 0,
          "the form leaving the contents parts them");
    check(links_ok(d, doc->document, 0) + links_ok(d, outer, 0) == d->records, "records after the template cases");
    os64_html_release(doc, x);
    os64_html_document_free(doc);

    // An ordinary nested control: the record is the ancestor, and a move
    // out clears it; libpage then finds the nearest ancestor by its own rule.
    doc = parse("<body><form id=f><div id=w><input id=q></div></form><p id=o></p><img id=i form=zz>");
    q = id_in(doc, "q"), f = id_in(doc, "f");
    HNode *w = id_in(doc, "w"), *o = id_in(doc, "o");
    d = (HDoc *)doc;
    check(q->form_owner == f, "the fixture: a nested control");
    check(os64_html_insert(doc, w, q, NULL) == OS64_HTML_OK && !q->form_owner,
          "any removal parts a control from a form outside what moved");
    os64_html_document_free(doc);

    // An img is tied by the parser but its form attribute means nothing.
    doc = parse("<body><form id=f><img id=i></form>");
    HNode *img = id_in(doc, "i");
    f = id_in(doc, "f");
    check(img->form_owner == f, "the fixture: an img's record");
    os64_html_set_attr(doc, img, "form", "x", 1);
    check(img->form_owner == f, "an img's form attribute does not void its record");
    HNode *copy = os64_html_clone(doc, f, true, NULL);
    check(copy->first_child && !copy->first_child->form_owner, "a clone has no form owner");
    d = (HDoc *)doc;
    check(links_ok(d, doc->document, 0) + links_ok(d, copy, 0) == d->records, "records after the form cases");
    os64_html_document_free(doc);
    check(live == 0, "form-owner cases freed");
    (void)o;
    (void)x;
    (void)t;
}

static void t_depth(void)
{
    // The parser's limit is on its stack of open elements, and its tree can
    // be deeper: a form taken off the stack by its end tag keeps what it
    // contains open. Three open elements here, and a tree five deep.
    os64_html_options_t opt = os64_html_options_default();
    opt.max_depth = 4;
    os64_html_document_t *doc = parse_with("<body><form><div id=a></form><div id=b>", &opt);
    HNode *b = id_in(doc, "b");
    check(doc && !doc->refusal && b && b->parent == id_in(doc, "a") && b->parent->parent->tag == OS64_HTML_TAG_FORM,
          "the fixture: a tree deeper than the stack that built it");
    HDoc *d = (HDoc *)doc;
    check(h_depth_limit(d) == 10, "the limit for a max_depth of 4 is 10, got %zu", h_depth_limit(d));
    // html is at depth 1, body 2, form 3, div a 4, div b 5.
    HNode *at = b;
    for (int depth = 6; depth <= 10; depth++) {
        HNode *e = os64_html_create_element(doc, OS64_HTML_NS_HTML, "div", NULL);
        check(os64_html_insert(doc, at, e, NULL) == OS64_HTML_OK, "a div at depth %d", depth);
        at = e;
    }
    HNode *leaf = os64_html_create_text(doc, "x", 1, NULL);
    check(os64_html_insert(doc, at, leaf, NULL) == OS64_HTML_TOO_DEEP, "a text node at depth 11");
    HNode *pair = os64_html_create_element(doc, OS64_HTML_NS_HTML, "div", NULL);
    os64_html_insert(doc, pair, os64_html_create_element(doc, OS64_HTML_NS_HTML, "i", NULL), NULL);
    check(os64_html_insert(doc, at->parent, pair, NULL) == OS64_HTML_TOO_DEEP,
          "a subtree whose foot would be at depth 11");
    check(os64_html_insert(doc, at->parent->parent, pair, NULL) == OS64_HTML_OK, "the same subtree one level up");
    HNode *frag = os64_html_create_fragment(doc, NULL), *two = os64_html_clone(doc, pair, true, NULL);
    os64_html_insert(doc, frag, os64_html_create_text(doc, "ok", 2, NULL), NULL);
    os64_html_insert(doc, frag, two, NULL);
    check(os64_html_insert(doc, at, frag, NULL) == OS64_HTML_TOO_DEEP && frag->first_child && frag->last_child == two,
          "a fragment with one child too tall is refused whole, and keeps its children");
    // A template's contents count as its children.
    HNode *tmpl = os64_html_create_element(doc, OS64_HTML_NS_HTML, "template", NULL);
    check(os64_html_insert(doc, at->parent, tmpl, NULL) == OS64_HTML_OK, "a template at depth 10");
    check(os64_html_insert(doc, tmpl->template_contents, leaf, NULL) == OS64_HTML_TOO_DEEP,
          "into its contents is depth 11");
    check(links_ok(d, doc->document, 0) == d->records, "links after the depth cases");
    os64_html_document_free(doc);
    check(live == 0, "depth cases freed");
}

// A tree far deeper than this thread's stack could hold a frame for each
// level of: the verbs that walk a subtree must not recurse.
static void *deep_tree(void *unused)
{
    (void)unused;
    os64_html_options_t opt = os64_html_options_default();
    opt.max_depth = 16000;
    opt.max_work = UINT64_MAX;      // the parser's own scope walks are quadratic in depth
    size_t n = 12000;
    char *markup = malloc(n * 5 + 1);
    for (size_t i = 0; i < n; i++)
        memcpy(markup + i * 5, "<div>", 5);
    markup[n * 5] = 0;
    os64_html_document_t *doc = parse_with(markup, &opt);
    free(markup);
    check(doc && !doc->refusal, "a 12000-deep document parsed");
    HNode *top = doc->body->first_child, *foot = top;
    while (foot->first_child)
        foot = foot->first_child;
    int64_t st = 0;
    HNode *copy = os64_html_clone(doc, top, true, &st);
    check(copy && st == OS64_HTML_OK, "and its subtree deep-cloned");
    size_t levels = 0;
    for (HNode *c = copy; c; c = c->first_child)
        levels++;
    check(levels == n, "the clone is %zu deep, want %zu", levels, n);
    HNode *again = os64_html_clone(doc, copy, true, NULL);
    check(os64_html_insert(doc, foot, copy, NULL) == OS64_HTML_OK, "24000 deep is within twice a max_depth of 16000");
    while (foot->first_child)
        foot = foot->first_child;
    check(again && os64_html_insert(doc, foot, again, NULL) == OS64_HTML_TOO_DEEP, "36000 deep is not");
    check(os64_html_insert(doc, doc->body, again, NULL) == OS64_HTML_OK, "the second clone beside the original");
    check(os64_html_remove(doc, top) == OS64_HTML_OK, "and the deep subtree removed");
    os64_html_document_free(doc);
    check(live == 0, "deep tree freed");
    return NULL;
}
static void t_deep(void)
{
    pthread_attr_t attr;
    pthread_t thread;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    if (pthread_create(&thread, &attr, deep_tree, NULL) != 0) {
        check(false, "deep tree thread");
        return;
    }
    pthread_join(thread, NULL);
}

// ── Pins and retirement ─────────────────────────────────────────────────

static void t_pins(void)
{
    os64_html_document_t *doc = parse("<body><p id=p class=one>first</p>");
    HNode *p = id_in(doc, "p"), *text = p->first_child;
    HDoc *d = (HDoc *)doc;

    os64_html_pin_t early = os64_html_pin(doc);
    const char *first = text->text;
    const char *one = os64_html_attr(p, "class")->value;
    check(os64_html_set_text(doc, text, "second", 6) == OS64_HTML_OK, "text replaced under a pin");
    size_t held = os64_html_retired_bytes(doc);
    check(held > 0 && strcmp(first, "first") == 0, "the old text is retired and still reads");
    check(doc->arena_bytes >= held, "retired bytes stay charged to the arena");

    os64_html_pin_t late = os64_html_pin(doc);
    const char *second = text->text;
    check(os64_html_set_text(doc, text, "third", 5) == OS64_HTML_OK, "replaced again under a later pin");
    size_t both = os64_html_retired_bytes(doc);
    check(both > held && strcmp(second, "second") == 0 && strcmp(first, "first") == 0, "both old texts read");

    os64_html_set_attr(doc, p, "class", "two", 3);
    os64_html_set_attr(doc, p, "class", "three", 5);
    check(strcmp(one, "one") == 0, "a parser-era attribute value is never freed alone");
    size_t all_held = os64_html_retired_bytes(doc);
    check(all_held > both, "replaced attribute records are retired too");

    os64_html_unpin(doc, early);
    check(os64_html_retired_bytes(doc) == all_held - held,
          "letting the early pin go frees exactly what only it could see");
    check(strcmp(second, "second") == 0, "and keeps what the later pin can");
    os64_html_unpin(doc, late);
    check(os64_html_retired_bytes(doc) == 0 && d->retired == NULL, "the last pin frees the rest");

    // A pin taken after a change cannot be holding what that change replaced.
    os64_html_pin_t after = os64_html_pin(doc);
    check(os64_html_retired_bytes(doc) == 0, "a fresh pin holds nothing");
    os64_html_set_text(doc, text, "fourth", 6);
    check(os64_html_retired_bytes(doc) > 0, "until something changes under it");
    os64_html_unpin(doc, after);

    // Sixteen pins, and no seventeenth.
    os64_html_pin_t pins[H_PINS];
    for (size_t i = 0; i < H_PINS; i++)
        pins[i] = os64_html_pin(doc);
    check(pins[H_PINS - 1] != 0 && os64_html_pin(doc) == 0, "the pin table is full at %d", H_PINS);
    for (size_t i = 1; i < H_PINS; i++)
        os64_html_unpin(doc, pins[i]);
    check(os64_html_pin(NULL) == 0 && os64_html_version(NULL) == 0, "no document, no pin");
    os64_html_unpin(doc, 0);

    // Freeing a pinned document, and letting a pin go twice, end the program.
    expecting_death = true;
    death_code = 0;
    if (setjmp(death_landing) == 0) {
        os64_html_document_free(doc);
        check(false, "a pinned document was freed");
    }
    check(death_code == (int32_t)OS64_HTML_FATAL_EXIT && strstr(death_said, "pinned"),
          "freeing a pinned document dies by name: %s", death_said);
    os64_html_unpin(doc, pins[0]);
    death_code = 0;
    if (setjmp(death_landing) == 0) {
        os64_html_unpin(doc, pins[0]);
        check(false, "a pin was let go twice");
    }
    check(death_code == (int32_t)OS64_HTML_FATAL_EXIT, "letting a pin go twice dies");

    // A pin let go names nothing afterwards, though its slot is taken again:
    // the stale number must not release the pin that now sits there.
    os64_html_pin_t stale = os64_html_pin(doc);
    os64_html_unpin(doc, stale);
    os64_html_pin_t fresh = os64_html_pin(doc);
    check(fresh != stale && fresh % H_PINS == stale % H_PINS, "the slot is reused and the number is not");
    os64_html_set_text(doc, text, "fifth", 5);
    size_t under_fresh = os64_html_retired_bytes(doc);
    death_code = 0;
    if (setjmp(death_landing) == 0) {
        os64_html_unpin(doc, stale);
        check(false, "a stale pin released the pin that took its slot");
    }
    check(death_code == (int32_t)OS64_HTML_FATAL_EXIT && under_fresh > 0 &&
              os64_html_retired_bytes(doc) == under_fresh,
          "a stale pin dies and frees nothing the new one holds");
    // Nor does another document's pin, which a slot number alone would match.
    os64_html_document_t *other = parse("<body>");
    os64_html_pin_t theirs = os64_html_pin(other);
    check(theirs % H_PINS == fresh % H_PINS && theirs != fresh, "two documents, one slot, two numbers");
    death_code = 0;
    if (setjmp(death_landing) == 0) {
        os64_html_unpin(doc, theirs);
        check(false, "another document's pin released this one's");
    }
    check(death_code == (int32_t)OS64_HTML_FATAL_EXIT && os64_html_retired_bytes(doc) == under_fresh,
          "another document's pin dies here");
    os64_html_unpin(other, theirs);
    os64_html_document_free(other);
    os64_html_unpin(doc, fresh);
    check(os64_html_retired_bytes(doc) == 0, "and the pin that was held still lets go");
    expecting_death = false;
    os64_html_document_free(doc);
    check(live == 0, "pin cases freed");
}

// ── Two documents ───────────────────────────────────────────────────────

static void t_documents(void)
{
    os64_html_options_t deep_opt = os64_html_options_default();
    deep_opt.max_depth = 64;
    // `b` twice: the second is the parser's own copy and shares the first's
    // attribute records. `own` has a list a verb made private.
    os64_html_document_t *src = parse_with("<!doctype html SYSTEM \"about:legacy-compat\"><body>"
                                           "<div id=s class=k><b title=shared>one<p>two</b>"
                                           "<template id=tm><i lang=en>in</i></template>"
                                           "<!--note--><span id=own>x</span>"
                                           "<svg viewBox='0 0 1 1'><foreignObject/></svg></div>",
                                           &deep_opt);
    os64_html_document_t *dst = parse("<body><p id=here></p>");
    HNode *s = id_in(src, "s"), *here = id_in(dst, "here");
    os64_html_set_attr(src, id_in(src, "own"), "data-n", "1", 1);
    char *want = spelled(s);
    char *src_before = spelled(src->document), *dst_before = spelled(dst->document);
    uint64_t src_version = os64_html_version(src), dst_version = os64_html_version(dst);
    HNode *theirs = id_in(src, "own"), *text = theirs->first_child;

    // One document's node is refused by every verb of another, in each seat.
    struct {
        const char *what;
        int64_t got;
    } refused[] = {
        {"insert, the node", os64_html_insert(dst, here, theirs, NULL)},
        {"insert, the parent", os64_html_insert(dst, s, here, NULL)},
        {"insert, the place", os64_html_insert(dst, here, os64_html_create_comment(dst, "c", 1, NULL), theirs)},
        {"replace, the node", os64_html_replace(dst, dst->body, theirs, here)},
        {"replace, the old", os64_html_replace(dst, dst->body, here, theirs)},
        {"remove", os64_html_remove(dst, theirs)},
        {"set_attr", os64_html_set_attr(dst, theirs, "x", "y", 1)},
        {"remove_attr", os64_html_remove_attr(dst, theirs, "data-n")},
        {"set_text", os64_html_set_text(dst, text, "y", 1)},
    };
    for (size_t i = 0; i < H_ARRAY(refused); i++)
        check(refused[i].got == OS64_HTML_BAD_ARGUMENT, "another document's node, %s: %s", refused[i].what,
              os64_html_status_name(refused[i].got));
    char *src_after = spelled(src->document), *dst_after = spelled(dst->document);
    check(strcmp(src_before, src_after) == 0 && strcmp(dst_before, dst_after) == 0 &&
              os64_html_version(src) == src_version && os64_html_version(dst) == dst_version,
          "the refusals changed neither document");
    free(src_before), free(dst_before), free(src_after), free(dst_after);

    // A clone is how a node crosses: it reads the same and shares nothing,
    // which freeing the source proves under the sanitizer.
    int64_t st = 99;
    HNode *copy = os64_html_clone(dst, s, true, &st);
    HNode *doctype = os64_html_clone(dst, src->document->first_child, false, NULL);
    HNode *shallow = os64_html_clone(dst, s, false, NULL);
    check(copy && st == OS64_HTML_OK && doctype && shallow, "a subtree, a doctype and one element cloned across");
    const char *system_id = src->document->first_child->system_id;
    check(doctype->name != src->document->first_child->name && doctype->system_id != system_id &&
              doctype->public_id == NULL && strcmp(doctype->system_id, "about:legacy-compat") == 0,
          "a doctype's identifiers are copied, and an absent one stays absent");
    check(copy->name != s->name && copy->attrs != s->attrs && copy->first_child->attrs != s->first_child->attrs,
          "names and attribute records are the copy's own");
    os64_html_document_free(src);
    expect_tree(copy, want, "the clone, read after its source is freed");
    expect_tree(shallow, "<div id='s' class='k'>", "the shallow one");
    check(strcmp(doctype->name, "html") == 0, "the doctype's name outlives the source");
    check(os64_html_insert(dst, here, copy, NULL) == OS64_HTML_OK &&
              os64_html_set_attr(dst, copy, "class", "changed", 7) == OS64_HTML_OK &&
              os64_html_remove_attr(dst, copy->first_child, "title") == OS64_HTML_OK,
          "the clone is this document's: it goes in and its attributes change");
    HDoc *d = (HDoc *)dst;
    check(links_ok(d, dst->document, 0) == d->records, "links after the crossing");
    free(want);
    os64_html_document_free(dst);

    // A subtree taller than the receiving document allows does not cross.
    char markup[512] = "<body><div id=top>";
    for (int i = 0; i < 20; i++)
        strcat(markup, "<div>");
    src = parse_with(markup, &deep_opt);
    os64_html_options_t shallow_opt = os64_html_options_default();
    shallow_opt.max_depth = 4;
    dst = parse_with("<body>", &shallow_opt);
    d = (HDoc *)dst;
    size_t arena = dst->arena_bytes;
    st = 0;
    check(!os64_html_clone(dst, id_in(src, "top"), true, &st) && st == OS64_HTML_TOO_DEEP &&
              dst->arena_bytes == arena,
          "a subtree %d deep into a document that allows %zu: %s", 21, h_depth_limit(d), os64_html_status_name(st));
    check(os64_html_clone(dst, id_in(src, "top"), false, &st) && st == OS64_HTML_OK, "its top alone crosses");
    os64_html_document_free(src);
    os64_html_document_free(dst);

    // A mark is never given out twice, so the count of documents ends: the
    // last one is made, the next is refused, and the count stays spent.
    check(((HDoc *)(src = parse("<p>")))->id != ((HDoc *)(dst = parse("<p>")))->id, "two documents, two marks");
    uint32_t serial = h_document_serial;
    h_document_serial = UINT32_MAX - 1;
    os64_html_document_t *last = parse("<p>last");
    check(last && ((HDoc *)last)->id == UINT32_MAX && ((HDoc *)last)->id != ((HDoc *)src)->id &&
              last->document->document_id == UINT32_MAX,
          "the last document the count allows is made");
    size_t held = live;
    check(!parse("<p>one too many") && !parse("<p>and again") && live == held &&
              h_document_serial == UINT32_MAX,
          "and no more are, at no cost in memory");
    check(os64_html_insert(last, last->body, os64_html_create_comment(last, "c", 1, NULL), NULL) == OS64_HTML_OK &&
              os64_html_insert(src, src->body, os64_html_create_comment(src, "c", 1, NULL), NULL) == OS64_HTML_OK,
          "the documents that exist still work");
    h_document_serial = serial;
    os64_html_document_free(last);
    os64_html_document_free(src);
    os64_html_document_free(dst);
    check(live == 0, "two-document cases freed");
}

// ── When memory runs out ────────────────────────────────────────────────

typedef int64_t (*Verb)(os64_html_document_t *doc);
static HNode *made;
static int64_t no_prepare(os64_html_document_t *doc)
{
    (void)doc;
    return 0;
}
// So that a clone has records of its own to copy, not a shared list.
static int64_t own_attrs(os64_html_document_t *doc)
{
    return os64_html_set_attr(doc, id_in(doc, "m"), "own", "1", 1);
}
static int64_t v_set_text(os64_html_document_t *doc)
{
    return os64_html_set_text(doc, id_in(doc, "b")->first_child, "replacement", 11);
}
static int64_t v_set_attr_first(os64_html_document_t *doc)
{
    return os64_html_set_attr(doc, id_in(doc, "a"), "class", "k", 1);
}
static int64_t v_set_attr_existing(os64_html_document_t *doc)
{
    return os64_html_set_attr(doc, id_in(doc, "m"), "title", "k", 1);
}
static int64_t v_remove_attr(os64_html_document_t *doc)
{
    return os64_html_remove_attr(doc, id_in(doc, "m"), "title");
}
static int64_t v_create_element(os64_html_document_t *doc)
{
    int64_t st = 0;
    made = os64_html_create_element(doc, OS64_HTML_NS_HTML, "template", &st);
    return st;
}
static int64_t v_create_text(os64_html_document_t *doc)
{
    int64_t st = 0;
    made = os64_html_create_text(doc, "some new text", 13, &st);
    return st;
}
static int64_t v_clone(os64_html_document_t *doc)
{
    int64_t st = 0;
    made = os64_html_clone(doc, doc->body, true, &st);
    return st;
}
// A clone from another document copies what one from its own may share.
static os64_html_document_t *elsewhere;
static int64_t open_elsewhere(os64_html_document_t *doc)
{
    (void)doc;
    elsewhere = parse("<!doctype html PUBLIC \"p\" \"s\"><body><div id=a x=1><b title=t>one<p>two</b>"
                      "<template><i lang=en>in</i></template></div>");
    return 0;
}
static void close_elsewhere(void)
{
    os64_html_document_free(elsewhere);
    elsewhere = NULL;
}
static int64_t v_clone_across(os64_html_document_t *doc)
{
    int64_t st = 0;
    made = os64_html_clone(doc, elsewhere->html, true, &st);
    return st;
}
static int64_t v_clone_doctype_across(os64_html_document_t *doc)
{
    int64_t st = 0;
    made = os64_html_clone(doc, elsewhere->document->first_child, false, &st);
    return st;
}
static void t_out_of_memory(void)
{
    static const struct {
        const char *name;
        Verb prepare, verb;
    } verbs[] = {{"set_text", no_prepare, v_set_text},
                 {"set_attr, first change", no_prepare, v_set_attr_first},
                 {"set_attr, existing", no_prepare, v_set_attr_existing},
                 {"remove_attr", no_prepare, v_remove_attr},
                 {"create_element", no_prepare, v_create_element},
                 {"create_text", no_prepare, v_create_text},
                 {"clone", own_attrs, v_clone},
                 {"clone from another document", open_elsewhere, v_clone_across},
                 {"clone of another document's doctype", open_elsewhere, v_clone_doctype_across}};
    const char *markup = "<body><div id=a x=1 y=2 z=3><span id=b>one</span></div>"
                         "<p id=m title=t lang=en>two<template><i>in</i></template></p>";
    size_t swept = 0;
    for (size_t v = 0; v < H_ARRAY(verbs); v++) {
        // How many allocations the verb makes when nothing fails.
        os64_html_document_t *doc = parse(markup);
        fail_at = 0;
        verbs[v].prepare(doc);
        allocations = 0;
        check(verbs[v].verb(doc) == OS64_HTML_OK, "%s succeeds", verbs[v].name);
        size_t needed = allocations;
        os64_html_document_free(doc);
        close_elsewhere();
        for (size_t n = 1; n <= needed; n++) {
            doc = parse(markup);
            verbs[v].prepare(doc);
            char *before = spelled(doc->document);
            uint64_t version = os64_html_version(doc);
            size_t arena = doc->arena_bytes, blocks = live;
            allocations = 0;
            fail_at = n;
            made = NULL;
            int64_t got = verbs[v].verb(doc);
            fail_at = 0;
            char *after = spelled(doc->document);
            check(got == OS64_HTML_NO_MEMORY, "%s, allocation %zu: %s", verbs[v].name, n,
                  os64_html_status_name(got));
            check(strcmp(before, after) == 0 && os64_html_version(doc) == version,
                  "%s, allocation %zu changed the tree", verbs[v].name, n);
            check(!made, "%s, allocation %zu answered a node", verbs[v].name, n);
            // A verb that only replaces gives back whatever it took before it
            // failed. One that makes nodes may have made some: those stay
            // charged, as any detached node does.
            bool makes_nodes = verbs[v].verb == v_create_element || verbs[v].verb == v_create_text ||
                               verbs[v].verb == v_clone || verbs[v].verb == v_clone_across ||
                               verbs[v].verb == v_clone_doctype_across;
            if (!makes_nodes)
                check(doc->arena_bytes == arena && live == blocks, "%s, allocation %zu kept memory", verbs[v].name, n);
            check(doc->refusal == 0, "%s left a refusal on the document", verbs[v].name);
            HDoc *d = (HDoc *)doc;
            check(links_ok(d, doc->document, 0) == d->records, "%s, allocation %zu: links", verbs[v].name, n);
            // And the document still works afterwards.
            check(verbs[v].verb(doc) == OS64_HTML_OK, "%s succeeds after the failure", verbs[v].name);
            free(before);
            free(after);
            os64_html_document_free(doc);
            close_elsewhere();
            check(live == 0, "%s, allocation %zu leaked", verbs[v].name, n);
            swept++;
        }
    }
    printf("Out of memory: %zu failure points over %zu verbs\n", swept, H_ARRAY(verbs));

    // The arena's own limit, reached by a script's churn: refused by name,
    // and the parse's verdict on the document is not touched.
    os64_html_options_t opt = os64_html_options_default();
    opt.max_arena_bytes = 16384;
    os64_html_document_t *doc = parse_with("<body><p id=p>x</p>", &opt);
    HNode *p = id_in(doc, "p");
    int64_t st = OS64_HTML_OK;
    size_t count = 0;
    while (st == OS64_HTML_OK && count < 100000) {
        HNode *e = os64_html_create_element(doc, OS64_HTML_NS_HTML, "span", &st);
        if (e)
            st = os64_html_insert(doc, p, e, NULL);
        count++;
    }
    check(st == OS64_HTML_ARENA_EXHAUSTED && doc->refusal == 0 && doc->arena_bytes <= opt.max_arena_bytes,
          "node churn meets the arena limit after %zu nodes: %s", count, os64_html_status_name(st));
    char big[20000];
    memset(big, 'x', sizeof(big));
    char *before = spelled(doc->document);
    check(os64_html_set_text(doc, p->first_child, big, sizeof(big)) == OS64_HTML_ARENA_EXHAUSTED,
          "a text larger than the arena is refused");
    check(os64_html_set_attr(doc, p, "a", big, sizeof(big)) == OS64_HTML_ARENA_EXHAUSTED,
          "and so is an attribute value");
    char *after = spelled(doc->document);
    check(strcmp(before, after) == 0, "both left the tree as it was");
    free(before);
    free(after);
    os64_html_document_free(doc);
    check(live == 0, "arena cases freed");
}

// ── The second tree ─────────────────────────────────────────────────────
//
// Written to be read against the DOM Standard and not against dom.c: nodes
// hold arrays of children, depth and ancestry are answered by walking those
// arrays, and nothing here is shared with the library but the status names.

typedef struct R R;
struct R {
    int kind, ns;
    char *name, *text;
    char **an, **av;        // attribute names and values, in order
    int nattrs;
    R **kids;
    int nkids, capkids;
    R *parent;
    R *contents, *host;     // a template's fragment, and the fragment's template
    R *owner;               // the form a control is tied to
    HNode *real;
    unsigned holds;
    bool candidate, deferred;
    uint64_t retired_at;
};
static R **all;
static int nall, capall;
// Real node to model node.
#define TIES 16384
static struct {
    const HNode *real;
    R *r;
} ties[TIES];
static size_t tie_slot(const HNode *real)
{
    size_t at = ((uintptr_t)real >> 4) * 2654435761u % TIES;
    while (ties[at].real && ties[at].real != real)
        at = (at + 1) % TIES;
    return at;
}
static void r_tie(R *r, HNode *real)
{
    r->real = real;
    size_t at = tie_slot(real);
    ties[at].real = real;
    ties[at].r = r;
}
static R *r_of(const HNode *real)
{
    return real ? ties[tie_slot(real)].r : NULL;
}
static R *r_new(int kind)
{
    R *r = calloc(1, sizeof(*r));
    r->kind = kind;
    if (nall == capall)
        all = realloc(all, (capall = capall ? capall * 2 : 256) * sizeof(*all));
    all[nall++] = r;
    return r;
}
static void r_free_all(void)
{
    for (int i = 0; i < nall; i++) {
        R *r = all[i];
        for (int a = 0; a < r->nattrs; a++) {
            free(r->an[a]);
            free(r->av[a]);
        }
        free(r->an);
        free(r->av);
        free(r->kids);
        free(r->name);
        free(r->text);
        free(r);
    }
    free(all);
    all = NULL;
    nall = capall = 0;
    memset(ties, 0, sizeof(ties));
}
static int r_index(const R *parent, const R *kid)
{
    for (int i = 0; i < parent->nkids; i++)
        if (parent->kids[i] == kid)
            return i;
    return -1;
}
static void r_put(R *parent, int at, R *kid)
{
    if (parent->nkids == parent->capkids)
        parent->kids = realloc(parent->kids, (parent->capkids = parent->capkids ? parent->capkids * 2 : 4) *
                                                 sizeof(*parent->kids));
    memmove(parent->kids + at + 1, parent->kids + at, (size_t)(parent->nkids - at) * sizeof(*parent->kids));
    parent->kids[at] = kid;
    parent->nkids++;
    kid->parent = parent;
}
static void r_take(R *kid)
{
    R *parent = kid->parent;
    int at = r_index(parent, kid);
    memmove(parent->kids + at, parent->kids + at + 1, (size_t)(parent->nkids - at - 1) * sizeof(*parent->kids));
    parent->nkids--;
    kid->parent = NULL;
}
static R *r_import(const HNode *n)
{
    R *r = r_new(n->kind);
    r_tie(r, (HNode *)n);
    r->ns = n->ns;
    r->name = n->name ? strdup(n->name) : NULL;
    r->text = n->text ? strdup(n->text) : NULL;
    for (const HAttr *a = n->attrs; a; a = a->next) {
        r->an = realloc(r->an, (size_t)(r->nattrs + 1) * sizeof(char *));
        r->av = realloc(r->av, (size_t)(r->nattrs + 1) * sizeof(char *));
        r->an[r->nattrs] = strdup(a->name);
        r->av[r->nattrs++] = strdup(a->value);
    }
    if (n->template_contents) {
        r->contents = r_import(n->template_contents);
        r->contents->host = r;
    }
    for (const HNode *c = n->first_child; c; c = c->next)
        r_put(r, r->nkids, r_import(c));
    return r;
}
static bool r_is(const R *r, const char *name)
{
    return r->kind == OS64_HTML_ELEMENT && r->ns == OS64_HTML_NS_HTML && strcmp(r->name, name) == 0;
}
static R *r_root(R *r)
{
    while (r->parent)
        r = r->parent;
    return r;
}
static int r_depth(const R *r)
{
    int depth = 0;
    while (r->parent || r->host) {
        if (r->parent) {
            r = r->parent;
            depth++;
        } else
            r = r->host;
    }
    return depth;
}
static int r_height(const R *r)
{
    int tallest = 0;
    for (int i = 0; i < r->nkids; i++) {
        int h = 1 + r_height(r->kids[i]);
        if (h > tallest)
            tallest = h;
    }
    if (r->contents)
        for (int i = 0; i < r->contents->nkids; i++) {
            int h = 1 + r_height(r->contents->kids[i]);
            if (h > tallest)
                tallest = h;
        }
    return tallest;
}
static int r_size(const R *r)
{
    int n = 1;
    for (int i = 0; i < r->nkids; i++)
        n += r_size(r->kids[i]);
    for (int i = 0; r->contents && i < r->contents->nkids; i++)
        n += r_size(r->contents->kids[i]);
    return n;
}
static int r_count(const R *parent, int kind, const R *except)
{
    int n = 0;
    for (int i = 0; i < parent->nkids; i++)
        if (parent->kids[i]->kind == kind && parent->kids[i] != except)
            n++;
    return n;
}
// The DOM Standard's "ensure pre-insertion validity" (replacing: its twin
// in "replace"), then libhtml's two rules of its own.
static int64_t r_may(R *document, int limit, R *parent, R *node, R *child, bool replacing)
{
    if (!parent || !node || (replacing && !child))
        return OS64_HTML_BAD_ARGUMENT;
    if (parent->kind != OS64_HTML_DOCUMENT && parent->kind != OS64_HTML_FRAGMENT &&
        parent->kind != OS64_HTML_ELEMENT)
        return OS64_HTML_HIERARCHY;
    for (R *a = parent; a; a = a->parent ? a->parent : a->host)
        if (a == node)
            return OS64_HTML_HIERARCHY;
    if (child && child->parent != parent)
        return OS64_HTML_NOT_FOUND;
    if (node->kind == OS64_HTML_DOCUMENT)
        return OS64_HTML_HIERARCHY;
    if (node->kind == OS64_HTML_TEXT && parent->kind == OS64_HTML_DOCUMENT)
        return OS64_HTML_HIERARCHY;
    if (node->kind == OS64_HTML_DOCTYPE && parent->kind != OS64_HTML_DOCUMENT)
        return OS64_HTML_HIERARCHY;
    R *bringing = node->kind == OS64_HTML_ELEMENT ? node : NULL;
    if (parent->kind == OS64_HTML_DOCUMENT) {
        int at = child ? r_index(parent, child) : parent->nkids;
        bool doctype_after = false, element_before = false;
        for (int i = at + 1; child && i < parent->nkids; i++)
            doctype_after |= parent->kids[i]->kind == OS64_HTML_DOCTYPE;
        for (int i = 0; child && i < at; i++)
            element_before |= parent->kids[i]->kind == OS64_HTML_ELEMENT;
        int elements = r_count(parent, OS64_HTML_ELEMENT, replacing ? child : NULL);
        int doctypes = r_count(parent, OS64_HTML_DOCTYPE, replacing ? child : NULL);
        if (node->kind == OS64_HTML_FRAGMENT) {
            int inside = r_count(node, OS64_HTML_ELEMENT, NULL);
            if (inside > 1 || r_count(node, OS64_HTML_TEXT, NULL))
                return OS64_HTML_HIERARCHY;
            for (int i = 0; inside == 1 && i < node->nkids; i++)
                if (node->kids[i]->kind == OS64_HTML_ELEMENT)
                    bringing = node->kids[i];
        }
        if (bringing) {
            if (elements || doctype_after)
                return OS64_HTML_HIERARCHY;
            if (!replacing && child && child->kind == OS64_HTML_DOCTYPE)
                return OS64_HTML_HIERARCHY;
        }
        if (node->kind == OS64_HTML_DOCTYPE) {
            if (doctypes || element_before)
                return OS64_HTML_HIERARCHY;
            if (!replacing && !child && r_count(parent, OS64_HTML_ELEMENT, NULL))
                return OS64_HTML_HIERARCHY;
        }
    }
    if (parent == document) {
        if (bringing && !r_is(bringing, "html"))
            return OS64_HTML_ROOT_REQUIRED;
        if (replacing && child->kind == OS64_HTML_ELEMENT && !bringing)
            return OS64_HTML_ROOT_REQUIRED;
    } else if (node->parent == document && node->kind == OS64_HTML_ELEMENT)
        return OS64_HTML_ROOT_REQUIRED;
    int under = r_depth(parent) + 1;
    if (node->kind == OS64_HTML_FRAGMENT) {
        for (int i = 0; i < node->nkids; i++)
            if (under + r_height(node->kids[i]) > limit)
                return OS64_HTML_TOO_DEEP;
    } else if (under + r_height(node) > limit)
        return OS64_HTML_TOO_DEEP;
    return OS64_HTML_OK;
}
static bool r_inside(R *r, const R *top)
{
    for (; r; r = r->parent)
        if (r == top)
            return true;
    return false;
}
// The removing half of the HTML Standard's rule: after `moved` leaves its
// tree, a control and the form it is tied to that are no longer in one tree
// are parted.
static void r_part(R *moved)
{
    for (int i = 0; i < nall; i++) {
        R *control = all[i];
        if (control->real && control->owner && r_inside(control, moved) != r_inside(control->owner, moved) &&
            (r_root(control) == moved || r_root(control->owner) == moved))
            control->owner = NULL;
    }
}
static int r_tied(void)
{
    int tied = 0;
    for (int i = 0; i < nall; i++)
        tied += all[i]->real && all[i]->owner != NULL;
    return tied;
}
static void r_move(R *parent, R *node, R *before)
{
    node->candidate = false;
    node->retired_at = 0;
    if (node->parent) {
        r_take(node);
        r_part(node);
    }
    r_put(parent, before ? r_index(parent, before) : parent->nkids, node);
}
static void r_insert(R *parent, R *node, R *before)
{
    if (before == node) {
        int at = r_index(parent, node);
        before = at + 1 < parent->nkids ? parent->kids[at + 1] : NULL;
    }
    if (node->kind == OS64_HTML_FRAGMENT)
        while (node->nkids)
            r_move(parent, node->kids[0], before);
    else
        r_move(parent, node, before);
}
static R *r_clone(const R *from, bool deep)
{
    R *r = r_new(from->kind);
    r->ns = from->ns;
    r->name = from->name ? strdup(from->name) : NULL;
    r->text = from->text ? strdup(from->text) : NULL;
    for (int a = 0; a < from->nattrs; a++) {
        r->an = realloc(r->an, (size_t)(r->nattrs + 1) * sizeof(char *));
        r->av = realloc(r->av, (size_t)(r->nattrs + 1) * sizeof(char *));
        r->an[r->nattrs] = strdup(from->an[a]);
        r->av[r->nattrs++] = strdup(from->av[a]);
    }
    if (from->contents) {
        r->contents = r_new(OS64_HTML_FRAGMENT);
        r->contents->host = r;
        for (int i = 0; deep && i < from->contents->nkids; i++)
            r_put(r->contents, r->contents->nkids, r_clone(from->contents->kids[i], true));
    }
    for (int i = 0; deep && i < from->nkids; i++)
        r_put(r, r->nkids, r_clone(from->kids[i], true));
    return r;
}
// Tie a freshly made R subtree to the real one the library made for it.
static void r_bind(R *r, HNode *real)
{
    r_tie(r, real);
    if (r->contents && real->template_contents)
        r_bind(r->contents, real->template_contents);
    HNode *c = real->first_child;
    for (int i = 0; i < r->nkids && c; i++, c = c->next)
        r_bind(r->kids[i], c);
}
static bool r_listed(const R *r)
{
    static const char *const names[] = {"button", "fieldset", "input", "object", "output", "select", "textarea"};
    for (size_t i = 0; i < H_ARRAY(names); i++)
        if (r_is(r, names[i]))
            return true;
    return false;
}
static int r_attr(const R *r, const char *name)
{
    for (int a = 0; a < r->nattrs; a++)
        if (strcmp(r->an[a], name) == 0)
            return a;
    return -1;
}

// Every node the model knows, against the real node it stands for.
static void r_compare(os64_html_document_t *doc, R *document, unsigned step, const char *did)
{
    size_t bad = checks_failed, records = 0, form_inputs = 0;
    for (int i = 0; i < nall && checks_failed == bad; i++) {
        const R *r = all[i];
        const HNode *n = r->real;
        if (!n) continue;
        bool same = (int)n->kind == r->kind && (r->kind != OS64_HTML_ELEMENT || (int)n->ns == r->ns);
        same = same && (r->name ? n->name && strcmp(n->name, r->name) == 0 : true);
        if (r->kind == OS64_HTML_TEXT || r->kind == OS64_HTML_COMMENT)
            same = same && n->text && strcmp(n->text, r->text) == 0 && n->text_len == strlen(r->text);
        same = same && r_of(n->parent) == r->parent;
        same = same && (r->contents ? r->contents->real == n->template_contents : !n->template_contents);
        same = same && r_of(n->form_owner) == r->owner;
        records += n->form_owner != NULL;
        const HNode *c = n->first_child, *prev = NULL;
        for (int k = 0; k < r->nkids; k++, prev = c, c = c ? c->next : NULL)
            same = same && c == r->kids[k]->real && c->prev == prev && c->parent == n;
        same = same && c == NULL && n->last_child == prev;
        const HAttr *a = n->attrs;
        for (int k = 0; k < r->nattrs; k++, a = a ? a->next : NULL)
            same = same && a && strcmp(a->name, r->an[k]) == 0 && strcmp(a->value, r->av[k]) == 0;
        if (r_is(r, "input"))
            for (int k = 0; k < r->nattrs; k++)
                form_inputs += strcmp(r->an[k], "form") == 0;
        same = same && a == NULL;
        if (!same) {
            char *got = spelled(n);
            check(false, "step %u (%s): node %d kind=%d candidate=%d deferred=%d stamp=%llu holds=%u parent=%p host=%p differs from model; real is %s", step, did, i, r->kind, r->candidate, r->deferred, (unsigned long long)r->retired_at, r->holds, (void *)r->parent, (void *)r->host, got);
            free(got);
        }
    }
    const HDoc *d = (const HDoc *)doc;
    check(records == d->records, "step %u (%s): %zu records in the tree, the document counts %zu", step, did,
          records, d->records);
    check(form_inputs == os64_html_form_input_count(doc),
          "step %u (%s): %zu owned explicit-form inputs, the document counts %zu", step, did,
          form_inputs, os64_html_form_input_count(doc));
    // The landmarks, by their definitions.
    R *html = NULL, *head = NULL, *body = NULL;
    for (int i = 0; i < document->nkids && !html; i++)
        if (document->kids[i]->kind == OS64_HTML_ELEMENT)
            html = document->kids[i];
    for (int i = 0; html && i < html->nkids; i++) {
        if (!head && r_is(html->kids[i], "head"))
            head = html->kids[i];
        if (!body && r_is(html->kids[i], "body"))
            body = html->kids[i];
    }
    check(html && doc->html == html->real && doc->head == (head ? head->real : NULL) &&
              doc->body == (body ? body->real : NULL),
          "step %u (%s): the landmarks", step, did);
    for (int i = 0; i < nall; i++)
        if (all[i]->real && !all[i]->parent && !all[i]->host)
            (void)links_ok(d, all[i]->real, (size_t)r_depth(all[i]));
}

// Strings a snapshot borrowed from nodes available when it was taken.
typedef struct {
    const char *at;
    char *copy;
} Borrowed;
typedef struct {
    os64_html_pin_t pin;
    Borrowed *items;
    size_t n;
} Snapshot;
static void borrow(Snapshot *s, const char *at)
{
    if (!at)
        return;
    s->items = realloc(s->items, (s->n + 1) * sizeof(*s->items));
    s->items[s->n++] = (Borrowed){at, strdup(at)};
}
static bool r_available(const R *r);
static void snapshot_take(os64_html_document_t *doc, Snapshot *s)
{
    s->pin = os64_html_pin(doc);
    s->items = NULL;
    s->n = 0;
    for (int i = 0; i < nall; i++) {
        if (!r_available(all[i])) continue;
        const HNode *n = all[i]->real;
        borrow(s, n->name);
        borrow(s, n->text);
        for (const HAttr *a = n->attrs; a; a = a->next) {
            borrow(s, a->name);
            borrow(s, a->value);
        }
    }
}
static void snapshot_check(const Snapshot *s, unsigned step)
{
    for (size_t i = 0; i < s->n; i++)
        if (strcmp(s->items[i].at, s->items[i].copy) != 0) {
            check(false, "step %u: a pinned string changed: \"%s\" was \"%s\"", step, s->items[i].at,
                  s->items[i].copy);
            return;
        }
}
static void snapshot_drop(os64_html_document_t *doc, Snapshot *s)
{
    os64_html_unpin(doc, s->pin);
    for (size_t i = 0; i < s->n; i++)
        free(s->items[i].copy);
    free(s->items);
    s->pin = 0;
}

static uint32_t rng;
static uint32_t pick(uint32_t n)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng % n;
}
// Reclamation decisions use model ownership and holds, independently of
// libhtml's metadata and live-node registry. A pin-only retired unit is no
// longer offered to the operation picker; its counters remain until unpin.
static bool r_held(const R *r)
{
    if (r->holds) return true;
    if (r->contents && r_held(r->contents)) return true;
    for (int i = 0; i < r->nkids; i++) if (r_held(r->kids[i])) return true;
    return false;
}
static void r_forget(R *r)
{
    if (r->contents) r_forget(r->contents);
    for (int i = 0; i < r->nkids; i++) r_forget(r->kids[i]);
    ties[tie_slot(r->real)].r = NULL;
    r->real = NULL;
}
static bool r_available(const R *r)
{
    if (!r->real) return false;
    for (const R *a = r; a; a = a->parent ? a->parent : a->host)
        if (a->retired_at) return false;
    return true;
}
static R *r_pick(void)
{
    R *r;
    do r = all[pick((uint32_t)nall)]; while (!r_available(r));
    return r;
}
static size_t r_live(void)
{
    size_t n = 0;
    for (int i = 0; i < nall; i++) n += all[i]->real != NULL;
    return n;
}
static void r_collect(Snapshot *snaps, size_t nsnaps, uint64_t version, const uint64_t *pin_versions)
{
    for (int i = 0; i < nall; i++) {
        R *r = all[i];
        if (!r->real || !r->candidate || r->parent || r->host || r_held(r)) continue;
        if (!r->retired_at) r->retired_at = version + (r->deferred ? 1 : 0);
        bool protected = false;
        for (size_t j = 0; j < nsnaps; j++)
            protected |= snaps[j].pin && pin_versions[j] < r->retired_at;
        if (!protected) r_forget(r);
    }
}
static size_t oracle_success[14][2], oracle_freed[14];
static void t_random(const char *markup, uint32_t seed, unsigned steps, size_t max_depth)
{
    static const char *const names[] = {"div",  "span", "p",      "b",     "form",   "input", "select",
                                        "html", "head", "body",   "table", "template", "img", "button"};
    static const char *const attrs[] = {"id", "class", "form", "name", "title", "xlink:href"};
    static const char *const values[] = {"", "a", "f", "long value with spaces", "caf\xc3\xa9", "\xe6\x97\xa5\xe6\x9c\xac"};
    static const char *const bad[] = {"\xff", "a\xc3", "\xed\xa0\x80"};
    os64_html_options_t opt = os64_html_options_default();
    opt.max_depth = max_depth;
    os64_html_document_t *doc = parse_with(markup, &opt);
    check(doc && !doc->refusal, "random walk: fixture parsed");
    R *document = r_import(doc->document);
    for (int i = 0; i < nall; i++)
        all[i]->owner = r_of(all[i]->real->form_owner);
    r_compare(doc, document, 0, "import");
    rng = seed;
    Snapshot snaps[3] = {{0}};
    uint64_t pin_versions[3] = {0};
    size_t coverage[14][2] = {{0}}, reclaimed[14] = {0};
    size_t refused = 0, done = 0, by_name[11] = {0};
    for (unsigned step = 1; step <= steps && !checks_failed; step++) {
        R *x = r_pick(), *y = r_pick();
        // Under a low limit, lean towards deep parents: left to chance a
        // random tree stays too shallow to meet the limit at all.
        for (int tries = 0; max_depth < 8 && tries < 6; tries++) {
            R *other = r_pick();
            if (r_depth(other) > r_depth(y))
                y = other;
        }
        R *z = pick(3) ? NULL : r_pick();
        if (z == NULL && y->nkids && pick(2))
            z = y->kids[pick((uint32_t)y->nkids)];
        char did[64];
        int64_t want = OS64_HTML_OK, got = OS64_HTML_OK;
        uint64_t version = os64_html_version(doc);
        bool changes = true;
        uint32_t what_to_do = pick(14);
        size_t owned_before = r_live();
        bool held_before = r_held(x);
        if (r_live() > 1500 && (what_to_do == 9 || what_to_do == 10))
            what_to_do = 4;         // the walk has made enough nodes: take one out instead
        switch (what_to_do) {
        case 0:
        case 1:
        case 2:     // insert x under y before z
            snprintf(did, sizeof(did), "insert");
            want = r_may(document, (int)(2 * max_depth + 2), y, x, z, false);
            got = os64_html_insert(doc, y->real, x->real, z ? z->real : NULL);
            if (!want) {
                // What a reader could tell apart afterwards: where x is, and
                // who is tied to whom.
                R *was_under = x->parent;
                int was_at = was_under ? r_index(was_under, x) : -1, tied = r_tied();
                bool brought = x->kind != OS64_HTML_FRAGMENT || x->nkids > 0;
                if (x->kind == OS64_HTML_FRAGMENT) { x->candidate = true; x->deferred = x->holds != 0 || !x->nkids; }
                r_insert(y, x, z);
                changes = brought && (x->kind == OS64_HTML_FRAGMENT || x->parent != was_under ||
                                      r_index(x->parent, x) != was_at || r_tied() != tied);
            }
            break;
        case 3:     // replace z under y by x
            snprintf(did, sizeof(did), "replace");
            want = r_may(document, (int)(2 * max_depth + 2), y, x, z, true);
            got = os64_html_replace(doc, y->real, x->real, z ? z->real : NULL);
            if (!want && x != z) {
                int at = r_index(y, z);
                R *before = at + 1 < y->nkids ? y->kids[at + 1] : NULL;
                if (before == x) {
                    int xat = r_index(y, x);
                    before = xat + 1 < y->nkids ? y->kids[xat + 1] : NULL;
                }
                z->candidate = true; z->deferred = r_held(z);
                if (x->kind == OS64_HTML_FRAGMENT) { x->candidate = true; x->deferred = x->holds != 0; }
                r_take(z);
                r_part(z);
                r_insert(y, x, before);
            }
            changes = x != z;
            break;
        case 4:     // remove x
            snprintf(did, sizeof(did), "remove");
            if (x->parent == document && x->kind == OS64_HTML_ELEMENT)
                want = OS64_HTML_ROOT_REQUIRED;
            changes = x->parent != NULL;
            got = os64_html_remove(doc, x->real);
            if (!want && x->parent) {
                x->candidate = true; x->deferred = r_held(x);
                r_take(x);
                r_part(x);
            }
            break;
        case 5:
        case 6: {   // set an attribute on x
            const char *name = attrs[pick(H_ARRAY(attrs))], *value = values[pick(H_ARRAY(values))];
            bool poison = pick(12) == 0;
            if (poison)
                value = bad[pick(H_ARRAY(bad))];
            snprintf(did, sizeof(did), "set_attr %s", name);
            want = x->kind != OS64_HTML_ELEMENT ? OS64_HTML_BAD_ARGUMENT : poison ? OS64_HTML_BAD_TEXT : 0;
            got = os64_html_set_attr(doc, x->real, name, value, strlen(value));
            if (!want) {
                int at = r_attr(x, name);
                changes = at < 0 || strcmp(x->av[at], value) != 0;
                if (at < 0) {
                    x->an = realloc(x->an, (size_t)(x->nattrs + 1) * sizeof(char *));
                    x->av = realloc(x->av, (size_t)(x->nattrs + 1) * sizeof(char *));
                    x->an[x->nattrs] = strdup(name);
                    x->av[x->nattrs++] = strdup(value);
                } else {
                    free(x->av[at]);
                    x->av[at] = strdup(value);
                }
                if (strcmp(name, "form") == 0 && r_listed(x))
                    x->owner = NULL;
            }
            break;
        }
        case 7: {   // remove an attribute from x
            const char *name = attrs[pick(H_ARRAY(attrs))];
            snprintf(did, sizeof(did), "remove_attr %s", name);
            want = x->kind != OS64_HTML_ELEMENT ? OS64_HTML_BAD_ARGUMENT : 0;
            got = os64_html_remove_attr(doc, x->real, name);
            int at = want ? -1 : r_attr(x, name);
            changes = at >= 0;
            if (at >= 0) {
                free(x->an[at]);
                free(x->av[at]);
                memmove(x->an + at, x->an + at + 1, (size_t)(x->nattrs - at - 1) * sizeof(char *));
                memmove(x->av + at, x->av + at + 1, (size_t)(x->nattrs - at - 1) * sizeof(char *));
                x->nattrs--;
                if (strcmp(name, "form") == 0 && r_listed(x))
                    x->owner = NULL;
            }
            break;
        }
        case 8: {   // set x's text
            const char *value = values[pick(H_ARRAY(values))];
            snprintf(did, sizeof(did), "set_text");
            want = x->kind != OS64_HTML_TEXT && x->kind != OS64_HTML_COMMENT ? OS64_HTML_BAD_ARGUMENT : 0;
            got = os64_html_set_text(doc, x->real, value, strlen(value));
            if (!want) {
                changes = strcmp(x->text, value) != 0;
                free(x->text);
                x->text = strdup(value);
            }
            break;
        }
        case 9: {   // a new element, text, comment or fragment
            snprintf(did, sizeof(did), "create");
            changes = false;
            R *r;
            HNode *real;
            uint32_t what = pick(8);
            if (what < 5) {
                const char *name = names[pick(H_ARRAY(names))];
                int ns = pick(10) ? OS64_HTML_NS_HTML : OS64_HTML_NS_SVG;
                real = os64_html_create_element(doc, (os64_html_ns_t)ns, name, &got);
                r = r_new(OS64_HTML_ELEMENT);
                r->ns = ns;
                r->name = strdup(name);
                if (r_is(r, "template")) {
                    r->contents = r_new(OS64_HTML_FRAGMENT);
                    r->contents->host = r;
                }
            } else if (what < 7) {
                const char *value = values[pick(H_ARRAY(values))];
                int kind = what == 5 ? OS64_HTML_TEXT : OS64_HTML_COMMENT;
                real = kind == OS64_HTML_TEXT ? os64_html_create_text(doc, value, strlen(value), &got)
                                             : os64_html_create_comment(doc, value, strlen(value), &got);
                r = r_new(kind);
                r->text = strdup(value);
            } else {
                real = os64_html_create_fragment(doc, &got);
                r = r_new(OS64_HTML_FRAGMENT);
            }
            if (real)
                r_bind(r, real);
            else
                want = OS64_HTML_NO_MEMORY;     // not expected: the arena here is large
            break;
        }
        case 10: {  // clone x
            bool deep = pick(2) && r_size(x) <= 40;
            snprintf(did, sizeof(did), "%s", deep ? "clone deep" : "clone");
            changes = false;
            want = x->kind == OS64_HTML_DOCUMENT ? OS64_HTML_BAD_ARGUMENT : 0;
            HNode *real = os64_html_clone(doc, x->real, deep, &got);
            if (!want && real)
                r_bind(r_clone(x, deep), real);
            else if (!want)
                want = OS64_HTML_NO_MEMORY;
            break;
        }
        case 12: {
            snprintf(did, sizeof(did), "hold/release");
            changes = false;
            if (x->holds && pick(2)) {
                x->holds--;
                os64_html_release(doc, x->real);
            } else {
                x->holds++;
                os64_html_hold(doc, x->real);
            }
            break;
        }
        case 13: {
            snprintf(did, sizeof(did), "parse_fragment");
            changes = false;
            const char *payload = "<div><input form=f><template><span>x</span></template></div>";
            HNode *real = os64_html_parse_fragment(doc, doc->body ? doc->body : doc->html,
                                                   payload, strlen(payload), false, &got);
            if (real) {
                int start = nall;
                r_import(real);
                for (int i = start; i < nall; i++) all[i]->owner = r_of(all[i]->real->form_owner);
            } else want = got;
            break;
        }
        default: {  // take a snapshot, or let one go
            snprintf(did, sizeof(did), "pin");
            changes = false;
            Snapshot *s = &snaps[pick(H_ARRAY(snaps))];
            if (s->pin) {
                snapshot_check(s, step);
                snapshot_drop(doc, s);
            } else {
                pin_versions[s - snaps] = os64_html_version(doc);
                snapshot_take(doc, s);
            }
            break;
        }
        }
        check(got == want, "step %u (%s): %s, the model says %s", step, did, os64_html_status_name(got),
              os64_html_status_name(want));
        if (got) {
            refused++;
            if (got < 0 && got >= -10)
                by_name[-got]++;
        } else
            done++;
        check(got || !changes ? os64_html_version(doc) == version : os64_html_version(doc) > version,
              "step %u (%s): the version %s", step, did, got || !changes ? "moved" : "stood still");
        coverage[what_to_do][held_before]++;
        if (!got) oracle_success[what_to_do][held_before]++;
        r_collect(snaps, H_ARRAY(snaps), os64_html_version(doc), pin_versions);
        size_t now_owned = r_live();
        if (now_owned < owned_before) {
            reclaimed[what_to_do] += owned_before - now_owned;
            oracle_freed[what_to_do] += owned_before - now_owned;
        }
        check(doc->node_count == now_owned, "step %u (%s): model owns %zu nodes, document counts %zu",
              step, did, now_owned, doc->node_count);
        r_compare(doc, document, step, did);
        bool pinned = false;
        for (size_t i = 0; i < H_ARRAY(snaps); i++)
            if (snaps[i].pin) {
                pinned = true;
                snapshot_check(&snaps[i], step);
            }
        if (!pinned)
            check(os64_html_retired_bytes(doc) == 0, "step %u: bytes retired with no pin held", step);
    }
    for (size_t i = 0; i < H_ARRAY(snaps); i++)
        if (snaps[i].pin) {
            snapshot_check(&snaps[i], steps);
            snapshot_drop(doc, &snaps[i]);
        }
    r_collect(snaps, H_ARRAY(snaps), os64_html_version(doc), pin_versions);
    for (int i = 0; i < nall; i++) {
        R *r = all[i];
        while (r->real && r->holds) {
            r->holds--;
            os64_html_release(doc, r->real);
            r_collect(snaps, H_ARRAY(snaps), os64_html_version(doc), pin_versions);
        }
    }
    check(doc->node_count == r_live(), "random walk: model node count after releasing borrowers");
    check(os64_html_retired_bytes(doc) == 0, "random walk: nothing retired once every pin is gone");
    printf("D6 oracle coverage seed=0x%x:", seed);
    for (unsigned i = 0; i < H_ARRAY(coverage); i++)
        printf(" op%u=%zu/%zu freed=%zu", i, coverage[i][0], coverage[i][1], reclaimed[i]);
    putchar('\n');
    printf("Random walk seed=0x%x: %u steps, %zu done, %zu refused (hierarchy %zu, not found %zu, root %zu, "
           "too deep %zu, bad text %zu, bad argument %zu), %zu nodes, version %llu\n",
           seed, steps, done, refused, by_name[6], by_name[7], by_name[9], by_name[4], by_name[8], by_name[10],
           r_live(), (unsigned long long)os64_html_version(doc));
    r_free_all();
    os64_html_document_free(doc);
    check(live == 0, "random walk freed");
}

// Count allocated ownership rather than connectivity: held detached nodes
// can be inserted later, and fragment publication transfers its owned count.
static void t_form_input_count(void)
{
    const char *markup="<div id=host></div><input id=a form=missing><input id=plain>"
        "<input id=empty form><button form=missing></button>"
        "<template id=t><input form=missing></template>";
    os64_html_document_t *doc=parse(markup);
    check(doc&&!doc->refusal,"form count: parsed fixture");
    HNode *a=id_in(doc,"a"),*plain=id_in(doc,"plain"),*host=id_in(doc,"host"),*t=id_in(doc,"t");
    check(os64_html_form_input_count(NULL)==0&&os64_html_form_input_count(doc)==3,
          "form count: parser includes empty values and template contents, excludes other kinds");
    int64_t why=0;
    HNode *foreign=os64_html_create_element(doc,OS64_HTML_NS_SVG,"input",&why);
    check(foreign&&os64_html_set_attr(doc,foreign,"form","missing",7)==0&&
          os64_html_form_input_count(doc)==3,"form count: foreign input excluded");
    check(os64_html_set_attr(doc,a,"form","other",5)==0&&os64_html_form_input_count(doc)==3,
          "form count: value replacement counted once");
    check(os64_html_remove_attr(doc,a,"form")==0&&os64_html_form_input_count(doc)==2,
          "form count: single removal");
    check(os64_html_remove_attr(doc,a,"form")==0&&os64_html_form_input_count(doc)==2,
          "form count: absent removal");
    check(os64_html_set_attr(doc,a,"form","",0)==0&&os64_html_form_input_count(doc)==3,
          "form count: single creation");
    os64_html_attr_change_t batch[]={{"form","f",1,false},{"form",NULL,0,true}};
    check(os64_html_set_attrs(doc,plain,batch,2)==0&&os64_html_form_input_count(doc)==3,
          "form count: batch net absence");
    batch[1]=(os64_html_attr_change_t){"form","",0,false};
    check(os64_html_set_attrs(doc,plain,batch,2)==0&&os64_html_form_input_count(doc)==4,
          "form count: batch creation counted once");
    check(os64_html_set_attrs(doc,plain,batch,2)==0&&os64_html_form_input_count(doc)==4,
          "form count: batch no-op");
    batch[0]=(os64_html_attr_change_t){"form",NULL,0,true};
    check(os64_html_set_attrs(doc,plain,batch,1)==0&&os64_html_form_input_count(doc)==3,
          "form count: batch removal");
    HNode *copy=os64_html_clone(doc,a,false,&why);
    check(copy&&os64_html_form_input_count(doc)==4,"form count: detached shallow clone");
    check(os64_html_clone(doc,t,true,&why)&&os64_html_form_input_count(doc)==5,
          "form count: deep template clone");
    os64_html_hold(doc,copy);
    check(os64_html_insert(doc,host,copy,NULL)==0&&os64_html_remove(doc,copy)==0&&
          os64_html_form_input_count(doc)==5,"form count: detach does not destroy ownership");
    os64_html_document_t *other=parse("<p>foreign clone target</p>");
    check(os64_html_clone(other,a,false,&why)&&os64_html_form_input_count(other)==1,
          "form count: cross-document clone");
    os64_html_document_free(other);
    const char *fragment="<input form=f><input><div>fragment</div>";
    check(os64_html_parse_fragment(doc,host,fragment,strlen(fragment),false,&why)&&
          os64_html_form_input_count(doc)==6,"form count: published fragment count");
    os64_html_release(doc,copy);
    os64_html_document_free(doc);
    check(live==0,"form count: fixture teardown");

    // Attribute/fragment refusals must not publish staged counts. A failed
    // clone may retain owned nodes; the fragment entrance instead rolls back
    // its entire temporary ledger, so its count must remain exact on refusal.
    for (int operation=0; operation<3; operation++) {
        bool reached=false;
        for (size_t cut=1; cut<100&&!reached; cut++) {
            doc=parse("<div id=host></div><input id=a>");
            a=id_in(doc,"a");host=id_in(doc,"host");
            allocations=0;fail_at=cut;
            if (operation==0) why=os64_html_set_attr(doc,a,"form","f",1);
            else if (operation==1) {
                os64_html_attr_change_t change={"form","f",1,false};
                why=os64_html_set_attrs(doc,a,&change,1);
            } else (void)os64_html_parse_fragment(doc,host,fragment,strlen(fragment),false,&why);
            fail_at=0;
            reached=why==OS64_HTML_OK;
            check(reached||why==OS64_HTML_NO_MEMORY,"form count: allocation refusal %d/%zu",operation,cut);
            check(os64_html_form_input_count(doc)==(reached?1u:0u),
                  "form count: staged accounting %d/%zu",operation,cut);
            os64_html_document_free(doc);
            check(live==0,"form count: failure sweep teardown %d/%zu",operation,cut);
        }
        check(reached,"form count: sweep reached success %d",operation);
    }
}

// The unheld churn workload drops pointers to replaced units.
// A section, 49 elements with text children and one comment total 100 nodes.
static char *churn_markup(void)
{
    char *s = calloc(4096, 1);
    strcat(s, "<section>");
    for (unsigned i = 0; i < 49; i++) strcat(s, "<i class=x>payload</i>");
    strcat(s, "<!--tail--></section>");
    return s;
}
static void t_reclaim_churn(unsigned cycles, unsigned mode)
{
    char *markup = churn_markup();
    os64_html_options_t opt = os64_html_options_default();
    opt.max_arena_bytes = 64u * 1024u * 1024u;
    os64_html_document_t *doc = parse_with("<body>", &opt);
    size_t base_nodes = doc->node_count, flat_bytes = 0, flat_nodes = 0;
    HNode *current = NULL;
    HNode **held = mode == 1 ? calloc(cycles + 1, sizeof(*held)) : NULL;
    size_t nheld = 0;
    os64_html_pin_t pin = mode == 2 ? os64_html_pin(doc) : 0;
    unsigned done = 0;
    int64_t why = 0;
    for (unsigned i = 1; i <= cycles; i++) {
        HNode *fragment = os64_html_parse_fragment(doc, doc->body, markup, strlen(markup), false, &why);
        if (!fragment) break;
        HNode *next = fragment->first_child;
        if (mode == 1) {
            held[nheld] = next->first_child->first_child;
            os64_html_hold(doc, held[nheld++]);
        }
        int64_t status = current ? os64_html_replace(doc, doc->body, fragment, current)
                                 : os64_html_insert(doc, doc->body, fragment, NULL);
        check(!status, "churn mode %u step %u insertion: %s", mode, i, os64_html_status_name(status));
        if (status) break;
        current = next;
        done = i;
        if (!mode && i % 1000 == 0) {
            if (!flat_bytes) { flat_bytes = doc->arena_bytes; flat_nodes = doc->node_count; }
            check(doc->arena_bytes == flat_bytes && doc->node_count == flat_nodes,
                  "churn step %u: bytes %zu/%zu nodes %zu/%zu", i, doc->arena_bytes, flat_bytes,
                  doc->node_count, flat_nodes);
            check(doc->node_count == base_nodes + 100, "churn step %u: exactly 100 live payload nodes", i);
        }
    }
    if (!mode) check(done == cycles, "churn: completed %u/%u replacements (%s)", done, cycles,
                     os64_html_status_name(why));
    else {
        check(done < cycles && why == OS64_HTML_ARENA_EXHAUSTED,
              "retained churn mode %u: budget refuses by name after %u steps (%s)", mode, done,
              os64_html_status_name(why));
        check(doc->node_count >= base_nodes + (size_t)done * 100,
              "retained churn mode %u keeps removed payloads", mode);
        if (pin) os64_html_unpin(doc, pin);
        for (size_t i = 0; i < nheld; i++) os64_html_release(doc, held[i]);
        check(doc->node_count == base_nodes + 100 && !os64_html_retired_bytes(doc),
              "retained churn mode %u: releasing borrowers frees obsolete units", mode);
    }
    printf("D6 churn mode=%u steps=%u bytes=%zu nodes=%zu first_sample=%zu/%zu\n", mode, done,
           doc->arena_bytes, doc->node_count, flat_bytes, flat_nodes);
    os64_html_document_free(doc); free(held); free(markup);
    check(live == 0, "churn mode %u teardown", mode);
}
static void t_reclaim_fatal(void)
{
    os64_html_document_t *doc = parse("<body><div>");
    os64_html_document_t *other = parse("<body>");
    HNode *node = doc->body->first_child;
    for (unsigned mode = 0; mode < 8; mode++) {
        death_code = 0; death_said[0] = 0; expecting_death = true;
        if (!setjmp(death_landing)) {
            if (mode == 0) os64_html_release(doc, node);
            if (mode == 1) os64_html_hold(other, node);
            if (mode == 2) os64_html_release(NULL, node);
            if (mode == 3) os64_html_hold(doc, NULL);
            if (mode == 4) { h_meta(node)->holds = UINT32_MAX; os64_html_hold(doc, node); }
            if (mode == 5) { os64_html_hold(doc, node); os64_html_document_free(doc); }
            if (mode == 6) { os64_html_hold(doc, node); os64_html_remove(doc, node); os64_html_document_free(doc); }
            if (mode == 7) { ((HDoc *)doc)->held = SIZE_MAX; os64_html_hold(doc, node); }
            check(false, "fatal lifetime mode %u returned", mode);
        } else check(death_code == OS64_HTML_FATAL_EXIT && strstr(death_said, "html"),
                     "fatal lifetime mode %u names HTML and exits", mode);
        expecting_death = false;
        if (mode == 5 || mode == 6) os64_html_release(doc, node);
        else h_meta(node)->holds = 0;
        if (mode == 6) node = os64_html_create_element(doc, OS64_HTML_NS_HTML, "div", NULL);
        if (mode == 7) ((HDoc *)doc)->held = 0;
    }
    os64_html_document_free(other); os64_html_document_free(doc);
    check(live == 0, "fatal lifetime cases teardown");
}
static size_t reclaim_tree_nodes(const HNode *node)
{
    size_t count = 1;
    if (node->template_contents) count += reclaim_tree_nodes(node->template_contents);
    for (const HNode *child = node->first_child; child; child = child->next)
        count += reclaim_tree_nodes(child);
    return count;
}
static void t_reclaim_parser(void)
{
    for (unsigned finish = 0; finish < 3; finish++) {
        os64_html_options_t opt = os64_html_options_default();
        opt.charset = "utf-8"; opt.scripting = true;
        os64_html_parser_t *parser = os64_html_parser_new(&opt);
        char input[1400]; memset(input, ' ', 1024);
        strcpy(input + 1024, "<body><div id=open><b>text<script>stop</script><i>tail</i></b></div><p>end</p>");
        check(os64_html_parser_feed(parser, input, strlen(input)) == OS64_HTML_SCRIPT,
              "parser mode %u stops", finish);
        os64_html_document_t *doc = os64_html_parser_document(parser);
        HNode *open = id_in(doc, "open"), *script = os64_html_parser_script(parser);
        size_t nodes = doc->node_count;
        check(open && script && !os64_html_remove(doc, open) && doc->node_count == nodes,
              "parser's references protect entire removed open subtree");
        check(script == os64_html_parser_script(parser) && !strcmp(script->first_child->text, "stop"),
              "stopped script remains readable without external hold");
        if (finish == 0) {
            check(os64_html_parser_resume(parser) == OS64_HTML_OK, "resume parses remaining held input safely");
            doc = os64_html_parser_finish(parser);
        } else if (finish == 1) doc = os64_html_parser_finish(parser);
        else doc = os64_html_parser_abandon(parser);
        check(doc && doc->node_count == reclaim_tree_nodes(doc->document),
              "finish/abandon releases parser-held detached unit with exact reachable node count");
        os64_html_document_free(doc);
        check(live == 0, "parser mode %u teardown", finish);
    }
}
static void t_reclaim_parser_nonstack_refs(void)
{
    for (unsigned which = 0; which < 2; which++) {
        os64_html_options_t opt = os64_html_options_default();
        opt.charset = "utf-8"; opt.scripting = true;
        os64_html_parser_t *parser = os64_html_parser_new(&opt);
        char input[1500]; memset(input, ' ', 1024);
        strcpy(input + 1024, "<head><title>t</title></head><body><table><form id=f><tr><td><script>stop</script></table><p>tail</p>");
        check(os64_html_parser_feed(parser, input, strlen(input)) == OS64_HTML_SCRIPT, "nonstack reference stop");
        os64_html_document_t *doc = os64_html_parser_document(parser);
        HNode *node = which ? id_in(doc, "f") : doc->head;
        bool on_stack = false;
        for (size_t i = 0; i < parser->stack.n; i++) on_stack |= parser->stack.v[i] == node;
        check(node && !on_stack && node == (which ? parser->form : parser->head),
              "head/form reference is outside the open-element stack");
        size_t nodes = doc->node_count;
        check(!os64_html_remove(doc, node) && doc->node_count == nodes,
              "parser's nonstack head/form reference protects removal");
        check(!os64_html_parser_resume(parser), "nonstack reference resume");
        doc = os64_html_parser_finish(parser);
        check(doc->node_count == reclaim_tree_nodes(doc->document), "finish frees removed nonstack reference exactly");
        os64_html_document_free(doc);
        check(live == 0, "nonstack parser reference teardown");
    }
}
static void t_reclaim_order(void)
{
    for (unsigned order = 0; order < 2; order++) {
        os64_html_document_t *doc = parse("<body><div id=r><span id=k>x</span></div>");
        HNode *root = id_in(doc, "r"), *kid = id_in(doc, "k");
        size_t nodes = doc->node_count;
        os64_html_hold(doc, root); os64_html_hold(doc, kid);
        check(!os64_html_remove(doc, root), "release-order removal");
        os64_html_release(doc, order ? kid : root);
        check(doc->node_count == nodes, "ancestor/descendant order %u retains whole unit", order);
        os64_html_release(doc, order ? root : kid);
        check(doc->node_count == nodes - 3, "ancestor/descendant order %u last release reclaims", order);
        os64_html_document_free(doc);
    }
    os64_html_document_t *doc = parse("<body><div id=r><span id=k>x</span></div>");
    HNode *root = id_in(doc, "r"), *kid = id_in(doc, "k");
    size_t nodes = doc->node_count;
    os64_html_hold(doc, kid);
    check(!os64_html_remove(doc, root) && !os64_html_insert(doc, doc->body, root, NULL),
          "retained removed root reinserted");
    os64_html_release(doc, kid);
    check(doc->node_count == nodes && root->parent == doc->body && !strcmp(kid->first_child->text, "x"),
          "reinsertion cancels obsolete retirement candidate");
    os64_html_document_free(doc);
    doc = parse("<body><div id=r><span id=k>x</span></div>");
    root = id_in(doc, "r"); kid = id_in(doc, "k"); nodes = doc->node_count;
    os64_html_hold(doc, kid);
    check(!os64_html_remove(doc, root), "held subtree detached before snapshot");
    os64_html_pin_t later = os64_html_pin(doc);
    uint64_t version = os64_html_version(doc);
    os64_html_release(doc, kid);
    check(doc->node_count == nodes && os64_html_version(doc) == version &&
              !strcmp(kid->first_child->text, "x"), "snapshot acquired while held protects last-release bytes");
    os64_html_unpin(doc, later);
    check(doc->node_count == nodes - 3, "unpin reclaims delayed retirement");
    os64_html_document_free(doc);
    // Pin-only retired roots can move beneath another retained candidate.
    doc = parse("<body><div id=a><span>A</span></div><div id=b><span>B</span></div>");
    HNode *a = id_in(doc, "a"), *b = id_in(doc, "b");
    os64_html_pin_t old = os64_html_pin(doc);
    nodes = doc->node_count;
    check(!os64_html_remove(doc, a) && !os64_html_remove(doc, b), "two retired roots protected by old snapshot");
    os64_html_hold(doc, a);
    check(!os64_html_insert(doc, a, b, NULL), "retired root moved under another retained candidate");
    os64_html_unpin(doc, old);
    check(doc->node_count == nodes && b->parent == a, "outer hold preserves nested unit after unpin");
    os64_html_release(doc, a);
    check(doc->node_count == 4, "nested pending root canceled before whole outer unit freed");
    os64_html_document_free(doc);
    check(live == 0, "release order teardown");
}
/* Compare parser slot multiplicities with node counts independently of the
 * library's reference helpers, including duplicate stack/formatting entries. */
static void parser_refs_match(os64_html_parser_t *p)
{
    for (HNode *n = p->d->live_nodes; n; n = h_meta(n)->live_next) {
        size_t refs = (n == &p->d->root) + (n == &p->d->html);
        refs += (n == p->head) + (n == p->script);
        refs += !p->form_borrowed && n == p->form;
        for (size_t i = 0; i < p->stack.n; i++) refs += p->stack.v[i] == n;
        for (size_t i = 0; i < p->formatting.n; i++) refs += p->formatting.v[i] == n;
        check(refs == h_meta(n)->parser_refs, "parser reference multiplicity %s: %zu/%u",
              n->name ? n->name : "#text", refs, h_meta(n)->parser_refs);
    }
}
static void t_reclaim_parser_slots(void)
{
    const char *cases[] = {
        "<body><b><i>x</b>y<script>one</script></i><script>two</script>",
        "<body><table><b><tr><td><i>x</b><script>one</script></table><script>two</script>",
        "<body><template><form><b>x</form><script>one</script></template><script>two</script>",
        "<body><form><div></form><p><b><p>y<script>one</script></b><script>two</script>",
    };
    for (size_t i = 0; i < H_ARRAY(cases); i++) {
        os64_html_options_t opt = os64_html_options_default(); opt.charset = "utf-8"; opt.scripting = true;
        os64_html_parser_t *p = os64_html_parser_new(&opt);
        char input[1536]; memset(input, ' ', 1024); strcpy(input + 1024, cases[i]);
        int64_t status = os64_html_parser_feed(p, input, strlen(input));
        unsigned stops = 0;
        while (status == OS64_HTML_SCRIPT) {
            parser_refs_match(p); stops++;
            HNode *script = os64_html_parser_script(p);
            check(!os64_html_remove(&p->d->pub, script) && !script->parent &&
                  h_meta(script)->parser_refs && script->first_child && script->first_child->text,
                  "script field holds removed script after it left the open stack");
            parser_refs_match(p);
            status = os64_html_parser_resume(p);
        }
        check(!status && stops, "parser slots fixture %zu resumes", i);
        parser_refs_match(p);
        os64_html_document_t *doc = os64_html_parser_finish(p);
        for (HNode *n = ((HDoc *)doc)->live_nodes; n; n = h_meta(n)->live_next)
            check(!h_meta(n)->parser_refs, "finish balances parser slots");
        check(!((HDoc *)doc)->pending_units, "finish collects removed scripts");
        os64_html_document_free(doc);
        check(!live, "parser slots fixture %zu teardown", i);
    }
}
#ifdef HTML_RECLAIM_TEST
static double reclaim_time(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static size_t reclaim_cost(unsigned count, bool stopped)
{
    const char *unit = "<div><span>x</span></div>";
    size_t len = strlen(unit), size = 6 + count * len;
    char *markup = malloc(size + 32);
    memcpy(markup, "<body>", 6);
    for (unsigned i = 0; i < count; i++) memcpy(markup + 6 + i * len, unit, len);
    strcpy(markup + size, stopped ? "<script>stop</script>" : "");
    os64_html_options_t opt = os64_html_options_default(); opt.charset = "utf-8"; opt.scripting = stopped;
    os64_html_parser_t *parser = stopped ? os64_html_parser_new(&opt) : NULL;
    if (stopped) check(os64_html_parser_feed(parser, markup, strlen(markup)) == OS64_HTML_SCRIPT,
                       "cost probe stopped parser");
    os64_html_document_t *doc = stopped ? os64_html_parser_document(parser) : parse_with(markup, &opt);
    HNode **held = calloc(count, sizeof(*held));
    HNode *n = doc->body->first_child;
    double start = reclaim_time();
    for (unsigned i = 0; i < count; i++) {
        HNode *next = n->next; held[i] = n->first_child;
        os64_html_hold(doc, held[i]);
        check(!os64_html_remove(doc, n), "cost probe detach %u", i);
        check(h_meta(n)->unit_holds == 1, "detachment sums the named unit's holders");
        n = next;
    }
    double detach_time = reclaim_time() - start;
    check(((HDoc *)doc)->pending_units == count && !((HDoc *)doc)->pending_nodes &&
          !((HDoc *)doc)->waiting_nodes, "held units require no collection queue scan");
    h_reclaim_visits = 0;
    start = reclaim_time();
    for (unsigned i = 0; i < 10000; i++) {
        HNode *fresh = os64_html_create_element(doc, OS64_HTML_NS_HTML, "p", NULL);
        check(fresh && !os64_html_insert(doc, doc->body, fresh, NULL) && !os64_html_remove(doc, fresh),
              "cost probe unrelated edit");
    }
    double edit_time = reclaim_time() - start;
    size_t visits = h_reclaim_visits;
    start = reclaim_time();
    for (unsigned i = 0; i < count; i++) os64_html_release(doc, held[i]);
    double release_time = reclaim_time() - start;
    check(!((HDoc *)doc)->held && !((HDoc *)doc)->pending_units, "cost probe releases every unit");
    printf("D6 cost stopped=%u held=%u detach=%.3fms edit=%.3fus release=%.3fms visits=%zu\n",
           stopped, count, detach_time * 1000, edit_time * 1e6 / 10000, release_time * 1000, visits);
    if (stopped) { parser_refs_match(parser); doc = os64_html_parser_finish(parser); }
    check(doc->node_count == reclaim_tree_nodes(doc->document), "cost probe exact reachable nodes");
    os64_html_document_free(doc); free(held); free(markup);
    check(!live, "cost probe teardown");
    return visits;
}
static void t_reclaim_cost(void)
{
    for (unsigned stopped = 0; stopped < 2; stopped++) {
        size_t small = reclaim_cost(2000, stopped), large = reclaim_cost(10000, stopped);
        check(small == large && large < 100000, "unrelated edit work independent of held unit/page count");
    }
}
#endif
static void t_reclaim_empty(void)
{
    os64_html_document_t *doc = parse("<body>");
    size_t nodes = doc->node_count, bytes = doc->arena_bytes;
    uint64_t version = os64_html_version(doc);
    for (unsigned i = 0; i < 1000; i++) {
        HNode *fragment = os64_html_parse_fragment(doc, doc->body, "", 0, false, NULL);
        check(fragment && !os64_html_insert(doc, doc->body, fragment, NULL), "empty fragment consumed %u", i);
        check(doc->node_count == nodes && doc->arena_bytes == bytes && os64_html_version(doc) == version,
              "empty fragment flat counters and unchanged tree version %u", i);
    }
    HNode *fragment = os64_html_parse_fragment(doc, doc->body, "", 0, false, NULL);
    os64_html_hold(doc, fragment);
    check(!os64_html_insert(doc, doc->body, fragment, NULL) && doc->node_count == nodes + 1 &&
              !fragment->first_child, "hold preserves consumed empty fragment");
    os64_html_release(doc, fragment);
    check(doc->node_count == nodes && doc->arena_bytes == bytes, "empty fragment final release restores counters");
    fragment = os64_html_parse_fragment(doc, doc->body, "", 0, false, NULL);
    os64_html_pin_t pin = os64_html_pin(doc);
    check(!os64_html_insert(doc, doc->body, fragment, NULL) && doc->node_count == nodes + 1,
          "unchanged-version empty consumption protects earlier pin");
    os64_html_unpin(doc, pin);
    check(doc->node_count == nodes && doc->arena_bytes == bytes, "empty fragment unpin restores counters");
    os64_html_document_free(doc);
    check(live == 0, "empty fragments teardown");
}
static void t_reclaim_allocations(void)
{
    for (unsigned operation = 0; operation < 3; operation++) {
        for (unsigned retained = 0; retained < 3; retained++) {
            os64_html_document_t *doc = parse("<body>");
            const char *payload = "<form><input><template><input form=f></template><span>text</span></form>";
            HNode *fragment = os64_html_parse_fragment(doc, doc->body, payload, strlen(payload), false, NULL);
            HNode *unit = fragment->first_child;
            check(!os64_html_insert(doc, doc->body, fragment, NULL), "detach allocation fixture inserted");
            if (retained == 1) os64_html_hold(doc, unit->last_child->first_child);
            os64_html_pin_t pin = retained == 2 ? os64_html_pin(doc) : 0;
            HNode *replacement = os64_html_create_element(doc, OS64_HTML_NS_HTML, "div", NULL);
            HNode *incoming = os64_html_parse_fragment(doc, doc->body, "<p>next</p>", 11, false, NULL);
            size_t before = allocations;
            uint64_t version = os64_html_version(doc);
            fail_at = allocations + 1;
            int64_t status = operation == 0 ? os64_html_remove(doc, unit)
                           : operation == 1 ? os64_html_replace(doc, doc->body, replacement, unit)
                                            : os64_html_replace(doc, doc->body, incoming, unit);
            fail_at = 0;
            check(!status && allocations == before && os64_html_version(doc) == version + 1,
                  "detach operation %u retention %u succeeds without allocation", operation, retained);
            if (retained == 1) os64_html_release(doc, unit->last_child->first_child);
            if (pin) os64_html_unpin(doc, pin);
            os64_html_document_free(doc);
            check(live == 0, "detach allocation operation %u retention %u teardown", operation, retained);
        }
    }
}
static void t_reclaim_rules(void)
{
    os64_html_document_t *doc = parse("<body><div id=r><span id=k>text</span></div>");
    HNode *r = id_in(doc, "r"), *k = id_in(doc, "k");
    size_t nodes = doc->node_count, allocs = allocations;
    os64_html_hold(doc, k); os64_html_hold(doc, k);
    fail_at = allocations + 1;
    check(!os64_html_remove(doc, r) && doc->node_count == nodes,
          "held descendant protects entire removed subtree without allocation");
    fail_at = 0;
    check(allocations == allocs, "hold/remove allocate nothing");
    os64_html_release(doc, k);
    check(doc->node_count == nodes && !strcmp(k->first_child->text, "text"), "one of two holders remains");
    os64_html_release(doc, k);
    check(doc->node_count == nodes - 3, "last release reconsiders the whole removed unit");
    os64_html_document_free(doc);

    doc = parse("<body><template id=t><input form=f><template><b>x</b></template></template>");
    HNode *t = id_in(doc, "t"), *contents = t->template_contents;
    nodes = doc->node_count;
    os64_html_hold(doc, contents);
    check(!os64_html_remove(doc, t) && doc->node_count == nodes && os64_html_form_input_count(doc) == 1,
          "held template fragment protects template and nested contents");
    os64_html_release(doc, contents);
    check(doc->node_count == 4 && os64_html_form_input_count(doc) == 0,
          "template reclamation includes fragments and explicit inputs");
    os64_html_document_free(doc);

    doc = parse("<body>");
    HNode *made = os64_html_create_element(doc, OS64_HTML_NS_HTML, "div", NULL);
    nodes = doc->node_count;
    os64_html_hold(doc, made); os64_html_release(doc, made);
    check(doc->node_count == nodes, "never-inserted creation remains charged after release");
    check(!os64_html_insert(doc, doc->body, made, NULL) && !os64_html_remove(doc, made), "creation removed");
    HNode *reuse = os64_html_create_element(doc, OS64_HTML_NS_HTML, "span", NULL);
    check(reuse == made, "next creation reuses reclaimed permanent node address");
    check(!reuse->parent && !reuse->attrs && !reuse->first_child && !strcmp(reuse->name, "span"),
          "reused node starts with clean public fields");
    os64_html_document_free(doc);

    doc = parse("<body>");
    size_t empty_bytes = doc->arena_bytes;
    const char *payload = "<form id=f><input><input form=f><template><input form=f></template>text</form>";
    HNode *fragment = os64_html_parse_fragment(doc, doc->body, payload, strlen(payload), false, NULL);
    r = fragment->first_child;
    size_t baseline;
    check(!os64_html_set_attr(doc, r, "private", "value", 5) && os64_html_attr(r, "id") &&
              (*h_word(r) & (H_ATTRS_INLINE | H_ATTRS_PRIVATE)) == (H_ATTRS_INLINE | H_ATTRS_PRIVATE),
          "packed original inline attribute and later private attributes coexist");
    check(!os64_html_insert(doc, doc->body, fragment, NULL), "packed payload inserted");
    check(((HDoc *)doc)->records > 0 && os64_html_form_input_count(doc) == 2, "packed payload owner/input counts");
    os64_html_pin_t old = os64_html_pin(doc);
    baseline = doc->arena_bytes;
    nodes = doc->node_count;
    check(!os64_html_remove(doc, r) && doc->node_count == nodes && doc->arena_bytes == baseline &&
              os64_html_form_input_count(doc) == 2, "old pin protects payload, attributes, text and counts");
    os64_html_pin_t fresh = os64_html_pin(doc);
    os64_html_unpin(doc, old);
    check(doc->node_count == 4 && os64_html_form_input_count(doc) == 0 && ((HDoc *)doc)->records == 0 &&
              !os64_html_retired_bytes(doc) && doc->arena_bytes == empty_bytes,
          "pin taken after retirement does not protect old units or owned payload bytes");
    os64_html_unpin(doc, fresh);
    os64_html_document_free(doc);
    check(live == 0, "reclamation rules teardown");
}
int main(int argc, char **argv)
{
#ifdef HTML_RECLAIM_TEST
    if (argc == 2 && !strcmp(argv[1], "--reclaim-cost")) {
        t_reclaim_cost();
        printf("html reclamation cost: %zu checks, %zu failed\n", checks_run, checks_failed);
        return checks_failed ? 1 : 0;
    }
#endif
    if (argc == 2 && strcmp(argv[1], "--form-input-count") == 0) {
        t_form_input_count();
        printf("html form-input count: %zu checks, %zu failed\n", checks_run, checks_failed);
        return checks_failed ? 1 : 0;
    }
    if (argc == 2 && (!strcmp(argv[1], "--reclaim") || !strcmp(argv[1], "--reclaim-rules"))) {
        t_reclaim_fatal();
        t_reclaim_parser();
        t_reclaim_parser_nonstack_refs();
        t_reclaim_parser_slots();
#ifdef HTML_RECLAIM_TEST
        t_reclaim_cost();
#endif
        t_reclaim_order();
        t_reclaim_empty();
        t_reclaim_allocations();
        t_reclaim_rules();
        if (!strcmp(argv[1], "--reclaim-rules")) {
            printf("html reclamation rules: %zu checks, %zu failed\n", checks_run, checks_failed);
            return checks_failed ? 1 : 0;
        }
        t_reclaim_churn(216000, 0);
        t_reclaim_churn(216000, 1);
        t_reclaim_churn(216000, 2);
        printf("html reclamation: %zu checks, %zu failed\n", checks_run, checks_failed);
        return checks_failed ? 1 : 0;
    }
    if (!(argc == 2 && !strcmp(argv[1], "--random"))) {
        t_validity();
        t_structure();
        t_templates();
        t_attributes();
        t_text();
        t_form_owners();
        t_form_input_count();
        t_depth();
        t_deep();
        t_pins();
        t_documents();
        t_out_of_memory();
    }
    static const char *const pages[] = {
        "<!doctype html><html><head><title>t</title></head><body><div id=a class=x><span>one</span></div>"
        "<p>two</p><!--c--></body></html>",
        // Parser records to a form that is not an ancestor, and one that is.
        "<body><table><form id=f><tr><td><input id=q><select name=s></select></table>"
        "<form id=g><div><input name=n><button>b</button></div></form>",
        // Clones that share one attribute list, and a template.
        "<body><p><b class=x id=k>a<p>b<p>c<template><i>t</i><template><u></u></template></template>",
    };
    for (size_t p = 0; p < H_ARRAY(pages); p++)
        for (uint32_t seed = 1; seed <= 6; seed++)
            t_random(pages[p], 0x64d00000u + (uint32_t)p * 16 + seed, 4000, seed % 2 ? 512 : 9);
    // A limit low enough for a random walk to keep running into it.
    for (uint32_t seed = 1; seed <= 4; seed++)
        t_random("<body><div><p>x</p></div>", 0x64d00100u + seed, 4000, 4);
    for (unsigned held = 0; held < 2; held++) {
        check(oracle_success[0][held] + oracle_success[1][held] + oracle_success[2][held] > 0,
              "oracle: successful insert with held=%u input", held);
        check(oracle_success[3][held] > 0 && oracle_success[4][held] > 0,
              "oracle: successful replace/remove with held=%u input", held);
    }
    check(oracle_freed[0] + oracle_freed[1] + oracle_freed[2] > 0 && oracle_freed[3] > 0 &&
              oracle_freed[4] > 0 && oracle_freed[11] > 0, "oracle: physical reclamation under insert/replace/remove/unpin");
    printf("html dom: %zu checks, %zu failed\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
