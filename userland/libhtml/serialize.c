#include "internal.h"

typedef struct {
    char *out;
    size_t cap, length;
    bool scripting;
} SWriter;
static void bytes(SWriter *w, const char *s, size_t len)
{
    size_t room = w->cap && w->length < w->cap - 1 ? w->cap - 1 - w->length : 0;
    size_t copy = len < room ? len : room;
    for (size_t i = 0; i < copy; i++)
        w->out[w->length + i] = s[i];
    w->length = len > SIZE_MAX - w->length ? SIZE_MAX : w->length + len;
}
static void literal(SWriter *w, const char *s)
{
    bytes(w, s, h_len(s));
}
static bool html(const HNode *n)
{
    return n && n->kind == OS64_HTML_ELEMENT && n->ns == OS64_HTML_NS_HTML;
}
static bool is_void(const HNode *n)
{
    return html(n) && h_in(n->name, "area base basefont bgsound br col embed frame hr img input "
                                  "keygen link meta param source track wbr");
}
static bool raw_parent(const HNode *n, bool scripting)
{
    return html(n) && (h_in(n->name, "style script xmp iframe noembed noframes plaintext") ||
                       (scripting && h_eq(n->name, "noscript")));
}
static void escaped(SWriter *w, const char *s, size_t len, bool attribute)
{
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        const char *replacement = NULL;
        size_t take = 1;
        if (s[i] == '&')
            replacement = "&amp;";
        else if (s[i] == '<')
            replacement = "&lt;";
        else if (s[i] == '>')
            replacement = "&gt;";
        else if (attribute && s[i] == '"')
            replacement = "&quot;";
        else if ((unsigned char)s[i] == 0xc2 && i + 1 < len && (unsigned char)s[i + 1] == 0xa0) {
            replacement = "&nbsp;";
            take = 2;
        }
        if (replacement) {
            bytes(w, s + start, i - start);
            literal(w, replacement);
            i += take - 1;
            start = i + 1;
        }
    }
    if (len > start)
        bytes(w, s + start, len - start);
}
static const HNode *children(const HNode *n)
{
    if (is_void(n))
        return NULL;
    return html(n) && h_eq(n->name, "template") && n->template_contents
               ? n->template_contents : n->first_child;
}
static bool open_node(SWriter *w, const HNode *n)
{
    switch (n->kind) {
    case OS64_HTML_ELEMENT:
        literal(w, "<");
        literal(w, n->name);
        for (const HAttr *a = n->attrs; a; a = a->next) {
            literal(w, " ");
            /* Records already retain namespace-adjusted qualified spelling. */
            literal(w, a->name);
            literal(w, "=\"");
            escaped(w, a->value, h_len(a->value), true);
            literal(w, "\"");
        }
        literal(w, ">");
        return !is_void(n);
    case OS64_HTML_TEXT:
        if (raw_parent(n->parent, w->scripting))
            bytes(w, n->text, n->text_len);
        else
            escaped(w, n->text, n->text_len, false);
        return false;
    case OS64_HTML_COMMENT:
        literal(w, "<!--");
        bytes(w, n->text, n->text_len);
        literal(w, "-->");
        return false;
    case OS64_HTML_DOCTYPE:
        literal(w, "<!DOCTYPE ");
        literal(w, n->name);
        literal(w, ">");
        return false;
    case OS64_HTML_DOCUMENT:
    case OS64_HTML_FRAGMENT:
        return true;
    }
    return false;
}
static void close_node(SWriter *w, const HNode *n)
{
    if (n->kind == OS64_HTML_ELEMENT && !is_void(n)) {
        literal(w, "</");
        literal(w, n->name);
        literal(w, ">");
    }
}
static const HNode *above(const HNode *n)
{
    return n->kind == OS64_HTML_FRAGMENT && !n->parent ? (const HNode *)*h_word(n) : n->parent;
}
size_t os64_html_serialize(const os64_html_node_t *node, bool children_only,
                           bool scripting, char *out, size_t cap)
{
    SWriter w = {out, out ? cap : 0, 0, scripting};
    if (node && ! (children_only && is_void(node))) {
        const HNode *root = node;
        if (children_only && html(root) && h_eq(root->name, "template") && root->template_contents)
            root = root->template_contents;
        const HNode *n = children_only ? root->first_child : root;
        while (n) {
            bool descend = open_node(&w, n);
            const HNode *child = descend ? children(n) : NULL;
            if (child) {
                n = child;
                continue;
            }
            if (descend)
                close_node(&w, n);
            while (n != root && !n->next) {
                n = above(n);
                if (!n || (children_only && n == root))
                    break;
                close_node(&w, n);
            }
            if (!n || n == root)
                break;
            n = n->next;
        }
    }
    if (w.cap)
        w.out[w.length < w.cap ? w.length : w.cap - 1] = 0;
    return w.length;
}
