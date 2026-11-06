/* Slot assignment and the flat tree (docs/design/pending/SHADOW_DOM.md § 4).
 *
 * DOM's named slot assignment: each slottable child of a host (an element
 * or a text) is assigned to the first `slot` in tree order in the host's
 * shadow tree whose `name` equals the child's `slot` attribute, the empty
 * name standing for none. Assignment is a function of the host's children
 * and its shadow tree, so it is computed per shadow root and kept against
 * the document's version: any change makes it stale, and the next question
 * computes it again, which is one walk of the shadow tree and one of the
 * host's children.
 *
 * The cache is an optimisation and nothing else: when it cannot be
 * allocated, the same answers come from walking the trees for each
 * question. A refused allocation never makes content disappear.
 *
 * The flat tree is what renders and inherits: a host's flat children are
 * its shadow root's children, a slot's are its assigned nodes or, with
 * none, its own children (its fallback content), everyone else's their
 * own. A slot stays a node of the flat tree (HTML's UA sheet gives it
 * `display: contents`), so a slot assigned to an outer slot is that slot's
 * flat child and its own flat children are its own assigned nodes. */
#include "internal.h"

typedef struct HSlots {
    const HNode *root;
    uint64_t version;
    /* The tree's slots in tree order, and the host's slottable children
     * in order, each with the index of its slot (UINT32_MAX for none). */
    const HNode **slot;
    size_t nslots;
    const HNode **kid;
    uint32_t *kid_slot;
    size_t nkids;
    /* Each slot's assigned nodes: `assigned[first[s] .. first[s] + count[s])`. */
    uint32_t *first, *count;
    const HNode **assigned;
    /* Where the last walk of a slot's list was, so a walk in order is
     * constant time a step. */
    const HNode *hint_slot;
    size_t hint_at;
} HSlots;

static bool is_slot(const HNode *n)
{
    return n->kind == OS64_HTML_ELEMENT && n->ns == OS64_HTML_NS_HTML && n->tag == OS64_HTML_TAG_SLOT;
}
static bool slottable(const HNode *n)
{
    return n->kind == OS64_HTML_ELEMENT || n->kind == OS64_HTML_TEXT;
}
static const char *slot_name(const HNode *slot)
{
    const os64_html_attr_t *a = os64_html_attr(slot, "name");
    return a && a->value ? a->value : "";
}
static const char *slottable_name(const HNode *n)
{
    const os64_html_attr_t *a = n->kind == OS64_HTML_ELEMENT ? os64_html_attr(n, "slot") : NULL;
    return a && a->value ? a->value : "";
}
static bool same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}
/* Preorder over the tree `root` is the root of: its own descendants, no
 * template's contents and no other shadow root, neither being on a child
 * list. */
static const HNode *tree_next(const HNode *root, const HNode *n)
{
    if (n->first_child)
        return n->first_child;
    for (; n != root; n = n->parent)
        if (n->next)
            return n->next;
    return NULL;
}
/* The first slot in `root`'s tree named `name`, walking the trees. */
static const HNode *find_slot(const HNode *root, const char *name)
{
    for (const HNode *n = root->first_child ? root->first_child : NULL; n; n = tree_next(root, n))
        if (is_slot(n) && same(slot_name(n), name))
            return n;
    return NULL;
}

/* ── The cache ─────────────────────────────────────────────────────────── */

static size_t hash_of(const HNode *root, size_t cap)
{
    uintptr_t k = (uintptr_t)root;
    k ^= k >> 17;
    k *= (uintptr_t)0x9E3779B97F4A7C15ull;
    return (size_t)(k >> 7) & (cap - 1);
}
static HSlots **find_entry(HDoc *d, const HNode *root)
{
    if (!d->slots_cap)
        return NULL;
    for (size_t i = hash_of(root, d->slots_cap);; i = (i + 1) & (d->slots_cap - 1)) {
        if (!d->slots[i] || d->slots[i]->root == root)
            return &d->slots[i];
    }
}
static bool table_grow(HDoc *d)
{
    size_t cap = d->slots_cap ? d->slots_cap * 2 : 16;
    int64_t why = OS64_HTML_OK;
    HSlots **table = d_alloc(d, cap * sizeof(*table), &why);
    if (!table)
        return false;
    for (size_t i = 0; i < cap; i++)
        table[i] = NULL;
    HSlots **old = d->slots;
    size_t old_cap = d->slots_cap;
    d->slots = table;
    d->slots_cap = cap;
    for (size_t i = 0; i < old_cap; i++)
        if (old[i])
            *find_entry(d, old[i]->root) = old[i];
    h_free(d, old);
    return true;
}
/* A root's assignment as of now, or NULL when it cannot be allocated. */
static HSlots *slots_of(HDoc *d, const HNode *root)
{
    HSlots **at = find_entry(d, root);
    if (at && *at && (*at)->version == d->version)
        return *at;
    const HNode *host = os64_html_shadow_host(root);
    size_t nslots = 0, nkids = 0;
    for (const HNode *n = root->first_child; n; n = tree_next(root, n))
        nslots += is_slot(n);
    for (const HNode *c = host->first_child; c; c = c->next)
        nkids += slottable(c);
    size_t bytes = sizeof(HSlots) + (nslots + 2 * nkids) * sizeof(const HNode *) +
                   (nkids + 2 * nslots) * sizeof(uint32_t);
    int64_t why = OS64_HTML_OK;
    HSlots *s = d_alloc(d, bytes, &why);
    if (!s)
        return NULL;
    if (!at || (!*at && (d->slots_used + 1) * 4 > d->slots_cap * 3)) {
        if (!table_grow(d)) {
            h_free(d, s);
            return NULL;
        }
        at = find_entry(d, root);
    }
    if (*at)
        h_free(d, *at);
    else
        d->slots_used++;
    *at = s;
    const HNode **nodes = (const HNode **)(s + 1);
    s->root = root;
    s->version = d->version;
    s->slot = nodes;
    s->kid = nodes + nslots;
    s->assigned = nodes + nslots + nkids;
    s->kid_slot = (uint32_t *)(nodes + nslots + 2 * nkids);
    s->first = s->kid_slot + nkids;
    s->count = s->first + nslots;
    s->nslots = nslots;
    s->nkids = nkids;
    s->hint_slot = NULL;
    s->hint_at = 0;
    size_t i = 0;
    for (const HNode *n = root->first_child; n; n = tree_next(root, n))
        if (is_slot(n)) {
            s->slot[i] = n;
            s->count[i] = 0;
            i++;
        }
    i = 0;
    for (const HNode *c = host->first_child; c; c = c->next) {
        if (!slottable(c))
            continue;
        const char *name = slottable_name(c);
        uint32_t found = UINT32_MAX;
        for (size_t k = 0; k < nslots && found == UINT32_MAX; k++)
            if (same(slot_name(s->slot[k]), name))
                found = (uint32_t)k;
        s->kid[i] = c;
        s->kid_slot[i] = found;
        if (found != UINT32_MAX)
            s->count[found]++;
        i++;
    }
    uint32_t start = 0;
    for (size_t k = 0; k < nslots; k++) {
        s->first[k] = start;
        start += s->count[k];
        s->count[k] = 0;
    }
    for (size_t k = 0; k < nkids; k++)
        if (s->kid_slot[k] != UINT32_MAX) {
            uint32_t at_slot = s->kid_slot[k];
            s->assigned[s->first[at_slot] + s->count[at_slot]++] = s->kid[k];
        }
    return s;
}
void h_slots_forget(HDoc *d, const HNode *root)
{
    HSlots **at = find_entry(d, root);
    if (!at || !*at)
        return;
    h_free(d, *at);
    *at = NULL;
    d->slots_used--;
    /* Open addressing: entries after the hole that hashed before it move
     * back, so every lookup still finds them. */
    size_t hole = (size_t)(at - d->slots);
    for (size_t i = (hole + 1) & (d->slots_cap - 1); d->slots[i]; i = (i + 1) & (d->slots_cap - 1)) {
        HSlots *e = d->slots[i];
        d->slots[i] = NULL;
        *find_entry(d, e->root) = e;
    }
}

/* ── Assignment ────────────────────────────────────────────────────────── */

/* The shadow root a slot belongs to, or NULL for a slot in no shadow tree,
 * which an ordinary element is. */
static const HNode *slot_root(const HNode *slot)
{
    const HNode *top = slot;
    while (top->parent)
        top = top->parent;
    return os64_html_shadow_host(top) ? top : NULL;
}
static size_t slot_index(const HSlots *s, const HNode *slot)
{
    for (size_t k = 0; k < s->nslots; k++)
        if (s->slot[k] == slot)
            return k;
    return SIZE_MAX;
}

const os64_html_node_t *os64_html_assigned_slot(const os64_html_document_t *doc,
                                                const os64_html_node_t *node)
{
    HDoc *d = (HDoc *)doc;
    const HNode *host = node ? node->parent : NULL;
    if (!d || !host || !host->shadow_root || !slottable(node))
        return NULL;
    HSlots *s = slots_of(d, host->shadow_root);
    if (!s)
        return find_slot(host->shadow_root, slottable_name(node));
    for (size_t k = 0; k < s->nkids; k++)
        if (s->kid[k] == node)
            return s->kid_slot[k] == UINT32_MAX ? NULL : s->slot[s->kid_slot[k]];
    return NULL;
}

size_t os64_html_slot_assigned_count(const os64_html_document_t *doc, const os64_html_node_t *slot)
{
    HDoc *d = (HDoc *)doc;
    const HNode *root = d && slot && is_slot(slot) ? slot_root(slot) : NULL;
    if (!root)
        return 0;
    HSlots *s = slots_of(d, root);
    if (s) {
        size_t k = slot_index(s, slot);
        return k == SIZE_MAX ? 0 : s->count[k];
    }
    size_t n = 0;
    for (const HNode *c = os64_html_shadow_host(root)->first_child; c; c = c->next)
        n += slottable(c) && find_slot(root, slottable_name(c)) == slot;
    return n;
}

const os64_html_node_t *os64_html_slot_assigned(const os64_html_document_t *doc,
                                                const os64_html_node_t *slot, size_t index)
{
    HDoc *d = (HDoc *)doc;
    const HNode *root = d && slot && is_slot(slot) ? slot_root(slot) : NULL;
    if (!root)
        return NULL;
    HSlots *s = slots_of(d, root);
    if (s) {
        size_t k = slot_index(s, slot);
        return k != SIZE_MAX && index < s->count[k] ? s->assigned[s->first[k] + index] : NULL;
    }
    for (const HNode *c = os64_html_shadow_host(root)->first_child; c; c = c->next)
        if (slottable(c) && find_slot(root, slottable_name(c)) == slot && index-- == 0)
            return c;
    return NULL;
}

/* ── The flat tree ─────────────────────────────────────────────────────── */

const os64_html_node_t *os64_html_flat_first_child(const os64_html_document_t *doc,
                                                   const os64_html_node_t *n)
{
    if (!n)
        return NULL;
    if (n->shadow_root)
        return n->shadow_root->first_child;
    const HNode *first = is_slot(n) ? os64_html_slot_assigned(doc, n, 0) : NULL;
    return first ? first : n->first_child;
}

const os64_html_node_t *os64_html_flat_next_sibling(const os64_html_document_t *doc,
                                                    const os64_html_node_t *n)
{
    HDoc *d = (HDoc *)doc;
    const HNode *host = n ? n->parent : NULL;
    if (!d || !host || !host->shadow_root)
        return n ? n->next : NULL;
    /* A host's child: its flat siblings are its slot's other assigned nodes. */
    const HNode *slot = os64_html_assigned_slot(doc, n);
    if (!slot)
        return NULL;
    HSlots *s = slots_of(d, host->shadow_root);
    if (!s) {
        for (const HNode *c = n->next; c; c = c->next)
            if (slottable(c) && find_slot(host->shadow_root, slottable_name(c)) == slot)
                return c;
        return NULL;
    }
    size_t k = slot_index(s, slot);
    const HNode **list = s->assigned + s->first[k];
    size_t count = s->count[k], at = s->hint_slot == slot && s->hint_at < count && list[s->hint_at] == n
                                          ? s->hint_at
                                          : SIZE_MAX;
    for (size_t i = 0; at == SIZE_MAX && i < count; i++)
        if (list[i] == n)
            at = i;
    if (at == SIZE_MAX || at + 1 >= count)
        return NULL;
    s->hint_slot = slot;
    s->hint_at = at + 1;
    return list[at + 1];
}

const os64_html_node_t *os64_html_flat_parent(const os64_html_document_t *doc,
                                              const os64_html_node_t *n)
{
    const HNode *p = n ? n->parent : NULL;
    if (!p)
        return NULL;
    if (p->shadow_root)
        return os64_html_assigned_slot(doc, n);
    const HNode *host = os64_html_shadow_host(p);
    return host ? host : p;
}
