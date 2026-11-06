/* Slot assignment and the flat tree (docs/design/pending/SHADOW_DOM.md § 4).
 *
 * DOM's named slot assignment: each slottable child of a host (an element
 * or a text) is assigned to the first `slot` in tree order in the host's
 * shadow tree whose `name` equals the child's `slot` attribute, the empty
 * name standing for none. Assignment is a function of the host's children
 * and its shadow tree, so it is computed per shadow root and kept until
 * the document's version moves or a change that can move the root's
 * assignment is described (h_slots_mark: the parser moves the version
 * once a call, not once an append); the next question computes it again,
 * which is one walk of the shadow tree and one of the host's children.
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
    /* The serials (HMeta.serial) of `slot` and `assigned`: a reported
     * assignment is compared by pointer and serial, so a node freed and a
     * new one at its address are two nodes. */
    uint32_t *slot_serial, *assigned_serial;
    /* Where the last walk of a slot's list was, and the host child last
     * looked up, so a walk in order (a flat walk asks both questions at
     * each step) is constant time a step. */
    const HNode *hint_slot;
    size_t hint_at;
    size_t hint_kid;
    size_t bytes; /* the block, for os64_html_slot_changes' copy */
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
static HSlots **find_entry(HSlotTable *t, const HNode *root)
{
    if (!t->cap)
        return NULL;
    for (size_t i = hash_of(root, t->cap);; i = (i + 1) & (t->cap - 1))
        if (!t->e[i] || t->e[i]->root == root)
            return &t->e[i];
}
static bool table_grow(HDoc *d, HSlotTable *t)
{
    size_t cap = t->cap ? t->cap * 2 : 16;
    int64_t why = OS64_HTML_OK;
    HSlots **e = d_alloc(d, cap * sizeof(*e), &why);
    if (!e)
        return false;
    for (size_t i = 0; i < cap; i++)
        e[i] = NULL;
    HSlotTable old = *t;
    t->e = e;
    t->cap = cap;
    for (size_t i = 0; i < old.cap; i++)
        if (old.e[i])
            *find_entry(t, old.e[i]->root) = old.e[i];
    h_free(d, old.e);
    return true;
}
/* Puts `s` in `t` under its root, freeing what was there. False, with `s`
 * not put, when the table cannot grow. */
static bool table_put(HDoc *d, HSlotTable *t, HSlots *s)
{
    HSlots **at = find_entry(t, s->root);
    if (!at || (!*at && (t->used + 1) * 4 > t->cap * 3)) {
        if (!table_grow(d, t))
            return false;
        at = find_entry(t, s->root);
    }
    if (*at)
        h_free(d, *at);
    else
        t->used++;
    *at = s;
    return true;
}
/* Takes `root`'s entry out of `t`, NULL when it has none. */
static HSlots *table_take(HSlotTable *t, const HNode *root)
{
    HSlots **at = find_entry(t, root);
    if (!at || !*at)
        return NULL;
    HSlots *s = *at;
    *at = NULL;
    t->used--;
    /* Open addressing: entries after the hole that hashed before it move
     * back, so every lookup still finds them. */
    size_t hole = (size_t)(at - t->e);
    for (size_t i = (hole + 1) & (t->cap - 1); t->e[i]; i = (i + 1) & (t->cap - 1)) {
        HSlots *e = t->e[i];
        t->e[i] = NULL;
        *find_entry(t, e->root) = e;
    }
    return s;
}
/* Points an assignment's arrays into its own block. */
static void layout(HSlots *s, size_t nslots, size_t nkids)
{
    const HNode **nodes = (const HNode **)(s + 1);
    s->slot = nodes;
    s->kid = nodes + nslots;
    s->assigned = nodes + nslots + nkids;
    s->kid_slot = (uint32_t *)(nodes + nslots + 2 * nkids);
    s->first = s->kid_slot + nkids;
    s->count = s->first + nslots;
    s->slot_serial = s->count + nslots;
    s->assigned_serial = s->slot_serial + nslots;
    s->nslots = nslots;
    s->nkids = nkids;
}
/* A root's assignment as of now, or NULL when it cannot be allocated. */
static HSlots *slots_of(HDoc *d, const HNode *root)
{
    HSlots **at = find_entry(&d->slots, root);
    if (at && *at && (*at)->version == d->version)
        return *at;
    const HNode *host = os64_html_shadow_host(root);
    size_t nslots = 0, nkids = 0;
    for (const HNode *n = root->first_child; n; n = tree_next(root, n))
        nslots += is_slot(n);
    for (const HNode *c = host->first_child; c; c = c->next)
        nkids += slottable(c);
    size_t bytes = sizeof(HSlots) + (nslots + 2 * nkids) * sizeof(const HNode *) +
                   (2 * nkids + 3 * nslots) * sizeof(uint32_t);
    int64_t why = OS64_HTML_OK;
    HSlots *s = d_alloc(d, bytes, &why);
    if (!s)
        return NULL;
    s->root = root;
    s->version = d->version;
    s->bytes = bytes;
    s->hint_slot = NULL;
    s->hint_at = 0;
    s->hint_kid = 0;
    layout(s, nslots, nkids);
    if (!table_put(d, &d->slots, s)) {
        h_free(d, s);
        return NULL;
    }
    size_t i = 0;
    for (const HNode *n = root->first_child; n; n = tree_next(root, n))
        if (is_slot(n)) {
            s->slot[i] = n;
            s->slot_serial[i] = h_meta(n)->serial;
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
            uint32_t at = s->first[at_slot] + s->count[at_slot]++;
            s->assigned[at] = s->kid[k];
            s->assigned_serial[at] = h_meta(s->kid[k])->serial;
        }
    return s;
}
void h_slots_forget(HDoc *d, const HNode *root)
{
    h_free(d, table_take(&d->slots, root));
    h_free(d, table_take(&d->reported, root));
    /* The list keeps marking order, so the gap closes by shifting. */
    for (size_t i = 0; i < d->slot_dirty_n; i++)
        if (d->slot_dirty[i] == root) {
            for (size_t k = i + 1; k < d->slot_dirty_n; k++)
                d->slot_dirty[k - 1] = d->slot_dirty[k];
            d->slot_dirty_n--;
            break;
        }
}

/* ── Marking, for slotchange (SHADOW_DOM.md § 4) ─────────────────────────
 *
 * A recomputation happens when somebody asks, and slotchange must be heard
 * whether or not anybody asked, so a change that can move an assignment
 * marks its root at the mutation: a host's children changing, anything
 * changing inside a shadow tree (a slot added, removed or moved), a host
 * child's `slot` attribute or a slot's `name`. Marking is over-generous on
 * purpose; os64_html_slot_changes compares, so a mark that moved nothing
 * signals nothing. */

static size_t slot_index(const HSlots *s, const HNode *slot);

static const HNode *tree_top(const HNode *n)
{
    while (n->parent)
        n = n->parent;
    return n;
}
void h_slots_mark(HDoc *d, const HNode *root)
{
    /* The change may be one that left the version where it was (the
     * parser moves it once a call), so the cached assignment goes stale
     * here and not only when the version moves. */
    HSlots **cached = find_entry(&d->slots, root);
    if (cached && *cached)
        (*cached)->version = 0;
    for (size_t i = 0; i < d->slot_dirty_n; i++)
        if (d->slot_dirty[i] == root)
            return;
    if (d->slot_dirty_n == d->slot_dirty_cap) {
        size_t cap = d->slot_dirty_cap ? d->slot_dirty_cap * 2 : 8;
        int64_t why = OS64_HTML_OK;
        const HNode **list = d_alloc(d, cap * sizeof(*list), &why);
        if (!list) {
            /* A mark that cannot be kept makes the next report compare
             * every root it knows of: a slotchange is late, never lost. */
            d->slot_dirty_all = true;
            return;
        }
        for (size_t i = 0; i < d->slot_dirty_n; i++)
            list[i] = d->slot_dirty[i];
        h_free(d, (void *)d->slot_dirty);
        d->slot_dirty = list;
        d->slot_dirty_cap = cap;
    }
    d->slot_dirty[d->slot_dirty_n++] = root;
}
void h_slots_note(HDoc *d, const os64_html_change_t *c)
{
    if (!d->shadows || !c->target)
        return;
    const HNode *t = c->target;
    if (c->kind == OS64_HTML_CHANGE_CHILDREN) {
        if (t->shadow_root)
            h_slots_mark(d, t->shadow_root);
        const HNode *top = tree_top(t);
        if (os64_html_shadow_host(top))
            h_slots_mark(d, top);
    } else if (c->kind == OS64_HTML_CHANGE_ATTR && !c->attr_ns && c->attr_local) {
        if (same(c->attr_local, "slot") && t->parent && t->parent->shadow_root)
            h_slots_mark(d, t->parent->shadow_root);
        if (same(c->attr_local, "name") && is_slot(t)) {
            const HNode *top = tree_top(t);
            if (os64_html_shadow_host(top))
                h_slots_mark(d, top);
        }
    }
}

/* Whether slot `k` of `now` has the assigned list `before` had for the same
 * slot; a slot `before` did not have had nothing assigned. */
static bool assigned_same(const HSlots *now, size_t k, const HSlots *before)
{
    size_t j = SIZE_MAX;
    for (size_t i = 0; before && j == SIZE_MAX && i < before->nslots; i++)
        if (before->slot[i] == now->slot[k] && before->slot_serial[i] == now->slot_serial[k])
            j = i;
    size_t had = j == SIZE_MAX ? 0 : before->count[j];
    if (had != now->count[k])
        return false;
    for (size_t i = 0; i < had; i++)
        if (before->assigned[before->first[j] + i] != now->assigned[now->first[k] + i] ||
            before->assigned_serial[before->first[j] + i] != now->assigned_serial[now->first[k] + i])
            return false;
    return true;
}
/* One root: its assignment now against the one last reported, each slot
 * whose list moved handed to `changed`, then now kept as reported. False,
 * with nothing signalled and the root left to report again, when there is
 * no memory for it. */
static bool report_root(HDoc *d, const HNode *root, void (*changed)(void *, const os64_html_node_t *),
                        void *opaque)
{
    HSlots *now = slots_of(d, root);
    if (!now)
        return false;
    int64_t why = OS64_HTML_OK;
    HSlots *kept = d_alloc(d, now->bytes, &why);
    if (!kept)
        return false;
    unsigned char *to = (unsigned char *)kept;
    const unsigned char *from = (const unsigned char *)now;
    for (size_t i = 0; i < now->bytes; i++)
        to[i] = from[i];
    layout(kept, now->nslots, now->nkids);
    HSlots *before = table_take(&d->reported, root);
    if (!table_put(d, &d->reported, kept)) {
        h_free(d, kept);
        if (before)
            table_put(d, &d->reported, before); /* back where it was: the table has its room */
        return false;
    }
    for (size_t k = 0; k < now->nslots; k++)
        if (!assigned_same(now, k, before))
            changed(opaque, now->slot[k]);
    h_free(d, before);
    return true;
}

int64_t os64_html_slot_changes(os64_html_document_t *doc,
                               void (*changed)(void *opaque, const os64_html_node_t *slot), void *opaque)
{
    HDoc *d = (HDoc *)doc;
    if (!d || !changed)
        return OS64_HTML_BAD_ARGUMENT;
    int64_t status = OS64_HTML_OK;
    if (d->slot_dirty_all) {
        /* Every shadow root the document has, whether or not anything was
         * ever computed or reported for it: a root whose every mark was
         * lost is in neither table. */
        d->slot_dirty_all = false;
        for (const HNode *n = d->live_nodes; n; n = h_meta(n)->live_next)
            if (os64_html_shadow_host(n))
                h_slots_mark(d, n);
    }
    /* Reported roots leave the list and the rest close up behind them, so
     * what is left keeps marking order for a later call. */
    size_t kept = 0;
    for (size_t i = 0; i < d->slot_dirty_n; i++) {
        if (report_root(d, d->slot_dirty[i], changed, opaque))
            continue;
        status = OS64_HTML_NO_MEMORY;
        d->slot_dirty[kept++] = d->slot_dirty[i];
    }
    d->slot_dirty_n = kept;
    /* A recovery whose marks were lost again leaves roots to report too. */
    return d->slot_dirty_all ? OS64_HTML_NO_MEMORY : status;
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
    /* The child last asked about, and the one after it, before a scan. */
    size_t k = s->hint_kid < s->nkids && s->kid[s->hint_kid] == node ? s->hint_kid
             : s->hint_kid + 1 < s->nkids && s->kid[s->hint_kid + 1] == node ? s->hint_kid + 1
             : SIZE_MAX;
    for (size_t i = 0; k == SIZE_MAX && i < s->nkids; i++)
        if (s->kid[i] == node)
            k = i;
    if (k == SIZE_MAX)
        return NULL;
    s->hint_kid = k;
    return s->kid_slot[k] == UINT32_MAX ? NULL : s->slot[s->kid_slot[k]];
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
    if (host)
        return host;
    /* A slot's own children are its fallback, in the flat tree only while
     * nothing is assigned to it; otherwise they render nowhere, as a host's
     * child assigned to no slot does. */
    if (is_slot(p) && os64_html_slot_assigned_count(doc, p) > 0)
        return NULL;
    return p;
}
