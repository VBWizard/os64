// Review the equality and role inventories when these records change. Size
// guards supplement that review; a same-size field edit still needs inspection.
_Static_assert(sizeof(FBox) == 480 && sizeof(FItem) == 104 && sizeof(FInline) == 72,
               "review layout correspondence fields");
// Call-local correspondence. No old arena address escapes into the new tree.
typedef struct {
    FBox *box;
    const FBox *old;
    size_t parent;
    bool equal, ordinary;
    flow_reuse_reason_t reason;
} FReusePair;

typedef struct {
    FReusePair *pairs;
    uint32_t *slots;
    size_t count, capacity, nslots, bytes;
    flow_tree_t *tree;
    const flow_tree_t *previous;
} FReuseMatch;

static FReusePair *reuse_pair(FReuseMatch *m, const FBox *box)
{
    if (!m || !box) return NULL;
    size_t at = reuse_hash((const void *)box, m->nslots);
    while (m->slots[at]) {
        FReusePair *p = &m->pairs[m->slots[at] - 1];
        if (p->box == box) return p;
        at = (at + 1) & (m->nslots - 1);
    }
    return NULL;
}

static const os64_html_node_t *reuse_element(const os64_html_node_t *node)
{
    while (node && node->kind != OS64_HTML_ELEMENT) node = node->parent;
    return node;
}

static bool reuse_derived_equal(FReuseMatch *m, const os64_html_node_t *node)
{
    node = reuse_element(node);
    const FStyled *a = f_style_of(m->tree->styles, node);
    const FStyled *b = f_style_of(m->previous->styles, node);
    if (!a || !b) return a == b;
    // Links and controls are fresh model indices. Decoration ancestry is
    // copied into fragments, so its values are part of the certificate.
    return a->decoration == b->decoration &&
           !os64_memcmp(&a->decoration_colors, &b->decoration_colors, sizeof(a->decoration_colors)) &&
           a->transparent == b->transparent && a->in_nobr == b->in_nobr;
}

static bool reuse_bytes_equal(const char *a, const char *b, size_t n)
{
    return n == 0 || (a && b && !os64_memcmp(a, b, n));
}

static flow_reuse_reason_t reuse_node_reason(FReuseMatch *m, const os64_html_node_t *node,
                                            bool subtree)
{
    if (!node) return FLOW_REUSE_NREASONS;
    FReuseSnapshot *old = m->previous->reuse, *fresh = m->tree->reuse;
    size_t i = reuse_find(old, node), j = reuse_find(fresh, node);
    if (i == SIZE_MAX || j == SIZE_MAX) return FLOW_REUSE_STRUCTURE;
    FReuseNode *a = &reuse_nodes(old)[i], *b = &reuse_nodes(fresh)[j];
    if (a->changed || (subtree && a->dirty)) return FLOW_REUSE_DIRTY;
    if (a->animated || b->animated) return FLOW_REUSE_MOTION;
    if (!reuse_derived_equal(m, node)) return FLOW_REUSE_STYLE;
    return FLOW_REUSE_NREASONS;
}

static flow_reuse_reason_t reuse_items_equal(FReuseMatch *m, const FBox *a, const FBox *b,
                                            bool *ordinary)
{
    const FItem *x = a->items, *y = b->items;
    for (; x && y; x = x->next, y = y->next) {
        if (x->kind != y->kind || x->node != y->node || x->len != y->len || x->offset != y->offset ||
            x->generated != y->generated || x->continuation != y->continuation ||
            x->replaced != y->replaced || !!x->inl != !!y->inl ||
            !!x->content != !!y->content || !!x->absolute != !!y->absolute ||
            !!x->floated != !!y->floated || !reuse_bytes_equal(x->text, y->text, x->len))
            return FLOW_REUSE_STRUCTURE;
        flow_reuse_reason_t reason = reuse_node_reason(m, x->node, false);
        if (reason != FLOW_REUSE_NREASONS) return reason;
        if (!f_style_equal(x->style, y->style)) return FLOW_REUSE_STYLE;
        if (x->content || x->absolute || x->floated) *ordinary = false;
        if (x->inl) {
            const FInline *u = x->inl, *v = y->inl;
            if (u->node != v->node || u->pieces != v->pieces ||
                !f_style_equal(u->style, v->style)) return FLOW_REUSE_STRUCTURE;
            // A split inline's shared spans can escape a candidate root.
            // Keep it fresh until a larger ownership boundary is certified.
            if (u->pos || v->pos || u->pieces != 1 || u->open_in || v->open_in)
                *ordinary = false;
        }
    }
    return x || y ? FLOW_REUSE_STRUCTURE : FLOW_REUSE_NREASONS;
}

static void reuse_match_box(FReuseMatch *m, FBox *box, const FBox *ordered,
                            size_t parent, bool atomic)
{
    size_t index = m->count++;
    FReusePair *p = &m->pairs[index];
    *p = (FReusePair){.box = box, .parent = parent, .reason = FLOW_REUSE_NREASONS};
    size_t at = reuse_hash((const void *)box, m->nslots);
    while (m->slots[at]) at = (at + 1) & (m->nslots - 1);
    m->slots[at] = (uint32_t)index + 1;
    const os64_html_node_t *identity = box->node;
    if (identity) {
        size_t i = reuse_find(m->previous->reuse, identity);
        size_t j = reuse_find(m->tree->reuse, identity);
        if (i != SIZE_MAX && j != SIZE_MAX) {
            FReuseNode *n = &reuse_nodes(m->previous->reuse)[i];
            if (!n->ambiguous && !reuse_nodes(m->tree->reuse)[j].ambiguous) p->old = n->box;
        }
    } else {
        if (ordered && !ordered->node) p->old = ordered;
        // Anonymous content belongs to its nearest named containing box.
        const FBox *up = box->parent;
        while (up && !up->node) up = up->parent;
        identity = up ? up->node : NULL;
    }
    const FBox *old = p->old;
    p->ordinary = box->kind == FB_BLOCK && !box->flex && !box->grid &&
                  !box->positioned && !box->out_of_flow && !box->transformed &&
                  box->floated == FLOW_FLOAT_NONE && !box->pos && !box->homed && !atomic;
    if (!old || old->kind != box->kind || old->node != box->node ||
        old->ifc != box->ifc || old->flex != box->flex || old->grid != box->grid ||
        old->positioned != box->positioned || old->out_of_flow != box->out_of_flow ||
        old->transformed != box->transformed || old->floated != box->floated ||
        old->nitems != box->nitems || old->collapse_space != box->collapse_space ||
        old->marker_len != box->marker_len || !!old->marker != !!box->marker ||
        !reuse_bytes_equal(old->marker, box->marker, box->marker_len))
        p->reason = FLOW_REUSE_STRUCTURE;
    else if (!old->placed || old->unfinished || box->unfinished)
        p->reason = FLOW_REUSE_INCOMPLETE;
    else if ((p->reason = reuse_node_reason(m, identity, true)) != FLOW_REUSE_NREASONS) {
        // The reason is retained while descendants get their own checks.
    } else if (!f_style_equal(box->style, old->style)) p->reason = FLOW_REUSE_STYLE;
    else p->reason = reuse_items_equal(m, box, old, &p->ordinary);

    const FBox *c = old ? old->first : NULL;
    for (FBox *child = box->first; child; child = child->next) {
        reuse_match_box(m, child, c, index, false);
        if (c) c = c->next;
    }
    for (FItem *it = box->items; it; it = it->next)
        if (it->content) reuse_match_box(m, it->content, NULL, index, true);
}

static FReuseMatch *reuse_match_new(flow_tree_t *tree, const flow_tree_t *previous,
                                   const flow_env_t *env)
{
    if (!tree->reuse_stats.source_ready || !tree->reuse) return NULL;
    size_t count = tree->boxes->count, cap = f_arena_budget(env) - tree->reuse->bytes;
    if (cap < sizeof(FReuseMatch) || count > (cap - sizeof(FReuseMatch)) / sizeof(FReusePair) ||
        count >= UINT32_MAX / 2) goto budget;
    size_t slots = 2;
    while (slots < count * 2) slots *= 2;
    size_t bytes = sizeof(FReuseMatch) + count * sizeof(FReusePair);
    if (slots > (cap - bytes) / sizeof(uint32_t)) goto budget;
    bytes += slots * sizeof(uint32_t);
    FReuseMatch *m = os64_calloc(1, bytes);
    if (!m) { tree->reuse_stats.refused[FLOW_REUSE_NO_MEMORY]++; return NULL; }
    *m = (FReuseMatch){.pairs = (void *)(m + 1), .capacity = count, .nslots = slots,
        .bytes = bytes, .tree = tree, .previous = previous};
    m->slots = (void *)(m->pairs + count);
    if (tree->boxes->root) reuse_match_box(m, tree->boxes->root, previous->boxes->root, SIZE_MAX, false);
    // Compare ordered children once, then fold each child's result into its
    // parent. A dirty/excluded ancestor does not prevent a local child match.
    for (size_t i = m->count; i-- > 0;) {
        FReusePair *p = &m->pairs[i];
        const FBox *a = p->box->first, *b = p->old ? p->old->first : NULL;
        for (; a && b; a = a->next, b = b->next) {
            FReusePair *child = reuse_pair(m, a);
            if (!child || child->old != b || !child->equal) {
                if (p->reason == FLOW_REUSE_NREASONS) p->reason = FLOW_REUSE_STRUCTURE;
            }
        }
        if ((a || b) && p->reason == FLOW_REUSE_NREASONS) p->reason = FLOW_REUSE_STRUCTURE;
        p->equal = p->reason == FLOW_REUSE_NREASONS;
        tree->reuse_stats.boxes_compared++;
        tree->reuse_stats.subtrees_equal += p->equal;
        bool item = p->box->parent && (p->box->parent->flex || p->box->parent->grid);
        tree->reuse_stats.geometry_candidates += p->equal && p->ordinary && !item;
        if (!p->equal) tree->reuse_stats.refused[p->reason]++;
        else if (!p->ordinary || item) tree->reuse_stats.refused[FLOW_REUSE_BOX_ROLE]++;
        if (p->parent != SIZE_MAX) {
            FReusePair *up = &m->pairs[p->parent];
            up->ordinary &= p->ordinary;
            if (!p->equal && up->reason == FLOW_REUSE_NREASONS) up->reason = FLOW_REUSE_STRUCTURE;
        }
    }
    return m;
budget:
    tree->reuse_stats.refused[FLOW_REUSE_BUDGET]++;
    return NULL;
}
