#include "internal.h"

/* NODE'S COMPARISONS (DOM § 4.4): isSameNode, isEqualNode and
 * compareDocumentPosition, on Node's prototype and on Attr's, whose shadow
 * answers every Node member for an Attr (attr.c). Either side of each may
 * be an Attr, which libhtml's tree holds as a name and a value on its
 * element rather than as a node, so an operand is read as either one: a
 * tree node, or an Attr's view (d_attr_view) with its element standing in
 * the tree for it.
 *
 * react-helmet keeps a page's head by comparing its tags with isEqualNode.
 * One row of the Modern Web table, naming nothing on window. */

typedef struct {
    /* The tree node, or the Attr's element (NULL while it is detached). */
    const os64_html_node_t *node;
    bool attr;
    DAttrView view;     /* when `attr` */
} DOperand;

static int operand(os64_dom_t *dom, JSContext *ctx, JSValueConst value, DOperand *out)
{
    int is = d_attr_view(dom, ctx, value, &out->view);
    if (is < 0) return -1;
    out->attr = is == 1;
    out->node = out->attr ? out->view.element : d_node(dom, ctx, value);
    return out->attr || out->node != NULL ? 0 : -1;
}

/* A name that may be null: null equals only null. */
static bool same_name(const char *a, const char *b)
{
    return a == NULL || b == NULL ? a == b : os64_strcmp(a, b) == 0;
}

/* A doctype's strings, which libhtml leaves NULL where DOM reads "". */
static bool same_text(const char *a, const char *b)
{
    return os64_strcmp(a != NULL ? a : "", b != NULL ? b : "") == 0;
}

/* Two elements' attribute lists hold the same attributes, by namespace,
 * local name and value, in any order. */
static bool same_attributes(const os64_html_node_t *a, const os64_html_node_t *b)
{
    size_t count = 0;
    for (const os64_html_attr_t *x = a->attrs; x != NULL; x = x->next) count++;
    for (const os64_html_attr_t *y = b->attrs; y != NULL; y = y->next)
        if (count-- == 0) return false;
    if (count != 0) return false;
    for (const os64_html_attr_t *x = a->attrs; x != NULL; x = x->next) {
        const os64_html_attr_t *y = b->attrs;
        while (y != NULL && !(same_name(x->ns, y->ns) && os64_strcmp(d_attr_local(x->name, x->ns),
                                                                      d_attr_local(y->name, y->ns)) == 0 &&
                              os64_strcmp(x->value, y->value) == 0))
            y = y->next;
        if (y == NULL) return false;
    }
    return true;
}

/* DOM's "equals" for two nodes, their children aside. A libhtml kind is
 * one nodeType. A template's contents are not its children, so they are
 * not compared, as DOM says. */
static bool alike(const os64_html_node_t *a, const os64_html_node_t *b)
{
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case OS64_HTML_DOCTYPE:
        return same_text(a->name, b->name) && same_text(a->public_id, b->public_id) &&
               same_text(a->system_id, b->system_id);
    case OS64_HTML_ELEMENT:
        return same_name(os64_html_namespace_uri(a), os64_html_namespace_uri(b)) &&
               same_name(a->prefix, b->prefix) && os64_strcmp(a->name, b->name) == 0 && same_attributes(a, b);
    case OS64_HTML_PI: case OS64_HTML_TEXT: case OS64_HTML_COMMENT:
        // A PI's target is its name; it, a text node and a comment have data.
        return (a->kind != OS64_HTML_PI || same_text(a->name, b->name)) && a->text_len == b->text_len &&
               os64_memcmp(a->text, b->text, a->text_len) == 0;
    default:
        return true;
    }
}

/* Two trees equal, walked side by side in tree order rather than by
 * recursion, so a deep tree costs no stack. A child, a next sibling or a
 * climb out has to be there on both sides at every step, which is what
 * holds each node's children to the same count. */
static bool trees_equal(const os64_html_node_t *a, const os64_html_node_t *b)
{
    const os64_html_node_t *top = a;
    for (;;) {
        if (!alike(a, b) || (a->first_child == NULL) != (b->first_child == NULL)) return false;
        if (a->first_child != NULL) {
            a = a->first_child;
            b = b->first_child;
            continue;
        }
        while (a != top && a->next == NULL) {
            if (b->next != NULL) return false;
            a = a->parent;
            b = b->parent;
        }
        if (a == top) return true;
        if (b->next == NULL) return false;
        a = a->next;
        b = b->next;
    }
}

static bool attrs_equal(const DAttrView *a, const DAttrView *b)
{
    return same_name(a->ns, b->ns) && os64_strcmp(a->local, b->local) == 0 && a->length == b->length &&
           os64_memcmp(a->value, b->value, a->length) == 0;
}

static const os64_html_node_t *root_of(const os64_html_node_t *node)
{
    while (node->parent != NULL) node = node->parent;
    return node;
}

static size_t depth_of(const os64_html_node_t *node)
{
    size_t depth = 0;
    for (; node->parent != NULL; node = node->parent) depth++;
    return depth;
}

static bool is_ancestor(const os64_html_node_t *ancestor, const os64_html_node_t *node)
{
    for (node = node->parent; node != NULL; node = node->parent)
        if (node == ancestor) return true;
    return false;
}

/* Whether `a` comes before `b` in tree order: two different nodes of one
 * tree. The deeper is raised to its ancestor at the other's depth, then
 * both until they are siblings, whose order is the answer. */
static bool precedes(const os64_html_node_t *a, const os64_html_node_t *b)
{
    size_t depth_a = depth_of(a), depth_b = depth_of(b);
    bool a_shallower = depth_a < depth_b;
    for (; depth_a > depth_b; depth_a--) a = a->parent;
    for (; depth_b > depth_a; depth_b--) b = b->parent;
    // One was the other's ancestor, which comes first.
    if (a == b) return a_shallower;
    while (a->parent != b->parent) {
        a = a->parent;
        b = b->parent;
    }
    for (const os64_html_node_t *at = a->next; at != NULL; at = at->next)
        if (at == b) return true;
    return false;
}

#define POSITION_DISCONNECTED 0x01
#define POSITION_PRECEDING 0x02
#define POSITION_FOLLOWING 0x04
#define POSITION_CONTAINS 0x08
#define POSITION_CONTAINED_BY 0x10
#define POSITION_IMPLEMENTATION_SPECIFIC 0x20

/* compareDocumentPosition (DOM § 4.4): where `other` is from `self`.
 * `node1` and `node2` are the standard's names: other's node and self's,
 * an Attr's being its element. */
static int position(const DOperand *self, const DOperand *other)
{
    const os64_html_node_t *node1 = other->node, *node2 = self->node;
    const DAttrView *attr1 = other->attr ? &other->view : NULL, *attr2 = self->attr ? &self->view : NULL;
    /* Two Attrs of one element are in the order of its attribute list, as
     * DOM's steps walk it (the answer carrying IMPLEMENTATION_SPECIFIC).
     * An Attr is found by its qualified name, first match, as attr.c finds
     * one. */
    if (attr1 != NULL && attr2 != NULL && node1 != NULL && node1 == node2) {
        for (const os64_html_attr_t *a = node2->attrs; a != NULL; a = a->next) {
            if (os64_strcmp(a->name, attr1->name) == 0)
                return POSITION_IMPLEMENTATION_SPECIFIC | POSITION_PRECEDING;
            if (os64_strcmp(a->name, attr2->name) == 0)
                return POSITION_IMPLEMENTATION_SPECIFIC | POSITION_FOLLOWING;
        }
    }
    /* Not in one tree. "Consistent" is the standard's whole demand, and
     * the order of the two roots' addresses meets it: a tree whose root,
     * or detached Attr, is at the lower address precedes. */
    const os64_html_node_t *root1 = node1 != NULL ? root_of(node1) : NULL;
    const os64_html_node_t *root2 = node2 != NULL ? root_of(node2) : NULL;
    if (root1 == NULL || root2 == NULL || root1 != root2) {
        uintptr_t key1 = root1 != NULL ? (uintptr_t)root1 : (uintptr_t)attr1->entry;
        uintptr_t key2 = root2 != NULL ? (uintptr_t)root2 : (uintptr_t)attr2->entry;
        return POSITION_DISCONNECTED | POSITION_IMPLEMENTATION_SPECIFIC |
               (key1 < key2 ? POSITION_PRECEDING : POSITION_FOLLOWING);
    }
    if ((attr1 == NULL && is_ancestor(node1, node2)) || (attr2 != NULL && node1 == node2))
        return POSITION_CONTAINS | POSITION_PRECEDING;
    if ((attr2 == NULL && is_ancestor(node2, node1)) || (attr1 != NULL && node1 == node2))
        return POSITION_CONTAINED_BY | POSITION_FOLLOWING;
    return precedes(node1, node2) ? POSITION_PRECEDING : POSITION_FOLLOWING;
}

enum { C_SAME, C_EQUAL, C_POSITION };

static JSValue compare_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                              int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DOperand mine, theirs;
    if (operand(dom, ctx, self, &mine) < 0) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "Missing node argument");
    /* isSameNode's and isEqualNode's argument is a `Node?`, which WebIDL
     * converts undefined to null for; compareDocumentPosition's is a Node. */
    if (magic != C_POSITION && (JS_IsNull(argv[0]) || JS_IsUndefined(argv[0]))) return JS_FALSE;
    if (operand(dom, ctx, argv[0], &theirs) < 0) return JS_EXCEPTION;
    bool same = JS_VALUE_GET_PTR(self) == JS_VALUE_GET_PTR(argv[0]);
    switch (magic) {
    case C_SAME: return JS_NewBool(ctx, same);
    case C_EQUAL:
        if (mine.attr || theirs.attr)
            return JS_NewBool(ctx, mine.attr && theirs.attr && attrs_equal(&mine.view, &theirs.view));
        return JS_NewBool(ctx, trees_equal(mine.node, theirs.node));
    default:
        return JS_NewInt32(ctx, same ? 0 : position(&mine, &theirs));
    }
}

int d_compare_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    (void)global;
    static const struct { const char *name; int magic; } kMembers[] = {
        {"isSameNode", C_SAME}, {"isEqualNode", C_EQUAL}, {"compareDocumentPosition", C_POSITION},
    };
    static const int kOn[] = {D_PROTO_NODE, D_PROTO_ATTR};
    for (size_t on = 0; on < sizeof(kOn) / sizeof(kOn[0]); on++)
        for (size_t i = 0; i < sizeof(kMembers) / sizeof(kMembers[0]); i++)
            if (d_method(dom, ctx, dom->prototypes[kOn[on]]->value, kMembers[i].name, compare_member, 1,
                         kMembers[i].magic) < 0)
                return -1;
    return 0;
}
