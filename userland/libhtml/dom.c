#include "internal.h"

/* The verbs that change a document (html.h, DOM.md). Each validates
 * everything first and allocates everything second, so the links are only
 * touched by code that cannot fail. */

#define ELEMENT OS64_HTML_ELEMENT
#define TEXT OS64_HTML_TEXT
#define COMMENT OS64_HTML_COMMENT
#define PI OS64_HTML_PI
#define FRAGMENT OS64_HTML_FRAGMENT
#define DOCUMENT OS64_HTML_DOCUMENT
#define DOCTYPE OS64_HTML_DOCTYPE

static void say(int64_t *status, int64_t value)
{
    if (status)
        *status = value;
}

/* The parser charges for the walks it asks of this file (internal.h,
 * d_place); a verb's own are not counted, and it passes no counter. */
static void step(uint64_t *steps)
{
    if (steps)
        ++*steps;
}

/* UTF-8 as the Encoding Standard decodes it, and no NUL: what every string
 * already in the tree is, so that each is also a C string. */
bool d_text_ok(const char *s, size_t n)
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

/* Whether the C string `held` is exactly the `n` bytes at `s`, which hold no
 * NUL (d_text_ok). A held string that is NULL is the empty one. */
static bool same_bytes(const char *held, const char *s, size_t n)
{
    if (!held)
        return n == 0;
    for (size_t i = 0; i < n; i++)
        if (held[i] != s[i])
            return false;
    return held[n] == 0;
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
static HNode *root_of(HNode *n, uint64_t *steps)
{
    while (n->parent) {
        n = n->parent;
        step(steps);
    }
    return n;
}
/* True of no node, and of a node in this document's arena (HDoc.id). */
static bool ours(const HDoc *d, const HNode *n)
{
    return !n || n->document_id == d->id;
}
/* Preorder over the tree `root` is the root of, as the DOM counts a tree: a
 * template's contents are no part of it, being a tree of their own. */
static HNode *next_in_tree(HNode *root, HNode *n)
{
    if (n->first_child)
        return n->first_child;
    for (; n != root; n = n->parent)
        if (n->next)
            return n->next;
    return NULL;
}
/* How far below its root a node is, a template's contents counted as the
 * template's children (h_depth_limit). */
static size_t depth_of(const HNode *n, uint64_t *steps)
{
    size_t depth = 0;
    for (;;) {
        step(steps);
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
/* Lifetime walks include the contents fragment itself, since callers can
 * hold that identity independently of its template and its children. */
static HNode *lifetime_next(HNode *root, HNode *n)
{
    if (n->template_contents)
        return n->template_contents;
    if (n->first_child)
        return n->first_child;
    while (n != root) {
        if (n->next)
            return n->next;
        HNode *up = above(n);
        if (host(n) && up->first_child)
            return up->first_child;
        n = up;
    }
    return NULL;
}
/* Held units need no polling. Only ready units and units awaiting pins
 * are linked; both queues permit constant-time cancellation on reinsertion. */
#ifdef HTML_RECLAIM_TEST
size_t h_reclaim_visits;
#define RECLAIM_VISIT() (h_reclaim_visits++)
#else
#define RECLAIM_VISIT() ((void)0)
#endif
static void unqueue(HDoc *d, HNode *n)
{
    HMeta *m = h_meta(n);
    if (!(m->flags & H_NODE_QUEUED))
        return;
    HNode **head = (m->flags & H_NODE_WAIT) ? &d->waiting_nodes : &d->pending_nodes;
    if (m->pending_prev)
        h_meta(m->pending_prev)->pending_next = m->pending_next;
    else
        *head = m->pending_next;
    if (m->pending_next)
        h_meta(m->pending_next)->pending_prev = m->pending_prev;
    m->pending_next = m->pending_prev = NULL;
    m->flags &= ~(H_NODE_QUEUED | H_NODE_WAIT);
}
static void queue(HDoc *d, HNode *n, bool wait)
{
    HMeta *m = h_meta(n);
    unqueue(d, n);
    HNode **head = wait ? &d->waiting_nodes : &d->pending_nodes;
    m->pending_next = *head;
    if (*head)
        h_meta(*head)->pending_prev = n;
    *head = n;
    m->flags |= H_NODE_QUEUED | (wait ? H_NODE_WAIT : 0);
}
static void unit_changed(HDoc *d, HNode *root)
{
    HMeta *m = h_meta(root);
    if (!(m->flags & H_NODE_PENDING))
        return;
    if (m->unit_holds) {
        unqueue(d, root);
        m->flags &= ~(H_NODE_FRESH | H_NODE_RETIRED);
    } else if (!(m->flags & H_NODE_QUEUED))
        queue(d, root, false);
}
static HNode *unit_root(const HNode *n)
{
    while (above(n)) {
        RECLAIM_VISIT();
        n = above(n);
    }
    return (HNode *)n;
}
static size_t subtree_holds(HNode *root)
{
    if (h_meta(root)->flags & H_NODE_PENDING)
        return h_meta(root)->unit_holds;
    size_t count = 0;
    for (HNode *n = root; n; n = lifetime_next(root, n)) {
        RECLAIM_VISIT();
        size_t holds = (size_t)h_meta(n)->holds + h_meta(n)->parser_refs;
        if (holds > SIZE_MAX - count)
            d_fatal("libhtml: unit hold count overflow\n");
        count += holds;
    }
    return count;
}
void d_ref(HDoc *d, const HNode *n, bool parser, bool add)
{
    HMeta *m = h_meta(n);
    uint32_t count = parser ? m->parser_refs : m->holds;
    if (add ? count == (parser ? UINT16_MAX : UINT32_MAX) : !count)
        d_fatal("libhtml: node reference count overflow or underflow\n");
    HNode *root = d->pending_units ? unit_root(n) : NULL;
    HMeta *unit = root ? h_meta(root) : NULL;
    if (unit && (unit->flags & H_NODE_PENDING)) {
        if (add ? unit->unit_holds == SIZE_MAX : !unit->unit_holds)
            d_fatal("libhtml: unit reference count overflow or underflow\n");
        if (add) unit->unit_holds++; else unit->unit_holds--;
        unit_changed(d, root);
    }
    if (parser) {
        if (add) m->parser_refs++; else m->parser_refs--;
    } else {
        if (add) m->holds++; else m->holds--;
    }
}
void d_detached(HDoc *d, HNode *n)
{
    if (!n || above(n))
        return;
    HMeta *m = h_meta(n);
    if (!(m->flags & H_NODE_PENDING)) {
        m->unit_holds = subtree_holds(n);
        m->flags |= H_NODE_PENDING;
        d->pending_units++;
    }
    m->stamp = d->version;
    m->flags = (m->flags & ~H_NODE_RETIRED) | H_NODE_FRESH;
    unit_changed(d, n);
}
/* Link accounting happens before pointer changes. A verb may split or join
 * retained units, including across a template's host edge; collection waits
 * until its links and form-owner records have reached a stable boundary. */
void d_unlinking(HDoc *d, HNode *n)
{
    if (!d->pending_units || !n->parent)
        return;
    HNode *root = unit_root(n);
    HMeta *m = h_meta(root);
    if (m->flags & H_NODE_PENDING) {
        size_t count = subtree_holds(n);
        if (count > m->unit_holds)
            d_fatal("libhtml: unlink unit hold count underflow\n");
        m->unit_holds -= count;
        unit_changed(d, root);
    }
}
void d_linking(HDoc *d, HNode *parent, HNode *n)
{
    if (!d->pending_units)
        return;
    HNode *root = unit_root(parent);
    HMeta *unit = h_meta(root), *m = h_meta(n);
    if (unit->flags & H_NODE_PENDING) {
        size_t count = subtree_holds(n);
        if (count > SIZE_MAX - unit->unit_holds)
            d_fatal("libhtml: link unit hold count overflow\n");
        unit->unit_holds += count;
        unit_changed(d, root);
    }
    if (m->flags & H_NODE_PENDING) {
        unqueue(d, n);
        m->flags &= ~(H_NODE_PENDING | H_NODE_FRESH | H_NODE_RETIRED);
        m->unit_holds = 0;
        d->pending_units--;
    }
}
static bool older_pin(const HDoc *d, uint64_t stamp)
{
    for (size_t i = 0; i < H_PINS; i++)
        if (d->pins[i] && d->pins[i] < stamp)
            return true;
    return false;
}
/* Form-owner edges are within DOM trees, including separate template trees.
 * Verify both directions before freeing nodes: an incoming external record
 * would otherwise become a dangling pointer. The zero-record gate keeps
 * ordinary fragment churn from walking the document's retained allocations. */
static void assert_records(HDoc *d)
{
    if (!d->records)
        return;
    size_t count = 0;
    for (HNode *n = d->live_nodes; n; n = h_meta(n)->live_next)
        if (n->form_owner) {
            if (n->form_owner->document_id != d->id ||
                root_of(n, NULL) != root_of(n->form_owner, NULL))
                d_fatal("libhtml: form-owner record crosses a reclaimed tree edge\n");
            count++;
        }
    if (count != d->records)
        d_fatal("libhtml: form-owner record count mismatch\n");
}
static void node_free(HDoc *d, HNode *n)
{
    HMeta *m = h_meta(n);
    if (n->form_owner) {
        if (!d->records)
            d_fatal("libhtml: form-owner record count underflow\n");
        d->records--;
    }
    if (d_form_input(n)) {
        if (!d->form_inputs)
            d_fatal("libhtml: explicit input count underflow\n");
        d->form_inputs--;
    }
    if (n->kind == ELEMENT && (*h_word(n) & H_ATTRS_PRIVATE))
        for (HAttr *a = n->attrs, *next; a; a = next) {
            next = a->next;
            h_free(d, a);
        }
    if ((n->kind == TEXT || n->kind == COMMENT || n->kind == PI) && *h_word(n))
        h_free(d, (char *)n->text);
    if (m->live_prev)
        h_meta(m->live_prev)->live_next = m->live_next;
    else
        d->live_nodes = m->live_next;
    if (m->live_next)
        h_meta(m->live_next)->live_prev = m->live_prev;
    d->pub.node_count--;
    if (m->flags & H_NODE_PACKED)
        h_free(d, n);
    else {
        unsigned char *bytes = (unsigned char *)n;
        for (size_t i = 0; i < sizeof(*n) + sizeof(*m); i++)
            bytes[i] = 0;
        n->next = d->free_nodes;
        d->free_nodes = n;
    }
}
static void subtree_free(HDoc *d, HNode *root)
{
    assert_records(d);
    HNode *n = root;
    for (;;) {
        if (n->template_contents) {
            n = n->template_contents;
            continue;
        }
        if (n->first_child) {
            n = n->first_child;
            continue;
        }
        bool done = n == root;
        HNode *up = above(n);
        if (host(n))
            up->template_contents = NULL;
        else
            h_detach(d, n);
        node_free(d, n);
        if (done)
            break;
        n = up;
    }
}
void d_collect(HDoc *d)
{
    while (d->pending_nodes) {
        RECLAIM_VISIT();
        HNode *n = d->pending_nodes;
        HMeta *m = h_meta(n);
        unqueue(d, n);
        if (above(n) || m->unit_holds)
            d_fatal("libhtml: invalid ready unit\n");
        if (!(m->flags & H_NODE_RETIRED)) {
            /* Pins acquired while held may borrow this unit's current bytes.
             * Protect them without changing the visible content version. */
            if (!(m->flags & H_NODE_FRESH)) {
                if (d->version == UINT64_MAX)
                    d_fatal("libhtml: subtree retirement version overflow\n");
                m->stamp = d->version + 1;
            }
            m->flags = (m->flags & ~H_NODE_FRESH) | H_NODE_RETIRED;
        }
        if (older_pin(d, m->stamp))
            queue(d, n, true);
        else {
            m->flags &= ~(H_NODE_PENDING | H_NODE_FRESH | H_NODE_RETIRED);
            d->pending_units--;
            subtree_free(d, n);
        }
    }
}
void d_unpin_collect(HDoc *d)
{
    for (HNode *n = d->waiting_nodes, *next; n; n = next) {
        next = h_meta(n)->pending_next;
        if (!older_pin(d, h_meta(n)->stamp))
            queue(d, n, false);
    }
    d_collect(d);
}
size_t d_pending_bytes(const HDoc *d)
{
    size_t bytes = 0;
    for (HNode *root = d->waiting_nodes; root; root = h_meta(root)->pending_next)
        for (HNode *n = root; n; n = lifetime_next(root, n))
            bytes += d_owned_bytes(n);
    return bytes;
}

static size_t height_of(HNode *root, uint64_t *steps)
{
    size_t depth = 0, deepest = 0;
    for (HNode *n = root; n; n = next_in(root, n, &depth)) {
        step(steps);
        if (depth > deepest)
            deepest = depth;
    }
    return deepest;
}

static bool is_html(const HNode *n, os64_html_tag_t tag)
{
    return n->kind == ELEMENT && n->ns == OS64_HTML_NS_HTML && n->tag == tag;
}
static HNode *child_of_kind(const HNode *parent, os64_html_node_kind_t kind, const HNode *except,
                            uint64_t *steps)
{
    for (HNode *c = parent->first_child; c; c = c->next) {
        step(steps);
        if (c->kind == kind && c != except)
            return c;
    }
    return NULL;
}
static bool kind_follows(const HNode *child, os64_html_node_kind_t kind, uint64_t *steps)
{
    for (const HNode *c = child ? child->next : NULL; c; c = c->next) {
        step(steps);
        if (c->kind == kind)
            return true;
    }
    return false;
}
static bool kind_precedes(const HNode *child, os64_html_node_kind_t kind, uint64_t *steps)
{
    for (const HNode *c = child ? child->prev : NULL; c; c = c->prev) {
        step(steps);
        if (c->kind == kind)
            return true;
    }
    return false;
}

/* The DOM Standard's "ensure pre-insertion validity", and with `replacing`
 * its twin inside "replace", where `child` is the node being replaced. Then
 * the rules that are this library's own: an HTML document keeps its
 * `html` element, and the tree stays as shallow as a parse would keep it. */
static int64_t may_insert(HDoc *d, HNode *parent, HNode *node, HNode *child, bool replacing,
                          uint64_t *steps)
{
    if (!parent || !node || (replacing && !child) || !ours(d, parent) || !ours(d, node) ||
        !ours(d, child))
        return OS64_HTML_BAD_ARGUMENT;
    if (parent->kind != DOCUMENT && parent->kind != FRAGMENT && parent->kind != ELEMENT)
        return OS64_HTML_HIERARCHY;
    for (const HNode *a = parent; a; a = above(a)) {
        step(steps);
        if (a == node)
            return OS64_HTML_HIERARCHY;
    }
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
            element = child_of_kind(node, ELEMENT, NULL, steps);
            if (child_of_kind(node, TEXT, NULL, steps) ||
                (element && child_of_kind(node, ELEMENT, element, steps)))
                return OS64_HTML_HIERARCHY;
        }
        if (element) {
            if (child_of_kind(parent, ELEMENT, spared, steps) || kind_follows(child, DOCTYPE, steps) ||
                (!replacing && child && child->kind == DOCTYPE))
                return OS64_HTML_HIERARCHY;
        } else if (node->kind == DOCTYPE) {
            if (child_of_kind(parent, DOCTYPE, spared, steps) ||
                kind_precedes(child, ELEMENT, steps) ||
                (!replacing && !child && child_of_kind(parent, ELEMENT, NULL, steps)))
                return OS64_HTML_HIERARCHY;
        }
    }

    if (!d->pub.is_xml && parent == &d->root) {
        if (element && !is_html(element, OS64_HTML_TAG_HTML))
            return OS64_HTML_ROOT_REQUIRED;
        /* Replacing the html element with anything that brings no element. */
        if (replacing && child->kind == ELEMENT && !element)
            return OS64_HTML_ROOT_REQUIRED;
    } else if (!d->pub.is_xml && node->parent == &d->root && node->kind == ELEMENT)
        return OS64_HTML_ROOT_REQUIRED;     /* moving the html element out of the document */

    size_t under = depth_of(parent, steps) + 1, limit = h_depth_limit(d);
    for (HNode *c = node->kind == FRAGMENT ? node->first_child : node; c;
         c = node->kind == FRAGMENT ? c->next : NULL) {
        size_t height = height_of(c, steps);
        if (under > limit || height > limit - under)
            return OS64_HTML_TOO_DEEP;
    }
    return OS64_HTML_OK;
}

/* The landmarks consumers start from, by their definitions: the document's
 * element, and its first `head` and `body` children. */
static void landmarks(HDoc *d, uint64_t *steps)
{
    if (d->pub.is_xml) {
        d->pub.html = d->pub.head = d->pub.body = NULL;
        return;
    }
    HNode *html = child_of_kind(&d->root, ELEMENT, NULL, steps);
    if (html)
        d->pub.html = html;
    d->pub.head = d->pub.body = NULL;
    for (HNode *c = d->pub.html->first_child; c; c = c->next) {
        step(steps);
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
 * it is the root of everything in it.
 *
 * Both walks stay out of template contents. What lies in a template's
 * contents is in the tree those contents are the root of, before the move
 * and after it, so a control and its form in there are parted by nothing
 * that happens to the template. */
static void forget_owner(HDoc *d, HNode *control)
{
    control->form_owner = NULL;
    d->records--;
}
static void split_records(HDoc *d, HNode *moved, HNode *left_behind, uint64_t *steps)
{
    if (!d->records)
        return;
    bool carries_form = false;
    for (HNode *n = moved; n; n = next_in_tree(moved, n)) {
        step(steps);
        if (is_html(n, OS64_HTML_TAG_FORM))
            carries_form = true;
        if (n->form_owner && root_of(n->form_owner, steps) != moved)
            forget_owner(d, n);
    }
    if (!carries_form || !d->records)
        return;
    for (HNode *n = left_behind; n; n = next_in_tree(left_behind, n)) {
        step(steps);
        if (n->form_owner && root_of(n->form_owner, steps) == moved)
            forget_owner(d, n);
    }
}
static void take_out(HDoc *d, HNode *node, uint64_t *steps)
{
    if (!node->parent)
        return;
    HNode *left_behind = root_of(node->parent, steps);
    h_detach(d, node);
    split_records(d, node, left_behind, steps);
}
/* A verb moved a node. A parser still building the document checks each
 * link it makes from then on (internal.h, HDoc.disturbed). */
static void moved(HDoc *d)
{
    d->version++;
    d->disturbed = true;
}

/* The log owns identity holds without collecting while a verb is midway
 * through its links. Stable reader boundaries and the verbs collect them. */
struct HChange {
    os64_html_change_entry_t entry;
    size_t bytes;
};
struct os64_html_reader {
    HDoc *doc;
    os64_html_reader_t *next;
    uint64_t version;
};
static void log_ref(HDoc *d, const HNode *node, bool add)
{
    if (node) d_ref(d, node, false, add);
}
static void log_refs(HDoc *d, const os64_html_change_entry_t *e, bool add)
{
    log_ref(d, e->target, add);
    log_ref(d, e->previous_sibling, add);
    log_ref(d, e->next_sibling, add);
    for (size_t i = 0; i < e->nadded; ++i) log_ref(d, e->added[i], add);
    for (size_t i = 0; i < e->nremoved; ++i) log_ref(d, e->removed[i], add);
}
static void log_drop(HDoc *d)
{
    HChange *c = d->log_head;
    if (!c) return;
    d->log_head = (HChange *)c->entry.next;
    if (!d->log_head) d->log_tail = NULL;
    if (c->entry.version > d->log_floor) d->log_floor = c->entry.version;
    d->log_bytes -= c->bytes;
    log_refs(d, &c->entry, false);
    h_free(d, c);
}
static void log_prune(HDoc *d)
{
    uint64_t oldest = d->version;
    for (os64_html_reader_t *r = d->readers; r; r = r->next)
        if (r->version < oldest) oldest = r->version;
    while (d->log_head && d->log_head->entry.version <= oldest) log_drop(d);
}
static size_t log_limit(const HDoc *d)
{
    return d->log_limited ? d->log_limit : d->budget / 16;
}
static void log_lost(HDoc *d)
{
    while (d->log_head) log_drop(d);
    d->log_floor = d->version;
}
os64_html_reader_t *os64_html_reader_new(os64_html_document_t *doc, uint64_t version)
{
    HDoc *d = (HDoc *)doc;
    if (!d || version > d->version) return NULL;
    int64_t why = OS64_HTML_OK;
    os64_html_reader_t *r = d_alloc(d, sizeof(*r), &why);
    if (!r) return NULL;
    if (!d->readers) d->log_floor = d->version;
    r->doc = d; r->version = version; r->next = d->readers; d->readers = r;
    return r;
}
void os64_html_reader_free(os64_html_reader_t *r)
{
    if (!r) return;
    HDoc *d = r->doc;
    os64_html_reader_t **at = &d->readers;
    while (*at && *at != r) at = &(*at)->next;
    if (!*at) d_fatal("libhtml: change reader is not registered\n");
    *at = r->next;
    h_free(d, r);
    log_prune(d);
    d_collect(d);
}
int64_t os64_html_changes(os64_html_reader_t *r, const os64_html_change_entry_t **out)
{
    if (out) *out = NULL;
    if (!r || !out) return -1;
    HDoc *d = r->doc;
    log_prune(d);
    int64_t count = 0;
    if (r->version < d->log_floor) count = -1;
    else for (HChange *c = d->log_head; c; c = (HChange *)c->entry.next) {
        if (c->entry.version <= r->version) continue;
        if (!*out) *out = &c->entry;
        ++count;
    }
    r->version = d->version;
    d_collect(d);
    return count;
}
void os64_html_changes_limit(os64_html_document_t *doc, size_t bytes)
{
    HDoc *d = (HDoc *)doc;
    if (!d) return;
    d->log_limited = true; d->log_limit = bytes;
    while (d->log_head && d->log_bytes > bytes) log_drop(d);
    d_collect(d);
}
size_t os64_html_changes_bytes(const os64_html_document_t *doc)
{
    return doc ? ((const HDoc *)doc)->log_bytes : 0;
}
static char *log_string(char **at, const char *s)
{
    if (!s) return NULL;
    size_t n = h_len(s) + 1;
    char *result = *at;
    for (size_t i = 0; i < n; ++i) result[i] = s[i];
    *at += n;
    return result;
}
static void log_change(HDoc *d, const os64_html_change_t *c)
{
    if (!d->readers) return;
    log_prune(d);
    size_t cap = log_limit(d), bytes = sizeof(HChange);
    if (c->nadded > (SIZE_MAX - bytes) / sizeof(HNode *)) { log_lost(d); return; }
    bytes += c->nadded * sizeof(HNode *);
    if (c->nremoved > (SIZE_MAX - bytes) / sizeof(HNode *)) { log_lost(d); return; }
    bytes += c->nremoved * sizeof(HNode *);
    const char *names[] = {c->attr_name, c->attr_ns, c->attr_local};
    for (size_t i = 0; i < H_ARRAY(names); ++i) if (names[i]) {
        size_t n = h_len(names[i]) + 1;
        if (n > SIZE_MAX - bytes) { log_lost(d); return; }
        bytes += n;
    }
    if (bytes > cap) { log_lost(d); return; }
    while (d->log_head && bytes > cap - d->log_bytes) log_drop(d);
    int64_t why = OS64_HTML_OK;
    size_t before = d->pub.arena_bytes;
    HChange *copy = d_alloc(d, bytes, &why);
    if (!copy) { log_lost(d); return; }
    copy->bytes = d->pub.arena_bytes - before;
    /* Include the allocator's header in the cap, without duplicating its
     * private layout. A refused log never refuses the mutation itself. */
    while (d->log_head && copy->bytes > cap - d->log_bytes) log_drop(d);
    if (copy->bytes > cap) { h_free(d, copy); log_lost(d); return; }
    os64_html_change_entry_t *e = &copy->entry;
    e->version = d->version; e->kind = c->kind; e->target = c->target;
    e->previous_sibling = c->previous_sibling; e->next_sibling = c->next_sibling;
    e->nadded = c->nadded; e->nremoved = c->nremoved;
    const HNode **nodes = (const HNode **)(copy + 1);
    e->added = nodes;
    const HNode *node = c->added;
    for (size_t i = 0; i < c->nadded; ++i) { nodes[i] = node; node = node->next; }
    nodes += c->nadded; e->removed = nodes; node = c->removed;
    for (size_t i = 0; i < c->nremoved; ++i) { nodes[i] = node; node = node->next; }
    char *strings = (char *)(nodes + c->nremoved);
    e->attr_name = log_string(&strings, c->attr_name);
    e->attr_ns = log_string(&strings, c->attr_ns);
    e->attr_local = log_string(&strings, c->attr_local);
    log_refs(d, e, true);
    if (d->log_tail) d->log_tail->entry.next = e; else d->log_head = copy;
    d->log_tail = copy; d->log_bytes += copy->bytes;
}

/* ── The listener (html.h, THE LISTENER) ─────────────────────────────── */

void os64_html_observe(os64_html_document_t *doc,
                       void (*changed)(void *opaque, const os64_html_change_t *change), void *opaque)
{
    HDoc *d = (HDoc *)doc;
    if (!d)
        return;
    d->changed = changed;
    d->changed_opaque = changed ? opaque : NULL;
}
static void tell(HDoc *d, os64_html_change_t *c, bool log)
{
    c->version = d->version;
    if (log) log_change(d, c);
    if (d->changed) d->changed(d->changed_opaque, c);
}
/* The standard's insert names the sibling before the reference child as it
 * stood before anything moved, and the node's own place in the parent it is
 * leaving. A fragment's children are counted here, while they are still
 * its own. */
void d_seen(const HDoc *d, HNode *parent, HNode *node, HNode *before, HSeen *seen)
{
    *seen = (HSeen){0};
    if (!d->changed && !d->readers)
        return;
    seen->previous = before ? before->prev : parent->last_child;
    if (node->kind == FRAGMENT) {
        seen->first = node->first_child;
        for (HNode *c = seen->first; c; c = c->next)
            seen->count++;
    } else if (node->parent) {
        seen->from = node->parent;
        seen->from_prev = node->prev;
        seen->from_next = node->next;
    }
}
void d_tell_removed(HDoc *d, HNode *parent, HNode *node, HNode *prev, HNode *next, bool log)
{
    if (!d->changed && !d->readers)
        return;
    os64_html_change_t c = {.kind = OS64_HTML_CHANGE_CHILDREN, .target = parent, .removed = node,
                            .nremoved = 1, .previous_sibling = prev, .next_sibling = next};
    tell(d, &c, log);
}
/* The fragment's record, or the moved node's removal, comes before the
 * parent's, as the standard queues them. */
void d_tell_placed(HDoc *d, HNode *parent, HNode *node, HNode *before, HNode *replaced,
                   const HSeen *seen, bool log)
{
    if (!d->changed && !d->readers)
        return;
    HNode *added = node;
    size_t nadded = 1;
    if (node->kind == FRAGMENT) {
        added = seen->first;
        nadded = seen->count;
        if (nadded) {
            os64_html_change_t drained = {.kind = OS64_HTML_CHANGE_CHILDREN, .target = node,
                                          .removed = added, .nremoved = nadded};
            tell(d, &drained, log);
        }
    } else if (seen->from)
        d_tell_removed(d, seen->from, node, seen->from_prev, seen->from_next, log);
    os64_html_change_t c = {.kind = OS64_HTML_CHANGE_CHILDREN, .target = parent,
                            .added = nadded ? added : NULL, .nadded = nadded,
                            .removed = replaced, .nremoved = replaced != NULL,
                            .previous_sibling = seen->previous, .next_sibling = before};
    tell(d, &c, log);
}
void d_tell_text(HDoc *d, HNode *node, const char *old, size_t old_len, bool log)
{
    if (!d->changed && !d->readers)
        return;
    os64_html_change_t c = {.kind = OS64_HTML_CHANGE_TEXT, .target = node, .text_old = old,
                            .text_old_len = old_len};
    tell(d, &c, log);
}
/* An attribute's local name is its qualified name past the prefix, and a
 * prefix only a namespaced attribute has: `foo:bar` in no namespace is all
 * local name. */
static void tell_attr(HDoc *d, HNode *e, const char *name, const char *ns, const char *old,
                      size_t old_len, bool log)
{
    if (!d->changed && !d->readers)
        return;
    const char *local = name;
    for (const char *c = name; ns && *c; c++)
        if (*c == ':') {
            local = c + 1;
            break;
        }
    os64_html_change_t c = {.kind = OS64_HTML_CHANGE_ATTR, .target = e, .attr_name = name,
                            .attr_ns = ns, .attr_local = local, .attr_old = old,
                            .attr_old_len = old_len};
    tell(d, &c, log);
}

static void place(HDoc *d, HNode *parent, HNode *node, HNode *before)
{
    bool touched = is_landmark_parent(d, parent) || (node->parent && is_landmark_parent(d, node->parent));
    if (node->kind == FRAGMENT) {
        for (HNode *c; (c = node->first_child) != NULL;) {
            take_out(d, c, NULL);
            h_attach(d, parent, before, c);
        }
    } else {
        take_out(d, node, NULL);
        h_attach(d, parent, before, node);
    }
    if (touched)
        landmarks(d, NULL);
}

int64_t os64_html_may_insert(const os64_html_document_t *doc, const HNode *parent, const HNode *node)
{
    HDoc *d = (HDoc *)doc;
    return d ? may_insert(d, (HNode *)parent, (HNode *)node, NULL, false, NULL) : OS64_HTML_BAD_ARGUMENT;
}

int64_t os64_html_insert(os64_html_document_t *doc, HNode *parent, HNode *node, HNode *before)
{
    HDoc *d = (HDoc *)doc;
    if (!d)
        return OS64_HTML_BAD_ARGUMENT;
    int64_t verdict = may_insert(d, parent, node, before, false, NULL);
    if (verdict)
        return verdict;
    /* The version is every snapshot's signal to rebuild, so it moves when a
     * reader could see a difference and stays when none could. An empty
     * fragment brings nothing. A node put where it already sits is still
     * taken out and put back, which parts a control from a form outside
     * what moved; if that parted nobody, nothing is different. */
    if (node->kind == FRAGMENT && !node->first_child) {
        /* Empty insertion changes no visible tree, but consumes the result
         * container's lifetime just as draining a nonempty fragment does.
         * A pin at this unchanged version may still borrow the container. */
        d_detached(d, node);
        h_meta(node)->flags &= ~H_NODE_FRESH;
        d_collect(d);
        return OS64_HTML_OK;
    }
    if (before == node)
        before = node->next;
    bool stays = node->parent == parent && node->next == before;
    size_t records = d->records;
    HSeen seen;
    d_seen(d, parent, node, before, &seen);
    place(d, parent, node, before);
    if (!stays || d->records != records)
        moved(d);
    d_tell_placed(d, parent, node, before, NULL, &seen, !stays || d->records != records);
    if (node->kind == FRAGMENT)
        d_detached(d, node);
    d_collect(d);
    return OS64_HTML_OK;
}

int64_t os64_html_replace(os64_html_document_t *doc, HNode *parent, HNode *node, HNode *old)
{
    HDoc *d = (HDoc *)doc;
    if (!d)
        return OS64_HTML_BAD_ARGUMENT;
    int64_t verdict = may_insert(d, parent, node, old, true, NULL);
    if (verdict)
        return verdict;
    /* The standard's replace adopts the node, which takes it out of its own
     * parent, BEFORE it takes `old` out (DOM § 4.2.3, WPT's
     * MutationObserver-childList): so the node's removal names the siblings
     * it had with `old` still there, and the parent's record names `old`'s,
     * read before anything moved (the node itself, when it stood just
     * before `old`). A node replaced by itself is therefore two records: it
     * leaves, and by the time `old` would be taken out it has no parent, so
     * it comes back as an insertion with nothing removed. Nothing moves. */
    if (node == old) {
        HSeen seen;
        d_seen(d, parent, node, old->next, &seen);
        seen.previous = old->prev;
        d_tell_placed(d, parent, node, old->next, NULL, &seen, false);
        return OS64_HTML_OK;
    }
    HNode *before = old->next;
    if (before == node)
        before = node->next;
    HSeen seen;
    d_seen(d, parent, node, before, &seen);
    seen.previous = old->prev;
    moved(d);
    bool touched = is_landmark_parent(d, parent);
    take_out(d, old, NULL);
    place(d, parent, node, before);
    if (touched)
        landmarks(d, NULL);
    d_tell_placed(d, parent, node, before, old, &seen, true);
    d_detached(d, old);
    if (node->kind == FRAGMENT)
        d_detached(d, node);
    d_collect(d);
    return OS64_HTML_OK;
}

int64_t os64_html_remove(os64_html_document_t *doc, HNode *node)
{
    HDoc *d = (HDoc *)doc;
    if (!d || !node || !ours(d, node))
        return OS64_HTML_BAD_ARGUMENT;
    if (!node->parent)
        return OS64_HTML_OK;
    if (!d->pub.is_xml && node->parent == &d->root && node->kind == ELEMENT)
        return OS64_HTML_ROOT_REQUIRED;
    bool touched = is_landmark_parent(d, node->parent);
    HNode *parent = node->parent, *prev = node->prev, *next = node->next;
    moved(d);
    take_out(d, node, NULL);
    if (touched)
        landmarks(d, NULL);
    d_tell_removed(d, parent, node, prev, next, true);
    d_detached(d, node);
    d_collect(d);
    return OS64_HTML_OK;
}

/* ── What the parser asks (internal.h) ───────────────────────────────────
 *
 * While the tree is the one a parser built, the parser's own algorithm keeps
 * its links sound. Once a verb has moved something under it, the rules that
 * bind a verb bind the parser's links too, and these are how it asks. */

/* Whether linking or unlinking `node` under `parent` can change which
 * elements the landmarks name. */
static bool is_landmark(const HDoc *d, const HNode *parent, const HNode *node)
{
    return node->kind == ELEMENT && is_landmark_parent(d, parent);
}
int64_t d_place(HDoc *d, HNode *parent, HNode *node, HNode *before, uint64_t *steps)
{
    int64_t verdict = may_insert(d, parent, node, before, false, steps);
    if (verdict)
        return verdict;
    bool touched = is_landmark(d, parent, node) ||
                   (node->parent && is_landmark(d, node->parent, node));
    h_attach(d, parent, before, node);
    if (touched)
        landmarks(d, steps);
    return OS64_HTML_OK;
}
HNode *d_root(HNode *n, uint64_t *steps)
{
    return root_of(n, steps);
}
/* The parser's moves are judged by where things end up, since it gets a
 * subtree to its place in several links and builds part of it aside on the
 * way. Moving a node about inside one tree, as the adoption agency does, has
 * never parted a control from its form. When `moved` has ended in another
 * tree than `old_root`, the one it began in, each control in it and each
 * control it left behind is asked whether its form is still in its tree. */
void d_parted(HDoc *d, HNode *moved, HNode *old_root, uint64_t *steps)
{
    HNode *new_root = root_of(moved, steps);
    if (!d->records || new_root == old_root)
        return;
    bool carries_form = false;
    for (HNode *n = moved; n; n = next_in_tree(moved, n)) {
        step(steps);
        if (is_html(n, OS64_HTML_TAG_FORM))
            carries_form = true;
        if (n->form_owner && root_of(n->form_owner, steps) != new_root)
            forget_owner(d, n);
    }
    if (!carries_form || !d->records)
        return;
    for (HNode *n = old_root; n; n = next_in_tree(old_root, n)) {
        step(steps);
        if (n->form_owner && root_of(n->form_owner, steps) != old_root)
            forget_owner(d, n);
    }
}
void d_unlink(HDoc *d, HNode *node, uint64_t *steps)
{
    if (!node->parent)
        return;
    bool touched = is_landmark(d, node->parent, node);
    take_out(d, node, steps);
    if (touched)
        landmarks(d, steps);
}
/* What a finished document promises: it has its `html` element. A parse
 * that ended before it seated one is given the one every document is born
 * with. No control has a form owner yet: the parser ties none before the
 * document has its element. */
void d_seat_html(HDoc *d)
{
    if (child_of_kind(&d->root, ELEMENT, NULL, NULL))
        return;
    HSeen seen;
    d_seen(d, &d->root, &d->html, NULL, &seen);
    h_attach(d, &d->root, NULL, &d->html);
    d->version++;
    if (d->disturbed)
        landmarks(d, NULL);
    d_tell_placed(d, &d->root, &d->html, NULL, NULL, &seen, true);
}

/* ── New nodes ───────────────────────────────────────────────────────── */

static HNode *node_new(HDoc *d, os64_html_node_kind_t kind, int64_t *why)
{
    return d_node_new(d, kind, why);
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
    if (!d_text_ok(name, len)) {
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
    if (!d_text_ok(utf8, len)) {
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
os64_html_node_t *os64_html_create_document(os64_html_document_t *doc, int64_t *status)
{
    HDoc *d = (HDoc *)doc;
    int64_t why = d ? OS64_HTML_OK : OS64_HTML_BAD_ARGUMENT;
    HNode *n = d ? node_new(d, DOCUMENT, &why) : NULL;
    say(status, why);
    return n;
}
/* A string of the doctype's in a permanent chunk, as the parser keeps the
 * ones it reads: a doctype's strings are never replaced. */
static bool doctype_string(HDoc *d, const char *s, const char **out, int64_t *why)
{
    size_t len = h_len(s);
    char *copy = d_permanent(d, len + 1, why);
    if (!copy)
        return false;
    for (size_t i = 0; i < len; i++)
        copy[i] = s[i];
    *out = copy;
    return true;
}
os64_html_node_t *os64_html_create_doctype(os64_html_document_t *doc, const char *name,
                                           const char *public_id, const char *system_id,
                                           int64_t *status)
{
    HDoc *d = (HDoc *)doc;
    int64_t why = OS64_HTML_OK;
    if (!d || !name || !public_id || !system_id) {
        say(status, OS64_HTML_BAD_ARGUMENT);
        return NULL;
    }
    if (!d_text_ok(name, h_len(name)) || !d_text_ok(public_id, h_len(public_id)) ||
        !d_text_ok(system_id, h_len(system_id))) {
        say(status, OS64_HTML_BAD_TEXT);
        return NULL;
    }
    const char *strings[3] = {NULL, NULL, NULL};
    bool copied = doctype_string(d, name, &strings[0], &why) &&
                  doctype_string(d, public_id, &strings[1], &why) &&
                  doctype_string(d, system_id, &strings[2], &why);
    HNode *n = copied ? node_new(d, DOCTYPE, &why) : NULL;
    if (n) {
        n->name = strings[0];
        n->public_id = strings[1];
        n->system_id = strings[2];
    }
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

/* Record strings, including a nonstandard namespace URI, share its lifetime. */
static HAttr *attr_new(HDoc *d, const char *name, const char *value, size_t value_len,
                       const char *ns, int64_t *why)
{
    size_t name_len = h_len(name), ns_len = h_len(ns), size = sizeof(HAttr);
    size_t lengths[] = {value_len, name_len, ns_len};
    for (size_t i = 0; i < 3; ++i) {
        if (lengths[i] >= SIZE_MAX - size) {
            *why = OS64_HTML_ARENA_EXHAUSTED;
            return NULL;
        }
        size += lengths[i] + 1;
    }
    HAttr *a = d_alloc(d, size, why);
    if (!a)
        return NULL;
    char *v = (char *)(a + 1), *n = v + value_len + 1, *uri = n + name_len + 1;
    for (size_t i = 0; i < value_len; ++i)
        v[i] = value[i];
    for (size_t i = 0; i < name_len; ++i)
        n[i] = name[i];
    for (size_t i = 0; i < ns_len; ++i)
        uri[i] = ns[i];
    a->value = v;
    a->name = n;
    a->ns = ns ? uri : NULL;
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
 * chunks and so outlive every record. Inline fragment payloads are copied. */
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
    if (!attrs_copy(d, e->attrs, !(*h_word(e) & H_ATTRS_INLINE), &own, &why))
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
/* Private records are replaced and retired independently; inline records
 * expire with their node. Clones copy either kind instead of sharing it. */
bool d_attrs_inherit(HDoc *d, const HNode *from, HNode *to, int64_t *why)
{
    if (!(*h_word(from) & (H_ATTRS_PRIVATE | H_ATTRS_INLINE))) {
        to->attrs = from->attrs;
        return true;
    }
    if (!attrs_copy(d, from->attrs, false, &to->attrs, why))
        return false;
    *h_word(to) |= H_ATTRS_PRIVATE;
    return true;
}
/* A second `html` or `body` tag gives the element the attributes it lacks.
 * A record goes on the end of the element's own list, as a verb would put
 * it, so no clone that shares the parser's list gains it. */
int64_t d_attr_add(HDoc *d, HNode *e, const HAttr *a)
{
    int64_t why = OS64_HTML_OK;
    HAttr *fresh = attr_new(d, a->name, a->value, h_len(a->value), a->ns, &why);
    if (!fresh)
        return why;
    why = attrs_own(d, e);
    if (why) {
        h_free(d, fresh);
        return why;
    }
    HAttr **slot = &e->attrs;
    while (*slot)
        slot = &(*slot)->next;
    *slot = fresh;
    tell_attr(d, e, fresh->name, fresh->ns, NULL, 0, true);
    return OS64_HTML_OK;
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

static bool attrs_equal(const HAttr *a, const HAttr *b)
{
    for (; a && b; a = a->next, b = b->next)
        if (!h_eq(a->name, b->name) || !h_eq(a->value, b->value) || !h_eq(a->ns, b->ns))
            return false;
    return !a && !b;
}

/* One record for each change as it was applied, in order, to `had`, the
 * list the element had before: a set is told whether or not it changed the
 * value, a remove only when there was something to remove. A set keeps the
 * namespace of a record it replaces, and a name removed and set again comes
 * back in none, as set_attrs builds it. */
static void tell_attrs(HDoc *d, HNode *e, const HAttr *had,
                       const os64_html_attr_change_t *changes, size_t count, bool log)
{
    if (!d->changed && !d->readers)
        return;
    for (size_t i = 0; i < count; i++) {
        const os64_html_attr_change_t *c = &changes[i];
        const HAttr *a = had;
        while (a && !h_eq(a->name, c->name))
            a = a->next;
        const char *ns = a ? a->ns : NULL, *value = a ? a->value : NULL;
        size_t len = a ? h_len(a->value) : 0;
        for (size_t j = 0; j < i; j++) {
            if (!h_eq(changes[j].name, c->name))
                continue;
            if (changes[j].remove) {
                ns = value = NULL;
                len = 0;
            } else {
                value = changes[j].value ? changes[j].value : "";
                len = changes[j].value_len;
            }
        }
        if (!c->remove || value)
            tell_attr(d, e, c->name, ns, value, len, log);
    }
}

int64_t os64_html_set_attrs(os64_html_document_t *doc, HNode *e,
                            const os64_html_attr_change_t *changes, size_t count)
{
    HDoc *d = (HDoc *)doc;
    if (!d || !e || !ours(d, e) || e->kind != ELEMENT || (!changes && count))
        return OS64_HTML_BAD_ARGUMENT;
    bool effective = false;
    for (size_t i = 0; i < count; i++) {
        const os64_html_attr_change_t *c = &changes[i];
        if (!c->name || !*c->name || (!c->remove && !c->value && c->value_len))
            return OS64_HTML_BAD_ARGUMENT;
        if (!d_text_ok(c->name, h_len(c->name)) ||
            (!c->remove && !d_text_ok(c->value, c->value_len)))
            return OS64_HTML_BAD_TEXT;
        const HAttr *a = *attr_slot(e, c->name);
        effective |= c->remove ? a != NULL : !a || !same_bytes(a->value, c->value, c->value_len);
    }
    if (!effective) {
        tell_attrs(d, e, e->attrs, changes, count, false);
        return OS64_HTML_OK;
    }
    size_t peak = d->pub.peak_arena_bytes;
    int64_t why = OS64_HTML_OK;
    HNode prospective = {0};
    if (!attrs_copy(d, e->attrs, false, &prospective.attrs, &why)) {
        d->pub.peak_arena_bytes = peak;
        return why;
    }
    for (size_t i = 0; i < count; i++) {
        const os64_html_attr_change_t *c = &changes[i];
        HAttr **slot = attr_slot(&prospective, c->name), *old = *slot;
        if (c->remove) {
            if (old) {
                *slot = old->next;
                h_free(d, old);
            }
        } else if (!old || !same_bytes(old->value, c->value, c->value_len)) {
            HAttr *fresh = attr_new(d, c->name, c->value, c->value_len, old ? old->ns : NULL, &why);
            if (!fresh) {
                attrs_discard(d, prospective.attrs);
                d->pub.peak_arena_bytes = peak;
                return why;
            }
            fresh->next = old ? old->next : NULL;
            *slot = fresh;
            h_free(d, old);
        }
    }
    if (attrs_equal(e->attrs, prospective.attrs)) {
        attrs_discard(d, prospective.attrs);
        d->pub.peak_arena_bytes = peak;
        tell_attrs(d, e, e->attrs, changes, count, false);
        return OS64_HTML_OK;
    }
    HAttr *old = e->attrs;
    bool private = (*h_word(e) & H_ATTRS_PRIVATE) != 0;
    bool form_input = d_form_input(e);
    e->attrs = prospective.attrs;
    d->form_inputs = d->form_inputs - form_input + d_form_input(e);
    *h_word(e) |= H_ATTRS_PRIVATE;
    d->version++;
    for (size_t i = 0; i < count; i++)
        if (!changes[i].remove)
            form_attr_set(d, e, changes[i].name);
    tell_attrs(d, e, old, changes, count, true);
    if (private)
        while (old) {
            HAttr *next = old->next;
            d_retire(d, old);
            old = next;
        }
    return OS64_HTML_OK;
}

int64_t os64_html_set_attr(os64_html_document_t *doc, HNode *e, const char *name,
                           const char *value, size_t value_len)
{
    HDoc *d = (HDoc *)doc;
    if (!d || !e || !ours(d, e) || e->kind != ELEMENT || !name || !*name || (!value && value_len))
        return OS64_HTML_BAD_ARGUMENT;
    if (!d_text_ok(name, h_len(name)) || !d_text_ok(value, value_len))
        return OS64_HTML_BAD_TEXT;
    const HAttr *existing = *attr_slot(e, name);
    /* The value it has already: no string a reader holds is replaced, and
     * the version stays. A control with a `form` attribute has no record to
     * void. The listener is told all the same, as the standard tells. */
    if (existing && same_bytes(existing->value, value, value_len)) {
        tell_attr(d, e, name, existing->ns, existing->value, h_len(existing->value), false);
        return OS64_HTML_OK;
    }
    /* The new record first, so that a failure after it has one block to give
     * back and a failure before it has none. */
    int64_t why = OS64_HTML_OK;
    HAttr *fresh = attr_new(d, name, value, value_len, existing ? existing->ns : NULL, &why);
    if (!fresh)
        return why;
    why = attrs_own(d, e);
    if (why) {
        h_free(d, fresh);
        return why;
    }
    HAttr **slot = attr_slot(e, name), *old = *slot;
    bool form_input = d_form_input(e);
    d->version++;
    fresh->next = old ? old->next : NULL;
    *slot = fresh;
    d->form_inputs = d->form_inputs - form_input + d_form_input(e);
    form_attr_set(d, e, name);
    tell_attr(d, e, name, fresh->ns, old ? old->value : NULL, old ? h_len(old->value) : 0, true);
    d_retire(d, old);
    return OS64_HTML_OK;
}

int64_t os64_html_remove_attr(os64_html_document_t *doc, HNode *e, const char *name)
{
    HDoc *d = (HDoc *)doc;
    if (!d || !e || !ours(d, e) || e->kind != ELEMENT || !name)
        return OS64_HTML_BAD_ARGUMENT;
    if (!*attr_slot(e, name))
        return OS64_HTML_OK;
    int64_t why = attrs_own(d, e);
    if (why)
        return why;
    HAttr **slot = attr_slot(e, name), *old = *slot;
    bool form_input = d_form_input(e);
    d->version++;
    *slot = old->next;
    d->form_inputs = d->form_inputs - form_input + d_form_input(e);
    tell_attr(d, e, name, old->ns, old->value, h_len(old->value), true);
    d_retire(d, old);
    return OS64_HTML_OK;
}

int64_t os64_html_set_text(os64_html_document_t *doc, HNode *n, const char *utf8, size_t len)
{
    HDoc *d = (HDoc *)doc;
    if (!d || !n || !ours(d, n) || (n->kind != TEXT && n->kind != COMMENT && n->kind != PI) ||
        (!utf8 && len))
        return OS64_HTML_BAD_ARGUMENT;
    if (!d_text_ok(utf8, len))
        return OS64_HTML_BAD_TEXT;
    /* The text it has already: as in set_attr, nothing is replaced, and the
     * listener is told. */
    if (same_bytes(n->text, utf8, len)) {
        d_tell_text(d, n, n->text, n->text_len, false);
        return OS64_HTML_OK;
    }
    int64_t why = OS64_HTML_OK;
    char *bytes = bytes_new(d, utf8, len, &why);
    if (!bytes)
        return why;
    /* A buffer with no capacity recorded is a literal or lies in a permanent
     * chunk: it is left where it is and goes with the document. */
    const char *was = n->text;
    size_t was_len = n->text_len;
    char *old = *h_word(n) ? (char *)n->text : NULL;
    d->version++;
    n->text = bytes;
    n->text_len = len;
    *h_word(n) = len + 1;
    d_tell_text(d, n, was, was_len, true);
    d_retire(d, old);
    return OS64_HTML_OK;
}

/* ── Custom state (html.h, CUSTOM STATE) ─────────────────────────────── */

int64_t os64_html_set_ce(os64_html_document_t *doc, HNode *e, os64_html_ce_t ce)
{
    HDoc *d = (HDoc *)doc;
    if (!d || !e || !ours(d, e) || e->kind != ELEMENT || (unsigned)ce > OS64_HTML_CE_FAILED)
        return OS64_HTML_BAD_ARGUMENT;
    if (e->ce != ce) {
        d->version++;
        e->ce = (uint8_t)ce;
        os64_html_change_t change = {.kind = OS64_HTML_CHANGE_STATE, .target = e};
        log_change(d, &change);
    }
    return OS64_HTML_OK;
}

bool os64_html_custom_name(const char *name)
{
    static const char *const kReserved[] = {"annotation-xml", "color-profile", "font-face",
                                            "font-face-src", "font-face-uri", "font-face-format",
                                            "font-face-name", "missing-glyph"};
    if (!name || name[0] < 'a' || name[0] > 'z')
        return false;
    bool hyphen = false;
    for (const unsigned char *c = (const unsigned char *)name; *c; c++) {
        hyphen = hyphen || *c == '-';
        if (!(*c >= 0x80 || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' ||
              *c == '.' || *c == '_'))
            return false;
    }
    for (size_t i = 0; hyphen && i < H_ARRAY(kReserved); i++)
        if (h_eq(name, kReserved[i]))
            return false;
    return hyphen;
}

/* ── Clone ───────────────────────────────────────────────────────────── */

/* Names in permanent document chunks may be shared. Packed fragment names
 * and foreign names are copied so reclaiming their source cannot dangle a clone. */
static bool string_for(HDoc *d, bool foreign, const char *from, const char **out, int64_t *why)
{
    *out = from;
    if (!foreign || !from)
        return true;
    size_t len = h_len(from);
    char *copy = d_permanent(d, len + 1, why);
    if (!copy)
        return false;
    for (size_t i = 0; i < len; i++)
        copy[i] = from[i];
    *out = copy;
    return true;
}
static HNode *clone_one(HDoc *d, const HNode *from, bool foreign, int64_t *why)
{
    if (from->kind == TEXT || from->kind == COMMENT || from->kind == PI) {
        char *bytes = bytes_new(d, from->text, from->text_len, why);
        HNode *n = bytes ? node_new(d, from->kind, why) : NULL;
        if (!n) {
            h_free(d, bytes);
            return NULL;
        }
        if (from->kind == PI && !string_for(d, foreign, from->name, &n->name, why)) {
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
    n->ns = from->ns;
    n->tag = from->tag;
    bool copy_strings = foreign || (from->kind == ELEMENT && (*h_word(from) & H_ATTRS_INLINE));
    if (!string_for(d, copy_strings, from->name, &n->name, why) ||
        !string_for(d, copy_strings, from->prefix, &n->prefix, why) ||
        !string_for(d, copy_strings, from->namespace_uri, &n->namespace_uri, why) ||
        !string_for(d, copy_strings, from->public_id, &n->public_id, why) ||
        !string_for(d, copy_strings, from->system_id, &n->system_id, why))
        return NULL;
    if (from->kind == ELEMENT) {
        /* Permanent parser lists can be shared inside their document;
         * private and inline lists need a copy. */
        if (foreign) {
            if (!attrs_copy(d, from->attrs, false, &n->attrs, why))
                return NULL;
            *h_word(n) |= H_ATTRS_PRIVATE;
        } else if (!d_attrs_inherit(d, from, n, why))
            return NULL;
        d->form_inputs += d_form_input(n);
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
    /* A subtree is of one document throughout: nothing puts one document's
     * node under another's. */
    HNode *root = (HNode *)node;
    bool foreign = !ours(d, root);
    if (deep && foreign && height_of(root, NULL) > h_depth_limit(d)) {
        say(status, OS64_HTML_TOO_DEEP);
        return NULL;
    }
    HNode *copy = clone_one(d, root, foreign, &why);
    size_t depth = 0;
    /* `at` and `mirror` walk the original and the copy in step: the copy of
     * a node's parent is found by climbing both until the original's side
     * reaches that parent, or the template whose contents it is. */
    HNode *at = root, *mirror = copy;
    for (HNode *n = deep && copy ? next_in(root, root, &depth) : NULL; n;
         n = next_in(root, n, &depth)) {
        HNode *made = clone_one(d, n, foreign, &why);
        if (!made) {
            copy = NULL;
            break;
        }
        while (at != n->parent && at->template_contents != n->parent) {
            HNode *up = at->parent ? at->parent : host(at);
            mirror = mirror->parent ? mirror->parent : host(mirror);
            at = up;
        }
        h_attach(d, at == n->parent ? mirror : mirror->template_contents, NULL, made);
        at = n;
        mirror = made;
    }
    say(status, why);
    return copy;
}
