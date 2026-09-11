#include "internal.h"

typedef struct {
    const char *lower, *adjusted;
} HAdjustment;

static const HAdjustment svg_tags[] = {
#include "svg_tags.inc"
};

static const HAdjustment svg_attrs[] = {
#include "svg_attrs.inc"
};

static const char *const quirks_prefixes[] = {
#include "quirks.inc"
};

#define HTML OS64_HTML_NS_HTML
#define SVG OS64_HTML_NS_SVG
#define MATH OS64_HTML_NS_MATHML
#define ELEMENT OS64_HTML_ELEMENT
#define TEXT OS64_HTML_TEXT
#define COMMENT OS64_HTML_COMMENT
#define FRAGMENT OS64_HTML_FRAGMENT
#define NONE SIZE_MAX
static bool named(const HNode *n, const char *name)
{
    return n && n->kind == ELEMENT && n->ns == HTML && h_eq(n->name, name);
}
static bool named_in(const HNode *n, const char *set)
{
    return n && n->kind == ELEMENT && n->ns == HTML && h_in(n->name, set);
}
static bool start(const HToken *t, const char *name)
{
    return t->type == H_START && h_eq(t->name.s, name);
}
static bool end(const HToken *t, const char *name)
{
    return t->type == H_END && h_eq(t->name.s, name);
}
static bool white(const HToken *t)
{
    return t->type == H_CHAR && h_space(t->ch);
}
static bool special(const HNode *n)
{
    if (n->ns == MATH)
        return h_in(n->name, "mi mo mn ms mtext annotation-xml");

    if (n->ns == SVG)
        return h_in(n->name, "foreignObject desc title");

    return h_in(
        n->name,
        "address applet area article aside base basefont bgsound blockquote body br button caption "
        "center col colgroup dd details dir div dl dt embed fieldset figcaption figure footer form "
        "frame frameset h1 h2 h3 h4 h5 h6 head header hgroup hr html iframe img input keygen li "
        "link listing main marquee menu meta nav noembed noframes noscript object ol p param "
        "plaintext pre script search section select source style summary table tbody td template "
        "textarea tfoot th thead title tr track ul wbr xmp");
}
static size_t index_of(os64_html_parser_t *p, HNodes *list, HNode *n)
{
    for (size_t i = list->n; i; i--) {
        if (!h_work(p, 1))
            return NONE;
        if (list->v[i - 1] == n)
            return i - 1;
    }
    return NONE;
}
static void remove_index(os64_html_parser_t *p, HNodes *list, size_t i)
{
    if (i >= list->n || !h_work(p, list->n - i))
        return;

    for (size_t j = i + 1; j < list->n; j++)
        list->v[j - 1] = list->v[j];
    list->n--;
}
static bool insert_index(os64_html_parser_t *p, HNodes *list, size_t i, HNode *n)
{
    if (!h_work(p, list->n - i) || !h_nodes_push(p, list, n))
        return false;

    for (size_t j = list->n - 1; j > i; j--)
        list->v[j] = list->v[j - 1];
    list->v[i] = n;
    return true;
}
static void pop(os64_html_parser_t *p)
{
    if (p->stack.n)
        p->stack.n--;
}
/* Scope boundaries include foreign integration elements even though the
 * target searched for is an HTML element. */
static bool scope_boundary(const HNode *n, int kind)
{
    if (kind == 4)
        return !named_in(n, "option optgroup");

    if (kind == 3)
        return named_in(n, "html table template");

    if (named_in(n, "applet caption html table td th marquee object template"))
        return true;

    if (n->ns == MATH && h_in(n->name, "mi mo mn ms mtext annotation-xml"))
        return true;

    if (n->ns == SVG && h_in(n->name, "foreignObject desc title"))
        return true;

    return (kind == 1 && named_in(n, "ol ul")) || (kind == 2 && named(n, "button"));
}
static size_t scope(os64_html_parser_t *p, const char *name, int kind)
{
    for (size_t i = p->stack.n; i; i--) {
        if (!h_work(p, 1))
            return NONE;

        HNode *n = p->stack.v[i - 1];
        if (named(n, name))
            return i - 1;
        if (scope_boundary(n, kind))
            return NONE;
    }
    return NONE;
}
static bool template_open(os64_html_parser_t *p)
{
    for (size_t i = p->stack.n; i; i--) {
        if (!h_work(p, 1))
            return false;
        if (named(p->stack.v[i - 1], "template"))
            return true;
    }
    return false;
}
static void implied(os64_html_parser_t *p, const char *except, bool thorough)
{
    while (p->stack.n && !p->d->pub.refusal) {
        HNode *n = h_current(p);
        if (!h_work(p, 1))
            return;

        if (named(n, except) || !named_in(n, thorough ? "caption colgroup dd dt li optgroup option "
                                                        "p rb rp rt rtc tbody td tfoot th thead tr"
                                                      : "dd dt li optgroup option p rb rp rt rtc"))
            return;

        pop(p);
    }
}
static void close_p(os64_html_parser_t *p)
{
    size_t at = scope(p, "p", 2);
    if (at == NONE)
        return;
    implied(p, "p", false);

    if (!named(h_current(p), "p"))
        h_error(p, "unexpected-end-tag");
    p->stack.n = at;
}
static void clear_formatting(os64_html_parser_t *p)
{
    while (p->formatting.n) {
        if (!h_work(p, 1))
            return;
        HNode *n = p->formatting.v[--p->formatting.n];
        if (!n)
            break;
    }
}
static bool push_open(os64_html_parser_t *p, HNode *n)
{
    if (p->stack.n >= p->opt.max_depth) {
        h_refuse(p, OS64_HTML_TOO_DEEP);
        return false;
    }
    return h_nodes_push(p, &p->stack, n);
}
/* Foster parenting may insert before a table instead of under the current
 * node. Template contents redirect insertion to their detached fragment. */
static void insertion_place(os64_html_parser_t *p, HNode *override, HNode **parent, HNode **before)
{
    HNode *target = override ? override : h_current(p);
    *parent = target;
    *before = NULL;

    if (p->foster && named_in(target, "table tbody tfoot thead tr")) {
        size_t table = NONE, templ = NONE;

        for (size_t i = p->stack.n; i; i--) {
            if (!h_work(p, 1))
                return;

            HNode *n = p->stack.v[i - 1];

            if (templ == NONE && named(n, "template"))
                templ = i - 1;

            if (table == NONE && named(n, "table"))
                table = i - 1;
        }
        if (templ != NONE && (table == NONE || templ > table))
            *parent = p->stack.v[templ]->template_contents;

        else if (table == NONE)
            *parent = p->stack.n ? p->stack.v[0] : &p->d->html;

        else if (p->stack.v[table]->parent) {
            *parent = p->stack.v[table]->parent;
            *before = p->stack.v[table];
        } else
            *parent = table ? p->stack.v[table - 1] : &p->d->html;
    }
    if (named(*parent, "template"))
        *parent = (*parent)->template_contents;
}
static const char *adjust(const char *name, const HAdjustment *table, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (h_eq(name, table[i].lower))
            return table[i].adjusted;
    return name;
}
static void foreign_attrs(os64_html_parser_t *p, HAttr *attrs, os64_html_ns_t ns)
{
    for (HAttr *a = attrs; a; a = a->next) {
        if (!h_work(p, 1 + h_len(a->name)))
            return;
        if (ns == SVG)
            a->name = adjust(a->name, svg_attrs, H_ARRAY(svg_attrs));

        if (ns == MATH && h_eq(a->name, "definitionurl"))
            a->name = "definitionURL";

        if (h_in(a->name, "xlink:actuate xlink:arcrole xlink:href xlink:role xlink:show "
                          "xlink:title xlink:type"))
            a->ns = "http://www.w3.org/1999/xlink";

        else if (h_in(a->name, "xml:lang xml:space"))
            a->ns = "http://www.w3.org/XML/1998/namespace";

        else if (h_in(a->name, "xmlns xmlns:xlink"))
            a->ns = "http://www.w3.org/2000/xmlns/";
    }
}
static HNode *element(os64_html_parser_t *p, HToken *t, os64_html_ns_t ns, bool push)
{
    if (push && p->stack.n >= p->opt.max_depth) {
        h_refuse(p, OS64_HTML_TOO_DEEP);
        return NULL;
    }
    HNode *n = h_node(p, ELEMENT);
    if (!n)
        return NULL;

    n->ns = ns;
    n->name = h_copy(p, t->name.s, t->name.len);
    n->attrs = t->attrs;

    if (p->d->pub.refusal)
        return NULL;

    if (ns == HTML)
        n->tag = os64_html_tag_from_name(n->name);

    else {
        if (ns == SVG)
            n->name = adjust(n->name, svg_tags, H_ARRAY(svg_tags));
        foreign_attrs(p, n->attrs, ns);
    }
    if (named(n, "template")) {
        n->template_contents = h_node(p, FRAGMENT);
        if (!n->template_contents)
            return NULL;
    }
    HNode *parent, *before;
    insertion_place(p, NULL, &parent, &before);

    if (p->d->pub.refusal)
        return NULL;

    if (push && !push_open(p, n))
        return NULL;

    h_attach(parent, before, n);

    if (named(n, "head")) {
        p->head = n;
        p->d->pub.head = n;
    }
    if (named(n, "body"))
        p->d->pub.body = n;

    if (named(n, "meta"))
        h_late_meta(p, n);

    return n;
}
static HNode *synthetic(os64_html_parser_t *p, const char *name)
{
    HToken t = {0};
    t.type = H_START;
    t.name.s = (char *)name;
    t.name.len = h_len(name);
    return element(p, &t, HTML, true);
}
static void insert_comment(os64_html_parser_t *p, HToken *t, HNode *parent)
{
    HNode *n = h_node(p, COMMENT);
    if (!n)
        return;

    n->text = h_copy(p, t->data.s, t->data.len);
    n->text_len = t->data.len;

    HNode *before = NULL;
    if (!parent)
        insertion_place(p, NULL, &parent, &before);

    if (!p->d->pub.refusal)
        h_attach(parent, before, n);
}
static void insert_character(os64_html_parser_t *p, uint32_t c)
{
    HNode *parent, *before;
    insertion_place(p, NULL, &parent, &before);

    if (p->d->pub.refusal || parent->kind == OS64_HTML_DOCUMENT)
        return;

    HNode *n = before ? before->prev : parent->last_child;

    if (!n || n->kind != TEXT) {
        n = h_node(p, TEXT);
        if (!n)
            return;
        n->text = "";
        h_attach(parent, before, n);
    }
    size_t *cap = (size_t *)(n + 1);
    HBuf b = {*cap ? (char *)n->text : NULL, n->text_len, *cap};

    if (h_buf_put(p, &b, c)) {
        n->text = b.s;
        n->text_len = b.len;
        *cap = b.cap;
    }
}
static HNode *clone(os64_html_parser_t *p, HNode *old, bool insert)
{
    HNode *n;

    if (insert) {
        HToken t = {0};
        t.type = H_START;
        t.name.s = (char *)old->name;
        t.name.len = h_len(old->name);
        t.attrs = old->attrs;
        n = element(p, &t, old->ns, true);
    } else {
        n = h_node(p, ELEMENT);
        if (n) {
            n->name = old->name;
            n->ns = old->ns;
            n->tag = old->tag;
            n->attrs = old->attrs;
        }
    }
    return n;
}
/* Formatting entries outlive their open-stack nodes after misnesting. Walk
 * back to a marker or open entry, then clone forward to restore their order. */
static void reconstruct(os64_html_parser_t *p)
{
    size_t i = p->formatting.n;
    if (!i)
        return;

    HNode *last = p->formatting.v[i - 1];
    if (!last || index_of(p, &p->stack, last) != NONE)
        return;

    while (i) {
        if (!h_work(p, 1))
            return;
        HNode *n = p->formatting.v[i - 1];
        if (!n || index_of(p, &p->stack, n) != NONE)
            break;
        i--;
    }
    for (; i < p->formatting.n && !p->d->pub.refusal; i++) {
        HNode *n = clone(p, p->formatting.v[i], true);
        if (!n)
            return;
        p->formatting.v[i] = n;
    }
}
static bool same_attrs(os64_html_parser_t *p, HNode *a, HNode *b)
{
    if (!h_work(p, 1 + h_len(a->name)) || a->ns != b->ns || !h_eq(a->name, b->name))
        return false;

    size_t ac = 0, bc = 0;

    for (HAttr *x = a->attrs; x; x = x->next) {
        if (!h_work(p, 1))
            return false;
        ac++;
        bool found = false;

        for (HAttr *y = b->attrs; y; y = y->next) {
            if (!h_work(p, 1 + h_len(x->name) + h_len(x->value)))
                return false;

            if (h_eq(x->name, y->name) && h_eq(x->value, y->value) && h_eq(x->ns, y->ns)) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    for (HAttr *y = b->attrs; y; y = y->next) {
        if (!h_work(p, 1))
            return false;
        bc++;
    }
    return ac == bc;
}
static void add_formatting(os64_html_parser_t *p, HNode *n)
{
    if (!n)
        return;
    size_t matches = 0, earliest = NONE;

    for (size_t i = p->formatting.n; i; i--) {
        if (!h_work(p, 1))
            return;
        HNode *old = p->formatting.v[i - 1];
        if (!old) {
            break;
        }

        if (same_attrs(p, n, old)) {
            matches++;
            earliest = i - 1;
        }
    }
    if (matches >= 3)
        remove_index(p, &p->formatting, earliest);

    if (!p->d->pub.refusal)
        h_nodes_push(p, &p->formatting, n);
}
static void ordinary_end(os64_html_parser_t *p, const char *name)
{
    for (size_t i = p->stack.n; i; i--) {
        if (!h_work(p, 1))
            return;
        HNode *n = p->stack.v[i - 1];

        if (named(n, name)) {
            implied(p, name, false);
            if (h_current(p) != n)
                h_error(p, "unexpected-end-tag");
            p->stack.n = i - 1;
            return;
        }
        if (special(n)) {
            h_error(p, "unexpected-end-tag");
            return;
        }
    }
}
/* The adoption agency repairs misnested formatting by moving existing
 * subtrees and replacing list/stack entries with clones. The bookmark tracks
 * the formatting insertion point even when entries before it are removed. */
static void adoption(os64_html_parser_t *p, const char *subject)
{
    HNode *current = h_current(p);

    if (named(current, subject) && index_of(p, &p->formatting, current) == NONE) {
        pop(p);
        return;
    }
    for (unsigned outer = 0; outer < 8 && !p->d->pub.refusal; outer++) {
        size_t fi = NONE;

        for (size_t i = p->formatting.n; i; i--) {
            if (!h_work(p, 1))
                return;
            HNode *n = p->formatting.v[i - 1];
            if (!n)
                break;
            if (named(n, subject)) {
                fi = i - 1;
                break;
            }
        }
        if (fi == NONE) {
            ordinary_end(p, subject);
            return;
        }
        HNode *format = p->formatting.v[fi];
        size_t si = index_of(p, &p->stack, format);

        if (si == NONE) {
            h_error(p, "unexpected-end-tag");
            remove_index(p, &p->formatting, fi);
            return;
        }
        if (scope(p, subject, 0) == NONE) {
            h_error(p, "unexpected-end-tag");
            return;
        }
        if (format != h_current(p))
            h_error(p, "unexpected-end-tag");

        size_t furthest = NONE;

        for (size_t i = si + 1; i < p->stack.n; i++) {
            if (!h_work(p, 1))
                return;
            if (special(p->stack.v[i])) {
                furthest = i;
                break;
            }
        }
        if (furthest == NONE) {
            p->stack.n = si;
            remove_index(p, &p->formatting, fi);
            return;
        }
        if (!si) {
            h_error(p, "unexpected-end-tag");
            return;
        }
        HNode *block = p->stack.v[furthest], *ancestor = p->stack.v[si - 1], *last = block;

        size_t bookmark = fi, index = furthest;
        unsigned inner = 0;

        while (index && !p->d->pub.refusal) {
            HNode *node = p->stack.v[--index];
            if (node == format)
                break;
            inner++;

            size_t ai = index_of(p, &p->formatting, node);

            if (inner > 3 && ai != NONE) {
                remove_index(p, &p->formatting, ai);
                if (ai < bookmark)
                    bookmark--;
                ai = NONE;
            }
            if (ai == NONE) {
                remove_index(p, &p->stack, index);
                continue;
            }
            HNode *copy = clone(p, node, false);
            if (!copy)
                return;

            p->formatting.v[ai] = copy;
            p->stack.v[index] = copy;

            if (last == block)
                bookmark = ai + 1;

            h_attach(copy, NULL, last);
            last = copy;
        }
        HNode *parent, *before;
        insertion_place(p, ancestor, &parent, &before);
        if (p->d->pub.refusal)
            return;

        h_attach(parent, before, last);

        HNode *copy = clone(p, format, false);
        if (!copy)
            return;

        while (block->first_child) {
            if (!h_work(p, 1))
                return;
            h_attach(copy, NULL, block->first_child);
        }
        h_attach(block, NULL, copy);

        fi = index_of(p, &p->formatting, format);
        if (fi < bookmark)
            bookmark--;

        remove_index(p, &p->formatting, fi);
        if (!insert_index(p, &p->formatting, bookmark, copy))
            return;

        si = index_of(p, &p->stack, format);
        remove_index(p, &p->stack, si);

        size_t bi = index_of(p, &p->stack, block);
        if (bi == NONE)
            return;

        if (!insert_index(p, &p->stack, bi + 1, copy))
            return;
    }
}
static bool push_template(os64_html_parser_t *p, HMode mode)
{
    if (p->templates_n == p->templates_cap) {
        size_t cap = p->templates_cap ? p->templates_cap * 2 : 8;

        if (cap < p->templates_cap || cap > SIZE_MAX / sizeof(HMode)) {
            h_refuse(p, OS64_HTML_ARENA_EXHAUSTED);
            return false;
        }
        HMode *v = h_alloc(p, cap * sizeof(*v));
        if (!v)
            return false;

        for (size_t i = 0; i < p->templates_n; i++)
            v[i] = p->templates[i];
        h_free(p->d, p->templates);
        p->templates = v;
        p->templates_cap = cap;
    }
    p->templates[p->templates_n++] = mode;
    return true;
}
static void reset_mode(os64_html_parser_t *p)
{
    for (size_t i = p->stack.n; i; i--) {
        if (!h_work(p, 1))
            return;
        HNode *n = p->stack.v[i - 1];
        if (n->ns != HTML)
            continue;

        if (named(n, "select")) {
            p->mode = M_SELECT;

            for (size_t j = i - 1; j; j--) {
                if (!h_work(p, 1))
                    return;
                if (named(p->stack.v[j - 1], "template"))
                    break;
                if (named(p->stack.v[j - 1], "table")) {
                    p->mode = M_SELECT_TABLE;
                    break;
                }
            }
            return;
        }
        if (named_in(n, "td th")) {
            p->mode = M_CELL;
            return;
        }
        if (named(n, "tr")) {
            p->mode = M_ROW;
            return;
        }
        if (named_in(n, "tbody thead tfoot")) {
            p->mode = M_TABLE_BODY;
            return;
        }
        if (named(n, "caption")) {
            p->mode = M_CAPTION;
            return;
        }
        if (named(n, "colgroup")) {
            p->mode = M_COLGROUP;
            return;
        }
        if (named(n, "table")) {
            p->mode = M_TABLE;
            return;
        }
        if (named(n, "template")) {
            p->mode = p->templates_n ? p->templates[p->templates_n - 1] : M_TEMPLATE;
            return;
        }
        if (named(n, "head")) {
            p->mode = M_HEAD;
            return;
        }
        if (named(n, "body")) {
            p->mode = M_BODY;
            return;
        }
        if (named(n, "frameset")) {
            p->mode = M_FRAMESET;
            return;
        }
        if (named(n, "html")) {
            p->mode = p->head ? M_AFTER_HEAD : M_BEFORE_HEAD;
            return;
        }
    }
    p->mode = M_BODY;
}
static void raw_element(os64_html_parser_t *p, HToken *t, HState state)
{
    if (!element(p, t, HTML, true))
        return;
    p->state = state;
    p->original_mode = p->mode;
    p->mode = M_TEXT;
}
static bool ascii_ci_prefix(const char *s, const char *prefix)
{
    if (!s)
        return false;
    while (*prefix) {
        if (h_lower((unsigned char)*s++) != h_lower((unsigned char)*prefix++))
            return false;
    }
    return true;
}
static bool ascii_ci_equal(const char *s, const char *word)
{
    if (!s || !word)
        return s == word;
    while (*s && *word && h_lower((unsigned char)*s) == h_lower((unsigned char)*word)) {
        s++;
        word++;
    }
    return *s == *word;
}
static void doctype(os64_html_parser_t *p, HToken *t)
{
    HNode *n = h_node(p, OS64_HTML_DOCTYPE);
    if (!n)
        return;

    n->name = h_copy(p, t->name.s, t->name.len);

    n->public_id = t->has_public ? h_copy(p, t->public_id.s, t->public_id.len) : NULL;

    n->system_id = t->has_system ? h_copy(p, t->system_id.s, t->system_id.len) : NULL;

    if (p->d->pub.refusal)
        return;
    h_attach(&p->d->root, NULL, n);

    if (!h_eq(n->name, "html") || t->has_public ||
        (t->has_system && !h_eq(n->system_id, "about:legacy-compat")))
        h_error(p, "invalid-doctype");

    bool quirks =
        t->force_quirks || !h_eq(n->name, "html") ||
        ascii_ci_equal(n->public_id, "-//W3O//DTD W3 HTML Strict 3.0//EN//") ||
        ascii_ci_equal(n->public_id, "-/W3C/DTD HTML 4.0 Transitional/EN") ||
        ascii_ci_equal(n->public_id, "HTML") ||
        ascii_ci_equal(n->system_id, "http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd");

    for (size_t i = 0; i < H_ARRAY(quirks_prefixes); i++)
        if (ascii_ci_prefix(n->public_id, quirks_prefixes[i]))
            quirks = true;

    bool html4 = ascii_ci_prefix(n->public_id, "-//W3C//DTD HTML 4.01 Frameset//") ||
                 ascii_ci_prefix(n->public_id, "-//W3C//DTD HTML 4.01 Transitional//");

    if (html4 && !t->has_system)
        quirks = true;

    bool limited = ascii_ci_prefix(n->public_id, "-//W3C//DTD XHTML 1.0 Frameset//") ||
                   ascii_ci_prefix(n->public_id, "-//W3C//DTD XHTML 1.0 Transitional//") ||
                   (html4 && t->has_system);

    p->d->pub.quirks = quirks    ? OS64_HTML_QUIRKS
                       : limited ? OS64_HTML_LIMITED_QUIRKS
                                 : OS64_HTML_NO_QUIRKS;
}

static bool process(os64_html_parser_t *p, HToken *t, HMode mode);

static void merge_attrs(os64_html_parser_t *p, HNode *n, HAttr *attrs)
{
    HAttr **tail = &n->attrs;
    while (*tail) {
        if (!h_work(p, 1))
            return;
        tail = &(*tail)->next;
    }

    for (HAttr *a = attrs; a && !p->d->pub.refusal; a = a->next) {
        bool exists = false;

        for (HAttr *b = n->attrs; b; b = b->next) {
            if (!h_work(p, 1 + h_len(a->name)))
                return;
            if (h_eq(a->name, b->name)) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            HAttr *copy = h_permanent(p, sizeof(*copy));
            if (!copy)
                return;
            *copy = *a;
            copy->next = NULL;
            *tail = copy;
            tail = &copy->next;
        }
    }
}
static bool body(os64_html_parser_t *p, HToken *t)
{
    const char *name = t->name.s;

    if (t->type == H_CHAR) {
        if (!t->ch) {
            h_error(p, "unexpected-null-character");
            return false;
        }
        reconstruct(p);
        insert_character(p, t->ch);
        if (!h_space(t->ch))
            p->frameset_ok = false;
        return false;
    }
    if (t->type == H_COMMENT) {
        insert_comment(p, t, NULL);
        return false;
    }
    if (t->type == H_DOCTYPE) {
        h_error(p, "unexpected-doctype");
        return false;
    }
    if (t->type == H_END_INPUT) {
        if (p->templates_n)
            return process(p, t, M_TEMPLATE);

        for (size_t i = 0; i < p->stack.n; i++) {
            if (!h_work(p, 1))
                return false;
            if (!named_in(p->stack.v[i], "dd dt li optgroup option p rb rp rt rtc tbody td tfoot "
                                         "th thead tr body html")) {
                h_error(p, "eof-with-open-elements");
                break;
            }
        }
        return false;
    }
    if (t->type == H_START) {
        if (h_eq(name, "html")) {
            h_error(p, "unexpected-start-tag");
            if (!template_open(p))
                merge_attrs(p, &p->d->html, t->attrs);
            return false;
        }
        if (h_in(name, "base basefont bgsound link meta noframes script style template title"))
            return process(p, t, M_HEAD);

        if (h_eq(name, "body")) {
            h_error(p, "unexpected-start-tag");

            if (p->stack.n >= 2 && named(p->stack.v[1], "body") && !template_open(p)) {
                p->frameset_ok = false;
                merge_attrs(p, p->stack.v[1], t->attrs);
            }
            return false;
        }
        if (h_eq(name, "frameset")) {
            h_error(p, "unexpected-start-tag");

            if (!p->frameset_ok || p->stack.n < 2 || !named(p->stack.v[1], "body"))
                return false;

            h_detach(p->stack.v[1]);
            p->d->pub.body = NULL;
            p->stack.n = 1;
            element(p, t, HTML, true);
            p->mode = M_FRAMESET;
            return false;
        }
        if (h_in(name, "address article aside blockquote center details dialog dir div dl fieldset "
                       "figcaption figure footer header hgroup main menu nav ol p search section "
                       "summary ul")) {
            close_p(p);
            element(p, t, HTML, true);
            return false;
        }
        if (h_in(name, "h1 h2 h3 h4 h5 h6")) {
            close_p(p);
            if (named_in(h_current(p), "h1 h2 h3 h4 h5 h6")) {
                h_error(p, "unexpected-start-tag");
                pop(p);
            }
            element(p, t, HTML, true);
            return false;
        }
        if (h_in(name, "pre listing")) {
            close_p(p);
            element(p, t, HTML, true);
            p->ignore_lf = true;
            p->frameset_ok = false;
            return false;
        }
        if (h_eq(name, "form")) {
            bool templ = template_open(p);
            if (p->form && !templ) {
                h_error(p, "unexpected-start-tag");
                return false;
            }
            close_p(p);
            HNode *n = element(p, t, HTML, true);
            if (!templ)
                p->form = n;
            return false;
        }
        if (h_in(name, "li dd dt")) {
            p->frameset_ok = false;

            for (size_t i = p->stack.n; i; i--) {
                if (!h_work(p, 1))
                    return false;
                HNode *n = p->stack.v[i - 1];

                if ((h_eq(name, "li") && named(n, "li")) ||
                    (!h_eq(name, "li") && named_in(n, "dd dt"))) {
                    implied(p, n->name, false);
                    if (h_current(p) != n)
                        h_error(p, "unexpected-start-tag");
                    p->stack.n = i - 1;
                    break;
                }
                if (special(n) && !named_in(n, "address div p"))
                    break;
            }
            close_p(p);
            element(p, t, HTML, true);
            return false;
        }
        if (h_eq(name, "plaintext")) {
            close_p(p);
            element(p, t, HTML, true);
            p->state = T_PLAIN;
            return false;
        }
        if (h_eq(name, "button")) {
            size_t at = scope(p, "button", 0);
            if (at != NONE) {
                h_error(p, "unexpected-start-tag");
                implied(p, NULL, false);
                p->stack.n = at;
            }
            reconstruct(p);
            element(p, t, HTML, true);
            p->frameset_ok = false;
            return false;
        }
        if (h_eq(name, "a")) {
            HNode *old = NULL;

            for (size_t i = p->formatting.n; i; i--) {
                if (!h_work(p, 1))
                    return false;
                HNode *n = p->formatting.v[i - 1];
                if (!n)
                    break;
                if (named(n, "a")) {
                    old = n;
                    break;
                }
            }
            if (old) {
                h_error(p, "unexpected-start-tag");
                adoption(p, "a");
                remove_index(p, &p->formatting, index_of(p, &p->formatting, old));
                remove_index(p, &p->stack, index_of(p, &p->stack, old));
            }
            reconstruct(p);
            add_formatting(p, element(p, t, HTML, true));
            return false;
        }
        if (h_in(name, "b big code em font i s small strike strong tt u")) {
            reconstruct(p);
            add_formatting(p, element(p, t, HTML, true));
            return false;
        }
        if (h_eq(name, "nobr")) {
            reconstruct(p);
            if (scope(p, "nobr", 0) != NONE) {
                h_error(p, "unexpected-start-tag");
                adoption(p, "nobr");
                reconstruct(p);
            }
            add_formatting(p, element(p, t, HTML, true));
            return false;
        }
        if (h_in(name, "applet marquee object")) {
            reconstruct(p);
            element(p, t, HTML, true);
            h_nodes_push(p, &p->formatting, NULL);
            p->frameset_ok = false;
            return false;
        }
        if (h_eq(name, "table")) {
            if (p->d->pub.quirks != OS64_HTML_QUIRKS)
                close_p(p);
            element(p, t, HTML, true);
            p->frameset_ok = false;
            p->mode = M_TABLE;
            return false;
        }
        if (h_eq(name, "image")) {
            h_error(p, "unexpected-start-tag");
            h_buf_reset(&t->name);
            h_buf_bytes(p, &t->name, "img", 3);
            return true;
        }
        if (h_in(name, "area br embed img keygen wbr")) {
            reconstruct(p);
            element(p, t, HTML, false);
            t->acknowledged = true;
            p->frameset_ok = false;
            return false;
        }
        if (h_eq(name, "input")) {
            reconstruct(p);
            HNode *n = element(p, t, HTML, false);
            t->acknowledged = true;

            const HAttr *a = os64_html_attr(n, "type");
            if (!a || !ascii_ci_equal(a->value, "hidden"))
                p->frameset_ok = false;
            return false;
        }
        if (h_in(name, "param source track")) {
            element(p, t, HTML, false);
            t->acknowledged = true;
            return false;
        }
        if (h_eq(name, "hr")) {
            close_p(p);
            element(p, t, HTML, false);
            t->acknowledged = true;
            p->frameset_ok = false;
            return false;
        }
        if (h_eq(name, "textarea")) {
            raw_element(p, t, T_RCDATA);
            p->ignore_lf = true;
            p->frameset_ok = false;
            return false;
        }
        if (h_eq(name, "xmp")) {
            close_p(p);
            reconstruct(p);
            p->frameset_ok = false;
            raw_element(p, t, T_RAWTEXT);
            return false;
        }
        if (h_eq(name, "iframe")) {
            p->frameset_ok = false;
            raw_element(p, t, T_RAWTEXT);
            return false;
        }
        if (h_eq(name, "noembed")) {
            raw_element(p, t, T_RAWTEXT);
            return false;
        }
        if (h_eq(name, "select")) {
            reconstruct(p);
            element(p, t, HTML, true);
            p->frameset_ok = false;

            p->mode = p->mode == M_TABLE || p->mode == M_CAPTION || p->mode == M_TABLE_BODY ||
                              p->mode == M_ROW || p->mode == M_CELL
                          ? M_SELECT_TABLE
                          : M_SELECT;
            return false;
        }
        if (h_in(name, "optgroup option")) {
            if (named(h_current(p), "option"))
                pop(p);
            reconstruct(p);
            element(p, t, HTML, true);
            return false;
        }
        if (h_in(name, "rb rtc rp rt")) {
            if (scope(p, "ruby", 0) != NONE) {
                implied(p, h_in(name, "rp rt") ? "rtc" : NULL, false);
                if (!named(h_current(p), "ruby") &&
                    !(h_in(name, "rp rt") && named(h_current(p), "rtc")))
                    h_error(p, "unexpected-start-tag");
            }
            element(p, t, HTML, true);
            return false;
        }
        if (h_in(name, "math svg")) {
            reconstruct(p);
            element(p, t, h_eq(name, "svg") ? SVG : MATH, !t->self_closing);
            if (t->self_closing)
                t->acknowledged = true;
            return false;
        }
        if (h_in(name, "caption col colgroup frame head tbody td tfoot th thead tr")) {
            h_error(p, "unexpected-start-tag");
            return false;
        }
        reconstruct(p);
        element(p, t, HTML, true);
        return false;
    }
    if (end(t, "template"))
        return process(p, t, M_HEAD);

    if (h_in(name, "body html")) {
        if (scope(p, "body", 0) == NONE) {
            h_error(p, "unexpected-end-tag");
            return false;
        }
        for (size_t i = 0; i < p->stack.n; i++) {
            if (!h_work(p, 1))
                return false;
            if (!named_in(p->stack.v[i], "dd dt li optgroup option p rb rp rt rtc tbody td tfoot "
                                         "th thead tr body html")) {
                h_error(p, "unexpected-end-tag");
                break;
            }
        }
        p->mode = M_AFTER_BODY;
        return h_eq(name, "html");
    }
    if (h_eq(name, "form")) {
        if (!template_open(p)) {
            HNode *form = p->form;
            p->form = NULL;

            if (!form || scope(p, "form", 0) == NONE) {
                h_error(p, "unexpected-end-tag");
                return false;
            }
            implied(p, NULL, false);
            if (h_current(p) != form)
                h_error(p, "unexpected-end-tag");
            remove_index(p, &p->stack, index_of(p, &p->stack, form));

        } else {
            size_t at = scope(p, "form", 0);
            if (at == NONE)
                h_error(p, "unexpected-end-tag");
            else {
                implied(p, NULL, false);
                if (!named(h_current(p), "form"))
                    h_error(p, "unexpected-end-tag");
                p->stack.n = at;
            }
        }
        return false;
    }
    if (h_eq(name, "p")) {
        if (scope(p, "p", 2) == NONE) {
            h_error(p, "unexpected-end-tag");
            synthetic(p, "p");
        }
        close_p(p);
        return false;
    }
    if (h_in(name, "address article aside blockquote button center details dialog dir div dl "
                   "fieldset figcaption figure footer header hgroup listing main menu nav ol pre "
                   "search section summary ul li dd dt")) {
        size_t at = scope(p, name, h_eq(name, "li") ? 1 : 0);

        if (at == NONE) {
            h_error(p, "unexpected-end-tag");
            return false;
        }
        implied(p, h_in(name, "li dd dt") ? name : NULL, false);
        if (!named(h_current(p), name))
            h_error(p, "unexpected-end-tag");
        p->stack.n = at;
        return false;
    }
    if (h_in(name, "h1 h2 h3 h4 h5 h6")) {
        size_t at = NONE;

        for (size_t i = p->stack.n; i; i--) {
            if (!h_work(p, 1))
                return false;
            HNode *n = p->stack.v[i - 1];
            if (named_in(n, "h1 h2 h3 h4 h5 h6")) {
                at = i - 1;
                break;
            }
            if (scope_boundary(n, 0))
                break;
        }
        if (at == NONE) {
            h_error(p, "unexpected-end-tag");
            return false;
        }
        implied(p, NULL, false);
        if (!named(h_current(p), name))
            h_error(p, "unexpected-end-tag");
        p->stack.n = at;
        return false;
    }
    if (h_in(name, "a b big code em font i nobr s small strike strong tt u")) {
        adoption(p, name);
        return false;
    }
    if (h_in(name, "applet marquee object")) {
        size_t at = scope(p, name, 0);
        if (at == NONE) {
            h_error(p, "unexpected-end-tag");
            return false;
        }
        implied(p, NULL, false);
        if (!named(h_current(p), name))
            h_error(p, "unexpected-end-tag");
        p->stack.n = at;
        clear_formatting(p);
        return false;
    }
    if (h_eq(name, "br")) {
        h_error(p, "unexpected-end-tag");
        HToken fake = *t;
        fake.type = H_START;
        fake.attrs = NULL;
        fake.self_closing = false;
        return body(p, &fake);
    }
    ordinary_end(p, name);
    return false;
}
static void clear_table_context(os64_html_parser_t *p, const char *boundaries)
{
    while (p->stack.n && !named_in(h_current(p), boundaries)) {
        if (!h_work(p, 1))
            return;
        pop(p);
    }
}
static void close_cell(os64_html_parser_t *p)
{
    implied(p, NULL, false);
    if (!named_in(h_current(p), "td th"))
        h_error(p, "unexpected-end-tag");

    while (p->stack.n) {
        if (!h_work(p, 1))
            return;
        HNode *n = h_current(p);
        pop(p);
        if (named_in(n, "td th"))
            break;
    }
    clear_formatting(p);
    p->mode = M_ROW;
}
static void flush_table(os64_html_parser_t *p)
{
    bool nonwhite = false;

    for (size_t i = 0; i < p->table_text.len; i++)
        if (!h_space((unsigned char)p->table_text.s[i])) {
            nonwhite = true;
            break;
        }
    if (nonwhite)
        h_error(p, "unexpected-table-text");

    bool foster = p->foster;
    p->foster = nonwhite;

    /* The pending buffer contains UTF-8 from already-decoded character tokens. */
    for (size_t i = 0; i < p->table_text.len && !p->d->pub.refusal;) {
        uint32_t c = (unsigned char)p->table_text.s[i++];
        unsigned need = c < 0x80 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;

        if (need)
            c &= (1u << (6 - need)) - 1;

        while (need-- && i < p->table_text.len)
            c = (c << 6) | ((unsigned char)p->table_text.s[i++] & 63);

        if (nonwhite) {
            HToken t = {0};
            t.type = H_CHAR;
            t.ch = c;
            body(p, &t);
        } else
            insert_character(p, c);
    }
    p->foster = foster;
    h_buf_reset(&p->table_text);
}
static bool process(os64_html_parser_t *p, HToken *t, HMode mode)
{
    const char *name = t->name.s;
    size_t at;

    switch (mode) {
    case M_INITIAL:
        if (white(t))
            return false;

        if (t->type == H_COMMENT) {
            insert_comment(p, t, &p->d->root);
            return false;
        }
        if (t->type == H_DOCTYPE) {
            doctype(p, t);
            p->mode = M_BEFORE_HTML;
            return false;
        }
        h_error(p, "missing-doctype");
        p->d->pub.quirks = OS64_HTML_QUIRKS;
        p->mode = M_BEFORE_HTML;
        return true;

    case M_BEFORE_HTML:
        if (t->type == H_DOCTYPE) {
            h_error(p, "unexpected-doctype");
            return false;
        }
        if (t->type == H_COMMENT) {
            insert_comment(p, t, &p->d->root);
            return false;
        }
        if (white(t))
            return false;

        if (t->type == H_END && !h_in(name, "head body html br")) {
            h_error(p, "unexpected-end-tag");
            return false;
        }
        if (!push_open(p, &p->d->html))
            return false;

        h_attach(&p->d->root, NULL, &p->d->html);
        p->mode = M_BEFORE_HEAD;

        if (start(t, "html")) {
            p->d->html.attrs = t->attrs;
            return false;
        }
        return true;

    case M_BEFORE_HEAD:
        if (white(t))
            return false;

        if (t->type == H_COMMENT) {
            insert_comment(p, t, NULL);
            return false;
        }
        if (t->type == H_DOCTYPE) {
            h_error(p, "unexpected-doctype");
            return false;
        }
        if (start(t, "html"))
            return body(p, t);

        if (start(t, "head")) {
            element(p, t, HTML, true);
            p->mode = M_HEAD;
            return false;
        }
        if (t->type == H_END && !h_in(name, "head body html br")) {
            h_error(p, "unexpected-end-tag");
            return false;
        }
        synthetic(p, "head");
        p->mode = M_HEAD;
        return true;

    case M_HEAD:
        if (white(t)) {
            insert_character(p, t->ch);
            return false;
        }
        if (t->type == H_COMMENT) {
            insert_comment(p, t, NULL);
            return false;
        }
        if (t->type == H_DOCTYPE) {
            h_error(p, "unexpected-doctype");
            return false;
        }
        if (start(t, "html"))
            return body(p, t);

        if (t->type == H_START && h_in(name, "base basefont bgsound link meta")) {
            element(p, t, HTML, false);
            t->acknowledged = true;
            return false;
        }
        if (start(t, "title")) {
            raw_element(p, t, T_RCDATA);
            return false;
        }
        if (t->type == H_START && h_in(name, "noframes style")) {
            raw_element(p, t, T_RAWTEXT);
            return false;
        }
        if (start(t, "noscript")) {
            element(p, t, HTML, true);
            p->mode = M_HEAD_NOSCRIPT;
            return false;
        }
        if (start(t, "script")) {
            raw_element(p, t, T_SCRIPT);
            return false;
        }
        if (end(t, "head")) {
            pop(p);
            p->mode = M_AFTER_HEAD;
            return false;
        }
        if (start(t, "template")) {
            if (!element(p, t, HTML, true))
                return false;

            h_nodes_push(p, &p->formatting, NULL);
            p->frameset_ok = false;
            p->mode = M_TEMPLATE;
            push_template(p, M_TEMPLATE);
            return false;
        }
        if (end(t, "template")) {
            if (!template_open(p)) {
                h_error(p, "unexpected-end-tag");
                return false;
            }
            implied(p, NULL, true);
            if (!named(h_current(p), "template"))
                h_error(p, "unexpected-end-tag");

            while (p->stack.n && !p->d->pub.refusal) {
                if (!h_work(p, 1))
                    return false;
                HNode *n = h_current(p);
                pop(p);
                if (named(n, "template"))
                    break;
            }
            clear_formatting(p);
            if (p->templates_n)
                p->templates_n--;
            reset_mode(p);
            return false;
        }
        if (start(t, "head") || (t->type == H_END && !h_in(name, "body html br"))) {
            h_error(p, "unexpected-tag");
            return false;
        }
        pop(p);
        p->mode = M_AFTER_HEAD;
        return true;

    case M_HEAD_NOSCRIPT:
        if (t->type == H_DOCTYPE) {
            h_error(p, "unexpected-doctype");
            return false;
        }
        if (start(t, "html"))
            return body(p, t);

        if (end(t, "noscript")) {
            pop(p);
            p->mode = M_HEAD;
            return false;
        }
        if (white(t) || t->type == H_COMMENT ||
            (t->type == H_START && h_in(name, "basefont bgsound link meta noframes style")))
            return process(p, t, M_HEAD);

        if ((t->type == H_START && h_in(name, "head noscript")) ||
            (t->type == H_END && !h_eq(name, "br"))) {
            h_error(p, "unexpected-tag");
            return false;
        }
        h_error(p, "unexpected-token-in-noscript");
        pop(p);
        p->mode = M_HEAD;
        return true;

    case M_AFTER_HEAD:
        if (white(t)) {
            insert_character(p, t->ch);
            return false;
        }
        if (t->type == H_COMMENT) {
            insert_comment(p, t, NULL);
            return false;
        }
        if (t->type == H_DOCTYPE) {
            h_error(p, "unexpected-doctype");
            return false;
        }
        if (start(t, "html"))
            return body(p, t);

        if (start(t, "body")) {
            element(p, t, HTML, true);
            p->frameset_ok = false;
            p->mode = M_BODY;
            return false;
        }
        if (start(t, "frameset")) {
            element(p, t, HTML, true);
            p->mode = M_FRAMESET;
            return false;
        }
        if (t->type == H_START &&
            h_in(name, "base basefont bgsound link meta noframes script style template title")) {
            h_error(p, "unexpected-start-tag");
            if (!push_open(p, p->head))
                return false;

            bool again = process(p, t, M_HEAD);
            remove_index(p, &p->stack, index_of(p, &p->stack, p->head));
            return again;
        }
        if (end(t, "template"))
            return process(p, t, M_HEAD);

        if (start(t, "head") || (t->type == H_END && !h_in(name, "body html br"))) {
            h_error(p, "unexpected-tag");
            return false;
        }
        synthetic(p, "body");
        p->mode = M_BODY;
        return true;

    case M_BODY:
        return body(p, t);

    case M_TEXT:
        if (t->type == H_CHAR) {
            insert_character(p, t->ch);
            return false;
        }
        if (t->type == H_END_INPUT) {
            h_error(p, "eof-in-text");
            pop(p);
            p->mode = p->original_mode;
            return true;
        }
        pop(p);
        p->mode = p->original_mode;
        return false;

    case M_TABLE:
        if (t->type == H_CHAR && named_in(h_current(p), "table tbody tfoot thead tr")) {
            h_buf_reset(&p->table_text);
            p->original_mode = p->mode;
            p->mode = M_TABLE_TEXT;
            return true;
        }
        if (t->type == H_COMMENT) {
            insert_comment(p, t, NULL);
            return false;
        }
        if (t->type == H_DOCTYPE) {
            h_error(p, "unexpected-doctype");
            return false;
        }
        if (start(t, "caption")) {
            clear_table_context(p, "table template html");
            h_nodes_push(p, &p->formatting, NULL);
            element(p, t, HTML, true);
            p->mode = M_CAPTION;
            return false;
        }
        if (start(t, "colgroup")) {
            clear_table_context(p, "table template html");
            element(p, t, HTML, true);
            p->mode = M_COLGROUP;
            return false;
        }
        if (start(t, "col")) {
            clear_table_context(p, "table template html");
            synthetic(p, "colgroup");
            p->mode = M_COLGROUP;
            return true;
        }
        if (t->type == H_START && h_in(name, "tbody tfoot thead")) {
            clear_table_context(p, "table template html");
            element(p, t, HTML, true);
            p->mode = M_TABLE_BODY;
            return false;
        }
        if (t->type == H_START && h_in(name, "td th tr")) {
            clear_table_context(p, "table template html");
            synthetic(p, "tbody");
            p->mode = M_TABLE_BODY;
            return true;
        }
        if (start(t, "table") || end(t, "table")) {
            if (start(t, "table"))
                h_error(p, "unexpected-start-tag");

            at = scope(p, "table", 3);
            if (at == NONE) {
                if (end(t, "table"))
                    h_error(p, "unexpected-end-tag");
                return false;
            }
            p->stack.n = at;
            reset_mode(p);
            return start(t, "table");
        }
        if (t->type == H_END &&
            h_in(name, "body caption col colgroup html tbody td tfoot th thead tr")) {
            h_error(p, "unexpected-end-tag");
            return false;
        }
        if ((t->type == H_START && h_in(name, "style script template")) || end(t, "template"))
            return process(p, t, M_HEAD);

        if (start(t, "input")) {
            const HAttr *a = t->attrs;
            while (a && !h_eq(a->name, "type")) {
                if (!h_work(p, 1))
                    return false;
                a = a->next;
            }

            if (a && ascii_ci_equal(a->value, "hidden")) {
                h_error(p, "unexpected-start-tag");
                element(p, t, HTML, false);
                t->acknowledged = true;
                return false;
            }
        }
        if (start(t, "form")) {
            h_error(p, "unexpected-start-tag");
            if (p->form || template_open(p))
                return false;
            p->form = element(p, t, HTML, false);
            return false;
        }
        if (t->type == H_END_INPUT)
            return body(p, t);

        h_error(p, "unexpected-token-in-table");
        {
            bool saved = p->foster;
            p->foster = true;
            bool again = body(p, t);
            p->foster = saved;
            return again;
        }
    case M_TABLE_TEXT:
        if (t->type == H_CHAR) {
            if (!t->ch)
                h_error(p, "unexpected-null-character");
            else
                h_buf_put(p, &p->table_text, t->ch);
            return false;
        }
        flush_table(p);
        p->mode = p->original_mode;
        return true;

    case M_CAPTION:
        if (end(t, "caption") ||
            (t->type == H_START && h_in(name, "caption col colgroup tbody td tfoot th thead tr")) ||
            end(t, "table")) {
            at = scope(p, "caption", 3);
            if (at == NONE) {
                h_error(p, "unexpected-tag");
                return false;
            }
            implied(p, NULL, false);
            if (!named(h_current(p), "caption"))
                h_error(p, "unexpected-end-tag");
            p->stack.n = at;
            clear_formatting(p);
            p->mode = M_TABLE;
            return !end(t, "caption");
        }
        if (t->type == H_END && h_in(name, "body col colgroup html tbody td tfoot th thead tr")) {
            h_error(p, "unexpected-end-tag");
            return false;
        }
        return body(p, t);

    case M_COLGROUP:
        if (white(t)) {
            insert_character(p, t->ch);
            return false;
        }
        if (t->type == H_COMMENT) {
            insert_comment(p, t, NULL);
            return false;
        }
        if (t->type == H_DOCTYPE) {
            h_error(p, "unexpected-doctype");
            return false;
        }
        if (start(t, "html"))
            return body(p, t);

        if (start(t, "col")) {
            element(p, t, HTML, false);
            t->acknowledged = true;
            return false;
        }
        if (end(t, "colgroup")) {
            if (!named(h_current(p), "colgroup")) {
                h_error(p, "unexpected-end-tag");
                return false;
            }
            pop(p);
            p->mode = M_TABLE;
            return false;
        }
        if (end(t, "col")) {
            h_error(p, "unexpected-end-tag");
            return false;
        }
        if (start(t, "template") || end(t, "template"))
            return process(p, t, M_HEAD);

        if (t->type == H_END_INPUT)
            return body(p, t);

        if (!named(h_current(p), "colgroup")) {
            h_error(p, "unexpected-tag");
            return false;
        }
        pop(p);
        p->mode = M_TABLE;
        return true;

    case M_TABLE_BODY:
        if (start(t, "tr")) {
            clear_table_context(p, "tbody tfoot thead template html");
            element(p, t, HTML, true);
            p->mode = M_ROW;
            return false;
        }
        if (t->type == H_START && h_in(name, "td th")) {
            h_error(p, "unexpected-start-tag");
            clear_table_context(p, "tbody tfoot thead template html");
            synthetic(p, "tr");
            p->mode = M_ROW;
            return true;
        }
        if (t->type == H_END && h_in(name, "tbody tfoot thead")) {
            if (scope(p, name, 3) == NONE) {
                h_error(p, "unexpected-end-tag");
                return false;
            }
            clear_table_context(p, "tbody tfoot thead template html");
            pop(p);
            p->mode = M_TABLE;
            return false;
        }
        if ((t->type == H_START && h_in(name, "caption col colgroup tbody tfoot thead")) ||
            end(t, "table")) {
            if (scope(p, "tbody", 3) == NONE && scope(p, "thead", 3) == NONE &&
                scope(p, "tfoot", 3) == NONE) {
                h_error(p, "unexpected-tag");
                return false;
            }
            clear_table_context(p, "tbody tfoot thead template html");
            pop(p);
            p->mode = M_TABLE;
            return true;
        }
        if (t->type == H_END && h_in(name, "body caption col colgroup html td th tr")) {
            h_error(p, "unexpected-end-tag");
            return false;
        }
        return process(p, t, M_TABLE);

    case M_ROW:
        if (t->type == H_START && h_in(name, "td th")) {
            clear_table_context(p, "tr template html");
            element(p, t, HTML, true);
            p->mode = M_CELL;
            h_nodes_push(p, &p->formatting, NULL);
            return false;
        }
        if (end(t, "tr") ||
            (t->type == H_START && h_in(name, "caption col colgroup tbody tfoot thead tr")) ||
            end(t, "table")) {
            if (scope(p, "tr", 3) == NONE) {
                h_error(p, "unexpected-tag");
                return false;
            }
            clear_table_context(p, "tr template html");
            pop(p);
            p->mode = M_TABLE_BODY;
            return !end(t, "tr");
        }
        if (t->type == H_END && h_in(name, "tbody tfoot thead")) {
            if (scope(p, name, 3) == NONE) {
                h_error(p, "unexpected-end-tag");
                return false;
            }
            if (scope(p, "tr", 3) == NONE)
                return false;

            clear_table_context(p, "tr template html");
            pop(p);
            p->mode = M_TABLE_BODY;
            return true;
        }
        if (t->type == H_END && h_in(name, "body caption col colgroup html td th")) {
            h_error(p, "unexpected-end-tag");
            return false;
        }
        return process(p, t, M_TABLE);

    case M_CELL:
        if (t->type == H_END && h_in(name, "td th")) {
            if (scope(p, name, 3) == NONE) {
                h_error(p, "unexpected-end-tag");
                return false;
            }
            close_cell(p);
            return false;
        }
        if (t->type == H_START && h_in(name, "caption col colgroup tbody td tfoot th thead tr")) {
            if (scope(p, "td", 3) == NONE && scope(p, "th", 3) == NONE) {
                h_error(p, "unexpected-start-tag");
                return false;
            }
            close_cell(p);
            return true;
        }
        if (t->type == H_END && h_in(name, "body caption col colgroup html")) {
            h_error(p, "unexpected-end-tag");
            return false;
        }
        if (t->type == H_END && h_in(name, "table tbody tfoot thead tr")) {
            if (scope(p, name, 3) == NONE) {
                h_error(p, "unexpected-end-tag");
                return false;
            }
            close_cell(p);
            return true;
        }
        return body(p, t);

    case M_SELECT_TABLE:
        if (t->type == H_START && h_in(name, "caption table tbody tfoot thead tr td th")) {
            h_error(p, "unexpected-start-tag");
            at = scope(p, "select", 4);
            if (at == NONE)
                return false;
            p->stack.n = at;
            reset_mode(p);
            return true;
        }
        if (t->type == H_END && h_in(name, "caption table tbody tfoot thead tr td th")) {
            h_error(p, "unexpected-end-tag");
            if (scope(p, name, 3) == NONE)
                return false;
            at = scope(p, "select", 4);
            if (at == NONE)
                return false;
            p->stack.n = at;
            reset_mode(p);
            return true;
        }
        return process(p, t, M_SELECT);

    case M_SELECT:
        if (t->type == H_CHAR) {
            if (!t->ch)
                h_error(p, "unexpected-null-character");
            else
                insert_character(p, t->ch);
            return false;
        }
        if (t->type == H_COMMENT) {
            insert_comment(p, t, NULL);
            return false;
        }
        if (t->type == H_DOCTYPE) {
            h_error(p, "unexpected-doctype");
            return false;
        }
        if (start(t, "html"))
            return body(p, t);

        if (start(t, "option")) {
            if (named(h_current(p), "option"))
                pop(p);
            element(p, t, HTML, true);
            return false;
        }
        if (start(t, "optgroup")) {
            if (named(h_current(p), "option"))
                pop(p);
            if (named(h_current(p), "optgroup"))
                pop(p);
            element(p, t, HTML, true);
            return false;
        }
        if (start(t, "hr")) {
            if (named(h_current(p), "option"))
                pop(p);
            if (named(h_current(p), "optgroup"))
                pop(p);
            element(p, t, HTML, false);
            t->acknowledged = true;
            return false;
        }
        if (end(t, "optgroup")) {
            if (named(h_current(p), "option") && p->stack.n >= 2 &&
                named(p->stack.v[p->stack.n - 2], "optgroup"))
                pop(p);

            if (named(h_current(p), "optgroup"))
                pop(p);
            else
                h_error(p, "unexpected-end-tag");
            return false;
        }
        if (end(t, "option")) {
            if (named(h_current(p), "option"))
                pop(p);
            else
                h_error(p, "unexpected-end-tag");
            return false;
        }
        if (end(t, "select") || start(t, "select")) {
            if (start(t, "select"))
                h_error(p, "unexpected-start-tag");
            at = scope(p, "select", 4);

            if (at == NONE) {
                h_error(p, "unexpected-end-tag");
                return false;
            }
            p->stack.n = at;
            reset_mode(p);
            return false;
        }
        if (t->type == H_START && h_in(name, "input keygen textarea")) {
            h_error(p, "unexpected-start-tag");
            at = scope(p, "select", 4);
            if (at == NONE)
                return false;
            p->stack.n = at;
            reset_mode(p);
            return true;
        }
        if (start(t, "script") || start(t, "template") || end(t, "template"))
            return process(p, t, M_HEAD);

        if (t->type == H_END_INPUT)
            return body(p, t);

        h_error(p, "unexpected-token-in-select");
        return false;

    case M_TEMPLATE:
        if (t->type == H_CHAR || t->type == H_COMMENT || t->type == H_DOCTYPE)
            return body(p, t);

        if ((t->type == H_START &&
             h_in(name, "base basefont bgsound link meta noframes script style template title")) ||
            end(t, "template"))
            return process(p, t, M_HEAD);

        if (t->type == H_END) {
            h_error(p, "unexpected-end-tag");
            return false;
        }
        if (t->type == H_END_INPUT) {
            if (!template_open(p))
                return false;
            h_error(p, "eof-in-template");

            while (p->stack.n && !p->d->pub.refusal) {
                if (!h_work(p, 1))
                    return false;
                HNode *n = h_current(p);
                pop(p);
                if (named(n, "template"))
                    break;
            }
            clear_formatting(p);
            if (p->templates_n)
                p->templates_n--;
            reset_mode(p);
            return true;
        }
        p->mode = h_in(name, "caption colgroup tbody tfoot thead") ? M_TABLE
                  : h_eq(name, "col")                              ? M_COLGROUP
                  : h_eq(name, "tr")                               ? M_TABLE_BODY
                  : h_in(name, "td th")                            ? M_ROW
                                                                   : M_BODY;

        if (p->templates_n)
            p->templates[p->templates_n - 1] = p->mode;
        return true;

    case M_AFTER_BODY:
        if (white(t))
            return body(p, t);

        if (t->type == H_COMMENT) {
            insert_comment(p, t, &p->d->html);
            return false;
        }
        if (t->type == H_DOCTYPE) {
            h_error(p, "unexpected-doctype");
            return false;
        }
        if (start(t, "html"))
            return body(p, t);

        if (end(t, "html")) {
            p->mode = M_AFTER_AFTER_BODY;
            return false;
        }
        if (t->type == H_END_INPUT)
            return false;

        h_error(p, "unexpected-token-after-body");
        p->mode = M_BODY;
        return true;

    case M_FRAMESET:
        if (white(t)) {
            insert_character(p, t->ch);
            return false;
        }
        if (t->type == H_COMMENT) {
            insert_comment(p, t, NULL);
            return false;
        }
        if (t->type == H_DOCTYPE) {
            h_error(p, "unexpected-doctype");
            return false;
        }
        if (start(t, "html"))
            return body(p, t);

        if (start(t, "frameset")) {
            element(p, t, HTML, true);
            return false;
        }
        if (end(t, "frameset")) {
            if (named(h_current(p), "html")) {
                h_error(p, "unexpected-end-tag");
                return false;
            }
            pop(p);
            if (!named(h_current(p), "frameset"))
                p->mode = M_AFTER_FRAMESET;
            return false;
        }
        if (start(t, "frame")) {
            element(p, t, HTML, false);
            t->acknowledged = true;
            return false;
        }
        if (start(t, "noframes"))
            return process(p, t, M_HEAD);

        if (t->type == H_END_INPUT) {
            if (!named(h_current(p), "html"))
                h_error(p, "eof-in-frameset");
            return false;
        }
        h_error(p, "unexpected-token-in-frameset");
        return false;

    case M_AFTER_FRAMESET:
        if (white(t)) {
            insert_character(p, t->ch);
            return false;
        }
        if (t->type == H_COMMENT) {
            insert_comment(p, t, NULL);
            return false;
        }
        if (t->type == H_DOCTYPE) {
            h_error(p, "unexpected-doctype");
            return false;
        }
        if (start(t, "html"))
            return body(p, t);

        if (end(t, "html")) {
            p->mode = M_AFTER_AFTER_FRAMESET;
            return false;
        }
        if (start(t, "noframes"))
            return process(p, t, M_HEAD);

        if (t->type == H_END_INPUT)
            return false;
        h_error(p, "unexpected-token-after-frameset");
        return false;

    case M_AFTER_AFTER_BODY:
    case M_AFTER_AFTER_FRAMESET:
        if (t->type == H_COMMENT) {
            insert_comment(p, t, &p->d->root);
            return false;
        }
        if (white(t) || t->type == H_DOCTYPE || start(t, "html"))
            return body(p, t);

        if (t->type == H_END_INPUT)
            return false;

        if (mode == M_AFTER_AFTER_FRAMESET) {
            if (start(t, "noframes"))
                return process(p, t, M_HEAD);
            h_error(p, "unexpected-token-after-frameset");
            return false;
        }
        h_error(p, "unexpected-token-after-body");
        p->mode = M_BODY;
        return true;
    }
    return false;
}
static bool math_text(HNode *n)
{
    return n->ns == MATH && h_in(n->name, "mi mo mn ms mtext");
}
static bool integration(os64_html_parser_t *p, HNode *n)
{
    if (n->ns == SVG && h_in(n->name, "foreignObject desc title"))
        return true;

    if (n->ns == MATH && h_eq(n->name, "annotation-xml")) {
        const HAttr *a = n->attrs;
        while (a) {
            if (!h_work(p, 1))
                return false;
            if (h_eq(a->name, "encoding"))
                break;
            a = a->next;
        }
        return a && (ascii_ci_equal(a->value, "text/html") ||
                     ascii_ci_equal(a->value, "application/xhtml+xml"));
    }
    return false;
}
static bool foreign(os64_html_parser_t *p, HToken *t)
{
    if (t->type == H_CHAR) {
        bool was_null = !t->ch;
        if (was_null) {
            h_error(p, "unexpected-null-character");
            t->ch = 0xfffd;
        }
        insert_character(p, t->ch);
        if (!was_null && !h_space(t->ch))
            p->frameset_ok = false;
        return false;
    }
    if (t->type == H_COMMENT) {
        insert_comment(p, t, NULL);
        return false;
    }
    if (t->type == H_DOCTYPE) {
        h_error(p, "unexpected-doctype");
        return false;
    }
    if (t->type == H_START) {
        bool breakout =
            h_in(t->name.s, "b big blockquote body br center code dd div dl dt em embed h1 h2 h3 "
                            "h4 h5 h6 head hr i img li listing menu meta nobr ol p pre ruby s "
                            "small span strong strike sub sup table tt u ul var");

        if (h_eq(t->name.s, "font")) {
            for (HAttr *a = t->attrs; a; a = a->next) {
                if (!h_work(p, 1))
                    return false;
                if (h_in(a->name, "color face size")) {
                    breakout = true;
                    break;
                }
            }
        }
        if (breakout) {
            h_error(p, "html-start-tag-in-foreign-content");

            while (p->stack.n) {
                if (!h_work(p, 1))
                    return false;
                HNode *n = h_current(p);
                if (n->ns == HTML || integration(p, n) || math_text(n))
                    break;
                pop(p);
            }
            return true;
        }
        element(p, t, h_current(p)->ns, !t->self_closing);
        if (t->self_closing)
            t->acknowledged = true;
        return false;
    }
    if (t->type == H_END) {
        if (h_in(t->name.s, "br p")) {
            h_error(p, "html-end-tag-in-foreign-content");
            while (p->stack.n) {
                if (!h_work(p, 1))
                    return false;
                HNode *n = h_current(p);
                if (n->ns == HTML || integration(p, n) || math_text(n))
                    break;
                pop(p);
            }
            return true;
        }
        if (!ascii_ci_equal(h_current(p)->name, t->name.s))
            h_error(p, "unexpected-end-tag");

        for (size_t i = p->stack.n; i; i--) {
            if (!h_work(p, 1 + t->name.len))
                return false;
            HNode *n = p->stack.v[i - 1];

            if (ascii_ci_equal(n->name, t->name.s)) {
                p->stack.n = i - 1;
                return false;
            }
            if (i > 1 && p->stack.v[i - 2]->ns == HTML)
                return process(p, t, p->mode);
        }
    }
    return false;
}
void h_tree(os64_html_parser_t *p, HToken *t)
{
    if (p->ignore_lf) {
        p->ignore_lf = false;
        if (t->type == H_CHAR && t->ch == '\n')
            return;
    }
    bool again = true;

    while (again && !p->d->pub.refusal) {
        if (!h_work(p, 1))
            return;

        HNode *n = h_current(p);

        bool html = !p->stack.n || n->ns == HTML || t->type == H_END_INPUT;

        if (math_text(n) &&
            (t->type == H_CHAR || (t->type == H_START && !h_in(t->name.s, "mglyph malignmark"))))
            html = true;

        if (n->ns == MATH && h_eq(n->name, "annotation-xml") && start(t, "svg"))
            html = true;

        if (integration(p, n) && (t->type == H_CHAR || t->type == H_START))
            html = true;

        again = html ? process(p, t, p->mode) : foreign(p, t);
    }
}
