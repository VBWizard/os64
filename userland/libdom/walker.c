#include "internal.h"

/* TRAVERSAL (DOM § 6): document.createTreeWalker and createNodeIterator,
 * the TreeWalker and NodeIterator they make, and NodeFilter's constants.
 * The algorithms are the standard's, step for step, over libhtml's links
 * (parent, first and last child, previous and next sibling). A walker
 * traverses one node tree: a template's contents are no element's children,
 * and a shadow root, were there any, is no child of its host, so neither is
 * entered.
 *
 * A WALKER HOLDS ITS NODES. Its record (DWalk, binding memory) holds its
 * root and its current node (an iterator's reference) with libhtml's hold,
 * so they live as long as it does whatever the page takes out of the tree,
 * and are wrapped only when a script asks for one. A traversal holds the
 * node it stands on while it runs, since the filter is page code and may
 * remove that node, whose subtree libhtml would otherwise reclaim at once.
 *
 * AN ITERATOR FOLLOWS REMOVALS. DOM's "NodeIterator pre-removing steps"
 * move an iterator's reference off a node about to be removed. libhtml
 * tells the binding after the removal (mutation.c's consumers), with the
 * parent and the siblings the removed run lay between; the steps read the
 * tree around them as it was, which differs from the tree as it is only by
 * the nodes the change moved, so the steps' walks step around those
 * (retarget).
 *
 * LIFETIME is the observers' (resize.c): the records sit on one list, a
 * finalizer only marks its record dead, and the dead are swept when the
 * next walker is made and at teardown, which releases every hold. */

#define FILTER_ACCEPT 1
#define FILTER_REJECT 2
#define FILTER_SKIP 3

enum { W_WALKER, W_ITERATOR };

typedef struct DWalk {
    struct DWalk *next;
    os64_dom_t *dom;
    uint8_t kind;
    /* DOM's active flag; NodeIterator's pointerBeforeReferenceNode; the
     * object is gone. */
    bool active, before, dead;
    uint32_t what;
    const os64_html_node_t *root;   /* held */
    const os64_html_node_t *at;     /* held: a walker's current, an iterator's reference */
    /* An iterator's traversal while it runs: where it stands and whether
     * before it, which a removal moves as it moves the reference, and which
     * the reference becomes when a node is accepted. */
    const os64_html_node_t *flight; /* held; NULL between traversals */
    bool flight_before;
    JSValue filter;                 /* JS_NULL or the page's NodeFilter; traced */
} DWalk;

struct DWalks {
    DWalk *list;
    bool has_dead;
    JSClassID walker_class, iterator_class;
    DMutationConsumer mutations;
};

static JSClassID walker_slot, iterator_slot;

/* Each node kind's nodeType, whose bit (nodeType - 1) whatToShow tests. */
static const uint8_t kNodeTypes[] = {9, 11, 10, 1, 3, 8, 7};
_Static_assert(sizeof(kNodeTypes) == OS64_HTML_PI + 1, "a nodeType for each node kind");

/* `*held` let go of and `next` held instead: the node a traversal stands
 * on, which stays valid however the filter changes the tree. */
static void step_to(os64_dom_t *dom, const os64_html_node_t **held, const os64_html_node_t *next)
{
    if (next != NULL) os64_html_hold(dom->document, next);
    if (*held != NULL) os64_html_release(dom->document, *held);
    *held = next;
}

static void set_at(os64_dom_t *dom, DWalk *w, const os64_html_node_t *node)
{
    step_to(dom, &w->at, node);
}

static void walk_unlink_free(os64_dom_t *dom, DWalk **link)
{
    DWalk *w = *link;
    *link = w->next;
    if (w->root != NULL) os64_html_release(dom->document, w->root);
    if (w->at != NULL) os64_html_release(dom->document, w->at);
    if (w->flight != NULL) os64_html_release(dom->document, w->flight);
    d_free(dom, w);
}

static void sweep(os64_dom_t *dom)
{
    struct DWalks *s = dom->walks;
    if (s == NULL || !s->has_dead) return;
    for (DWalk **link = &s->list; *link != NULL;)
        if ((*link)->dead) walk_unlink_free(dom, link);
        else link = &(*link)->next;
    s->has_dead = false;
}

/* DOM § 6.1, "filter": FILTER_ACCEPT, FILTER_REJECT, FILTER_SKIP or
 * whatever else the page's filter answers (an unsigned short, which no
 * algorithm reads as any of the three), or -1 with the exception pending.
 * A function is called with `this` undefined, an object's acceptNode with
 * the object, and a filter that runs this walker again is an
 * InvalidStateError. */
static int filter_node(os64_dom_t *dom, JSContext *ctx, DWalk *w, const os64_html_node_t *node)
{
    if (w->active) {
        d_error(ctx, "InvalidStateError", "The traversal's filter is already running");
        return -1;
    }
    if ((w->what & (1u << (kNodeTypes[node->kind] - 1))) == 0) return FILTER_SKIP;
    if (JS_IsNull(w->filter)) return FILTER_ACCEPT;
    JSValue argument = d_wrap(dom, ctx, node);
    if (JS_IsException(argument)) return -1;
    JSValue filter = JS_DupValue(ctx, w->filter), call, self = JS_UNDEFINED;
    w->active = true;
    if (JS_IsFunction(ctx, filter)) call = JS_DupValue(ctx, filter);
    else {
        self = filter;
        call = JS_GetPropertyStr(ctx, filter, "acceptNode");
        if (!JS_IsException(call) && !JS_IsFunction(ctx, call)) {
            JS_FreeValue(ctx, call);
            call = JS_ThrowTypeError(ctx, "The NodeFilter's acceptNode is not a function");
        }
    }
    JSValue result = JS_IsException(call) ? JS_EXCEPTION : JS_Call(ctx, call, self, 1, &argument);
    uint32_t value = 0;
    int status = JS_IsException(result) || JS_ToUint32(ctx, &value, result) < 0 ? -1 : (int)(value & 0xFFFF);
    w->active = false;
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, call);
    JS_FreeValue(ctx, filter);
    JS_FreeValue(ctx, argument);
    return status;
}

/* ── TreeWalker (DOM § 6.2). Each answers 1 with `current` moved, 0 for
 * null, -1 with an exception pending. ── */

static int walker_parent(os64_dom_t *dom, JSContext *ctx, DWalk *w)
{
    const os64_html_node_t *node = NULL;
    int found = 0;
    step_to(dom, &node, w->at);
    while (node != NULL && node != w->root) {
        step_to(dom, &node, node->parent);
        if (node == NULL) break;
        int result = filter_node(dom, ctx, w, node);
        if (result < 0 || result == FILTER_ACCEPT) {
            found = result < 0 ? -1 : 1;
            if (found > 0) set_at(dom, w, node);
            break;
        }
    }
    step_to(dom, &node, NULL);
    return found;
}

/* "traverse children", first or last. */
static int walker_children(os64_dom_t *dom, JSContext *ctx, DWalk *w, bool first)
{
    const os64_html_node_t *node = NULL;
    int found = 0;
    step_to(dom, &node, first ? w->at->first_child : w->at->last_child);
    while (node != NULL) {
        int result = filter_node(dom, ctx, w, node);
        if (result < 0 || result == FILTER_ACCEPT) {
            found = result < 0 ? -1 : 1;
            if (found > 0) set_at(dom, w, node);
            break;
        }
        const os64_html_node_t *child = first ? node->first_child : node->last_child;
        if (result == FILTER_SKIP && child != NULL) {
            step_to(dom, &node, child);
            continue;
        }
        bool done = false;
        while (node != NULL) {
            const os64_html_node_t *sibling = first ? node->next : node->prev;
            if (sibling != NULL) {
                step_to(dom, &node, sibling);
                break;
            }
            const os64_html_node_t *parent = node->parent;
            if (parent == NULL || parent == w->root || parent == w->at) {
                done = true;
                break;
            }
            step_to(dom, &node, parent);
        }
        if (done) break;
    }
    step_to(dom, &node, NULL);
    return found;
}

/* "traverse siblings", next or previous. */
static int walker_siblings(os64_dom_t *dom, JSContext *ctx, DWalk *w, bool next)
{
    const os64_html_node_t *node = NULL;
    int found = 0;
    step_to(dom, &node, w->at);
    if (node == w->root) goto out;
    for (;;) {
        const os64_html_node_t *sibling = next ? node->next : node->prev;
        while (sibling != NULL) {
            step_to(dom, &node, sibling);
            int result = filter_node(dom, ctx, w, node);
            if (result < 0 || result == FILTER_ACCEPT) {
                found = result < 0 ? -1 : 1;
                if (found > 0) set_at(dom, w, node);
                goto out;
            }
            sibling = next ? node->first_child : node->last_child;
            if (result == FILTER_REJECT || sibling == NULL) sibling = next ? node->next : node->prev;
        }
        step_to(dom, &node, node->parent);
        if (node == NULL || node == w->root) goto out;
        int result = filter_node(dom, ctx, w, node);
        if (result < 0) found = -1;
        if (result < 0 || result == FILTER_ACCEPT) goto out;
    }
out:
    step_to(dom, &node, NULL);
    return found;
}

static int walker_previous(os64_dom_t *dom, JSContext *ctx, DWalk *w)
{
    const os64_html_node_t *node = NULL;
    int found = 0, result;
    step_to(dom, &node, w->at);
    while (node != w->root) {
        const os64_html_node_t *sibling = node->prev;
        while (sibling != NULL) {
            step_to(dom, &node, sibling);
            if ((result = filter_node(dom, ctx, w, node)) < 0) goto fail;
            while (result != FILTER_REJECT && node->last_child != NULL) {
                step_to(dom, &node, node->last_child);
                if ((result = filter_node(dom, ctx, w, node)) < 0) goto fail;
            }
            if (result == FILTER_ACCEPT) goto accept;
            sibling = node->prev;
        }
        if (node == w->root || node->parent == NULL) goto out;
        step_to(dom, &node, node->parent);
        if ((result = filter_node(dom, ctx, w, node)) < 0) goto fail;
        if (result == FILTER_ACCEPT) goto accept;
    }
    goto out;
accept:
    set_at(dom, w, node);
    found = 1;
    goto out;
fail:
    found = -1;
out:
    step_to(dom, &node, NULL);
    return found;
}

/* nextNode. A current node outside the root (currentNode set there) runs
 * out of ancestors without meeting the root; DOM's steps would then filter
 * the same node for ever, so the walk ends there instead, with null. */
static int walker_next(os64_dom_t *dom, JSContext *ctx, DWalk *w)
{
    const os64_html_node_t *node = NULL;
    int found = 0, result = FILTER_ACCEPT;
    step_to(dom, &node, w->at);
    for (;;) {
        while (result != FILTER_REJECT && node->first_child != NULL) {
            step_to(dom, &node, node->first_child);
            if ((result = filter_node(dom, ctx, w, node)) < 0) goto fail;
            if (result == FILTER_ACCEPT) goto accept;
        }
        const os64_html_node_t *sibling = NULL;
        for (const os64_html_node_t *at = node; at != NULL && sibling == NULL; at = at->parent) {
            if (at == w->root) goto out;
            sibling = at->next;
        }
        if (sibling == NULL) goto out;
        step_to(dom, &node, sibling);
        if ((result = filter_node(dom, ctx, w, node)) < 0) goto fail;
        if (result == FILTER_ACCEPT) goto accept;
    }
accept:
    set_at(dom, w, node);
    found = 1;
    goto out;
fail:
    found = -1;
out:
    step_to(dom, &node, NULL);
    return found;
}

/* ── NodeIterator (DOM § 6.1) ── */

/* The node after `node` in tree order within `root`, or NULL. */
static const os64_html_node_t *following(const os64_html_node_t *node, const os64_html_node_t *root)
{
    if (node->first_child != NULL) return node->first_child;
    for (const os64_html_node_t *at = node; at != NULL && at != root; at = at->parent)
        if (at->next != NULL) return at->next;
    return NULL;
}

/* The last inclusive descendant of `node` in tree order. */
static const os64_html_node_t *last_of(const os64_html_node_t *node)
{
    while (node->last_child != NULL) node = node->last_child;
    return node;
}

/* The node before `node` in tree order within `root`, or NULL. */
static const os64_html_node_t *preceding(const os64_html_node_t *node, const os64_html_node_t *root)
{
    if (node == root) return NULL;
    return node->prev != NULL ? last_of(node->prev) : node->parent;
}

/* DOM's "traverse", next or previous: 1 with `*accepted` the node the
 * filter accepted (held, the caller's to let go of), 0 for null, -1 with an
 * exception pending. The position travels on the record (`flight`) so that
 * a removal the filter makes moves it as it moves the reference
 * (iterators_changed); on acceptance the reference becomes the position as
 * it then stands, which is not the node returned when the filter removed
 * that node. While the filter runs, the reference is the last node
 * accepted. */
static int iterator_traverse(os64_dom_t *dom, JSContext *ctx, DWalk *w, bool next,
                             const os64_html_node_t **accepted)
{
    /* Called from this iterator's own filter: the position is the running
     * traversal's and is not touched. From the reference, which is still
     * the last node accepted, either there is no node to go to, null, or
     * the filter would be asked of it while it runs, InvalidStateError. */
    if (w->flight != NULL) {
        const os64_html_node_t *candidate = next == w->before ? w->at
            : next ? following(w->at, w->root) : preceding(w->at, w->root);
        if (candidate == NULL) return 0;
        d_error(ctx, "InvalidStateError", "The traversal's filter is already running");
        return -1;
    }
    int found = 0;
    step_to(dom, &w->flight, w->at);
    w->flight_before = w->before;
    for (;;) {
        if (next == !w->flight_before) {
            const os64_html_node_t *move = next ? following(w->flight, w->root) : preceding(w->flight, w->root);
            if (move == NULL) goto out;
            step_to(dom, &w->flight, move);
        } else {
            w->flight_before = !w->flight_before;
        }
        step_to(dom, accepted, w->flight);
        int result = filter_node(dom, ctx, w, *accepted);
        if (result < 0) {
            found = -1;
            goto out;
        }
        if (result == FILTER_ACCEPT) break;
    }
    set_at(dom, w, w->flight);
    w->before = w->flight_before;
    found = 1;
out:
    step_to(dom, &w->flight, NULL);
    if (found != 1) step_to(dom, accepted, NULL);
    return found;
}

static bool inclusive_ancestor(const os64_html_node_t *ancestor, const os64_html_node_t *node)
{
    for (; node != NULL; node = node->parent)
        if (node == ancestor) return true;
    return false;
}

static bool in_run(const os64_html_node_t *node, const os64_html_node_t *first, size_t count)
{
    for (size_t i = 0; i < count && first != NULL; i++, first = first->next)
        if (first == node) return true;
    return false;
}

/* Whether the change put `node` where it now is: a node it removed that
 * lies in another parent already (libhtml tells a move's removal once the
 * node is placed), or one it added. */
static bool moved(const os64_html_node_t *node, const os64_html_change_t *c)
{
    return in_run(node, c->removed, c->nremoved) || in_run(node, c->added, c->nadded);
}

/* DOM's "NodeIterator pre-removing steps" for one position (`*slot`, held,
 * and whether before it), from the removal's record: the removed run lay
 * between c->previous_sibling and c->next_sibling under c->target. The
 * steps read the tree as it was before the removal, and the tree differs
 * from that only by the nodes the change moved, so every walk here steps
 * around them. A removal that takes the root out of its tree, or
 * anything around it, leaves the iterator as it is: the steps are for a
 * node inside the root, as WPT's NodeIterator-removal.html and other
 * browsers have them. */
static void retarget(os64_dom_t *dom, const DWalk *w, const os64_html_change_t *c,
                     const os64_html_node_t **slot, bool *before)
{
    bool moves = false;
    const os64_html_node_t *gone = c->removed;
    for (size_t i = 0; i < c->nremoved && gone != NULL && !moves; i++, gone = gone->next)
        moves = !inclusive_ancestor(gone, w->root) && inclusive_ancestor(gone, *slot);
    if (!moves) return;
    if (*before) {
        /* The node after the run, within the root. */
        const os64_html_node_t *next = c->next_sibling;
        while (next != NULL && moved(next, c)) next = next->next;
        for (const os64_html_node_t *at = c->target; next == NULL && at != NULL && at != w->root; at = at->parent)
            for (next = at->next; next != NULL && moved(next, c);) next = next->next;
        if (next != NULL) {
            step_to(dom, slot, next);
            return;
        }
        *before = false;
    }
    /* The previous sibling's last inclusive descendant, else the parent. */
    const os64_html_node_t *last = c->previous_sibling;
    while (last != NULL && moved(last, c)) last = last->prev;
    for (const os64_html_node_t *child = last != NULL ? last->last_child : NULL; child != NULL;) {
        if (moved(child, c)) {
            child = child->prev;
            continue;
        }
        last = child;
        child = child->last_child;
    }
    step_to(dom, slot, last != NULL ? last : c->target);
}

/* The steps for every iterator's reference, and for its traversal's
 * position while one runs. */
static void iterators_changed(os64_dom_t *dom, const os64_html_change_t *c)
{
    struct DWalks *s = dom->walks;
    if (s == NULL || c->kind != OS64_HTML_CHANGE_CHILDREN || c->nremoved == 0) return;
    for (DWalk *w = s->list; w != NULL; w = w->next) {
        if (w->dead || w->kind != W_ITERATOR) continue;
        retarget(dom, w, c, &w->at, &w->before);
        if (w->flight != NULL) retarget(dom, w, c, &w->flight, &w->flight_before);
    }
}

/* ── The interfaces ── */

enum {
    T_ROOT, T_WHAT, T_FILTER, T_CURRENT, T_REFERENCE, T_BEFORE,
    T_PARENT, T_FIRST, T_LAST, T_PREVIOUS_SIBLING, T_NEXT_SIBLING, T_PREVIOUS, T_NEXT, T_DETACH
};

static DWalk *walk_of(os64_dom_t *dom, JSContext *ctx, JSValueConst self, int kind)
{
    DWalk *w = dom->walks != NULL
        ? JS_GetOpaque(self, kind == W_WALKER ? dom->walks->walker_class : dom->walks->iterator_class) : NULL;
    if (w == NULL) JS_ThrowTypeError(ctx, kind == W_WALKER ? "Expected a TreeWalker" : "Expected a NodeIterator");
    return w;
}

static JSValue walk_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                           JSValue *data, int kind)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DWalk *w = walk_of(dom, ctx, self, kind);
    if (w == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    if (set) {
        /* currentNode's setter: any node, inside the root or not. */
        const os64_html_node_t *node = d_node(dom, ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
        if (node == NULL) return JS_EXCEPTION;
        set_at(dom, w, node);
        return JS_UNDEFINED;
    }
    int found;
    switch (magic) {
    case T_ROOT: return d_wrap(dom, ctx, w->root);
    case T_WHAT: return JS_NewUint32(ctx, w->what);
    case T_FILTER: return JS_DupValue(ctx, w->filter);
    case T_CURRENT: case T_REFERENCE: return d_wrap(dom, ctx, w->at);
    case T_BEFORE: return JS_NewBool(ctx, w->before);
    case T_DETACH: return JS_UNDEFINED;
    case T_PARENT: found = walker_parent(dom, ctx, w); break;
    case T_FIRST: case T_LAST: found = walker_children(dom, ctx, w, magic == T_FIRST); break;
    case T_PREVIOUS_SIBLING: case T_NEXT_SIBLING:
        found = walker_siblings(dom, ctx, w, magic == T_NEXT_SIBLING);
        break;
    default:
        if (kind == W_ITERATOR) {
            const os64_html_node_t *accepted = NULL;
            found = iterator_traverse(dom, ctx, w, magic == T_NEXT, &accepted);
            if (found <= 0) return found < 0 ? JS_EXCEPTION : JS_NULL;
            JSValue node = d_wrap(dom, ctx, accepted);
            step_to(dom, &accepted, NULL);
            return node;
        }
        found = magic == T_NEXT ? walker_next(dom, ctx, w) : walker_previous(dom, ctx, w);
        break;
    }
    if (found < 0) return JS_EXCEPTION;
    return found > 0 ? d_wrap(dom, ctx, w->at) : JS_NULL;
}

static JSValue walker_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                             JSValue *data)
{
    return walk_member(ctx, self, argc, argv, magic, data, W_WALKER);
}

static JSValue iterator_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                               JSValue *data)
{
    return walk_member(ctx, self, argc, argv, magic, data, W_ITERATOR);
}

/* createTreeWalker(root, whatToShow, filter) and createNodeIterator (DOM
 * § 4.5): the arguments converted in order, the root any node (an Attr,
 * which is no node of the tree here, is a TypeError: DEBTS.md), whatToShow
 * an unsigned long defaulting to SHOW_ALL, the filter null or an object. */
static JSValue create_walk(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                           JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    const os64_html_node_t *document = d_node(dom, ctx, self);
    if (document == NULL) return JS_EXCEPTION;
    if (document->kind != OS64_HTML_DOCUMENT) return JS_ThrowTypeError(ctx, "Expected a document");
    if (argc < 1) return JS_ThrowTypeError(ctx, "Missing root argument");
    const os64_html_node_t *root = d_node(dom, ctx, argv[0]);
    if (root == NULL) return JS_EXCEPTION;
    uint32_t what = 0xFFFFFFFFu;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToUint32(ctx, &what, argv[1]) < 0) return JS_EXCEPTION;
    JSValueConst filter = argc > 2 ? argv[2] : JS_UNDEFINED;
    if (!JS_IsNull(filter) && !JS_IsUndefined(filter) && !JS_IsObject(filter))
        return JS_ThrowTypeError(ctx, "A NodeFilter is an object or null");
    struct DWalks *s = dom->walks;
    sweep(dom);
    JSValue object = JS_NewObjectClass(ctx, (int)(magic == W_WALKER ? s->walker_class : s->iterator_class));
    if (JS_IsException(object)) return object;
    DWalk *w = d_alloc(dom, sizeof(*w));
    if (w == NULL) {
        JS_FreeValue(ctx, object);
        return d_error(ctx, "QuotaExceededError", "DOM traversal quota exceeded");
    }
    os64_memset(w, 0, sizeof(*w));
    w->dom = dom;
    w->kind = (uint8_t)magic;
    w->what = what;
    w->before = true;
    w->filter = JS_IsObject(filter) ? JS_DupValue(ctx, filter) : JS_NULL;
    step_to(dom, &w->root, root);
    step_to(dom, &w->at, root);
    w->next = s->list;
    s->list = w;
    JS_SetOpaque(object, w);
    return object;
}

/* The filter is taken off the record before it is let go of: freeing it
 * can free a closure that held the walker's own object, whose finalizer
 * then reads the record. */
static void filter_free(JSRuntime *rt, DWalk *w)
{
    JSValue filter = w->filter;
    w->filter = JS_NULL;
    JS_FreeValueRT(rt, filter);
}

static void walk_dies(JSRuntime *rt, DWalk *w)
{
    if (w == NULL) return;
    filter_free(rt, w);
    w->dead = true;
    if (w->dom->walks != NULL) w->dom->walks->has_dead = true;
}

static void walker_finalizer(JSRuntime *rt, JSValue value)
{
    walk_dies(rt, JS_GetOpaque(value, os64_js_class_id(&walker_slot)));
}

static void iterator_finalizer(JSRuntime *rt, JSValue value)
{
    walk_dies(rt, JS_GetOpaque(value, os64_js_class_id(&iterator_slot)));
}

static void walker_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DWalk *w = JS_GetOpaque(value, os64_js_class_id(&walker_slot));
    if (w != NULL) JS_MarkValue(rt, w->filter, mark);
}

static void iterator_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DWalk *w = JS_GetOpaque(value, os64_js_class_id(&iterator_slot));
    if (w != NULL) JS_MarkValue(rt, w->filter, mark);
}

/* NodeFilter is a legacy callback interface (WebIDL § 3.7.7): a function
 * that is no constructor, with the constants and no prototype. */
static JSValue node_filter_called(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "NodeFilter is not a constructor");
}

/* The traversal row (modern.c): TreeWalker and NodeIterator, Illegal
 * constructors; NodeFilter and its constants; and Document's
 * createTreeWalker and createNodeIterator. */
int d_walkers_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef walker_def = {.class_name = "TreeWalker", .finalizer = walker_finalizer,
                                          .gc_mark = walker_mark};
    static const JSClassDef iterator_def = {.class_name = "NodeIterator", .finalizer = iterator_finalizer,
                                            .gc_mark = iterator_mark};
    struct DWalks *s = d_alloc(dom, sizeof(*s));
    if (s == NULL) {
        d_error(ctx, "QuotaExceededError", "DOM traversal quota exceeded");
        return -1;
    }
    os64_memset(s, 0, sizeof(*s));
    dom->walks = s;
    s->walker_class = os64_js_class_id(&walker_slot);
    s->iterator_class = os64_js_class_id(&iterator_slot);
    if ((!JS_IsRegisteredClass(dom->engine, s->walker_class) &&
         JS_NewClass(dom->engine, s->walker_class, &walker_def) < 0) ||
        (!JS_IsRegisteredClass(dom->engine, s->iterator_class) &&
         JS_NewClass(dom->engine, s->iterator_class, &iterator_def) < 0))
        return -1;
    DValue *walker = d_retain(dom, ctx, JS_NewObject(ctx));
    DValue *iterator = walker != NULL ? d_retain(dom, ctx, JS_NewObject(ctx)) : NULL;
    if (iterator == NULL) return -1;
    JS_SetClassProto(ctx, s->walker_class, JS_DupValue(ctx, walker->value));
    JS_SetClassProto(ctx, s->iterator_class, JS_DupValue(ctx, iterator->value));
    static const struct { const char *name; int magic; bool writable; } kWalkerAttributes[] = {
        {"root", T_ROOT, false}, {"whatToShow", T_WHAT, false}, {"filter", T_FILTER, false},
        {"currentNode", T_CURRENT, true},
    };
    static const struct { const char *name; int magic; } kWalkerMethods[] = {
        {"parentNode", T_PARENT}, {"firstChild", T_FIRST}, {"lastChild", T_LAST},
        {"previousSibling", T_PREVIOUS_SIBLING}, {"nextSibling", T_NEXT_SIBLING},
        {"previousNode", T_PREVIOUS}, {"nextNode", T_NEXT},
    };
    static const struct { const char *name; int magic; } kIteratorAttributes[] = {
        {"root", T_ROOT}, {"referenceNode", T_REFERENCE}, {"pointerBeforeReferenceNode", T_BEFORE},
        {"whatToShow", T_WHAT}, {"filter", T_FILTER},
    };
    static const struct { const char *name; int magic; } kIteratorMethods[] = {
        {"nextNode", T_NEXT}, {"previousNode", T_PREVIOUS}, {"detach", T_DETACH},
    };
    for (size_t i = 0; i < sizeof(kWalkerAttributes) / sizeof(kWalkerAttributes[0]); i++)
        if (d_accessor(dom, ctx, walker->value, kWalkerAttributes[i].name, walker_member, kWalkerAttributes[i].magic,
                       kWalkerAttributes[i].writable) < 0)
            return -1;
    for (size_t i = 0; i < sizeof(kWalkerMethods) / sizeof(kWalkerMethods[0]); i++)
        if (d_method_named(dom, ctx, walker->value, kWalkerMethods[i].name, walker_member, 0,
                           kWalkerMethods[i].magic) < 0)
            return -1;
    for (size_t i = 0; i < sizeof(kIteratorAttributes) / sizeof(kIteratorAttributes[0]); i++)
        if (d_accessor(dom, ctx, iterator->value, kIteratorAttributes[i].name, iterator_member,
                       kIteratorAttributes[i].magic, false) < 0)
            return -1;
    for (size_t i = 0; i < sizeof(kIteratorMethods) / sizeof(kIteratorMethods[0]); i++)
        if (d_method_named(dom, ctx, iterator->value, kIteratorMethods[i].name, iterator_member, 0,
                           kIteratorMethods[i].magic) < 0)
            return -1;
    const struct { const char *name; JSValueConst prototype; } kInterfaces[] = {
        {"TreeWalker", walker->value}, {"NodeIterator", iterator->value},
    };
    for (int i = 0; i < 2; i++) {
        JSValue interface = d_interface_new(ctx, kInterfaces[i].name, 0, NULL, kInterfaces[i].prototype,
                                            JS_UNDEFINED);
        if (JS_IsException(interface) ||
            JS_DefinePropertyValueStr(ctx, global, kInterfaces[i].name, interface,
                                      JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
            return -1;
    }
    static const struct { const char *name; uint32_t value; } kConstants[] = {
        {"FILTER_ACCEPT", FILTER_ACCEPT}, {"FILTER_REJECT", FILTER_REJECT}, {"FILTER_SKIP", FILTER_SKIP},
        {"SHOW_ALL", 0xFFFFFFFFu}, {"SHOW_ELEMENT", 0x1}, {"SHOW_ATTRIBUTE", 0x2}, {"SHOW_TEXT", 0x4},
        {"SHOW_CDATA_SECTION", 0x8}, {"SHOW_ENTITY_REFERENCE", 0x10}, {"SHOW_ENTITY", 0x20},
        {"SHOW_PROCESSING_INSTRUCTION", 0x40}, {"SHOW_COMMENT", 0x80}, {"SHOW_DOCUMENT", 0x100},
        {"SHOW_DOCUMENT_TYPE", 0x200}, {"SHOW_DOCUMENT_FRAGMENT", 0x400}, {"SHOW_NOTATION", 0x800},
    };
    JSValue filter = JS_NewCFunction2(ctx, node_filter_called, "NodeFilter", 0, JS_CFUNC_generic, 0);
    if (JS_IsException(filter)) return -1;
    for (size_t i = 0; i < sizeof(kConstants) / sizeof(kConstants[0]); i++)
        if (JS_DefinePropertyValueStr(ctx, filter, kConstants[i].name, JS_NewUint32(ctx, kConstants[i].value),
                                      JS_PROP_ENUMERABLE) < 0) {
            JS_FreeValue(ctx, filter);
            return -1;
        }
    if (JS_DefinePropertyValueStr(ctx, global, "NodeFilter", filter, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    JSValueConst document = dom->prototypes[D_PROTO_DOCUMENT]->value;
    if (d_method_named(dom, ctx, document, "createTreeWalker", create_walk, 1, W_WALKER) < 0 ||
        d_method_named(dom, ctx, document, "createNodeIterator", create_walk, 1, W_ITERATOR) < 0)
        return -1;
    d_mutation_register(dom, &s->mutations, iterators_changed);
    return 0;
}

/* Before the engine goes: the filters let go of. */
void d_walkers_drain(os64_dom_t *dom)
{
    struct DWalks *s = dom->walks;
    if (s == NULL) return;
    for (DWalk *w = s->list; w != NULL; w = w->next) filter_free(dom->engine, w);
}

/* After it: every record and its holds. */
void d_walkers_free(os64_dom_t *dom)
{
    struct DWalks *s = dom->walks;
    if (s == NULL) return;
    while (s->list != NULL) walk_unlink_free(dom, &s->list);
    d_free(dom, s);
    dom->walks = NULL;
}
