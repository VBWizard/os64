#include "internal.h"

/* The verbs that change a finished document (html.h, DOM.md). Each validates
 * everything first and allocates everything second, so the links are only
 * touched by code that cannot fail. */

#define ELEMENT OS64_HTML_ELEMENT
#define TEXT OS64_HTML_TEXT
#define COMMENT OS64_HTML_COMMENT
#define FRAGMENT OS64_HTML_FRAGMENT
#define DOCUMENT OS64_HTML_DOCUMENT
#define DOCTYPE OS64_HTML_DOCTYPE

static void say(int64_t *status, int64_t value)
{
    if (status)
        *status = value;
}

/* UTF-8 as the Encoding Standard decodes it, and no NUL: what every string
 * already in the tree is, so that each is also a C string. */
static bool text_ok(const char *s, size_t n)
{
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; i < n;) {
        unsigned char lead = u[i++];
        if (lead == 0)
            return false;
        if (lead < 0x80)
            continue;
        unsigned need = lead >= 0xc2 && lead <= 0xdf   ? 1
                        : lead >= 0xe0 && lead <= 0xef ? 2
                        : lead >= 0xf0 && lead <= 0xf4 ? 3
                                                       : 0;
        if (!need || need > n - i)
            return false;
        for (unsigned k = 0; k < need; k++) {
            unsigned char b = u[i++];
            unsigned lo = k == 0 && lead == 0xe0 ? 0xa0 : k == 0 && lead == 0xf0 ? 0x90 : 0x80;
            unsigned hi = k == 0 && lead == 0xed ? 0x9f : k == 0 && lead == 0xf4 ? 0x8f : 0xbf;
            if (b < lo || b > hi)
                return false;
        }
    }
    return true;
}

/* A template's contents hang off the template and have no parent. The
 * fragment remembers its template, which is what makes "is this node above
 * that one" answerable across the boundary. */
static HNode *host(const HNode *fragment)
{
    return fragment->kind == FRAGMENT ? (HNode *)*h_word(fragment) : NULL;
}
static HNode *above(const HNode *n)
{
    return n->parent ? n->parent : host(n);
}
static HNode *root_of(HNode *n)
{
    while (n->parent)
        n = n->parent;
    return n;
}
/* How far below its root a node is, a template's contents counted as the
 * template's children (h_depth_limit). */
static size_t depth_of(const HNode *n)
{
    size_t depth = 0;
    for (;;) {
        if (n->parent) {
            n = n->parent;
            depth++;
        } else if (host(n))
            n = host(n);
        else
            return depth;
    }
}
/* Preorder over `root`'s subtree, a template's contents before its own
 * children; `depth` follows by the same count as depth_of. No recursion: a
 * document may have been parsed with a `max_depth` no stack could hold. */
static HNode *next_in(HNode *root, HNode *n, size_t *depth)
{
    if (n->template_contents && n->template_contents->first_child) {
        ++*depth;
        return n->template_contents->first_child;
    }
    if (n->first_child) {
        ++*depth;
        return n->first_child;
    }
    while (n != root) {
        if (n->next)
            return n->next;
        HNode *parent = n->parent;
        if (parent != root && host(parent)) {
            /* The contents are done; the template's own children are next,
             * at the depth the contents were. */
            HNode *template = host(parent);
            if (template->first_child)
                return template->first_child;
            n = template;
        } else
            n = parent;
        --*depth;
    }
    return NULL;
}
static size_t height_of(HNode *root)
{
    size_t depth = 0, deepest = 0;
    for (HNode *n = root; n; n = next_in(root, n, &depth))
        if (depth > deepest)
            deepest = depth;
    return deepest;
}

static bool is_html(const HNode *n, os64_html_tag_t tag)
{
    return n->kind == ELEMENT && n->ns == OS64_HTML_NS_HTML && n->tag == tag;
}
static HNode *child_of_kind(const HNode *parent, os64_html_node_kind_t kind, const HNode *except)
{
    for (HNode *c = parent->first_child; c; c = c->next)
        if (c->kind == kind && c != except)
            return c;
    return NULL;
}
static bool kind_follows(const HNode *child, os64_html_node_kind_t kind)
{
    for (const HNode *c = child ? child->next : NULL; c; c = c->next)
        if (c->kind == kind)
            return true;
    return false;
}
static bool kind_precedes(const HNode *child, os64_html_node_kind_t kind)
{
    for (const HNode *c = child ? child->prev : NULL; c; c = c->prev)
        if (c->kind == kind)
            return true;
    return false;
}

/* The DOM Standard's "ensure pre-insertion validity", and with `replacing`
 * its twin inside "replace", where `child` is the node being replaced. Then
 * the two rules that are this library's own: the document's element is its
 * `html` element, and the tree stays as shallow as a parse would keep it. */
static int64_t may_insert(HDoc *d, HNode *parent, HNode *node, HNode *child, bool replacing)
{
    if (!parent || !node || (replacing && !child))
        return OS64_HTML_BAD_ARGUMENT;
    if (parent->kind != DOCUMENT && parent->kind != FRAGMENT && parent->kind != ELEMENT)
        return OS64_HTML_HIERARCHY;
    for (const HNode *a = parent; a; a = above(a))
        if (a == node)
            return OS64_HTML_HIERARCHY;
    if (child && child->parent != parent)
        return OS64_HTML_NOT_FOUND;
    if (node->kind == DOCUMENT)
        return OS64_HTML_HIERARCHY;
    if ((node->kind == TEXT && parent->kind == DOCUMENT) ||
        (node->kind == DOCTYPE && parent->kind != DOCUMENT))
        return OS64_HTML_HIERARCHY;

    const HNode *element = node->kind == ELEMENT ? node : NULL;
    if (parent->kind == DOCUMENT) {
        const HNode *spared = replacing ? child : NULL;
        if (node->kind == FRAGMENT) {
            element = child_of_kind(node, ELEMENT, NULL);
            if (child_of_kind(node, TEXT, NULL) || (element && child_of_kind(node, ELEMENT, element)))
                return OS64_HTML_HIERARCHY;
        }
        if (element) {
            if (child_of_kind(parent, ELEMENT, spared) || kind_follows(child, DOCTYPE) ||
                (!replacing && child && child->kind == DOCTYPE))
                return OS64_HTML_HIERARCHY;
        } else if (node->kind == DOCTYPE) {
            if (child_of_kind(parent, DOCTYPE, spared) || kind_precedes(child, ELEMENT) ||
                (!replacing && !child && child_of_kind(parent, ELEMENT, NULL)))
                return OS64_HTML_HIERARCHY;
        }
    }

    if (parent == &d->root) {
        if (element && !is_html(element, OS64_HTML_TAG_HTML))
            return OS64_HTML_ROOT_REQUIRED;
        /* Replacing the html element with anything that brings no element. */
        if (replacing && child->kind == ELEMENT && !element)
            return OS64_HTML_ROOT_REQUIRED;
    } else if (node->parent == &d->root && node->kind == ELEMENT)
        return OS64_HTML_ROOT_REQUIRED;     /* moving the html element out of the document */

    size_t under = depth_of(parent) + 1, limit = h_depth_limit(d);
    for (HNode *c = node->kind == FRAGMENT ? node->first_child : node; c;
         c = node->kind == FRAGMENT ? c->next : NULL) {
        size_t height = height_of(c);
        if (under > limit || height > limit - under)
            return OS64_HTML_TOO_DEEP;
    }
    return OS64_HTML_OK;
}

/* The landmarks consumers start from, by their definitions: the document's
 * element, and its first `head` and `body` children. */
static void landmarks(HDoc *d)
{
    HNode *html = child_of_kind(&d->root, ELEMENT, NULL);
    if (html)
        d->pub.html = html;
    d->pub.head = d->pub.body = NULL;
    for (HNode *c = d->pub.html->first_child; c; c = c->next) {
        if (!d->pub.head && is_html(c, OS64_HTML_TAG_HEAD))
            d->pub.head = c;
        if (!d->pub.body && is_html(c, OS64_HTML_TAG_BODY))
            d->pub.body = c;
    }
}
static bool is_landmark_parent(const HDoc *d, const HNode *n)
{
    return n == &d->root || n == d->pub.html;
}

/* "Reset the form owner", for the half of a move that takes a node out of
 * its tree: a control and its form that are now in different trees are no
 * longer tied, whichever of the two moved. `moved` is detached already, so
 * it is the root of everything in it. */
static void forget_owner(HDoc *d, HNode *control)
{
    control->form_owner = NULL;
    d->records--;
}
static void split_records(HDoc *d, HNode *moved, HNode *left_behind)
{
    if (!d->records)
        return;
    bool carries_form = false;
    size_t depth = 0;
    for (HNode *n = moved; n; n = next_in(moved, n, &depth)) {
        if (is_html(n, OS64_HTML_TAG_FORM))
            carries_form = true;
        if (n->form_owner && root_of(n->form_owner) != moved)
            forget_owner(d, n);
    }
    if (!carries_form || !d->records)
        return;
    depth = 0;
    for (HNode *n = left_behind; n; n = next_in(left_behind, n, &depth))
        if (n->form_owner && root_of(n->form_owner) == moved)
            forget_owner(d, n);
}
static void take_out(HDoc *d, HNode *node)
{
    if (!node->parent)
        return;
    HNode *left_behind = root_of(node->parent);
    h_detach(node);
    split_records(d, node, left_behind);
}

static void place(HDoc *d, HNode *parent, HNode *node, HNode *before)
{
    bool touched = is_landmark_parent(d, parent) || (node->parent && is_landmark_parent(d, node->parent));
    if (node->kind == FRAGMENT) {
        for (HNode *c; (c = node->first_child) != NULL;) {
            take_out(d, c);
            h_attach(parent, before, c);
        }
    } else {
        take_out(d, node);
        h_attach(parent, before, node);
    }
    if (touched)
        landmarks(d);
}

int64_t os64_html_insert(os64_html_document_t *doc, HNode *parent, HNode *node, HNode *before)
{
    HDoc *d = (HDoc *)doc;
    if (!d)
        return OS64_HTML_BAD_ARGUMENT;
    int64_t verdict = may_insert(d, parent, node, before, false);
    if (verdict)
        return verdict;
    if (before == node)
        before = node->next;
    d->version++;
    place(d, parent, node, before);
    return OS64_HTML_OK;
}

int64_t os64_html_replace(os64_html_document_t *doc, HNode *parent, HNode *node, HNode *old)
{
    HDoc *d = (HDoc *)doc;
    if (!d)
        return OS64_HTML_BAD_ARGUMENT;
    int64_t verdict = may_insert(d, parent, node, old, true);
    if (verdict)
        return verdict;
    if (node == old)
        return OS64_HTML_OK;
    HNode *before = old->next;
    if (before == node)
        before = node->next;
    d->version++;
    bool touched = is_landmark_parent(d, parent);
    take_out(d, old);
    place(d, parent, node, before);
    if (touched)
        landmarks(d);
    return OS64_HTML_OK;
}

int64_t os64_html_remove(os64_html_document_t *doc, HNode *node)
{
    HDoc *d = (HDoc *)doc;
    if (!d || !node)
        return OS64_HTML_BAD_ARGUMENT;
    if (!node->parent)
        return OS64_HTML_OK;
    if (node->parent == &d->root && node->kind == ELEMENT)
        return OS64_HTML_ROOT_REQUIRED;
    bool touched = is_landmark_parent(d, node->parent);
    d->version++;
    take_out(d, node);
    if (touched)
        landmarks(d);
    return OS64_HTML_OK;
}

/* ── New nodes ───────────────────────────────────────────────────────── */

static HNode *node_new(HDoc *d, os64_html_node_kind_t kind, int64_t *why)
{
    HNode *n = d_permanent(d, sizeof(*n) + sizeof(size_t), why);
    if (n) {
        n->kind = kind;
        d->pub.node_count++;
    }
    return n;
}
/* A text buffer on the ledger, so that replacing it later can retire it. */
static char *bytes_new(HDoc *d, const char *s, size_t len, int64_t *why)
{
    if (len > SIZE_MAX - 64) {
        *why = OS64_HTML_ARENA_EXHAUSTED;
        return NULL;
    }
    char *out = d_alloc(d, len + 1, why);
    if (out)
        for (size_t i = 0; i < len; i++)
            out[i] = s[i];
    return out;
}
static HNode *contents_new(HDoc *d, HNode *template, int64_t *why)
{
    HNode *fragment = node_new(d, FRAGMENT, why);
    if (fragment) {
        *h_word(fragment) = (size_t)template;
        template->template_contents = fragment;
    }
    return fragment;
}

os64_html_node_t *os64_html_create_element(os64_html_document_t *doc, os64_html_ns_t ns,
                                           const char *name, int64_t *status)
{
    HDoc *d = (HDoc *)doc;
    int64_t why = OS64_HTML_OK;
    size_t len = h_len(name);
    if (!d || !len || (ns != OS64_HTML_NS_HTML && ns != OS64_HTML_NS_SVG && ns != OS64_HTML_NS_MATHML)) {
        say(status, OS64_HTML_BAD_ARGUMENT);
        return NULL;
    }
    if (!text_ok(name, len)) {
        say(status, OS64_HTML_BAD_TEXT);
        return NULL;
    }
    HNode *n = node_new(d, ELEMENT, &why);
    char *copy = n ? d_permanent(d, len + 1, &why) : NULL;
    if (copy) {
        for (size_t i = 0; i < len; i++)
            copy[i] = name[i];
        n->name = copy;
        n->ns = ns;
        if (ns == OS64_HTML_NS_HTML)
            n->tag = os64_html_tag_from_name(copy);
        if (is_html(n, OS64_HTML_TAG_TEMPLATE) && !contents_new(d, n, &why))
            copy = NULL;
    }
    say(status, why);
    return copy ? n : NULL;
}

static HNode *data_new(os64_html_document_t *doc, os64_html_node_kind_t kind, const char *utf8,
                       size_t len, int64_t *status)
{
    HDoc *d = (HDoc *)doc;
    int64_t why = OS64_HTML_OK;
    if (!d || (!utf8 && len)) {
        say(status, OS64_HTML_BAD_ARGUMENT);
        return NULL;
    }
    if (!text_ok(utf8, len)) {
        say(status, OS64_HTML_BAD_TEXT);
        return NULL;
    }
    char *bytes = bytes_new(d, utf8, len, &why);
    HNode *n = bytes ? node_new(d, kind, &why) : NULL;
    if (n) {
        n->text = bytes;
        n->text_len = len;
        *h_word(n) = len + 1;
    } else
        h_free(d, bytes);
    say(status, why);
    return n;
}
os64_html_node_t *os64_html_create_text(os64_html_document_t *doc, const char *utf8, size_t len,
                                        int64_t *status)
{
    return data_new(doc, TEXT, utf8, len, status);
}
os64_html_node_t *os64_html_create_comment(os64_html_document_t *doc, const char *utf8, size_t len,
                                           int64_t *status)
{
    return data_new(doc, COMMENT, utf8, len, status);
}
os64_html_node_t *os64_html_create_fragment(os64_html_document_t *doc, int64_t *status)
{
    HDoc *d = (HDoc *)doc;
    int64_t why = d ? OS64_HTML_OK : OS64_HTML_BAD_ARGUMENT;
    HNode *n = d ? node_new(d, FRAGMENT, &why) : NULL;
    say(status, why);
    return n;
}

/* ── Attributes ──────────────────────────────────────────────────────────
 *
 * The parser's clones share one list of attribute records (a formatting
 * element reopened in every paragraph is many elements and one list), and
 * those records lie in permanent chunks. So an element's list is copied
 * the first time a verb changes it, each record into a block of its own;
 * from then on a record of that element can be replaced and retired
 * without touching any other element. */

/* A record that owns its strings: the value, then the name, in its block. */
static HAttr *attr_new(HDoc *d, const char *name, const char *value, size_t value_len,
                       const char *ns, int64_t *why)
{
    size_t name_len = h_len(name);
    if (value_len > SIZE_MAX - name_len - sizeof(HAttr) - 64) {
        *why = OS64_HTML_ARENA_EXHAUSTED;
        return NULL;
    }
    HAttr *a = d_alloc(d, sizeof(*a) + value_len + 1 + name_len + 1, why);
    if (!a)
        return NULL;
    char *v = (char *)(a + 1), *n = v + value_len + 1;
    for (size_t i = 0; i < value_len; i++)
        v[i] = value[i];
    for (size_t i = 0; i < name_len; i++)
        n[i] = name[i];
    a->value = v;
    a->name = n;
    a->ns = ns;
    return a;
}
/* Frees a list of blocks nothing has seen yet. */
static void attrs_discard(HDoc *d, HAttr *list)
{
    while (list) {
        HAttr *next = list->next;
        h_free(d, list);
        list = next;
    }
}
/* A list of blocks equal to `from`. With `alias` a copy points at the
 * original's strings, which is right exactly when those lie in permanent
 * chunks and so outlive every record. */
static bool attrs_copy(HDoc *d, const HAttr *from, bool alias, HAttr **out, int64_t *why)
{
    HAttr *head = NULL, **tail = &head;
    for (const HAttr *a = from; a; a = a->next) {
        HAttr *copy;
        if (alias) {
            copy = d_alloc(d, sizeof(*copy), why);
            if (copy)
                *copy = *a;
        } else
            copy = attr_new(d, a->name, a->value, h_len(a->value), a->ns, why);
        if (!copy) {
            attrs_discard(d, head);
            return false;
        }
        copy->next = NULL;
        *tail = copy;
        tail = &copy->next;
    }
    *out = head;
    return true;
}
static int64_t attrs_own(HDoc *d, HNode *e)
{
    if (*h_word(e) & H_ATTRS_PRIVATE)
        return OS64_HTML_OK;
    int64_t why = OS64_HTML_OK;
    HAttr *own = NULL;
    if (!attrs_copy(d, e->attrs, true, &own, &why))
        return why;
    e->attrs = own;
    *h_word(e) |= H_ATTRS_PRIVATE;
    return OS64_HTML_OK;
}
static HAttr **attr_slot(HNode *e, const char *name)
{
    HAttr **slot = &e->attrs;
    while (*slot && !h_eq((*slot)->name, name))
        slot = &(*slot)->next;
    return slot;
}
/* A listed control was given a `form` attribute, or a new value for one:
 * its record is void. Taking the attribute away needs no such step, since a
 * control that has the attribute has no record: the parser makes none for
 * it, and giving it the attribute here clears one. */
static void form_attr_set(HDoc *d, HNode *e, const char *name)
{
    if (e->form_owner && h_eq(name, "form") && e->ns == OS64_HTML_NS_HTML &&
        h_in(e->name, "button fieldset input object output select textarea"))
        forget_owner(d, e);
}

int64_t os64_html_set_attr(os64_html_document_t *doc, HNode *e, const char *name,
                           const char *value, size_t value_len)
{
    HDoc *d = (HDoc *)doc;
    if (!d || !e || e->kind != ELEMENT || !name || !*name || (!value && value_len))
        return OS64_HTML_BAD_ARGUMENT;
    if (!text_ok(name, h_len(name)) || !text_ok(value, value_len))
        return OS64_HTML_BAD_TEXT;
    /* The new record first, so that a failure after it has one block to give
     * back and a failure before it has none. */
    int64_t why = OS64_HTML_OK;
    const HAttr *existing = *attr_slot(e, name);
    HAttr *fresh = attr_new(d, name, value, value_len, existing ? existing->ns : NULL, &why);
    if (!fresh)
        return why;
    why = attrs_own(d, e);
    if (why) {
        h_free(d, fresh);
        return why;
    }
    HAttr **slot = attr_slot(e, name), *old = *slot;
    d->version++;
    fresh->next = old ? old->next : NULL;
    *slot = fresh;
    d_retire(d, old);
    form_attr_set(d, e, name);
    return OS64_HTML_OK;
}

int64_t os64_html_remove_attr(os64_html_document_t *doc, HNode *e, const char *name)
{
    HDoc *d = (HDoc *)doc;
    if (!d || !e || e->kind != ELEMENT || !name)
        return OS64_HTML_BAD_ARGUMENT;
    if (!*attr_slot(e, name))
        return OS64_HTML_OK;
    int64_t why = attrs_own(d, e);
    if (why)
        return why;
    HAttr **slot = attr_slot(e, name), *old = *slot;
    d->version++;
    *slot = old->next;
    d_retire(d, old);
    return OS64_HTML_OK;
}

int64_t os64_html_set_text(os64_html_document_t *doc, HNode *n, const char *utf8, size_t len)
{
    HDoc *d = (HDoc *)doc;
    if (!d || !n || (n->kind != TEXT && n->kind != COMMENT) || (!utf8 && len))
        return OS64_HTML_BAD_ARGUMENT;
    if (!text_ok(utf8, len))
        return OS64_HTML_BAD_TEXT;
    int64_t why = OS64_HTML_OK;
    char *bytes = bytes_new(d, utf8, len, &why);
    if (!bytes)
        return why;
    /* A buffer with no capacity recorded is a literal or lies in a permanent
     * chunk: it is left where it is and goes with the document. */
    char *old = *h_word(n) ? (char *)n->text : NULL;
    d->version++;
    n->text = bytes;
    n->text_len = len;
    *h_word(n) = len + 1;
    d_retire(d, old);
    return OS64_HTML_OK;
}

/* ── Clone ───────────────────────────────────────────────────────────── */

static HNode *clone_one(HDoc *d, const HNode *from, int64_t *why)
{
    if (from->kind == TEXT || from->kind == COMMENT) {
        char *bytes = bytes_new(d, from->text, from->text_len, why);
        HNode *n = bytes ? node_new(d, from->kind, why) : NULL;
        if (!n) {
            h_free(d, bytes);
            return NULL;
        }
        n->text = bytes;
        n->text_len = from->text_len;
        *h_word(n) = from->text_len + 1;
        return n;
    }
    HNode *n = node_new(d, from->kind, why);
    if (!n)
        return NULL;
    /* Names and a doctype's identifiers are never replaced, so are shared. */
    n->ns = from->ns;
    n->tag = from->tag;
    n->name = from->name;
    n->public_id = from->public_id;
    n->system_id = from->system_id;
    if (from->kind == ELEMENT) {
        if (*h_word(from) & H_ATTRS_PRIVATE) {
            if (!attrs_copy(d, from->attrs, false, &n->attrs, why))
                return NULL;
            *h_word(n) |= H_ATTRS_PRIVATE;
        } else
            n->attrs = from->attrs;
        if (from->template_contents && !contents_new(d, n, why))
            return NULL;
    }
    return n;
}

os64_html_node_t *os64_html_clone(os64_html_document_t *doc, const os64_html_node_t *node,
                                  bool deep, int64_t *status)
{
    HDoc *d = (HDoc *)doc;
    int64_t why = OS64_HTML_OK;
    if (!d || !node || node->kind == DOCUMENT) {
        say(status, OS64_HTML_BAD_ARGUMENT);
        return NULL;
    }
    HNode *root = (HNode *)node, *copy = clone_one(d, root, &why);
    size_t depth = 0;
    /* `at` and `mirror` walk the original and the copy in step: the copy of
     * a node's parent is found by climbing both until the original's side
     * reaches that parent, or the template whose contents it is. */
    HNode *at = root, *mirror = copy;
    for (HNode *n = deep && copy ? next_in(root, root, &depth) : NULL; n;
         n = next_in(root, n, &depth)) {
        HNode *made = clone_one(d, n, &why);
        if (!made) {
            copy = NULL;
            break;
        }
        while (at != n->parent && at->template_contents != n->parent) {
            HNode *up = at->parent ? at->parent : host(at);
            mirror = mirror->parent ? mirror->parent : host(mirror);
            at = up;
        }
        h_attach(at == n->parent ? mirror : mirror->template_contents, NULL, made);
        at = n;
        mirror = made;
    }
    say(status, why);
    return copy;
}
