#include "html/xml.h"
#include "internal.h"

struct os64_html_xml_parser {
    HDoc *doc;
    HNode *current;
    xml_parser_t *parser;
    os64_html_xml_options_t options;
    int64_t why;
    uint64_t work;
    HBuf text;
    bool busy;
};
static xml_status_t tree_verdict(int64_t why)
{
    return why == OS64_HTML_NO_MEMORY ? XML_NO_MEMORY : XML_LIMIT;
}
static bool charge(os64_html_xml_parser_t *b, size_t n)
{
    if (b->why)
        return false;
    if (n > b->options.max_tree_work - b->work) {
        b->why = OS64_HTML_WORK_EXHAUSTED;
        return false;
    }
    b->work += n;
    return true;
}
static void *parser_alloc(void *context, size_t size)
{
    os64_html_xml_parser_t *b = context;
    return d_alloc(b->doc, size, &b->why);
}
static void parser_free(void *context, void *memory, size_t size)
{
    (void)size;
    os64_html_xml_parser_t *b = context;
    h_free(b->doc, memory);
}
static bool cancelled(void *context)
{
    os64_html_xml_parser_t *b = context;
    return b->options.cancelled && b->options.cancelled(b->options.context);
}
static char *copy(os64_html_xml_parser_t *b, xml_slice_t s)
{
    if (s.size == SIZE_MAX) {
        b->why = OS64_HTML_ARENA_EXHAUSTED;
        return NULL;
    }
    if (!charge(b, s.size + 1))
        return NULL;
    char *out = d_permanent(b->doc, s.size + 1, &b->why);
    if (out)
        for (size_t i = 0; i < s.size; ++i)
            out[i] = s.data[i];
    return out;
}
static char *qualified(os64_html_xml_parser_t *b, xml_slice_t prefix, xml_slice_t local)
{
    if (!prefix.size)
        return copy(b, local);
    if (prefix.size > SIZE_MAX - 2 || local.size > SIZE_MAX - prefix.size - 2) {
        b->why = OS64_HTML_ARENA_EXHAUSTED;
        return NULL;
    }
    size_t len = prefix.size + local.size + 1;
    if (!charge(b, len + 1))
        return NULL;
    char *out = d_permanent(b->doc, len + 1, &b->why);
    if (!out)
        return NULL;
    for (size_t i = 0; i < prefix.size; ++i)
        out[i] = prefix.data[i];
    out[prefix.size] = ':';
    for (size_t i = 0; i < local.size; ++i)
        out[prefix.size + 1 + i] = local.data[i];
    return out;
}
static bool attr(os64_html_xml_parser_t *b, HAttr ***tail, xml_name_t name, xml_slice_t value)
{
    HAttr *a = d_permanent(b->doc, sizeof(*a), &b->why);
    if (!a)
        return false;
    a->name = qualified(b, name.prefix, name.local);
    a->value = copy(b, value);
    if (name.uri.size)
        a->ns = copy(b, name.uri);
    if (b->why || !a->name || !a->value)
        return false;
    **tail = a;
    *tail = &a->next;
    return true;
}
static bool text_append(os64_html_xml_parser_t *b, xml_slice_t s)
{
    if (!s.size)
        return true;
    if (s.size >= SIZE_MAX - b->text.len) {
        b->why = OS64_HTML_ARENA_EXHAUSTED;
        return false;
    }
    size_t need = b->text.len + s.size + 1;
    if (!charge(b, s.size))
        return false;
    if (need > b->text.cap) {
        size_t cap = b->text.cap ? b->text.cap : 64;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) {
                cap = need;
                break;
            }
            cap *= 2;
        }
        if (!charge(b, b->text.len))
            return false;
        char *grown = d_alloc(b->doc, cap, &b->why);
        if (!grown)
            return false;
        for (size_t i = 0; i < b->text.len; ++i)
            grown[i] = b->text.s[i];
        h_free(b->doc, b->text.s);
        b->text.s = grown;
        b->text.cap = cap;
    }
    for (size_t i = 0; i < s.size; ++i)
        b->text.s[b->text.len + i] = s.data[i];
    b->text.len += s.size;
    b->text.s[b->text.len] = 0;
    return true;
}
static bool flush(os64_html_xml_parser_t *b)
{
    if (!b->text.len)
        return true;
    if (!charge(b, 1))
        return false;
    HNode *n = d_node_new(b->doc, OS64_HTML_TEXT, &b->why);
    if (!n)
        return false;
    n->text = b->text.s;
    n->text_len = b->text.len;
    *h_word(n) = b->text.cap;
    b->text = (HBuf){0};
    h_attach(b->doc, b->current, NULL, n);
    return true;
}
static bool event(void *context, const xml_event_t *e)
{
    os64_html_xml_parser_t *b = context;
    if (!charge(b, 1))
        return false;
    if (e->in_doctype)
        return true;
    if (e->kind == XML_TEXT || e->kind == XML_CDATA)
        return text_append(b, e->text);
    if (!flush(b))
        return false;
    HNode *n = NULL;
    switch (e->kind) {
    case XML_START_DOCUMENT:
        b->doc->pub.charset = copy(b, e->encoding);
        break;
    case XML_START_ELEMENT: {
        n = d_node_new(b->doc, OS64_HTML_ELEMENT, &b->why);
        if (!n)
            return false;
        n->name = copy(b, e->name.local);
        if (e->name.prefix.size)
            n->prefix = copy(b, e->name.prefix);
        if (e->name.uri.size)
            n->namespace_uri = copy(b, e->name.uri);
        n->ns = !e->name.uri.size                                              ? OS64_HTML_NS_NONE
                : h_eq(n->namespace_uri, "http://www.w3.org/2000/svg")         ? OS64_HTML_NS_SVG
                : h_eq(n->namespace_uri, "http://www.w3.org/1999/xhtml")       ? OS64_HTML_NS_HTML
                : h_eq(n->namespace_uri, "http://www.w3.org/1998/Math/MathML") ? OS64_HTML_NS_MATHML
                                                                               : OS64_HTML_NS_OTHER;
        if (b->why)
            return false;
        if (n->ns == OS64_HTML_NS_HTML)
            n->tag = os64_html_tag_from_name(n->name);
        HAttr **tail = &n->attrs;
        for (size_t i = 0; i < e->namespace_count; ++i) {
            xml_namespace_t decl = e->namespaces[i];
            xml_name_t name = {.uri = {"http://www.w3.org/2000/xmlns/", 29}};
            if (decl.prefix.size) {
                name.prefix = (xml_slice_t){"xmlns", 5};
                name.local = decl.prefix;
            } else
                name.local = (xml_slice_t){"xmlns", 5};
            if (!attr(b, &tail, name, decl.uri))
                return false;
        }
        for (size_t i = 0; i < e->attribute_count; ++i)
            if (!attr(b, &tail, e->attributes[i].name, e->attributes[i].value))
                return false;
        b->doc->form_inputs += d_form_input(n);
        h_attach(b->doc, b->current, NULL, n);
        b->current = n;
        return true;
    }
    case XML_END_ELEMENT:
        b->current = b->current->parent;
        break;
    case XML_COMMENT:
    case XML_PI:
        n = d_node_new(b->doc, e->kind == XML_PI ? OS64_HTML_PI : OS64_HTML_COMMENT, &b->why);
        if (!n)
            return false;
        if (e->kind == XML_PI)
            n->name = copy(b, e->target);
        n->text = copy(b, e->text);
        n->text_len = e->text.size;
        break;
    case XML_DOCTYPE:
        n = d_node_new(b->doc, OS64_HTML_DOCTYPE, &b->why);
        if (!n)
            return false;
        n->name = copy(b, e->target);
        n->public_id = copy(b, e->public_id);
        n->system_id = copy(b, e->system_id);
        break;
    case XML_DECLARATION:
    case XML_END_DOCUMENT:
        break;
    case XML_TEXT:
    case XML_CDATA:
        break;
    }
    if (b->why)
        return false;
    if (n)
        h_attach(b->doc, b->current, NULL, n);
    return true;
}
os64_html_xml_options_t os64_html_xml_options_default(void)
{
    return (os64_html_xml_options_t){.max_arena_bytes = 64u * 1024u * 1024u,
                                     .max_tree_work = 64u * 1024u * 1024u,
                                     .parser_limits = xml_default_limits()};
}
os64_html_xml_result_t os64_html_xml_parser_result(const os64_html_xml_parser_t *b)
{
    if (!b)
        return (os64_html_xml_result_t){.status = XML_BAD_ARGUMENT};
    return (os64_html_xml_result_t){.status = b->why ? tree_verdict(b->why) : xml_status(b->parser),
                                    .tree_status = b->why,
                                    .location = xml_error_location(b->parser),
                                    .parser = xml_stats(b->parser),
                                    .tree_work = b->work,
                                    .arena_bytes = b->doc->pub.arena_bytes,
                                    .peak_arena_bytes = b->doc->pub.peak_arena_bytes};
}
os64_html_xml_parser_t *os64_html_xml_parser_new(const os64_html_xml_options_t *options,
                                                 os64_html_xml_result_t *result)
{
    os64_html_xml_options_t opt = options ? *options : os64_html_xml_options_default();
    int64_t why = OS64_HTML_OK;
    size_t depth = opt.parser_limits.depth ? opt.parser_limits.depth : xml_default_limits().depth;
    HDoc *d = d_xml_document_new(opt.max_arena_bytes, depth, &why);
    os64_html_xml_parser_t *b = d ? d_alloc(d, sizeof(*b), &why) : NULL;
    xml_status_t status = XML_NEED_INPUT;
    if (b) {
        b->doc = d;
        b->current = &d->root;
        b->options = opt;
        xml_options_t parser_options = {.memory = {b, parser_alloc, parser_free},
                                        .context = b,
                                        .event = event,
                                        .cancelled = cancelled,
                                        .limits = opt.parser_limits};
        status = xml_create(&parser_options, &b->parser);
        if (result)
            *result = os64_html_xml_parser_result(b);
        if (status == XML_NEED_INPUT)
            return b;
        why = b->why;
    }
    if (result)
        *result = (os64_html_xml_result_t){.status = why ? tree_verdict(why) : status,
                                           .tree_status = why};
    if (d)
        os64_html_document_free(&d->pub);
    return NULL;
}
xml_status_t os64_html_xml_parser_feed(os64_html_xml_parser_t *b, const void *bytes, size_t len,
                                       size_t *consumed)
{
    if (consumed)
        *consumed = 0;
    if (!b || b->busy || !consumed || (!bytes && len))
        return XML_BAD_ARGUMENT;
    if (b->why)
        return tree_verdict(b->why);
    b->busy = true;
    xml_status_t status = xml_feed(b->parser, bytes, len, consumed);
    b->busy = false;
    return b->why ? tree_verdict(b->why) : status;
}
xml_slice_t os64_html_xml_parser_encoding(const os64_html_xml_parser_t *b)
{
    return b ? xml_encoding(b->parser) : (xml_slice_t){0};
}
void os64_html_xml_parser_destroy(os64_html_xml_parser_t *b)
{
    if (!b || b->busy)
        return;
    HDoc *d = b->doc;
    xml_destroy(b->parser);
    os64_html_document_free(&d->pub);
}
os64_html_document_t *os64_html_xml_parser_finish(os64_html_xml_parser_t *b,
                                                  os64_html_xml_result_t *result)
{
    if (!b || b->busy) {
        if (result)
            *result = (os64_html_xml_result_t){.status = XML_BAD_ARGUMENT};
        return NULL;
    }
    b->busy = true;
    if (!b->why)
        xml_finish(b->parser);
    os64_html_xml_result_t r = os64_html_xml_parser_result(b);
    HDoc *d = b->doc;
    xml_destroy(b->parser);
    d->pub.input_bytes = r.parser.encoded_bytes;
    d->pub.work =
        r.tree_work > UINT64_MAX - r.parser.work ? UINT64_MAX : r.tree_work + r.parser.work;
    h_free(d, b->text.s);
    h_free(d, b);
    if (result)
        *result = r;
    if (r.status != XML_DONE) {
        os64_html_document_free(&d->pub);
        return NULL;
    }
    return &d->pub;
}
