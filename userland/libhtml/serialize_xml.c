/* XML serialization (DOM Parsing and Serialization § 3.2.1, "produce an XML
 * serialization" with require well-formed false): what XMLSerializer's
 * serializeToString makes of a node and its subtree.
 *
 * It differs from the HTML serializer beside it in what a namespace costs.
 * Every element names its namespace where it differs from the one it
 * inherits (an HTML element at the top says xmlns="http://www.w3.org/1999/xhtml"),
 * a namespaced attribute takes the latest prefix declared for its
 * namespace that no nearer declaration has rebound, else its own prefix
 * when the map binds that prefix to nothing, else a generated "ns<N>" that
 * spells no prefix in force, and the namespace prefix map is scoped
 * to the element that declares into it: its descendants see the
 * declaration, its siblings do not. An empty element is written
 * self-closing ("<b/>"), save an HTML element that is not void
 * ("<div></div>") and a void one with a space ("<br />"); text escapes
 * only &, < and >.
 *
 * Where the standard's prose and the browsers part, this follows the
 * browsers, as WPT's XMLSerializer-serializeToString.html records them: an
 * attribute named xmlns in no namespace is read as the default namespace
 * declaration it looks like, and an attribute value escapes tab, line feed
 * and carriage return so it reads back whole. WPT is of two minds about an
 * attribute's own prefix: it keeps xl:type's on a fresh element and
 * generates ns1 for an unbound p:name under another declaration. The rule
 * above keeps it, which is what lets an SVG's xlink:href come back through
 * the HTML parser as xlink:href rather than as a plain "ns1:href". And it
 * drops a root's xmlns="" when its own namespace is none, where WPT's
 * "prefix bound to an empty namespace URI" case keeps it. Two WPT cases
 * record the standard's prose writing output that reads back differently
 * (a prefix a descendant rebound still offered, a generated ns1 declared
 * beside an element's own xmlns:ns1); this does what Chromium does
 * instead, which round-trips.
 *
 * The walk keeps no recursion: a stack of open elements, each with the
 * namespace its children inherit and how much of the map it owns, and the
 * map itself as one array of declarations in the order they were made,
 * truncated as each element closes. */

#include "internal.h"

static const char kXHTML[] = "http://www.w3.org/1999/xhtml";
static const char kSVG[] = "http://www.w3.org/2000/svg";
static const char kMathML[] = "http://www.w3.org/1998/Math/MathML";
static const char kXMLNS[] = "http://www.w3.org/2000/xmlns/";
static const char kXMLNamespace[] = "http://www.w3.org/XML/1998/namespace";

/* One declaration in the namespace prefix map: `ns` (NULL for none) bound
 * to a prefix that is either the node's own spelling or a generated
 * "ns<generated>". `local` marks a declaration the element being written
 * made with its own xmlns:* attributes (the standard's "local prefixes
 * map"). */
typedef struct {
    const char *ns;
    const char *prefix;
    size_t prefix_len;
    uint32_t generated;
    bool local;
} XDecl;

/* An open element: its node, the namespace its children inherit, the map's
 * length before it declared anything, and the prefix its end tag repeats. */
typedef struct {
    const HNode *node;
    const char *inherited;
    size_t map_mark;
    const char *prefix;
    size_t prefix_len;
    uint32_t generated;
} XOpen;

typedef struct {
    char *out;
    size_t cap, length;
    XDecl *map;
    size_t count, map_cap;
    XOpen *open;
    size_t depth, open_cap;
    uint32_t next_index;
    bool failed;
} XWriter;

static void x_bytes(XWriter *w, const char *s, size_t len)
{
    size_t room = w->cap && w->length < w->cap - 1 ? w->cap - 1 - w->length : 0;
    size_t copy = len < room ? len : room;
    for (size_t i = 0; i < copy; i++)
        w->out[w->length + i] = s[i];
    w->length = len > SIZE_MAX - w->length ? SIZE_MAX : w->length + len;
}

static void x_literal(XWriter *w, const char *s)
{
    if (s)
        x_bytes(w, s, h_len(s));
}

static void x_number(XWriter *w, uint32_t n)
{
    char digits[10];
    size_t at = sizeof(digits);
    do {
        digits[--at] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    x_bytes(w, digits + at, sizeof(digits) - at);
}

/* Text's escapes, or with `attribute` an attribute value's ("serializing an
 * attribute value"): & < > always, and in a value " and the three
 * whitespace characters a parser would otherwise normalize away. */
static void x_escaped(XWriter *w, const char *s, size_t len, bool attribute)
{
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        const char *with = NULL;
        switch (s[i]) {
        case '&': with = "&amp;"; break;
        case '<': with = "&lt;"; break;
        case '>': with = "&gt;"; break;
        case '"': with = attribute ? "&quot;" : NULL; break;
        case '\t': with = attribute ? "&#9;" : NULL; break;
        case '\n': with = attribute ? "&#10;" : NULL; break;
        case '\r': with = attribute ? "&#13;" : NULL; break;
        default: break;
        }
        if (with) {
            x_bytes(w, s + start, i - start);
            x_literal(w, with);
            start = i + 1;
        }
    }
    if (len > start)
        x_bytes(w, s + start, len - start);
}

static void x_value(XWriter *w, const char *s)
{
    if (s)
        x_escaped(w, s, h_len(s), true);
}

/* An element's namespace URI, NULL for none. */
static const char *element_ns(const HNode *n)
{
    switch (n->ns) {
    case OS64_HTML_NS_HTML: return kXHTML;
    case OS64_HTML_NS_SVG: return kSVG;
    case OS64_HTML_NS_MATHML: return kMathML;
    case OS64_HTML_NS_NONE: return NULL;
    case OS64_HTML_NS_OTHER: return n->namespace_uri;
    }
    return NULL;
}

static bool void_element(const HNode *n)
{
    return n->ns == OS64_HTML_NS_HTML &&
           h_in(n->name, "area base basefont bgsound br col embed frame hr img input "
                         "keygen link meta param source track wbr");
}

/* The children an element is written with: a template's are its contents'. */
static const HNode *x_children(const HNode *n)
{
    if (n->kind == OS64_HTML_ELEMENT && n->ns == OS64_HTML_NS_HTML && h_eq(n->name, "template") &&
        n->template_contents)
        return n->template_contents->first_child;
    return n->first_child;
}

/* An attribute's prefix and local name. A record keeps the qualified
 * spelling; one in a namespace splits at its colon, one in none is its
 * whole name. */
static void attr_parts(const HAttr *a, const char **prefix, size_t *prefix_len, const char **local)
{
    *prefix = NULL;
    *prefix_len = 0;
    *local = a->name;
    if (!a->ns)
        return;
    for (const char *c = a->name; *c; c++)
        if (*c == ':') {
            *prefix = a->name;
            *prefix_len = (size_t)(c - a->name);
            *local = c + 1;
            return;
        }
}

/* "ns<n>" into `out` (room for 12), its length. */
static size_t generated_spelling(uint32_t n, char *out)
{
    char digits[10];
    size_t at = sizeof(digits), length = 2;
    do {
        digits[--at] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    out[0] = 'n';
    out[1] = 's';
    for (size_t i = at; i < sizeof(digits); i++)
        out[length++] = digits[i];
    return length;
}

/* Whether a declaration's prefix is spelled `prefix`: a generated one is
 * "ns<n>", so an element's own `ns1` and a generated ns1 are one prefix. */
static bool same_prefix(const XDecl *d, const char *prefix, size_t len)
{
    if (!prefix)
        return false;
    char spelling[12];
    const char *have = d->prefix;
    size_t have_len = d->prefix_len;
    if (d->generated) {
        have_len = generated_spelling(d->generated, spelling);
        have = spelling;
    }
    if (have_len != len)
        return false;
    for (size_t i = 0; i < len; i++)
        if (have[i] != prefix[i])
            return false;
    return true;
}

static bool same_spelling(const XDecl *a, const XDecl *b)
{
    if (a->generated && b->generated)
        return a->generated == b->generated;
    return a->generated ? same_prefix(a, b->prefix, b->prefix_len) : same_prefix(b, a->prefix, a->prefix_len);
}

/* The latest declaration of a prefix spelled `prefix`: what the prefix
 * means where the writer is, since a later one shadows an earlier; -1 for
 * an unbound prefix. */
static ptrdiff_t x_binding(const XWriter *w, const char *prefix, size_t len)
{
    for (size_t i = w->count; i-- > 0;)
        if (same_prefix(&w->map[i], prefix, len))
            return (ptrdiff_t)i;
    return -1;
}

/* Whether declaration `i` is still in force: no later one rebinds its
 * prefix. */
static bool x_in_force(const XWriter *w, size_t i)
{
    for (size_t j = w->count; --j > i;)
        if (same_spelling(&w->map[j], &w->map[i]))
            return false;
    return true;
}

static bool x_declare(XWriter *w, const char *ns, const char *prefix, size_t prefix_len, uint32_t generated,
                      bool local)
{
    if (w->count == w->map_cap) {
        size_t cap = w->map_cap ? w->map_cap * 2 : 8;
        XDecl *grown = os64_realloc(w->map, cap * sizeof(*grown));
        if (!grown) {
            w->failed = true;
            return false;
        }
        w->map = grown;
        w->map_cap = cap;
    }
    w->map[w->count++] = (XDecl){ns, prefix, prefix_len, generated, local};
    return true;
}

/* "Retrieve a preferred prefix string", as the browsers do it: `preferred`
 * when it is bound to `ns` where the writer is, else the latest declaration
 * for `ns` still in force; -1 when there is none. A declaration a
 * descendant shadows is not offered, or what it writes would read back in
 * the shadowing namespace (it is in force again once that element closes
 * and truncates the map). */
static ptrdiff_t x_preferred(const XWriter *w, const char *preferred, size_t preferred_len, const char *ns)
{
    ptrdiff_t bound = preferred ? x_binding(w, preferred, preferred_len) : -1;
    if (bound >= 0 && h_eq(w->map[bound].ns, ns))
        return bound;
    for (size_t i = w->count; i-- > 0;)
        if (h_eq(w->map[i].ns, ns) && x_in_force(w, i))
            return (ptrdiff_t)i;
    return -1;
}

/* "Generate a prefix": the next ns<N> no declaration in force spells, for
 * `ns`, declared; its index in the map, or -1 out of memory. Skipping a
 * spelling already bound is the browsers' answer to a collision (an
 * element's own xmlns:ns1) that the standard's prose would write twice. */
static ptrdiff_t x_generate(XWriter *w, const char *ns)
{
    char spelling[12];
    uint32_t n;
    do {
        n = w->next_index++;
    } while (x_binding(w, spelling, generated_spelling(n, spelling)) >= 0);
    return x_declare(w, ns, NULL, 0, n, false) ? (ptrdiff_t)w->count - 1 : -1;
}

static void x_prefix(XWriter *w, const char *prefix, size_t len, uint32_t generated)
{
    if (generated) {
        x_literal(w, "ns");
        x_number(w, generated);
    } else {
        x_bytes(w, prefix, len);
    }
}

static void x_decl_prefix(XWriter *w, const XDecl *d)
{
    x_prefix(w, d->prefix, d->prefix_len, d->generated);
}

/* Whether an attribute declares the default namespace: xmlns in the XMLNS
 * namespace, or (as the browsers read it) an attribute named xmlns in none. */
static bool default_declaration(const HAttr *a)
{
    return h_eq(a->name, "xmlns") && (!a->ns || h_eq(a->ns, kXMLNS));
}

/* Whether the map binds `prefix` to any namespace. */
static bool x_bound(const XWriter *w, const char *prefix, size_t len)
{
    for (size_t i = 0; i < w->count; i++)
        if (same_prefix(&w->map[i], prefix, len))
            return true;
    return false;
}

/* Whether `prefix` is bound to `ns` where the writer is: its latest
 * declaration, not any earlier one a nearer element shadows, so a
 * declaration that restores a shadowed binding is recorded and written. */
static bool x_holds(const XWriter *w, const char *ns, const char *prefix, size_t len)
{
    ptrdiff_t bound = x_binding(w, prefix, len);
    return bound >= 0 && h_eq(w->map[bound].ns, ns);
}

/* Whether the element's own xmlns:* declarations (the map from `from` on,
 * marked local) bind `prefix`; its namespace in *ns. */
static bool local_prefix(const XWriter *w, size_t from, const char *prefix, size_t len, const char **ns)
{
    for (size_t i = from; i < w->count; i++)
        if (w->map[i].local && same_prefix(&w->map[i], prefix, len)) {
            if (ns)
                *ns = w->map[i].ns;
            return true;
        }
    return false;
}

/* "Recording the namespace information": the element's xmlns:* attributes
 * join the map, marked local, save one binding the XML namespace and one
 * the map already holds; answers whether it declares the default
 * namespace, its value in *value. */
static bool x_record(XWriter *w, const HNode *n, const char **value)
{
    bool declared = false;
    *value = NULL;
    for (const HAttr *a = n->attrs; a; a = a->next) {
        if (default_declaration(a)) {
            declared = true;
            *value = a->value ? a->value : "";
            continue;
        }
        const char *prefix, *local;
        size_t prefix_len;
        attr_parts(a, &prefix, &prefix_len, &local);
        if (!h_eq(a->ns, kXMLNS) || !prefix)
            continue;
        const char *ns = a->value && *a->value ? a->value : NULL;
        if (h_eq(ns, kXMLNamespace))
            continue;
        if (!x_holds(w, ns, local, h_len(local)) && !x_declare(w, ns, local, h_len(local), 0, true))
            return false;
    }
    return declared;
}

/* The element's attributes ("XML serialization of the attributes"), after
 * its name. `from` is where its own declarations begin in the map;
 * `ignore_default` drops its default namespace declaration, which the
 * start tag has already accounted for. */
static void x_attributes(XWriter *w, const HNode *n, size_t from, bool ignore_default)
{
    for (const HAttr *a = n->attrs; a && !w->failed; a = a->next) {
        const char *prefix, *local;
        size_t prefix_len;
        attr_parts(a, &prefix, &prefix_len, &local);
        if (default_declaration(a) && ignore_default)
            continue;
        ptrdiff_t candidate = -1;
        bool own_prefix = false;
        if (a->ns) {
            candidate = x_preferred(w, prefix, prefix_len, a->ns);
            if (h_eq(a->ns, kXMLNS)) {
                /* A declaration of the XML namespace is not written, nor is
                 * a prefixed one the map already holds as written when it
                 * is not this element's own (an ancestor made it) or its
                 * own binds the prefix differently; a prefixed declaration
                 * keeps its own prefix, xmlns. */
                if (h_eq(a->value, kXMLNamespace))
                    continue;
                if (prefix) {
                    const char *bound = NULL;
                    size_t local_len = h_len(local);
                    bool own = local_prefix(w, from, local, local_len, &bound);
                    if ((!own || !h_eq(bound, a->value)) && x_holds(w, a->value, local, local_len))
                        continue;
                    own_prefix = true;
                }
            } else if (candidate < 0) {
                /* Its own prefix, declared here, unless the map binds that
                 * prefix already; a generated one otherwise. */
                candidate = prefix && !x_bound(w, prefix, prefix_len)
                                ? (x_declare(w, a->ns, prefix, prefix_len, 0, false) ? (ptrdiff_t)w->count - 1 : -1)
                                : x_generate(w, a->ns);
                if (candidate < 0)
                    return;
                x_literal(w, " xmlns:");
                x_decl_prefix(w, &w->map[candidate]);
                x_literal(w, "=\"");
                x_value(w, a->ns);
                x_literal(w, "\"");
            }
        }
        x_literal(w, " ");
        if (own_prefix) {
            x_bytes(w, prefix, prefix_len);
            x_literal(w, ":");
        } else if (candidate >= 0) {
            x_decl_prefix(w, &w->map[candidate]);
            x_literal(w, ":");
        }
        x_literal(w, local);
        x_literal(w, "=\"");
        x_value(w, a->value);
        x_literal(w, "\"");
    }
}

static bool x_push(XWriter *w, XOpen open)
{
    if (w->depth == w->open_cap) {
        size_t cap = w->open_cap ? w->open_cap * 2 : 16;
        XOpen *grown = os64_realloc(w->open, cap * sizeof(*grown));
        if (!grown) {
            w->failed = true;
            return false;
        }
        w->open = grown;
        w->open_cap = cap;
    }
    w->open[w->depth++] = open;
    return true;
}

static void x_name(XWriter *w, const char *prefix, size_t prefix_len, uint32_t generated, const char *local)
{
    if (prefix || generated) {
        x_prefix(w, prefix, prefix_len, generated);
        x_literal(w, ":");
    }
    x_literal(w, local);
}

/* An element's start tag, given the namespace it inherits. Answers whether
 * it is left open (it has children); an empty one is closed here and its
 * declarations go with it. */
static bool x_start(XWriter *w, const HNode *n, const char *inherited)
{
    size_t mark = w->count;
    const char *default_value;
    bool declares_default = x_record(w, n, &default_value);
    if (w->failed)
        return false;
    const char *ns = element_ns(n);
    bool ignore_default = false;
    const char *qprefix = NULL;
    size_t qprefix_len = 0;
    uint32_t qgenerated = 0;
    x_literal(w, "<");
    if (h_eq(inherited, ns)) {
        ignore_default = declares_default;
        if (h_eq(ns, kXMLNamespace)) {
            qprefix = "xml";
            qprefix_len = 3;
        }
        x_name(w, qprefix, qprefix_len, 0, n->name);
    } else {
        const char *prefix = n->prefix;
        size_t prefix_len = prefix ? h_len(prefix) : 0;
        ptrdiff_t candidate = x_preferred(w, prefix, prefix_len, ns);
        if (prefix && h_eq(prefix, "xmlns")) {
            candidate = -1;
            qprefix = prefix;
            qprefix_len = prefix_len;
            x_name(w, qprefix, qprefix_len, 0, n->name);
            if (declares_default && !h_eq(default_value, kXMLNamespace))
                inherited = *default_value ? default_value : NULL;
        } else if (candidate >= 0) {
            qprefix = w->map[candidate].prefix;
            qprefix_len = w->map[candidate].prefix_len;
            qgenerated = w->map[candidate].generated;
            x_name(w, qprefix, qprefix_len, qgenerated, n->name);
            if (declares_default && !h_eq(default_value, kXMLNamespace))
                inherited = *default_value ? default_value : NULL;
        } else if (prefix) {
            /* The element's own prefix, declared here, unless its own
             * attributes bind that prefix to something else. */
            ptrdiff_t made = local_prefix(w, mark, prefix, prefix_len, NULL)
                                 ? x_generate(w, ns)
                                 : (x_declare(w, ns, prefix, prefix_len, 0, false) ? (ptrdiff_t)w->count - 1 : -1);
            if (made < 0)
                return false;
            qprefix = w->map[made].prefix;
            qprefix_len = w->map[made].prefix_len;
            qgenerated = w->map[made].generated;
            x_name(w, qprefix, qprefix_len, qgenerated, n->name);
            x_literal(w, " xmlns:");
            x_decl_prefix(w, &w->map[made]);
            x_literal(w, "=\"");
            x_value(w, ns);
            x_literal(w, "\"");
            if (declares_default)
                inherited = *default_value ? default_value : NULL;
        } else if (!declares_default || !h_eq(default_value, ns)) {
            ignore_default = true;
            inherited = ns;
            x_literal(w, n->name);
            x_literal(w, " xmlns=\"");
            x_value(w, ns);
            x_literal(w, "\"");
        } else {
            inherited = ns;
            x_literal(w, n->name);
        }
    }
    x_attributes(w, n, mark, ignore_default);
    if (w->failed)
        return false;
    if (!x_children(n)) {
        if (n->ns == OS64_HTML_NS_HTML) {
            if (void_element(n)) {
                x_literal(w, " />");
            } else {
                x_literal(w, "></");
                x_name(w, qprefix, qprefix_len, qgenerated, n->name);
                x_literal(w, ">");
            }
        } else {
            x_literal(w, "/>");
        }
        w->count = mark;
        return false;
    }
    x_literal(w, ">");
    return x_push(w, (XOpen){n, inherited, mark, qprefix, qprefix_len, qgenerated});
}

static void x_end(XWriter *w, const XOpen *open)
{
    x_literal(w, "</");
    x_name(w, open->prefix, open->prefix_len, open->generated, open->node->name);
    x_literal(w, ">");
    w->count = open->map_mark;
}

/* A node that is not an element nor a container of children. */
static void x_leaf(XWriter *w, const HNode *n)
{
    switch (n->kind) {
    case OS64_HTML_TEXT:
        if (n->cdata) {
            x_literal(w, "<![CDATA[");
            x_bytes(w, n->text, n->text_len);
            x_literal(w, "]]>");
        } else {
            x_escaped(w, n->text, n->text_len, false);
        }
        return;
    case OS64_HTML_COMMENT:
        x_literal(w, "<!--");
        x_bytes(w, n->text, n->text_len);
        x_literal(w, "-->");
        return;
    case OS64_HTML_PI:
        x_literal(w, "<?");
        x_literal(w, n->name);
        x_literal(w, " ");
        x_bytes(w, n->text, n->text_len);
        x_literal(w, "?>");
        return;
    case OS64_HTML_DOCTYPE:
        x_literal(w, "<!DOCTYPE ");
        x_literal(w, n->name);
        if (n->public_id && *n->public_id) {
            x_literal(w, " PUBLIC \"");
            x_literal(w, n->public_id);
            x_literal(w, "\"");
        }
        if (n->system_id && *n->system_id) {
            if (!(n->public_id && *n->public_id))
                x_literal(w, " SYSTEM");
            x_literal(w, " \"");
            x_literal(w, n->system_id);
            x_literal(w, "\"");
        }
        x_literal(w, ">");
        return;
    default:
        return;
    }
}

int64_t os64_html_serialize_xml(const os64_html_node_t *node, char *out, size_t cap)
{
    XWriter w = {out, out ? cap : 0, 0, NULL, 0, 0, NULL, 0, 0, 1, false};
    if (!x_declare(&w, kXMLNamespace, "xml", 3, 0, false))
        return OS64_HTML_NO_MEMORY;
    const HNode *root = node;
    /* A document or a fragment is its children, each inheriting no
     * namespace; anything else is the node itself. */
    const HNode *at = NULL;
    if (root && (root->kind == OS64_HTML_DOCUMENT || root->kind == OS64_HTML_FRAGMENT))
        at = root->first_child;
    else if (root)
        at = root;
    while (at && !w.failed) {
        const char *inherited = w.depth ? w.open[w.depth - 1].inherited : NULL;
        if (at->kind == OS64_HTML_ELEMENT) {
            if (x_start(&w, at, inherited)) {
                at = x_children(at);
                continue;
            }
        } else {
            x_leaf(&w, at);
        }
        /* The next node: a sibling, else up, closing what is finished. The
         * root itself has no siblings to visit, and a container root stops
         * at its own children. */
        while (at && !w.failed) {
            if (at == root)
                at = NULL;
            else if (at->next)
                at = at->next;
            else if (w.depth) {
                XOpen open = w.open[--w.depth];
                x_end(&w, &open);
                at = open.node;
                continue;
            } else {
                at = NULL;
            }
            break;
        }
    }
    os64_free(w.map);
    os64_free(w.open);
    if (w.failed)
        return OS64_HTML_NO_MEMORY;
    if (w.cap)
        w.out[w.length < w.cap ? w.length : w.cap - 1] = 0;
    return w.length > (size_t)INT64_MAX ? INT64_MAX : (int64_t)w.length;
}
