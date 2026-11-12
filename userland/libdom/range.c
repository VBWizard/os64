#include "internal.h"

/* DOM RANGE (DOM § 5; docs/design/pending/RANGE.md § 5-§ 6): AbstractRange,
 * Range and StaticRange, and Document's createRange.
 *
 * A Range is libhtml's live range (os64_html_range_t): libhtml keeps its
 * boundary points right across every mutation, the parser's included, and
 * holds both boundary nodes. What is here is the interface: the standard's
 * algorithms, step for step, over boundary points read from it, and the
 * content algorithms built from the binding's own verbs (d_tree_change,
 * libpage's replace data and split) so a range's edits are seen as any
 * other edit is — MutationObserver records, form-control state, a script
 * connected by an insertion.
 *
 * A boundary point's root is its tree's plain root (->parent), as the
 * standard says: a range neither enters nor leaves a shadow tree, and a
 * template's contents are a tree of their own.
 *
 * An offset into character data is in UTF-16 code units; one inside a
 * surrogate pair is snapped down to the pair's start (DIVERGENCES.md § The
 * browser's script binding), by libhtml when a boundary is set and by the
 * byte lookups here when data is cut.
 *
 * A StaticRange is four fields checked once and never updated; it holds
 * its two nodes.
 *
 * LIFETIME is walker.c's: the records sit on one list, a finalizer only
 * marks its record dead (it must not reach into libhtml while the runtime
 * is torn down), and the dead are freed at the next creation and at the top
 * of the binding's tree changes (d_ranges_sweep), so a dropped range on a
 * large detached subtree neither pins it nor slows every mutation's walk;
 * d_ranges_free releases the rest. All of it exists only with
 * options.ranges and the Modern Web's switch, as one switch (RANGE.md § 2). */

static const char *const kRangeNames[] = {"AbstractRange", "Range", "StaticRange", NULL};

const char *d_ranges_name(size_t index)
{
    return index < sizeof(kRangeNames) / sizeof(kRangeNames[0]) ? kRangeNames[index] : NULL;
}

enum { R_LIVE, R_STATIC };

typedef struct DRange {
    struct DRange *next;
    os64_dom_t *dom;
    uint8_t kind;
    bool dead;
    os64_html_range_t *live;            /* R_LIVE */
    const os64_html_node_t *node[2];    /* R_STATIC: start, end; held */
    uint32_t offset[2];
} DRange;

struct DRanges {
    DRange *list;
    bool has_dead;
    JSClassID range_class, static_class;
    /* The page Selection's model (§ 9): `held` keeps its Range object as
     * property "r" (undefined for none), an object the binding retains,
     * so drain lets it go, with no prototype, so no accessor a page puts
     * on Object.prototype sees or answers for it; its direction, a D_DIR_. */
    DValue *held;
    uint8_t direction;
};

enum { D_DIR_NONE, D_DIR_FORWARD, D_DIR_BACKWARD };

static JSClassID range_slot, static_slot;

/* ── The tree, as the standard reads it ──────────────────────────────── */

static bool is_data(const os64_html_node_t *n)
{
    return n->kind == OS64_HTML_TEXT || n->kind == OS64_HTML_COMMENT || n->kind == OS64_HTML_PI;
}

static uint32_t index_of(const os64_html_node_t *n)
{
    uint32_t i = 0;
    for (const os64_html_node_t *p = n->prev; p != NULL; p = p->prev) i++;
    return i;
}

static bool inclusive_ancestor(const os64_html_node_t *ancestor, const os64_html_node_t *n)
{
    return ancestor == n || d_tree_ancestor(ancestor, n);
}

/* DOM § 5.2, the position of (a, ao) relative to (b, bo), two points of one
 * root: -1 before, 0 equal, 1 after. */
static int position(const os64_html_node_t *a, uint32_t ao, const os64_html_node_t *b, uint32_t bo)
{
    if (a == b) return ao == bo ? 0 : ao < bo ? -1 : 1;
    if (d_tree_precedes(b, a)) return -position(b, bo, a, ao);
    if (d_tree_ancestor(a, b)) {
        const os64_html_node_t *child = b;
        while (child->parent != a) child = child->parent;
        if (index_of(child) < ao) return 1;
    }
    return -1;
}

/* ── Records ─────────────────────────────────────────────────────────── */

static void point(const DRange *r, int which, const os64_html_node_t **node, uint32_t *offset)
{
    if (r->kind == R_LIVE) {
        os64_html_range_get(r->live, which, node, offset);
    } else {
        *node = r->node[which];
        *offset = r->offset[which];
    }
}

static void record_unlink_free(os64_dom_t *dom, DRange **link)
{
    DRange *r = *link;
    *link = r->next;
    if (r->kind == R_LIVE) {
        os64_html_range_free(r->live);
    } else {
        for (int i = 0; i < 2; i++)
            if (r->node[i] != NULL) os64_html_release(dom->document, r->node[i]);
    }
    d_free(dom, r);
}

void d_ranges_sweep(os64_dom_t *dom)
{
    struct DRanges *s = dom->ranges;
    if (s == NULL || !s->has_dead) return;
    for (DRange **link = &s->list; *link != NULL;)
        if ((*link)->dead) record_unlink_free(dom, link);
        else link = &(*link)->next;
    s->has_dead = false;
}

static DRange *record_new(os64_dom_t *dom, JSContext *ctx, uint8_t kind)
{
    DRange *r = d_alloc_kind(dom, sizeof(*r), OS64_DOM_MEM_RANGE);
    if (r == NULL) {
        d_error(ctx, "QuotaExceededError", "DOM range quota exceeded");
        return NULL;
    }
    os64_memset(r, 0, sizeof(*r));
    r->dom = dom;
    r->kind = kind;
    if (kind == R_LIVE && (r->live = os64_html_range_new(dom->document)) == NULL) {
        d_free(dom, r);
        d_error(ctx, "QuotaExceededError", "DOM range quota exceeded");
        return NULL;
    }
    r->next = dom->ranges->list;
    dom->ranges->list = r;
    return r;
}

/* A new Range object with both points at (document, 0), its prototype
 * `prototype` when that is an object (new.target's), else Range's. */
static JSValue range_new(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype,
                         const os64_html_node_t *document)
{
    struct DRanges *s = dom->ranges;
    d_ranges_sweep(dom);
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, s->range_class)
                                            : JS_NewObjectClass(ctx, (int)s->range_class);
    if (JS_IsException(object)) return object;
    DRange *r = record_new(dom, ctx, R_LIVE);
    if (r == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    (void)os64_html_range_set(r->live, 0, (os64_html_node_t *)document, 0);
    (void)os64_html_range_set(r->live, 1, (os64_html_node_t *)document, 0);
    JS_SetOpaque(object, r);
    return object;
}

static DRange *range_of(os64_dom_t *dom, JSContext *ctx, JSValueConst self, bool live_only)
{
    struct DRanges *s = dom->ranges;
    DRange *r = s != NULL ? JS_GetOpaque(self, s->range_class) : NULL;
    if (r == NULL && !live_only && s != NULL) r = JS_GetOpaque(self, s->static_class);
    if (r == NULL) JS_ThrowTypeError(ctx, live_only ? "Expected a Range" : "Expected an AbstractRange");
    return r;
}

static int throw_status(JSContext *ctx, int64_t status)
{
    d_html_error(ctx, status);
    return -1;
}

/* DOM § 5.5, "set the start or end" (which: 0 start, 1 end). 0, or -1 with
 * the exception thrown. */
static int set_point(JSContext *ctx, DRange *r, int which, const os64_html_node_t *node, uint32_t offset)
{
    if (node->kind == OS64_HTML_DOCTYPE) return throw_status(ctx, OS64_HTML_INVALID_NODE_TYPE);
    if (offset > os64_html_node_length(node)) return throw_status(ctx, OS64_HTML_INDEX_SIZE);
    const os64_html_node_t *start, *end;
    uint32_t start_offset, end_offset;
    point(r, 0, &start, &start_offset);
    point(r, 1, &end, &end_offset);
    bool both = d_tree_root(start) != d_tree_root(node) ||
                (which == 0 ? position(node, offset, end, end_offset) > 0
                            : position(node, offset, start, start_offset) < 0);
    int64_t status = os64_html_range_set(r->live, which, (os64_html_node_t *)node, offset);
    if (status == OS64_HTML_OK && both)
        status = os64_html_range_set(r->live, 1 - which, (os64_html_node_t *)node, offset);
    return status < 0 ? throw_status(ctx, status) : 0;
}

/* Both points to (node, offset): a node already checked. */
static int set_both(JSContext *ctx, DRange *r, const os64_html_node_t *node, uint32_t offset)
{
    int64_t status = os64_html_range_set(r->live, 0, (os64_html_node_t *)node, offset);
    if (status == OS64_HTML_OK) status = os64_html_range_set(r->live, 1, (os64_html_node_t *)node, offset);
    return status < 0 ? throw_status(ctx, status) : 0;
}

/* ── Containment (DOM § 5.5) ─────────────────────────────────────────── */

typedef struct {
    const os64_html_node_t *start, *end;
    uint32_t start_offset, end_offset;
} Bounds;

static Bounds bounds_of(const DRange *r)
{
    Bounds b;
    point(r, 0, &b.start, &b.start_offset);
    point(r, 1, &b.end, &b.end_offset);
    return b;
}

static bool contained(const Bounds *b, const os64_html_node_t *n)
{
    return d_tree_root(n) == d_tree_root(b->start) && position(n, 0, b->start, b->start_offset) > 0 &&
           position(n, os64_html_node_length(n), b->end, b->end_offset) < 0;
}

/* `n`'s ancestor-or-self whose parent is `common`, which is a proper
 * ancestor of `n`. */
static const os64_html_node_t *child_toward(const os64_html_node_t *common, const os64_html_node_t *n)
{
    while (n->parent != common) n = n->parent;
    return n;
}

/* ── Character data, cut in UTF-16 units ─────────────────────────────── */

/* The bytes of `count` units of `n`'s data from `offset` (DOM's
 * "substring data"). The offset was checked against the length when the
 * algorithm read it, but page code can run between (a clone upgrades a
 * custom element), so it is held to the length as it is now. */
static void substring(const os64_html_node_t *n, uint32_t offset, uint32_t count, const char **bytes, size_t *len)
{
    uint32_t length = os64_html_node_length(n);
    if (offset > length) offset = length;
    size_t from = os64_html_utf16_byte(n->text, n->text_len, offset);
    size_t to = count > length - offset ? n->text_len : os64_html_utf16_byte(n->text, n->text_len, offset + count);
    *bytes = n->text + from;
    *len = to - from;
}

static int replace_data(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *n, uint32_t offset,
                        uint32_t count)
{
    int64_t status = os64_page_node_replace_data(d_owner(dom, n)->state, (os64_html_node_t *)n, offset, count, "", 0);
    return status < 0 ? throw_status(ctx, status) : 0;
}

/* A shallow clone of character-data node `n` whose data is `count` units
 * of `n`'s from `offset`, appended to `parent`. */
static int clone_cut(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *n, uint32_t offset, uint32_t count,
                     const os64_html_node_t *parent, const os64_html_node_t *document)
{
    JSValue copy = d_node_clone(dom, ctx, n, false, document);
    if (JS_IsException(copy)) return -1;
    const os64_html_node_t *c = d_node(dom, ctx, copy);
    const char *bytes;
    size_t len;
    substring(n, offset, count, &bytes, &len);
    int64_t status = c != NULL ? os64_page_node_set_text(d_owner(dom, c)->state, (os64_html_node_t *)c, bytes, len)
                               : OS64_HTML_BAD_ARGUMENT;
    int done = status < 0 ? throw_status(ctx, status) : d_tree_change(dom, ctx, D_APPEND, parent, c, NULL);
    JS_FreeValue(ctx, copy);
    return done;
}

/* ── The content algorithms (DOM § 5.5) ──────────────────────────────── */

/* Holds kept on the nodes an algorithm works with while it moves others:
 * a clone upgrades custom elements, whose constructors are page code. */
typedef struct {
    os64_dom_t *dom;
    const os64_html_node_t **held;
    size_t count;
} Holds;

static void hold(Holds *h, const os64_html_node_t *n)
{
    if (n == NULL) return;
    os64_html_hold(h->dom->document, n);
    h->held[h->count++] = n;
}

static void holds_free(Holds *h)
{
    while (h->count > 0) os64_html_release(h->dom->document, h->held[--h->count]);
    d_free(h->dom, h->held);
}

/* New point for extract and delete (steps 5-6 of each): where the range
 * collapses once its contents are gone. */
static void collapse_point(const Bounds *b, const os64_html_node_t **node, uint32_t *offset)
{
    if (inclusive_ancestor(b->start, b->end)) {
        *node = b->start;
        *offset = b->start_offset;
        return;
    }
    const os64_html_node_t *reference = b->start;
    while (reference->parent != NULL && !inclusive_ancestor(reference->parent, b->end))
        reference = reference->parent;
    *node = reference->parent;
    *offset = index_of(reference) + 1;
}

/* Extract (`clone` false) or clone the contents (true) of `b`, into a new
 * fragment of the start node's document; its wrapper, or JS_EXCEPTION.
 * `r` is the range itself, collapsed at the end of an extract; NULL for
 * the subranges the algorithm recurses into, which are plain bounds (the
 * standard's live subrange is read once, at its start). */
static JSValue contents(os64_dom_t *dom, JSContext *ctx, DRange *r, const Bounds *b, bool clone)
{
    dom = d_owner(dom, b->start);
    const os64_html_node_t *document = d_node_document(dom, b->start);
    int64_t status = OS64_HTML_OK;
    os64_html_node_t *made = os64_html_create_fragment(dom->document, &status);
    if (made == NULL) return d_html_error(ctx, status);
    JSValue fragment = d_wrap_in(dom, ctx, made, document);
    if (JS_IsException(fragment) || (b->start == b->end && b->start_offset == b->end_offset)) return fragment;
    /* A subrange's points were read before a clone ran page code that may
     * have moved one; points in two trees hold nothing to take. */
    if (d_tree_root(b->start) != d_tree_root(b->end)) return fragment;
    if (b->start == b->end && is_data(b->start)) {
        uint32_t count = b->end_offset - b->start_offset;
        if (clone_cut(dom, ctx, b->start, b->start_offset, count, made, document) < 0 ||
            (!clone && replace_data(dom, ctx, b->start, b->start_offset, count) < 0)) {
            JS_FreeValue(ctx, fragment);
            return JS_EXCEPTION;
        }
        return fragment;
    }
    const os64_html_node_t *common = b->start;
    while (!inclusive_ancestor(common, b->end)) common = common->parent;
    const os64_html_node_t *first = inclusive_ancestor(b->start, b->end) ? NULL : child_toward(common, b->start);
    const os64_html_node_t *last = inclusive_ancestor(b->end, b->start) ? NULL : child_toward(common, b->end);
    size_t children = 0;
    for (const os64_html_node_t *c = common->first_child; c != NULL; c = c->next) children++;
    Holds h = {.dom = dom, .held = d_alloc_kind(dom, (children + 6) * sizeof(*h.held), OS64_DOM_MEM_RANGE)};
    if (h.held == NULL) {
        JS_FreeValue(ctx, fragment);
        return d_error(ctx, "QuotaExceededError", "DOM range quota exceeded");
    }
    hold(&h, b->start);
    hold(&h, b->end);
    hold(&h, common);
    hold(&h, first);
    hold(&h, last);
    size_t contained_from = h.count;
    for (const os64_html_node_t *c = common->first_child; c != NULL; c = c->next)
        if (contained(b, c)) {
            if (c->kind == OS64_HTML_DOCTYPE) {
                holds_free(&h);
                JS_FreeValue(ctx, fragment);
                return d_error(ctx, "HierarchyRequestError", "A range holding a doctype cannot be moved or copied");
            }
            hold(&h, c);
        }
    size_t contained_to = h.count;
    const os64_html_node_t *new_node = NULL;
    uint32_t new_offset = 0;
    if (!clone) collapse_point(b, &new_node, &new_offset);
    if (new_node != NULL) hold(&h, new_node);
    int failed = 0;
    if (first != NULL && is_data(first)) {
        uint32_t count = os64_html_node_length(b->start) - b->start_offset;
        failed = clone_cut(dom, ctx, b->start, b->start_offset, count, made, document) < 0 ||
                 (!clone && replace_data(dom, ctx, b->start, b->start_offset, count) < 0);
    } else if (first != NULL) {
        Bounds sub = {b->start, first, b->start_offset, os64_html_node_length(first)};
        failed = 1;
        JSValue copy = d_node_clone(dom, ctx, first, false, document);
        const os64_html_node_t *c = JS_IsException(copy) ? NULL : d_node(dom, ctx, copy);
        if (c != NULL && d_tree_change(dom, ctx, D_APPEND, made, c, NULL) == 0) {
            JSValue inner = contents(dom, ctx, NULL, &sub, clone);
            const os64_html_node_t *f = JS_IsException(inner) ? NULL : d_node(dom, ctx, inner);
            failed = f == NULL || d_tree_change(dom, ctx, D_APPEND, c, f, NULL) < 0;
            JS_FreeValue(ctx, inner);
        }
        JS_FreeValue(ctx, copy);
    }
    for (size_t i = contained_from; !failed && i < contained_to; i++) {
        if (!clone) {
            failed = d_tree_change(dom, ctx, D_APPEND, made, h.held[i], NULL) < 0;
            continue;
        }
        JSValue copy = d_node_clone(dom, ctx, h.held[i], true, document);
        const os64_html_node_t *c = JS_IsException(copy) ? NULL : d_node(dom, ctx, copy);
        failed = c == NULL || d_tree_change(dom, ctx, D_APPEND, made, c, NULL) < 0;
        JS_FreeValue(ctx, copy);
    }
    if (!failed && last != NULL && is_data(last)) {
        failed = clone_cut(dom, ctx, b->end, 0, b->end_offset, made, document) < 0 ||
                 (!clone && replace_data(dom, ctx, b->end, 0, b->end_offset) < 0);
    } else if (!failed && last != NULL) {
        Bounds sub = {last, b->end, 0, b->end_offset};
        failed = 1;
        JSValue copy = d_node_clone(dom, ctx, last, false, document);
        const os64_html_node_t *c = JS_IsException(copy) ? NULL : d_node(dom, ctx, copy);
        if (c != NULL && d_tree_change(dom, ctx, D_APPEND, made, c, NULL) == 0) {
            JSValue inner = contents(dom, ctx, NULL, &sub, clone);
            const os64_html_node_t *f = JS_IsException(inner) ? NULL : d_node(dom, ctx, inner);
            failed = f == NULL || d_tree_change(dom, ctx, D_APPEND, c, f, NULL) < 0;
            JS_FreeValue(ctx, inner);
        }
        JS_FreeValue(ctx, copy);
    }
    if (!failed && r != NULL && !clone) failed = set_both(ctx, r, new_node, new_offset) < 0;
    holds_free(&h);
    if (failed) {
        JS_FreeValue(ctx, fragment);
        return JS_EXCEPTION;
    }
    return fragment;
}

/* The contained nodes whose parent is not contained, in tree order: walked
 * from the first node after the start point, a contained subtree skipped
 * whole, until a node whose (node, 0) is not before the end. A contained
 * node the walk meets has a parent that is not: the start node and its
 * ancestors contain the start point, and a contained parent's subtree was
 * skipped. Counted when `h` is NULL, held into `h` otherwise. */
static size_t top_contained(const Bounds *b, Holds *h)
{
    size_t count = 0;
    /* The first node after the start point: the start's child at the
     * offset, else the node after the start's subtree. */
    const os64_html_node_t *n = is_data(b->start) ? NULL : b->start->first_child;
    for (uint32_t i = 0; n != NULL && i < b->start_offset; i++) n = n->next;
    if (n == NULL) {
        n = b->start;
        while (n != NULL && n->next == NULL) n = n->parent;
        n = n != NULL ? n->next : NULL;
    }
    while (n != NULL && position(n, 0, b->end, b->end_offset) < 0) {
        bool in = contained(b, n);
        if (in) {
            count++;
            if (h != NULL) hold(h, n);
        }
        if (!in && n->first_child != NULL) {
            n = n->first_child;
        } else {
            while (n != NULL && n->next == NULL) n = n->parent;
            n = n != NULL ? n->next : NULL;
        }
    }
    return count;
}

/* DOM § 5.5, deleteContents. */
static int delete_contents(os64_dom_t *dom, JSContext *ctx, DRange *r)
{
    Bounds b = bounds_of(r);
    if (b.start == b.end && b.start_offset == b.end_offset) return 0;
    if (b.start == b.end && is_data(b.start))
        return replace_data(dom, ctx, b.start, b.start_offset, b.end_offset - b.start_offset);
    Holds h = {.dom = dom, .held = d_alloc_kind(dom, (top_contained(&b, NULL) + 3) * sizeof(*h.held), OS64_DOM_MEM_RANGE)};
    if (h.held == NULL) {
        d_error(ctx, "QuotaExceededError", "DOM range quota exceeded");
        return -1;
    }
    hold(&h, b.start);
    hold(&h, b.end);
    size_t remove_from = h.count;
    top_contained(&b, &h);
    size_t remove_to = h.count;
    const os64_html_node_t *new_node;
    uint32_t new_offset;
    collapse_point(&b, &new_node, &new_offset);
    hold(&h, new_node);
    int failed = 0;
    if (is_data(b.start))
        failed = replace_data(dom, ctx, b.start, b.start_offset, os64_html_node_length(b.start) - b.start_offset) < 0;
    for (size_t i = remove_from; !failed && i < remove_to; i++)
        if (h.held[i]->parent != NULL)
            failed = d_tree_change(dom, ctx, D_REMOVE, h.held[i]->parent, h.held[i], NULL) < 0;
    if (!failed && is_data(b.end)) failed = replace_data(dom, ctx, b.end, 0, b.end_offset) < 0;
    if (!failed) failed = set_both(ctx, r, new_node, new_offset) < 0;
    holds_free(&h);
    return failed ? -1 : 0;
}

/* DOM § 5.5, "insert" a node into a range (insertNode, and
 * surroundContents' step 5). */
static int insert_node(os64_dom_t *dom, JSContext *ctx, DRange *r, const os64_html_node_t *node)
{
    Bounds b = bounds_of(r);
    const os64_html_node_t *start = b.start;
    if (start->kind == OS64_HTML_PI || start->kind == OS64_HTML_COMMENT ||
        (start->kind == OS64_HTML_TEXT && start->parent == NULL) || start == node) {
        d_error(ctx, "HierarchyRequestError", "A node cannot be inserted at this range's start");
        return -1;
    }
    const os64_html_node_t *reference = NULL;
    if (start->kind == OS64_HTML_TEXT) {
        reference = start;
    } else {
        reference = start->first_child;
        for (uint32_t i = 0; reference != NULL && i < b.start_offset; i++) reference = reference->next;
    }
    const os64_html_node_t *parent = reference == NULL ? start : reference->parent;
    /* Pre-insert validity comes before anything moves. The insertion
     * itself (d_tree_change, libhtml's insert) checks it against the
     * reference child before it moves anything, and takes the node out of
     * its old place as part of moving it — the standard's step 9 and its
     * pre-insert at once, so a refused node stays where it was. A Text
     * start splits first, so it is asked before the split: libhtml's verdict
     * for the end of `parent`, which is the reference child's for the
     * element or fragment a Text has for a parent (only a document's rules
     * hang on where among its children a node goes). */
    int64_t status = OS64_HTML_OK;
    if (start->kind == OS64_HTML_TEXT && (status = os64_html_may_insert(d_owner(dom, parent)->document, parent, node)) < 0)
        return throw_status(ctx, status);
    os64_html_hold(dom->document, node);
    os64_html_hold(dom->document, parent);
    int failed = 0;
    if (start->kind == OS64_HTML_TEXT) {
        os64_html_node_t *tail = NULL;
        status = os64_page_node_split_text(d_owner(dom, start)->state, (os64_html_node_t *)start, b.start_offset, &tail);
        if (status < 0) {
            failed = throw_status(ctx, status);
        } else {
            reference = tail;
        }
    }
    if (!failed && node == reference) reference = reference->next;
    if (reference != NULL) os64_html_hold(dom->document, reference);
    uint32_t new_offset = 0;
    if (!failed) {
        /* The offset after the node, counted as if it had already left its
         * old place (step 10 follows step 9's removal): one less when it
         * stood before the reference among `parent`'s own children. */
        new_offset = reference == NULL ? os64_html_node_length(parent) : index_of(reference);
        if (node->parent == parent && new_offset > 0 &&
            (reference == NULL || d_tree_precedes(node, reference)))
            new_offset--;
        if (node->kind == OS64_HTML_FRAGMENT) new_offset += os64_html_node_length(node);
        else new_offset++;
        failed = d_tree_change(dom, ctx, D_INSERT, parent, node, reference) < 0;
    }
    if (!failed) {
        Bounds now = bounds_of(r);
        if (now.start == now.end && now.start_offset == now.end_offset)
            failed = set_point(ctx, r, 1, parent, new_offset) < 0;
    }
    if (reference != NULL) os64_html_release(dom->document, reference);
    os64_html_release(dom->document, parent);
    os64_html_release(dom->document, node);
    return failed ? -1 : 0;
}

/* DOM § 5.5, surroundContents. */
static int surround(os64_dom_t *dom, JSContext *ctx, DRange *r, const os64_html_node_t *parent)
{
    Bounds b = bounds_of(r);
    /* A partially contained node: an inclusive ancestor of one point that
     * is none of the other's. Each must be a Text. */
    for (int side = 0; side < 2; side++)
        for (const os64_html_node_t *n = side == 0 ? b.start : b.end; n != NULL; n = n->parent)
            if (!inclusive_ancestor(n, side == 0 ? b.end : b.start) && n->kind != OS64_HTML_TEXT) {
                d_error(ctx, "InvalidStateError", "The range partially contains a node that is not text");
                return -1;
            }
    if (parent->kind == OS64_HTML_DOCUMENT || parent->kind == OS64_HTML_DOCTYPE ||
        parent->kind == OS64_HTML_FRAGMENT)
        return throw_status(ctx, OS64_HTML_INVALID_NODE_TYPE);
    os64_html_hold(dom->document, parent);
    int failed = 0;
    JSValue fragment = contents(dom, ctx, r, &b, false);
    const os64_html_node_t *f = JS_IsException(fragment) ? NULL : d_node(dom, ctx, fragment);
    if (f == NULL) failed = 1;
    if (!failed && parent->first_child != NULL)
        failed = d_tree_change(dom, ctx, D_REPLACE_CHILDREN, parent, NULL, NULL) < 0;
    if (!failed) failed = insert_node(dom, ctx, r, parent) < 0;
    if (!failed) failed = d_tree_change(dom, ctx, D_APPEND, parent, f, NULL) < 0;
    if (!failed) {
        if (parent->parent == NULL) {
            failed = throw_status(ctx, OS64_HTML_INVALID_NODE_TYPE);
        } else {
            uint32_t i = index_of(parent);
            failed = set_both(ctx, r, parent->parent, i) < 0 ||
                     os64_html_range_set(r->live, 1, (os64_html_node_t *)parent->parent, i + 1) < 0;
        }
    }
    JS_FreeValue(ctx, fragment);
    os64_html_release(dom->document, parent);
    return failed ? -1 : 0;
}

/* DOM § 5.5, the stringifier: the Text data the range holds, cut at its
 * ends. */
static JSValue stringify(os64_dom_t *dom, JSContext *ctx, const DRange *r)
{
    Bounds b = bounds_of(r);
    const char *bytes;
    size_t len;
    if (b.start == b.end && b.start->kind == OS64_HTML_TEXT) {
        substring(b.start, b.start_offset, b.end_offset - b.start_offset, &bytes, &len);
        return JS_NewStringLen(ctx, bytes, len);
    }
    /* Twice over the same walk: the length, then the bytes. */
    char *out = NULL;
    size_t total = 0;
    for (int pass = 0; pass < 2; pass++) {
        size_t at = 0;
        if (b.start->kind == OS64_HTML_TEXT) {
            substring(b.start, b.start_offset, os64_html_node_length(b.start) - b.start_offset, &bytes, &len);
            if (out != NULL) os64_memcpy(out + at, bytes, len);
            at += len;
        }
        const os64_html_node_t *n = b.start;
        for (;;) {
            if (n->first_child != NULL) {
                n = n->first_child;
            } else {
                while (n != NULL && n->next == NULL) n = n->parent;
                if (n == NULL) break;
                n = n->next;
            }
            if (position(n, 0, b.end, b.end_offset) >= 0) break;
            if (n->kind == OS64_HTML_TEXT && contained(&b, n)) {
                if (out != NULL) os64_memcpy(out + at, n->text, n->text_len);
                at += n->text_len;
            }
        }
        if (b.end->kind == OS64_HTML_TEXT) {
            substring(b.end, 0, b.end_offset, &bytes, &len);
            if (out != NULL) os64_memcpy(out + at, bytes, len);
            at += len;
        }
        if (pass == 0) {
            total = at;
            out = d_alloc_kind(dom, total + 1, OS64_DOM_MEM_RANGE);
            if (out == NULL) return d_error(ctx, "QuotaExceededError", "DOM range quota exceeded");
        }
    }
    JSValue result = JS_NewStringLen(ctx, out, total);
    d_free(dom, out);
    return result;
}

/* createContextualFragment (DOM Parsing § 7.1): the markup parsed with the
 * start node as context when it is an element, its parent element when it
 * is text or a comment, and a body otherwise, in place of an HTML
 * document's html (d_body_context), or for one in no namespace or another
 * one (d_fragment_context). Its scripts are not marked started (innerHTML's and
 * insertAdjacentHTML's are), so each runs when the fragment is inserted. */
static JSValue contextual_fragment(os64_dom_t *dom, JSContext *ctx, const DRange *r, const DString *markup)
{
    Bounds b = bounds_of(r);
    const os64_html_node_t *node = b.start, *element = NULL;
    dom = d_owner(dom, node);
    if (node->kind == OS64_HTML_ELEMENT)
        element = node;
    else if ((node->kind == OS64_HTML_TEXT || node->kind == OS64_HTML_COMMENT) && node->parent != NULL &&
             node->parent->kind == OS64_HTML_ELEMENT)
        element = node->parent;
    const os64_html_node_t *document = d_node_document(dom, node);
    int64_t status = OS64_HTML_OK;
    if (element == NULL ||
        (d_document_type(dom, document) == D_DOCUMENT_HTML && element->ns == OS64_HTML_NS_HTML &&
         element->tag == OS64_HTML_TAG_HTML)) {
        element = d_body_context(dom, &status);
        if (element == NULL) return d_html_error(ctx, status);
    } else {
        element = d_fragment_context(dom, element, &status);
        if (element == NULL) return d_html_error(ctx, status);
    }
    /* Scripting is the start node's document's: a substituted body is no
     * node of any document's tree to ask. */
    os64_html_node_t *fragment = os64_html_parse_fragment(dom->document, element, markup->data, markup->length,
                                                          d_scripting(dom, node), &status);
    if (fragment == NULL) return d_html_error(ctx, status);
    /* Initialize native ownership before exposing or upgrading the fragment. */
    JSValue wrapped = d_wrap_in(dom, ctx, fragment, document);
    if (JS_IsException(wrapped)) return wrapped;
    if (d_custom_upgrade_new(dom, ctx, fragment) < 0) {
        JS_FreeValue(ctx, wrapped);
        return JS_EXCEPTION;
    }
    return wrapped;
}

/* ── Members ─────────────────────────────────────────────────────────── */

enum {
    M_START_CONTAINER, M_START_OFFSET, M_END_CONTAINER, M_END_OFFSET, M_COLLAPSED,
    M_COMMON, M_SET_START, M_SET_END, M_START_BEFORE, M_START_AFTER, M_END_BEFORE, M_END_AFTER,
    M_COLLAPSE, M_SELECT, M_SELECT_CONTENTS, M_COMPARE, M_DELETE, M_EXTRACT, M_CLONE_CONTENTS,
    M_INSERT, M_SURROUND, M_CLONE_RANGE, M_DETACH, M_IN_RANGE, M_COMPARE_POINT, M_INTERSECTS,
    M_TO_STRING, M_CONTEXTUAL, M_CLIENT_RECTS, M_BOUNDING
};

/* ── A range's rects (CSSOM View § 6.1; RANGE.md § 7) ────────────────── */

void os64_dom_set_text_rects(os64_dom_t *dom, os64_dom_text_rects_provider_t provider, void *opaque)
{
    if (dom == NULL || dom->closed) return;
    dom->text_rects = provider;
    dom->text_rects_opaque = opaque;
}

typedef struct {
    os64_dom_t *dom;
    double *v;          /* x, y, width, height, four by four */
    size_t count, cap;
} Rects;

static int rects_room(Rects *r, size_t more)
{
    if (r->count + more <= r->cap) return 0;
    size_t cap = r->cap ? r->cap * 2 : 16;
    while (cap < r->count + more) cap *= 2;
    double *v = d_alloc_kind(r->dom, cap * 4 * sizeof(double), OS64_DOM_MEM_RANGE);
    if (v == NULL) return -1;
    if (r->v != NULL) {
        os64_memcpy(v, r->v, r->count * 4 * sizeof(double));
        d_free(r->dom, r->v);
    }
    r->v = v;
    r->cap = cap;
    return 0;
}

static uint64_t rect_add_sat(uint64_t a, uint64_t b)
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

/* The rects of bytes [from, to) of Text node `n`, from the host. */
static int text_rects(JSContext *ctx, Rects *r, const os64_html_node_t *n, size_t from, size_t to)
{
    os64_dom_t *dom = r->dom;
    if (dom->text_rects == NULL) return 0;
    double few[4 * 8];
    os64_dom_geometry_stats_t cost = {0};
    if (!d_provider_begin(dom, ctx)) return -1;
    size_t count = dom->text_rects(dom->text_rects_opaque, n, from, to, few, 8, &cost);
    d_provider_leave(dom);
    dom->geometry_stats.layouts = rect_add_sat(dom->geometry_stats.layouts, cost.layouts);
    dom->geometry_stats.elapsed_us = rect_add_sat(dom->geometry_stats.elapsed_us, cost.elapsed_us);
    if (!d_require_active(dom, ctx)) return -1;
    const double *source = few;
    double *more = NULL;
    if (count > 8) {
        more = d_alloc_kind(dom, count * 4 * sizeof(double), OS64_DOM_MEM_RANGE);
        if (more == NULL) {
            d_error(ctx, "QuotaExceededError", "DOM range quota exceeded");
            return -1;
        }
        os64_dom_geometry_stats_t again = {0};
        if (!d_provider_begin(dom, ctx)) { d_free(dom, more); return -1; }
        size_t got = dom->text_rects(dom->text_rects_opaque, n, from, to, more, count, &again);
        d_provider_leave(dom);
        dom->geometry_stats.layouts = rect_add_sat(dom->geometry_stats.layouts, again.layouts);
        dom->geometry_stats.elapsed_us = rect_add_sat(dom->geometry_stats.elapsed_us, again.elapsed_us);
        if (!d_require_active(dom, ctx)) { d_free(dom, more); return -1; }
        count = got < count ? got : count;
        source = more;
    }
    int result = rects_room(r, count) < 0 ? (d_error(ctx, "QuotaExceededError", "DOM range quota exceeded"), -1) : 0;
    if (result == 0) {
        os64_memcpy(r->v + r->count * 4, source, count * 4 * sizeof(double));
        r->count += count;
    }
    d_free(dom, more);
    return result;
}

/* The range's client rects in tree order: each element it selects whose
 * parent it does not select, that element's border box (an element's
 * getClientRects); each Text node it selects or partly selects, the rects
 * of the selected part of its text, a collapsed range's zero-width one
 * included. Generated content and markers name their element, not a Text
 * node, and add nothing. -1 with the exception thrown. */
static int range_rects(os64_dom_t *dom, JSContext *ctx, const DRange *range, Rects *r)
{
    Bounds b = bounds_of(range);
    const os64_html_node_t *n = b.start;
    bool end_done = false;
    for (;;) {
        if (n->kind == OS64_HTML_TEXT && (n == b.start || n == b.end || contained(&b, n))) {
            size_t from = n == b.start ? os64_html_utf16_byte(n->text, n->text_len, b.start_offset) : 0;
            size_t to = n == b.end ? os64_html_utf16_byte(n->text, n->text_len, b.end_offset) : n->text_len;
            if (to < from) to = from;
            if (text_rects(ctx, r, n, from, to) < 0) return -1;
            end_done = end_done || n == b.end;
        } else if (n->kind == OS64_HTML_ELEMENT && contained(&b, n) &&
                   (n->parent == NULL || !contained(&b, n->parent))) {
            os64_dom_geometry_t box;
            if (!d_geometry_read(dom, ctx, n, &box)) return -1;
            if (box.boxed) {
                if (rects_room(r, 1) < 0) {
                    d_error(ctx, "QuotaExceededError", "DOM range quota exceeded");
                    return -1;
                }
                double *v = r->v + r->count++ * 4;
                v[0] = box.x;
                v[1] = box.y;
                v[2] = box.width;
                v[3] = box.height;
            }
        }
        if (n->first_child != NULL) {
            n = n->first_child;
        } else {
            while (n != NULL && n->next == NULL) n = n->parent;
            if (n == NULL) break;
            n = n->next;
        }
        if (position(n, 0, b.end, b.end_offset) >= 0) break;
    }
    /* An end Text node at offset 0 is never reached by the walk, which
     * stops at a node not before the end: it is partly selected all the
     * same, its zero-width rect the answer. */
    if (!end_done && b.end->kind == OS64_HTML_TEXT && b.end != b.start && b.end_offset == 0)
        return text_rects(ctx, r, b.end, 0, 0);
    return 0;
}

/* getBoundingClientRect: the union of the rects with width and height, the
 * first rect when none has both, a zero rect when there are none. */
static JSValue bounding(os64_dom_t *dom, JSContext *ctx, const Rects *r)
{
    if (r->count == 0) return d_rect_new(dom, ctx, false, 0, 0, 0, 0);
    double left = 0, top = 0, right = 0, bottom = 0;
    bool any = false;
    for (size_t i = 0; i < r->count; i++) {
        const double *v = r->v + i * 4;
        if (v[2] == 0 || v[3] == 0) continue;
        if (!any || v[0] < left) left = v[0];
        if (!any || v[1] < top) top = v[1];
        if (!any || v[0] + v[2] > right) right = v[0] + v[2];
        if (!any || v[1] + v[3] > bottom) bottom = v[1] + v[3];
        any = true;
    }
    if (!any) return d_rect_new(dom, ctx, false, r->v[0], r->v[1], r->v[2], r->v[3]);
    return d_rect_new(dom, ctx, false, left, top, right - left, bottom - top);
}

static int offset_argument(JSContext *ctx, int argc, JSValueConst *argv, int at, uint32_t *offset)
{
    if (argc <= at) {
        JS_ThrowTypeError(ctx, "Missing offset argument");
        return -1;
    }
    return JS_ToUint32(ctx, offset, argv[at]);
}

/* A node argument. An Attr is a Node, and the standard lets a range sit in
 * one (its root is the Attr, its length 0); libhtml holds an attribute as
 * its element's, not as a node, so a range here cannot (DEBTS.md § Classic
 * browser scripting). An Attr argument comes back as NULL with `*attr` set
 * and nothing thrown, for each member to answer as the standard does where
 * that needs no such range. */
static const os64_html_node_t *node_argument(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv, int at,
                                             bool *attr)
{
    *attr = false;
    if (argc <= at) {
        JS_ThrowTypeError(ctx, "Missing node argument");
        return NULL;
    }
    DAttrView view;
    int is_attr = d_attr_view(dom, ctx, argv[at], &view);
    if (is_attr < 0) return NULL;
    if (is_attr == 1) {
        *attr = true;
        return NULL;
    }
    return d_node(dom, ctx, argv[at]);
}

/* A member given an Attr: the answer that needs no range rooted at it. */
static JSValue attr_answer(os64_dom_t *dom, JSContext *ctx, DRange *r, int magic, int argc, JSValueConst *argv)
{
    uint32_t offset = 0;
    switch (magic) {
    case M_SET_START: case M_SET_END:
        if (offset_argument(ctx, argc, argv, 1, &offset) < 0) return JS_EXCEPTION;
        if (offset > 0) return throw_status(ctx, OS64_HTML_INDEX_SIZE), JS_EXCEPTION;
        return d_error(ctx, "NotSupportedError", "A range in an Attr is not supported");
    case M_SELECT_CONTENTS:
        return d_error(ctx, "NotSupportedError", "A range in an Attr is not supported");
    case M_START_BEFORE: case M_START_AFTER: case M_END_BEFORE: case M_END_AFTER: case M_SELECT:
        return throw_status(ctx, OS64_HTML_INVALID_NODE_TYPE), JS_EXCEPTION;
    case M_INSERT:
        return d_error(ctx, "HierarchyRequestError", "An Attr cannot be inserted");
    case M_SURROUND: {
        /* The standard's order: the checks a non-Attr parent passes too,
         * the contents extracted, then the insertion refused. */
        Bounds b = bounds_of(r);
        for (int side = 0; side < 2; side++)
            for (const os64_html_node_t *n = side == 0 ? b.start : b.end; n != NULL; n = n->parent)
                if (!inclusive_ancestor(n, side == 0 ? b.end : b.start) && n->kind != OS64_HTML_TEXT)
                    return d_error(ctx, "InvalidStateError", "The range partially contains a node that is not text");
        JSValue fragment = contents(dom, ctx, r, &b, false);
        if (JS_IsException(fragment)) return fragment;
        JS_FreeValue(ctx, fragment);
        return d_error(ctx, "HierarchyRequestError", "An Attr cannot be inserted");
    }
    case M_INTERSECTS: return JS_FALSE;
    case M_IN_RANGE: case M_COMPARE_POINT:
        /* An Attr is a root of its own, never this range's. */
        if (offset_argument(ctx, argc, argv, 1, &offset) < 0) return JS_EXCEPTION;
        return magic == M_IN_RANGE ? JS_FALSE : d_error(ctx, "WrongDocumentError", "The point is in another tree");
    default: return JS_UNDEFINED;
    }
}

static JSValue range_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                            JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DRange *r = range_of(dom, ctx, self, magic > M_COLLAPSED);
    if (r == NULL) return JS_EXCEPTION;
    Bounds b = bounds_of(r);
    switch (magic) {
    case M_START_CONTAINER: return d_wrap(dom, ctx, b.start);
    case M_START_OFFSET: return JS_NewUint32(ctx, b.start_offset);
    case M_END_CONTAINER: return d_wrap(dom, ctx, b.end);
    case M_END_OFFSET: return JS_NewUint32(ctx, b.end_offset);
    case M_COLLAPSED: return JS_NewBool(ctx, b.start == b.end && b.start_offset == b.end_offset);
    case M_COMMON: {
        const os64_html_node_t *container = b.start;
        while (!inclusive_ancestor(container, b.end)) container = container->parent;
        return d_wrap(dom, ctx, container);
    }
    case M_DETACH: return JS_UNDEFINED;
    case M_TO_STRING: return stringify(dom, ctx, r);
    case M_CLIENT_RECTS: case M_BOUNDING: {
        Rects rects = {.dom = dom};
        JSValue result = range_rects(dom, ctx, r, &rects) < 0 ? JS_EXCEPTION
                       : magic == M_CLIENT_RECTS ? d_rect_list_new(dom, ctx, rects.v, rects.count)
                                                 : bounding(dom, ctx, &rects);
        d_free(dom, rects.v);
        return result;
    }
    case M_CLONE_RANGE: {
        JSValue copy = range_new(dom, ctx, JS_UNDEFINED, dom->document->document);
        DRange *c = JS_IsException(copy) ? NULL : JS_GetOpaque(copy, dom->ranges->range_class);
        if (c != NULL) {
            (void)os64_html_range_set(c->live, 0, (os64_html_node_t *)b.start, b.start_offset);
            (void)os64_html_range_set(c->live, 1, (os64_html_node_t *)b.end, b.end_offset);
        }
        return copy;
    }
    case M_DELETE: return delete_contents(dom, ctx, r) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    case M_EXTRACT: return contents(dom, ctx, r, &b, false);
    case M_CLONE_CONTENTS: return contents(dom, ctx, r, &b, true);
    case M_COLLAPSE: {
        bool to_start = argc > 0 && JS_ToBool(ctx, argv[0]);
        return (to_start ? set_both(ctx, r, b.start, b.start_offset) : set_both(ctx, r, b.end, b.end_offset)) < 0
            ? JS_EXCEPTION : JS_UNDEFINED;
    }
    case M_COMPARE: {
        if (argc < 2) return JS_ThrowTypeError(ctx, "Missing argument");
        int32_t how32;
        if (JS_ToInt32(ctx, &how32, argv[0]) < 0) return JS_EXCEPTION;
        DRange *source = range_of(dom, ctx, argv[1], true);
        if (source == NULL) return JS_EXCEPTION;
        uint16_t how = (uint16_t)how32;
        if (how > 3) return d_error(ctx, "NotSupportedError", "The comparison is none of the four");
        b = bounds_of(r);
        Bounds o = bounds_of(source);
        if (d_tree_root(b.start) != d_tree_root(o.start))
            return d_error(ctx, "WrongDocumentError", "The ranges are in different trees");
        /* START_TO_START 0, START_TO_END 1, END_TO_END 2, END_TO_START 3:
         * this range's point first, the source's second. */
        bool mine_end = how == 1 || how == 2, theirs_end = how == 2 || how == 3;
        return JS_NewInt32(ctx, position(mine_end ? b.end : b.start, mine_end ? b.end_offset : b.start_offset,
                                         theirs_end ? o.end : o.start, theirs_end ? o.end_offset : o.start_offset));
    }
    default: break;
    }
    if (magic == M_CONTEXTUAL) {
        if (argc < 1) return JS_ThrowTypeError(ctx, "Missing markup argument");
        DString markup = {0};
        if (!d_string(dom, ctx, argv[0], &markup)) return JS_EXCEPTION;
        JSValue result = contextual_fragment(dom, ctx, r, &markup);
        d_string_free(dom, &markup);
        return result;
    }
    bool attr;
    const os64_html_node_t *node = node_argument(dom, ctx, argc, argv, 0, &attr);
    if (attr) return attr_answer(dom, ctx, r, magic, argc, argv);
    if (node == NULL) return JS_EXCEPTION;
    uint32_t offset = 0;
    switch (magic) {
    case M_SET_START: case M_SET_END:
        if (offset_argument(ctx, argc, argv, 1, &offset) < 0) return JS_EXCEPTION;
        return set_point(ctx, r, magic == M_SET_END, node, offset) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    case M_START_BEFORE: case M_START_AFTER: case M_END_BEFORE: case M_END_AFTER:
        if (node->parent == NULL) return throw_status(ctx, OS64_HTML_INVALID_NODE_TYPE), JS_EXCEPTION;
        offset = index_of(node) + (magic == M_START_AFTER || magic == M_END_AFTER);
        return set_point(ctx, r, magic == M_END_BEFORE || magic == M_END_AFTER, node->parent, offset) < 0
            ? JS_EXCEPTION : JS_UNDEFINED;
    case M_SELECT: {
        if (node->parent == NULL) return throw_status(ctx, OS64_HTML_INVALID_NODE_TYPE), JS_EXCEPTION;
        uint32_t i = index_of(node);
        int64_t status = os64_html_range_set(r->live, 0, (os64_html_node_t *)node->parent, i);
        if (status == OS64_HTML_OK)
            status = os64_html_range_set(r->live, 1, (os64_html_node_t *)node->parent, i + 1);
        return status < 0 ? (throw_status(ctx, status), JS_EXCEPTION) : JS_UNDEFINED;
    }
    case M_SELECT_CONTENTS: {
        if (node->kind == OS64_HTML_DOCTYPE) return throw_status(ctx, OS64_HTML_INVALID_NODE_TYPE), JS_EXCEPTION;
        int64_t status = os64_html_range_set(r->live, 0, (os64_html_node_t *)node, 0);
        if (status == OS64_HTML_OK)
            status = os64_html_range_set(r->live, 1, (os64_html_node_t *)node, os64_html_node_length(node));
        return status < 0 ? (throw_status(ctx, status), JS_EXCEPTION) : JS_UNDEFINED;
    }
    case M_INSERT: return insert_node(dom, ctx, r, node) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    case M_SURROUND: return surround(dom, ctx, r, node) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    case M_INTERSECTS: {
        b = bounds_of(r);
        if (d_tree_root(node) != d_tree_root(b.start)) return JS_FALSE;
        if (node->parent == NULL) return JS_TRUE;
        uint32_t i = index_of(node);
        return JS_NewBool(ctx, position(node->parent, i, b.end, b.end_offset) < 0 &&
                                   position(node->parent, i + 1, b.start, b.start_offset) > 0);
    }
    case M_IN_RANGE: case M_COMPARE_POINT: {
        if (offset_argument(ctx, argc, argv, 1, &offset) < 0) return JS_EXCEPTION;
        b = bounds_of(r);
        if (d_tree_root(node) != d_tree_root(b.start))
            return magic == M_IN_RANGE ? JS_FALSE
                                       : d_error(ctx, "WrongDocumentError", "The point is in another tree");
        if (node->kind == OS64_HTML_DOCTYPE) return throw_status(ctx, OS64_HTML_INVALID_NODE_TYPE), JS_EXCEPTION;
        if (offset > os64_html_node_length(node)) return throw_status(ctx, OS64_HTML_INDEX_SIZE), JS_EXCEPTION;
        int where = position(node, offset, b.start, b.start_offset) < 0 ? -1
                  : position(node, offset, b.end, b.end_offset) > 0 ? 1 : 0;
        return magic == M_IN_RANGE ? JS_NewBool(ctx, where == 0) : JS_NewInt32(ctx, where);
    }
    default: return JS_UNDEFINED;
    }
}

/* ── Constructors ────────────────────────────────────────────────────── */

/* new Range(): both points at (the window's document, 0). */
static JSValue range_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL || dom->ranges == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSValue object = range_new(dom, ctx, prototype, dom->document->document);
    JS_FreeValue(ctx, prototype);
    return object;
}

/* document.createRange(): both points at (this document, 0). */
static JSValue create_range(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                            JSValue *data)
{
    (void)argc; (void)argv; (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *document = d_node(dom, ctx, self);
    if (document == NULL) return JS_EXCEPTION;
    if (document->kind != OS64_HTML_DOCUMENT) return JS_ThrowTypeError(ctx, "Expected a document");
    return range_new(dom, ctx, JS_UNDEFINED, document);
}

/* One member of StaticRangeInit, required. */
static int init_member(os64_dom_t *dom, JSContext *ctx, JSValueConst init, const char *name, bool node,
                       const os64_html_node_t **out_node, uint32_t *out_offset, bool *attr)
{
    JSValue value = JS_GetPropertyStr(ctx, init, name);
    if (JS_IsException(value)) return -1;
    int result = 0;
    if (JS_IsUndefined(value)) {
        JS_ThrowTypeError(ctx, "StaticRangeInit's %s is required", name);
        result = -1;
    } else if (node) {
        DAttrView view;
        int is_attr = d_attr_view(dom, ctx, value, &view);
        if (is_attr < 0) result = -1;
        else if (is_attr == 1) *attr = true;
        else if ((*out_node = d_node(dom, ctx, value)) == NULL) result = -1;
    } else if (JS_ToUint32(ctx, out_offset, value) < 0) {
        result = -1;
    }
    JS_FreeValue(ctx, value);
    return result;
}

/* new StaticRange(init) (DOM § 5.4): the dictionary's members read in
 * WebIDL's order (by name), each required; a doctype or an Attr refused;
 * offsets not checked against lengths, as the standard says. */
static JSValue static_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL || dom->ranges == NULL) return JS_EXCEPTION;
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "StaticRange needs a StaticRangeInit");
    const os64_html_node_t *start = NULL, *end = NULL;
    uint32_t start_offset = 0, end_offset = 0;
    bool attr = false;
    if (init_member(dom, ctx, argv[0], "endContainer", true, &end, NULL, &attr) < 0 ||
        init_member(dom, ctx, argv[0], "endOffset", false, NULL, &end_offset, &attr) < 0 ||
        init_member(dom, ctx, argv[0], "startContainer", true, &start, NULL, &attr) < 0 ||
        init_member(dom, ctx, argv[0], "startOffset", false, NULL, &start_offset, &attr) < 0)
        return JS_EXCEPTION;
    if (attr || start->kind == OS64_HTML_DOCTYPE || end->kind == OS64_HTML_DOCTYPE)
        return throw_status(ctx, OS64_HTML_INVALID_NODE_TYPE), JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    struct DRanges *s = dom->ranges;
    d_ranges_sweep(dom);
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, s->static_class)
                                            : JS_NewObjectClass(ctx, (int)s->static_class);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object)) return object;
    DRange *r = record_new(dom, ctx, R_STATIC);
    if (r == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    os64_html_hold(dom->document, start);
    os64_html_hold(dom->document, end);
    r->node[0] = start;
    r->node[1] = end;
    r->offset[0] = start_offset;
    r->offset[1] = end_offset;
    JS_SetOpaque(object, r);
    return object;
}

static void record_dies(DRange *r)
{
    if (r == NULL) return;
    r->dead = true;
    if (r->dom->ranges != NULL) r->dom->ranges->has_dead = true;
}

static void range_finalizer(JSRuntime *rt, JSValue value)
{
    (void)rt;
    record_dies(JS_GetOpaque(value, os64_js_class_id(&range_slot)));
}

static void static_finalizer(JSRuntime *rt, JSValue value)
{
    (void)rt;
    record_dies(JS_GetOpaque(value, os64_js_class_id(&static_slot)));
}

/* ── The page Selection, a model (RANGE.md § 9; the Selection API) ───── */

/* With ranges, window.getSelection()'s Selection holds zero or one Range
 * and a direction, and its members are the Selection API's, on the
 * prototype selection.c made (which leaves its "None" members off then).
 * Setting the selection makes a new Range at the window's document (the standard's
 * "set the selection's range"), except addRange, which keeps the page's
 * own. Its anchor is the range's start and its focus the end, the other way
 * round when backward. yonder draws no page-text selection and fires no
 * selectionchange (DEBTS.md): a script's selection is a model only, which
 * is what the scripts that save and restore one around their own edits
 * ask of it. */

enum {
    S_ANCHOR_NODE, S_ANCHOR_OFFSET, S_FOCUS_NODE, S_FOCUS_OFFSET, S_COLLAPSED, S_COUNT, S_TYPE, S_DIRECTION,
    S_GET, S_ADD, S_REMOVE, S_REMOVE_ALL, S_COLLAPSE, S_TO_START, S_TO_END, S_EXTEND, S_BASE_EXTENT,
    S_SELECT_ALL, S_DELETE, S_CONTAINS, S_TO_STRING
};

static JSValue selection_range(os64_dom_t *dom, JSContext *ctx)
{
    return JS_GetPropertyStr(ctx, dom->ranges->held->value, "r");
}

/* The selection as it stands into `range` (a reference the caller frees;
 * the one it held is freed first), its Range, its bounds and whether it is
 * backward. A member reads it again after it converts its arguments: an
 * offset's valueOf is the page's code, which may change the selection or
 * let go of the nodes the last read named. 0, or -1 thrown. */
static int selection_read(os64_dom_t *dom, JSContext *ctx, JSValue *range, DRange **r, Bounds *b, bool *backward)
{
    JS_FreeValue(ctx, *range);
    *range = selection_range(dom, ctx);
    *r = NULL;
    *b = (Bounds){0};
    *backward = dom->ranges->direction == D_DIR_BACKWARD;
    if (JS_IsException(*range)) {
        *range = JS_UNDEFINED;
        return -1;
    }
    if (!JS_IsUndefined(*range)) *r = JS_GetOpaque(*range, dom->ranges->range_class);
    if (*r != NULL) *b = bounds_of(*r);
    return 0;
}

/* The selection's range becomes `range` (a Range object, taken; undefined
 * for none) and its direction `direction`. */
static int selection_set(os64_dom_t *dom, JSContext *ctx, JSValue range, uint8_t direction)
{
    dom->ranges->direction = JS_IsUndefined(range) ? D_DIR_NONE : direction;
    return JS_SetPropertyStr(ctx, dom->ranges->held->value, "r", range) < 0 ? -1 : 0;
}

/* A new Range at the window's document with both points given; JS_EXCEPTION
 * with the set's refusal thrown. */
static JSValue selection_new_range(os64_dom_t *dom, JSContext *ctx, const os64_html_node_t *sn, uint32_t so,
                                   const os64_html_node_t *en, uint32_t eo)
{
    JSValue object = range_new(dom, ctx, JS_UNDEFINED, dom->document->document);
    DRange *r = JS_IsException(object) ? NULL : JS_GetOpaque(object, dom->ranges->range_class);
    if (r == NULL) return object;
    if (set_both(ctx, r, sn, so) < 0 || (en != sn || eo != so ? set_point(ctx, r, 1, en, eo) : 0) < 0) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
}

/* A point a member is handed, converted: its node, NULL for an Attr (no
 * node of the tree, and so never in the window's document), and its
 * offset. Converting an offset runs the page's valueOf, which may change
 * any node, so a member converts every argument before it reads a length
 * or the selection, and checks them in its own order, as the standard
 * does. 0, or -1 thrown. */
static int selection_take(os64_dom_t *dom, JSContext *ctx, JSValueConst node_value, JSValueConst offset_value,
                          const os64_html_node_t **node, uint32_t *offset)
{
    DAttrView view;
    int attr = d_attr_view(dom, ctx, node_value, &view);
    if (attr < 0) return -1;
    *node = NULL;
    if (attr != 1 && (*node = d_node(dom, ctx, node_value)) == NULL) return -1;
    *offset = 0;
    if (!JS_IsUndefined(offset_value) && JS_ToUint32(ctx, offset, offset_value) < 0) return -1;
    return 0;
}

/* A taken node's length as it stands: an Attr's is 0. */
static uint32_t selection_length(const os64_html_node_t *node)
{
    return node != NULL ? os64_html_node_length(node) : 0;
}

/* Whether the window's document is a shadow-including inclusive ancestor
 * of `node`: a member handed a node anywhere else does nothing. */
static bool selection_in_document(os64_dom_t *dom, const os64_html_node_t *node)
{
    return node != NULL && os64_html_shadow_including_root(node) == dom->document->document;
}

/* The checks "set the start or end" makes of a point, for a member about
 * to set one: 0, or -1 thrown. */
static int selection_settable(JSContext *ctx, const os64_html_node_t *node, uint32_t offset)
{
    if (node != NULL && node->kind == OS64_HTML_DOCTYPE) return throw_status(ctx, OS64_HTML_INVALID_NODE_TYPE);
    return offset > selection_length(node) ? throw_status(ctx, OS64_HTML_INDEX_SIZE) : 0;
}

static JSValue selection_ranges_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                                       JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (!d_same_object(self, dom->selection)) return JS_ThrowTypeError(ctx, "Expected a Selection");
    JSValue range = JS_UNDEFINED;
    DRange *r;
    Bounds b;
    bool backward;
    if (selection_read(dom, ctx, &range, &r, &b, &backward) < 0) return JS_EXCEPTION;
    JSValue result = JS_UNDEFINED;
    switch (magic) {
    case S_ANCHOR_NODE: case S_FOCUS_NODE: case S_ANCHOR_OFFSET: case S_FOCUS_OFFSET: {
        /* Null and 0 for a point not in the document tree: the range is
         * the page's own after addRange, and the page may move it. */
        bool anchor = magic == S_ANCHOR_NODE || magic == S_ANCHOR_OFFSET;
        const os64_html_node_t *n = r == NULL ? NULL : anchor != backward ? b.start : b.end;
        uint32_t o = anchor != backward ? b.start_offset : b.end_offset;
        if (n != NULL && d_tree_root(n) != dom->document->document) n = NULL;
        result = magic == S_ANCHOR_NODE || magic == S_FOCUS_NODE ? n == NULL ? JS_NULL : d_wrap(dom, ctx, n)
                                                                 : JS_NewUint32(ctx, n == NULL ? 0 : o);
        break;
    }
    case S_COLLAPSED: result = JS_NewBool(ctx, r == NULL || (b.start == b.end && b.start_offset == b.end_offset)); break;
    case S_COUNT: result = JS_NewUint32(ctx, r != NULL); break;
    case S_TYPE:
        result = JS_NewString(ctx, r == NULL ? "None" : b.start == b.end && b.start_offset == b.end_offset ? "Caret"
                                                                                                          : "Range");
        break;
    case S_DIRECTION:
        result = JS_NewString(ctx, dom->ranges->direction == D_DIR_FORWARD ? "forward"
                                 : dom->ranges->direction == D_DIR_BACKWARD ? "backward" : "none");
        break;
    case S_GET: {
        uint32_t index;
        if (argc < 1) { result = JS_ThrowTypeError(ctx, "Missing index"); break; }
        if (JS_ToUint32(ctx, &index, argv[0]) < 0 || selection_read(dom, ctx, &range, &r, &b, &backward) < 0) {
            result = JS_EXCEPTION;
            break;
        }
        result = r == NULL || index != 0 ? d_error(ctx, "IndexSizeError", "The selection holds no such range")
                                         : JS_DupValue(ctx, range);
        break;
    }
    case S_ADD: {
        /* addRange: a range of the window document's tree, kept as it is,
         * when the selection holds none; otherwise nothing. */
        DRange *given = argc > 0 ? JS_GetOpaque(argv[0], dom->ranges->range_class) : NULL;
        if (given == NULL) { result = JS_ThrowTypeError(ctx, "addRange needs a Range"); break; }
        Bounds g = bounds_of(given);
        if (r == NULL && d_tree_root(g.start) == dom->document->document &&
            selection_set(dom, ctx, JS_DupValue(ctx, argv[0]), D_DIR_FORWARD) < 0)
            result = JS_EXCEPTION;
        break;
    }
    case S_REMOVE: {
        DRange *given = argc > 0 ? JS_GetOpaque(argv[0], dom->ranges->range_class) : NULL;
        if (given == NULL) { result = JS_ThrowTypeError(ctx, "removeRange needs a Range"); break; }
        if (given != r) result = d_error(ctx, "NotFoundError", "The range is not the selection's");
        else if (selection_set(dom, ctx, JS_UNDEFINED, D_DIR_NONE) < 0) result = JS_EXCEPTION;
        break;
    }
    case S_REMOVE_ALL:
        if (selection_set(dom, ctx, JS_UNDEFINED, D_DIR_NONE) < 0) result = JS_EXCEPTION;
        break;
    case S_COLLAPSE: {
        /* collapse(node, offset) and setPosition: null empties it, once
         * the offset is converted as every argument is. */
        if (argc < 1) { result = JS_ThrowTypeError(ctx, "Missing node"); break; }
        if (JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
            uint32_t unused;
            if ((argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToUint32(ctx, &unused, argv[1]) < 0) ||
                selection_set(dom, ctx, JS_UNDEFINED, D_DIR_NONE) < 0)
                result = JS_EXCEPTION;
            break;
        }
        const os64_html_node_t *n;
        uint32_t o;
        if (selection_take(dom, ctx, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED, &n, &o) < 0 ||
            selection_settable(ctx, n, o) < 0) {
            result = JS_EXCEPTION;
            break;
        }
        if (!selection_in_document(dom, n)) break;
        JSValue made = selection_new_range(dom, ctx, n, o, n, o);
        if (JS_IsException(made) || selection_set(dom, ctx, made, D_DIR_FORWARD) < 0) result = JS_EXCEPTION;
        break;
    }
    case S_TO_START: case S_TO_END: {
        if (r == NULL) { result = d_error(ctx, "InvalidStateError", "The selection is empty"); break; }
        const os64_html_node_t *n = magic == S_TO_START ? b.start : b.end;
        uint32_t o = magic == S_TO_START ? b.start_offset : b.end_offset;
        JSValue made = selection_new_range(dom, ctx, n, o, n, o);
        if (JS_IsException(made) || selection_set(dom, ctx, made, D_DIR_FORWARD) < 0) result = JS_EXCEPTION;
        break;
    }
    case S_EXTEND: {
        if (argc < 1) { result = JS_ThrowTypeError(ctx, "Missing node"); break; }
        const os64_html_node_t *n;
        uint32_t o;
        if (selection_take(dom, ctx, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED, &n, &o) < 0 ||
            selection_read(dom, ctx, &range, &r, &b, &backward) < 0) {
            result = JS_EXCEPTION;
            break;
        }
        if (!selection_in_document(dom, n)) break;
        if (r == NULL) { result = d_error(ctx, "InvalidStateError", "The selection is empty"); break; }
        if (selection_settable(ctx, n, o) < 0) { result = JS_EXCEPTION; break; }
        const os64_html_node_t *an = backward ? b.end : b.start;
        uint32_t ao = backward ? b.end_offset : b.start_offset;
        bool focus_first = d_tree_root(an) == d_tree_root(n) && position(n, o, an, ao) < 0;
        JSValue made = d_tree_root(an) != d_tree_root(n) ? selection_new_range(dom, ctx, n, o, n, o)
                     : focus_first ? selection_new_range(dom, ctx, n, o, an, ao)
                                   : selection_new_range(dom, ctx, an, ao, n, o);
        if (JS_IsException(made) || selection_set(dom, ctx, made, focus_first ? D_DIR_BACKWARD : D_DIR_FORWARD) < 0)
            result = JS_EXCEPTION;
        break;
    }
    case S_BASE_EXTENT: {
        if (argc < 4) { result = JS_ThrowTypeError(ctx, "setBaseAndExtent needs four arguments"); break; }
        const os64_html_node_t *an, *fn;
        uint32_t ao, fo;
        if (selection_take(dom, ctx, argv[0], argv[1], &an, &ao) < 0 ||
            selection_take(dom, ctx, argv[2], argv[3], &fn, &fo) < 0) {
            result = JS_EXCEPTION;
            break;
        }
        if (ao > selection_length(an) || fo > selection_length(fn)) {
            throw_status(ctx, OS64_HTML_INDEX_SIZE);
            result = JS_EXCEPTION;
            break;
        }
        if (!selection_in_document(dom, an) || !selection_in_document(dom, fn)) break;
        if (selection_settable(ctx, an, ao) < 0 || selection_settable(ctx, fn, fo) < 0) {
            result = JS_EXCEPTION;
            break;
        }
        bool focus_first = d_tree_root(an) == d_tree_root(fn) && position(fn, fo, an, ao) < 0;
        JSValue made = focus_first ? selection_new_range(dom, ctx, fn, fo, an, ao)
                                   : selection_new_range(dom, ctx, an, ao, fn, fo);
        if (JS_IsException(made) || selection_set(dom, ctx, made, focus_first ? D_DIR_BACKWARD : D_DIR_FORWARD) < 0)
            result = JS_EXCEPTION;
        break;
    }
    case S_SELECT_ALL: {
        if (argc < 1) { result = JS_ThrowTypeError(ctx, "Missing node"); break; }
        const os64_html_node_t *n;
        uint32_t o;
        if (selection_take(dom, ctx, argv[0], JS_UNDEFINED, &n, &o) < 0 || selection_settable(ctx, n, 0) < 0) {
            result = JS_EXCEPTION;
            break;
        }
        /* The node's own root, not its shadow-including one: a node in a
         * shadow tree is not selected, unlike collapse's point. */
        if (n == NULL || d_tree_root(n) != dom->document->document) break;
        uint32_t children = 0;
        for (const os64_html_node_t *c = n->first_child; c != NULL; c = c->next) children++;
        JSValue made = selection_new_range(dom, ctx, n, 0, n, children);
        if (JS_IsException(made) || selection_set(dom, ctx, made, D_DIR_FORWARD) < 0) result = JS_EXCEPTION;
        break;
    }
    case S_DELETE:
        if (r != NULL && delete_contents(dom, ctx, r) < 0) result = JS_EXCEPTION;
        break;
    case S_CONTAINS: {
        if (argc < 1) { result = JS_ThrowTypeError(ctx, "Missing node"); break; }
        /* False when empty or when the node's root is not the document
         * (an Attr's root is itself). */
        const os64_html_node_t *n;
        uint32_t o;
        if (selection_take(dom, ctx, argv[0], JS_UNDEFINED, &n, &o) < 0) { result = JS_EXCEPTION; break; }
        bool partial = argc > 1 && JS_ToBool(ctx, argv[1]);
        if (r == NULL || n == NULL || d_tree_root(n) != dom->document->document) { result = JS_FALSE; break; }
        uint32_t length = selection_length(n);
        result = JS_NewBool(ctx, partial
            ? position(b.start, b.start_offset, n, length) <= 0 && position(b.end, b.end_offset, n, 0) >= 0
            : position(b.start, b.start_offset, n, 0) <= 0 && position(b.end, b.end_offset, n, length) >= 0);
        break;
    }
    case S_TO_STRING: result = r == NULL ? JS_NewString(ctx, "") : stringify(dom, ctx, r); break;
    default: break;
    }
    JS_FreeValue(ctx, range);
    return result;
}

/* The Selection API's members on the Selection's prototype, which
 * selection.c made and left bare with ranges on, and the object that holds
 * its range. */
static int selection_install(os64_dom_t *dom, JSContext *ctx)
{
    if (dom->selection == NULL) return 0;
    dom->ranges->held = d_retain(dom, ctx, JS_NewObjectProto(ctx, JS_NULL));
    if (dom->ranges->held == NULL) return -1;
    JSValue prototype = JS_GetPrototype(ctx, dom->selection->value);
    if (JS_IsException(prototype)) return -1;
    static const struct { const char *name; int magic; } kReads[] = {
        {"anchorNode", S_ANCHOR_NODE}, {"anchorOffset", S_ANCHOR_OFFSET}, {"focusNode", S_FOCUS_NODE},
        {"focusOffset", S_FOCUS_OFFSET}, {"isCollapsed", S_COLLAPSED}, {"rangeCount", S_COUNT},
        {"type", S_TYPE}, {"direction", S_DIRECTION},
    };
    static const struct { const char *name; int magic, argc; } kMethods[] = {
        {"getRangeAt", S_GET, 1}, {"addRange", S_ADD, 1}, {"removeRange", S_REMOVE, 1},
        {"removeAllRanges", S_REMOVE_ALL, 0}, {"empty", S_REMOVE_ALL, 0}, {"collapse", S_COLLAPSE, 1},
        {"setPosition", S_COLLAPSE, 1}, {"collapseToStart", S_TO_START, 0}, {"collapseToEnd", S_TO_END, 0},
        {"extend", S_EXTEND, 1}, {"setBaseAndExtent", S_BASE_EXTENT, 4}, {"selectAllChildren", S_SELECT_ALL, 1},
        {"deleteFromDocument", S_DELETE, 0}, {"containsNode", S_CONTAINS, 1}, {"toString", S_TO_STRING, 0},
    };
    /* d_accessor and d_method_named answer a negative number on failure
     * and anything else on success. */
    int result = 0;
    for (size_t i = 0; result >= 0 && i < sizeof(kReads) / sizeof(kReads[0]); i++)
        result = d_accessor(dom, ctx, prototype, kReads[i].name, selection_ranges_member, kReads[i].magic, false);
    for (size_t i = 0; result >= 0 && i < sizeof(kMethods) / sizeof(kMethods[0]); i++)
        result = d_method_named(dom, ctx, prototype, kMethods[i].name, selection_ranges_member, kMethods[i].argc,
                                kMethods[i].magic);
    JS_FreeValue(ctx, prototype);
    return result < 0 ? -1 : 0;
}

/* ── The install ─────────────────────────────────────────────────────── */

static int define_interface(JSContext *ctx, JSValueConst global, const char *name, int length,
                            JSCFunction *constructor, JSValueConst prototype, JSValueConst parent, JSValue *out)
{
    JSValue interface = d_interface_new(ctx, name, length, constructor, prototype, parent);
    if (JS_IsException(interface)) return -1;
    /* The define takes `interface`; the caller's copy is handed over only
     * once it is on the global, so a failed define leaves nothing to free. */
    JSValue copy = out != NULL ? JS_DupValue(ctx, interface) : JS_UNDEFINED;
    if (JS_DefinePropertyValueStr(ctx, global, name, interface, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(ctx, copy);
        return -1;
    }
    if (out != NULL) *out = copy;
    return 0;
}

int d_ranges_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    if (!dom->options.ranges || !dom->options.modern) return 0;
    static const JSClassDef range_def = {.class_name = "Range", .finalizer = range_finalizer};
    static const JSClassDef static_def = {.class_name = "StaticRange", .finalizer = static_finalizer};
    struct DRanges *s = d_alloc_kind(dom, sizeof(*s), OS64_DOM_MEM_RANGE);
    if (s == NULL) {
        d_error(ctx, "QuotaExceededError", "DOM range quota exceeded");
        return -1;
    }
    os64_memset(s, 0, sizeof(*s));
    dom->ranges = s;
    s->range_class = os64_js_class_id(&range_slot);
    s->static_class = os64_js_class_id(&static_slot);
    if ((!JS_IsRegisteredClass(dom->engine, s->range_class) &&
         JS_NewClass(dom->engine, s->range_class, &range_def) < 0) ||
        (!JS_IsRegisteredClass(dom->engine, s->static_class) &&
         JS_NewClass(dom->engine, s->static_class, &static_def) < 0))
        return -1;
    DValue *abstract = d_retain(dom, ctx, JS_NewObject(ctx));
    DValue *range = abstract != NULL ? d_retain(dom, ctx, JS_NewObjectProto(ctx, abstract->value)) : NULL;
    DValue *fixed = range != NULL ? d_retain(dom, ctx, JS_NewObjectProto(ctx, abstract->value)) : NULL;
    if (fixed == NULL) return -1;
    JS_SetClassProto(ctx, s->range_class, JS_DupValue(ctx, range->value));
    JS_SetClassProto(ctx, s->static_class, JS_DupValue(ctx, fixed->value));
    static const struct { const char *name; int magic; } kAbstract[] = {
        {"startContainer", M_START_CONTAINER}, {"startOffset", M_START_OFFSET},
        {"endContainer", M_END_CONTAINER}, {"endOffset", M_END_OFFSET}, {"collapsed", M_COLLAPSED},
    };
    for (size_t i = 0; i < sizeof(kAbstract) / sizeof(kAbstract[0]); i++)
        if (d_accessor(dom, ctx, abstract->value, kAbstract[i].name, range_member, kAbstract[i].magic, false) < 0)
            return -1;
    if (d_accessor(dom, ctx, range->value, "commonAncestorContainer", range_member, M_COMMON, false) < 0)
        return -1;
    static const struct { const char *name; int magic, argc; } kMethods[] = {
        {"setStart", M_SET_START, 2}, {"setEnd", M_SET_END, 2},
        {"setStartBefore", M_START_BEFORE, 1}, {"setStartAfter", M_START_AFTER, 1},
        {"setEndBefore", M_END_BEFORE, 1}, {"setEndAfter", M_END_AFTER, 1},
        {"collapse", M_COLLAPSE, 0}, {"selectNode", M_SELECT, 1}, {"selectNodeContents", M_SELECT_CONTENTS, 1},
        {"compareBoundaryPoints", M_COMPARE, 2}, {"deleteContents", M_DELETE, 0},
        {"extractContents", M_EXTRACT, 0}, {"cloneContents", M_CLONE_CONTENTS, 0},
        {"insertNode", M_INSERT, 1}, {"surroundContents", M_SURROUND, 1}, {"cloneRange", M_CLONE_RANGE, 0},
        {"detach", M_DETACH, 0}, {"isPointInRange", M_IN_RANGE, 2}, {"comparePoint", M_COMPARE_POINT, 2},
        {"intersectsNode", M_INTERSECTS, 1}, {"toString", M_TO_STRING, 0},
        {"createContextualFragment", M_CONTEXTUAL, 1}, {"getClientRects", M_CLIENT_RECTS, 0},
        {"getBoundingClientRect", M_BOUNDING, 0},
    };
    for (size_t i = 0; i < sizeof(kMethods) / sizeof(kMethods[0]); i++)
        if (d_method_named(dom, ctx, range->value, kMethods[i].name, range_member, kMethods[i].argc,
                           kMethods[i].magic) < 0)
            return -1;
    JSValue abstract_interface = JS_UNDEFINED, range_interface = JS_UNDEFINED;
    if (define_interface(ctx, global, "AbstractRange", 0, NULL, abstract->value, JS_UNDEFINED,
                         &abstract_interface) < 0)
        return -1;
    int defined = define_interface(ctx, global, "Range", 0, range_constructor, range->value, abstract_interface,
                                   &range_interface) == 0 &&
                  define_interface(ctx, global, "StaticRange", 1, static_constructor, fixed->value,
                                   abstract_interface, NULL) == 0;
    /* Range's constants, on the interface and its prototype as WebIDL puts
     * every constant. */
    static const struct { const char *name; int value; } kConstants[] = {
        {"START_TO_START", 0}, {"START_TO_END", 1}, {"END_TO_END", 2}, {"END_TO_START", 3},
    };
    for (size_t i = 0; defined && i < sizeof(kConstants) / sizeof(kConstants[0]); i++)
        defined = JS_DefinePropertyValueStr(ctx, range_interface, kConstants[i].name,
                                            JS_NewInt32(ctx, kConstants[i].value), JS_PROP_ENUMERABLE) >= 0 &&
                  JS_DefinePropertyValueStr(ctx, range->value, kConstants[i].name,
                                            JS_NewInt32(ctx, kConstants[i].value), JS_PROP_ENUMERABLE) >= 0;
    JS_FreeValue(ctx, abstract_interface);
    JS_FreeValue(ctx, range_interface);
    if (!defined) return -1;
    if (d_method_named(dom, ctx, dom->prototypes[D_PROTO_DOCUMENT]->value, "createRange", create_range, 0, 0) < 0)
        return -1;
    return selection_install(dom, ctx);
}

void d_ranges_free(os64_dom_t *dom)
{
    struct DRanges *s = dom->ranges;
    if (s == NULL) return;
    while (s->list != NULL) record_unlink_free(dom, &s->list);
    d_free(dom, s);
    dom->ranges = NULL;
}
