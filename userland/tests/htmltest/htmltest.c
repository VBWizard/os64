#include "html/html.h"
#include "os64/os64.h"

#define HTMLTEST_OK 0x48640000u
#define HTMLTEST_FAIL 0x48640001u
static const char page[] = "<!DOCTYPE html><!--kept--><p><b>one<i>two</b>three</i>"
                           "<table>outside<tr><td>&euro;</table>"
                           "<svg><foreignObject><p>foreign</p></foreignObject></svg>"
                           "<template><p>inert</template>";

static void require(bool value, const char *message)
{
    if (value)
        return;
    os64_serial_log(message);
    os64_printf("htmltest: FAIL: %s\n", message);
    os64_exit(HTMLTEST_FAIL);
}
static const os64_html_node_t *find(const os64_html_node_t *n, const char *name)
{
    for (; n; n = n->next) {
        if (n->kind == OS64_HTML_ELEMENT && os64_streq(n->name, name))
            return n;
        const os64_html_node_t *child = find(n->first_child, name);
        if (child)
            return child;
    }
    return NULL;
}
static void form_owners(void)
{
    static const char *const markup[] = {
        "<table><form action=/s><tr><td><input name=q></td></tr></form></table>",
        "<form><input>",
        "<form id=f></form><input form=f>"};
    for (size_t i = 0; i < sizeof(markup) / sizeof(markup[0]); i++) {
        os64_html_parser_t *p = os64_html_parser_new(NULL);
        require(p != NULL, "form owner constructor");
        require(os64_html_parser_feed(p, markup[i], os64_strlen(markup[i])) == 0,
                "form owner feed");
        os64_html_document_t *doc = os64_html_parser_finish(p);
        require(doc && !doc->refusal, "form owner document");
        const os64_html_node_t *form = find(doc->body, "form");
        const os64_html_node_t *input = find(doc->body, "input");
        require(form && input, "form owner nodes");
        require(input->form_owner == (i == 2 ? NULL : form), "parser form association");
        if (i == 0)
            require(!form->first_child, "table form remains empty");
        os64_html_document_free(doc);
    }
}
static os64_html_node_t *with_id(os64_html_node_t *n, const char *id)
{
    for (; n; n = n->next) {
        const os64_html_attr_t *a = os64_html_attr(n, "id");
        if (a && os64_streq(a->value, id))
            return n;
        os64_html_node_t *child = with_id(n->first_child, id);
        if (child)
            return child;
    }
    return NULL;
}
static os64_html_document_t *parsed(const char *markup)
{
    os64_html_parser_t *p = os64_html_parser_new(NULL);
    require(p != NULL, "verb fixture constructor");
    require(os64_html_parser_feed(p, markup, os64_strlen(markup)) == 0, "verb fixture feed");
    os64_html_document_t *doc = os64_html_parser_finish(p);
    require(doc && !doc->refusal, "verb fixture document");
    return doc;
}
/* The verbs that change a finished document, through the real library on
 * the real heap. The host harness proves the rules; this proves they are
 * exported, linked and sound in ring 3. */
static void verbs(void)
{
    os64_html_document_t *doc = parsed("<body><p id=p1><b class=x>a<p id=p2>b</p>"
                                       "<table id=t><form id=f><tr><td><input id=q></table>");
    os64_html_node_t *p1 = with_id(doc->document, "p1"), *p2 = with_id(doc->document, "p2");
    os64_html_node_t *q = with_id(doc->document, "q"), *f = with_id(doc->document, "f");
    require(p1 && p2 && q && f && q->form_owner == f, "verb fixture nodes");
    uint64_t version = os64_html_version(doc);
    require(version != 0, "a document has a version");

    int64_t status = -1;
    os64_html_node_t *div = os64_html_create_element(doc, OS64_HTML_NS_HTML, "div", &status);
    os64_html_node_t *text = os64_html_create_text(doc, "made", 4, NULL);
    require(div && text && status == OS64_HTML_OK, "create");
    require(os64_html_insert(doc, div, text, NULL) == OS64_HTML_OK &&
                os64_html_insert(doc, doc->body, div, p1) == OS64_HTML_OK &&
                doc->body->first_child == div && div->next == p1 && text->parent == div,
            "insert");
    require(os64_html_insert(doc, text, p1, NULL) == OS64_HTML_HIERARCHY &&
                os64_html_insert(doc, p1, doc->body, NULL) == OS64_HTML_HIERARCHY &&
                os64_html_remove(doc, doc->html) == OS64_HTML_ROOT_REQUIRED &&
                os64_html_set_text(doc, text, "\xff", 1) == OS64_HTML_BAD_TEXT,
            "refusals by name");
    require(os64_html_replace(doc, doc->body, p2, div) == OS64_HTML_OK && !div->parent &&
                doc->body->first_child == p2,
            "replace");
    require(os64_html_remove(doc, p2) == OS64_HTML_OK && !p2->parent &&
                os64_html_insert(doc, doc->body, p2, NULL) == OS64_HTML_OK && doc->body->last_child == p2,
            "remove and put back");
    require(os64_html_version(doc) > version, "the version moved");

    /* Two elements from one start tag share an attribute list until one changes. */
    os64_html_node_t *b1 = p1->first_child, *b2 = p2->first_child;
    require(b1 && b2 && b1 != b2 && b1->attrs == b2->attrs, "shared attribute list");
    require(os64_html_set_attr(doc, b1, "class", "y", 1) == OS64_HTML_OK &&
                os64_streq(os64_html_attr(b1, "class")->value, "y") &&
                os64_streq(os64_html_attr(b2, "class")->value, "x"),
            "an attribute set on one clone only");
    require(os64_html_remove_attr(doc, b1, "class") == OS64_HTML_OK && !b1->attrs, "attribute removed");

    /* A snapshot's pin keeps replaced bytes readable; letting go frees them. */
    os64_html_pin_t pin = os64_html_pin(doc);
    const char *old = b1->first_child->text;
    require(pin && os64_html_set_text(doc, b1->first_child, "changed", 7) == OS64_HTML_OK &&
                os64_streq(old, "a") && os64_html_retired_bytes(doc) > 0,
            "retired under a pin");
    os64_html_unpin(doc, pin);
    require(os64_html_retired_bytes(doc) == 0 && os64_streq(b1->first_child->text, "changed"),
            "reclaimed when the pin lets go");

    /* The parser tied q to a form that is not its ancestor; the control's own
     * form attribute voids that, and removing the attribute does not undo it. */
    require(os64_html_set_attr(doc, q, "form", "none", 4) == OS64_HTML_OK && !q->form_owner &&
                os64_html_remove_attr(doc, q, "form") == OS64_HTML_OK && !q->form_owner,
            "form owner reset");

    os64_html_node_t *copy = os64_html_clone(doc, doc->body, true, NULL);
    require(copy && copy->first_child && copy->first_child != doc->body->first_child, "deep clone");
    for (unsigned i = 0; i < 2000; i++) {
        os64_html_node_t *e = os64_html_create_element(doc, OS64_HTML_NS_HTML, "span", NULL);
        require(e && os64_html_insert(doc, p1, e, NULL) == OS64_HTML_OK &&
                    os64_html_set_attr(doc, e, "n", "1", 1) == OS64_HTML_OK &&
                    os64_html_set_attr(doc, e, "n", "22", 2) == OS64_HTML_OK &&
                    os64_html_remove(doc, e) == OS64_HTML_OK,
                "churn");
    }
    require(os64_heap_verify() == 0, "heap during verbs");
    os64_html_document_free(doc);
    require(os64_heap_verify() == 0, "heap after verbs");
}
int main(int argc, char **argv)
{
    if (argc > 1 && os64_streq(argv[1], "pinned")) {
        /* Passes by dying: a document freed while pinned ends the program. */
        os64_html_document_t *doc = parsed("<p>x");
        require(os64_html_pin(doc) != 0, "pin");
        os64_html_document_free(doc);
        os64_printf("htmltest: a pinned document was freed and the program lived\n");
        return (int)HTMLTEST_FAIL;
    }
    require(os64_heap_verify() == 0, "heap before parse");
    for (unsigned round = 0; round < 16; round++) {
        os64_html_parser_t *p = os64_html_parser_new(NULL);
        require(p != NULL, "constructor");
        for (size_t i = 0; i < sizeof(page) - 1; i++)
            require(os64_html_parser_feed(p, page + i, 1) == 0, "one-byte feed");
        os64_html_document_t *doc = os64_html_parser_finish(p);
        require(doc && doc->refusal == 0 && doc->quirks == OS64_HTML_NO_QUIRKS, "document status");
        require(doc->html && doc->head && doc->body && doc->html->parent == doc->document,
                "document structure");
        const os64_html_node_t *table = find(doc->body, "table");
        require(table && table->prev && table->prev->kind == OS64_HTML_TEXT &&
                    os64_streq(table->prev->text, "outside"),
                "foster parenting");
        const os64_html_node_t *cell = find(table, "td");
        require(cell && cell->first_child && os64_streq(cell->first_child->text, "\xe2\x82\xac"),
                "entity UTF-8");
        const os64_html_node_t *svg = find(doc->body, "svg");
        require(svg && svg->ns == OS64_HTML_NS_SVG, "SVG namespace");
        const os64_html_node_t *foreign = find(svg, "foreignObject");
        require(foreign && foreign->first_child && foreign->first_child->ns == OS64_HTML_NS_HTML,
                "integration point");
        const os64_html_node_t *templ = find(doc->head, "template");
        if (!templ)
            templ = find(doc->body, "template");
        require(templ && !templ->first_child && templ->template_contents &&
                    templ->template_contents->kind == OS64_HTML_FRAGMENT,
                "template isolation");
        os64_html_document_free(doc);
    }
    form_owners();
    verbs();
    os64_html_options_t options = os64_html_options_default();
    options.max_depth = 3;
    os64_html_parser_t *p = os64_html_parser_new(&options);
    require(p != NULL, "bounded constructor");
    os64_html_parser_feed(p, "<div><div>", 10);
    os64_html_document_t *doc = os64_html_parser_finish(p);
    require(doc && doc->html && doc->refusal == OS64_HTML_TOO_DEEP, "named depth refusal");
    os64_html_document_free(doc);
    p = os64_html_parser_new(NULL);
    require(p != NULL, "cancel constructor");
    os64_html_parser_feed(p, page, 13);
    os64_html_parser_destroy(p);
    require(os64_heap_verify() == 0, "heap after free/cancel");
    os64_printf(
        "htmltest: PASS (streaming, tree repair, UTF-8, namespaces, templates, form owners, verbs, bounds, heap)\n");
    return HTMLTEST_OK;
}
