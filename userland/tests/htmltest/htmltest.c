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

    /* A node belongs to one document. Another's verbs refuse it, and a clone
     * carries it across whole: it reads the same once its source is freed. */
    os64_html_document_t *other = parsed("<body><p id=o class=k><b title=t>x<p>y</b>");
    os64_html_node_t *o = with_id(other->document, "o");
    require(o && os64_html_insert(doc, doc->body, o, NULL) == OS64_HTML_BAD_ARGUMENT &&
                os64_html_remove(doc, o) == OS64_HTML_BAD_ARGUMENT &&
                os64_html_set_attr(doc, o, "class", "z", 1) == OS64_HTML_BAD_ARGUMENT && o->parent,
            "another document's node refused");
    os64_html_node_t *across = os64_html_clone(doc, o, true, NULL);
    os64_html_document_free(other);
    require(across && os64_streq(across->name, "p") &&
                os64_streq(os64_html_attr(across, "class")->value, "k") && across->first_child &&
                os64_streq(os64_html_attr(across->first_child, "title")->value, "t") &&
                os64_html_insert(doc, doc->body, across, NULL) == OS64_HTML_OK,
            "a clone across documents outlives its source");
    /* A pin's number is not handed out twice, though its slot is. */
    os64_html_pin_t first = os64_html_pin(doc);
    os64_html_unpin(doc, first);
    os64_html_pin_t second = os64_html_pin(doc);
    require(first && second && first != second, "pin numbers are not reused");
    os64_html_unpin(doc, second);

    os64_html_document_free(doc);
    require(os64_heap_verify() == 0, "heap after verbs");
}
/* The parse that stops at a script, through the real library on the real
 * heap: the host harness proves the rules, this proves the calls are
 * exported, linked and sound in ring 3. */
static int64_t feed(os64_html_parser_t *p, const char *markup)
{
    return os64_html_parser_feed(p, markup, os64_strlen(markup));
}
static void stops(void)
{
    /* Past the sniff window, so that a feed is parsed as it arrives. */
    static char window[1025];
    for (size_t i = 0; i < sizeof(window) - 1; i++)
        window[i] = ' ';
    os64_html_options_t options = os64_html_options_default();
    options.scripting = true;

    os64_html_parser_t *p = os64_html_parser_new(&options);
    require(p != NULL, "scripting constructor");
    os64_html_document_t *doc = os64_html_parser_document(p);
    require(feed(p, window) == OS64_HTML_OK, "window feed");
    require(feed(p, "<p id=p>before<script>one</script> after<script>two</script>end") ==
                OS64_HTML_SCRIPT,
            "the first script stops the parse");
    os64_html_node_t *script = os64_html_parser_script(p);
    require(script && script->first_child && os64_streq(script->first_child->text, "one") &&
                script->prev && os64_streq(script->prev->text, "before"),
            "the script is whole and in the tree");
    require(feed(p, "<p>later") == OS64_HTML_SCRIPT && os64_html_parser_script(p) == script,
            "a feed while stopped is kept");

    /* What a script does at its stop: a snapshot is pinned, the script takes
     * itself out, and the text after it joins the text a snapshot can see. */
    os64_html_node_t *text = script->prev;
    const char *held = text->text;
    os64_html_pin_t pin = os64_html_pin(doc);
    require(pin && os64_html_remove(doc, script) == OS64_HTML_OK, "a verb at a stop");
    require(os64_html_parser_resume(p) == OS64_HTML_SCRIPT, "resume stops at the next script");
    require(os64_streq(held, "before") && os64_streq(text->text, "before after") &&
                os64_html_retired_bytes(doc) > 0,
            "pinned bytes stay while the text grows elsewhere");
    os64_html_unpin(doc, pin);
    require(os64_html_retired_bytes(doc) == 0, "unpin reclaims");
    require(os64_html_parser_resume(p) == OS64_HTML_OK && !os64_html_parser_script(p),
            "the last resume parses what was kept");
    require(os64_html_parser_end(p) == OS64_HTML_OK && feed(p, "x") == OS64_HTML_BAD_ARGUMENT,
            "the end of the input");
    require(os64_html_parser_finish(p) == doc && !doc->refusal && find(doc->body, "script") &&
                !find(doc->body, "script")->next->next,
            "the finished document");
    os64_html_document_free(doc);

    /* A page left mid-load keeps its document. */
    p = os64_html_parser_new(&options);
    require(p != NULL, "abandon constructor");
    feed(p, window);
    require(feed(p, "<p>shown<script>s</script>never parsed") == OS64_HTML_SCRIPT, "stopped");
    doc = os64_html_parser_abandon(p);
    require(doc && doc->html->parent == doc->document && find(doc->body, "script") &&
                !find(doc->body, "script")->next,
            "an abandoned parser's document, as far as it was built");
    os64_html_document_free(doc);

    /* An end tag at an integration point once went round until the work
     * budget ran out. */
    doc = parsed("<svg><foreignObject></p>x");
    const os64_html_node_t *foreign = find(doc->body, "foreignObject");
    require(doc->work < 1000 && foreign && foreign->first_child &&
                os64_streq(foreign->first_child->name, "p"),
            "an end tag at an integration point");
    os64_html_document_free(doc);
    require(os64_heap_verify() == 0, "heap after stops");
}
/* Fragment nodes are built off the visible tree, then inserted by the same
 * verbs the binding uses. This also exercises both exports on the guest heap. */
static void fragments(void)
{
    os64_html_document_t *doc = parsed("<body><div id=target>old</div><table id=table></table>");
    os64_html_node_t *target = with_id(doc->document, "target");
    os64_html_node_t *table = with_id(doc->document, "table");
    require(target && table, "fragment fixture");
    static const char markup[] = "<b title='&quot;&amp;&lt;&gt;'>new &amp; \xc2\xa0</b>"
                                 "<script>let n=1</script><template><i>kept</i></template>";
    static const char expected[] = "<b title=\"&quot;&amp;&lt;&gt;\">new &amp; &nbsp;</b>"
                                   "<script>let n=1</script><template><i>kept</i></template>";
    uint64_t version = os64_html_version(doc);
    os64_html_pin_t pin = os64_html_pin(doc);
    int64_t status = -1;
    os64_html_node_t *fragment = os64_html_parse_fragment(doc, target, markup,
                                                        sizeof(markup) - 1, true, &status);
    require(fragment && status == OS64_HTML_OK && fragment->kind == OS64_HTML_FRAGMENT &&
                !fragment->parent && os64_html_version(doc) == version && !doc->refusal &&
                os64_streq(target->first_child->text, "old"),
            "fragment parse preserves the pinned live tree and does not stop");
    size_t length = os64_html_serialize(fragment, true, true, NULL, 0);
    require(length == sizeof(expected) - 1, "serialize size without output");
    char *output = os64_malloc(length + 1);
    require(output && os64_html_serialize(fragment, true, true, output, length + 1) == length &&
                os64_streq(output, expected),
            "fragment attribute escaping, raw script and template output");
    char short_output[2] = {'x', 'x'};
    require(os64_html_serialize(fragment, true, true, short_output, sizeof(short_output)) == length &&
                short_output[0] == '<' && short_output[1] == 0,
            "serialize truncation reports the full size and terminates");
    while (target->first_child)
        require(os64_html_remove(doc, target->first_child) == OS64_HTML_OK, "replace old children");
    require(os64_html_insert(doc, target, fragment, NULL) == OS64_HTML_OK &&
                !fragment->first_child && os64_html_version(doc) > version &&
                os64_html_serialize(target, true, true, output, length + 1) == length &&
                os64_streq(output, expected),
            "innerHTML-shaped replacement and readback");
    os64_html_unpin(doc, pin);
    os64_free(output);

    static const char row[] = "<tr><td>cell</td></tr>";
    fragment = os64_html_parse_fragment(doc, table, row, sizeof(row) - 1, false, &status);
    char table_output[64];
    require(fragment && status == OS64_HTML_OK &&
                os64_html_serialize(fragment, true, false, table_output, sizeof(table_output)) ==
                    sizeof("<tbody><tr><td>cell</td></tr></tbody>") - 1 &&
                os64_streq(table_output, "<tbody><tr><td>cell</td></tr></tbody>"),
            "table context inserts tbody");
    os64_html_node_t *svg = os64_html_create_element(doc, OS64_HTML_NS_SVG, "svg", &status);
    static const char foreign[] = "</html><circle/>";
    version = os64_html_version(doc);
    fragment = os64_html_parse_fragment(doc, svg, foreign, sizeof(foreign) - 1, false, &status);
    require(svg && fragment && status == OS64_HTML_OK && fragment->first_child &&
                fragment->first_child->ns == OS64_HTML_NS_SVG &&
                os64_html_serialize(fragment, true, false, table_output, sizeof(table_output)) ==
                    sizeof("<circle></circle>") - 1 &&
                os64_streq(table_output, "<circle></circle>") &&
                os64_html_version(doc) == version,
            "foreign end tag preserves the fragment root and following nodes");
    os64_html_document_t *other = parsed("<body>other");
    version = os64_html_version(doc);
    require(!os64_html_parse_fragment(doc, other->body, "x", 1, false, &status) &&
                status == OS64_HTML_BAD_ARGUMENT && !doc->refusal &&
                os64_html_version(doc) == version,
            "foreign fragment context refused without changing the tree");
    os64_html_document_free(other);
    os64_html_document_free(doc);
    require(os64_heap_verify() == 0, "heap after fragments and serialisation");
    os64_printf("htmltest: fragments and serialisation PASS\n");
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
    stops();
    fragments();
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
        "htmltest: PASS (streaming, tree repair, UTF-8, namespaces, templates, form owners, verbs, stops, fragments, serialisation, bounds, heap)\n");
    return HTMLTEST_OK;
}
