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
int main(void)
{
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
        "htmltest: PASS (streaming, tree repair, UTF-8, namespaces, templates, bounds, heap)\n");
    return HTMLTEST_OK;
}
