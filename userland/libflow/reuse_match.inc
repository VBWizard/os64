// Review the equality and role inventories when these records change. Size
// guards supplement that review; a same-size field edit still needs inspection.
_Static_assert(sizeof(FBox) == 560 && sizeof(FItem) == 104 && sizeof(FInline) == 72,
               "review layout correspondence fields");
#define ROLE(r) ((uint16_t)(1u << (r)))
// Call-local correspondence. No old arena address escapes into the new tree.
FReusePair *f_reuse_pair(const FReuseMatch *m, const FBox *box)
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

bool f_reuse_role_root(const FReuseMatch *m, const FReusePair *p, flow_role_t *role)
{
    if (!p->role_refused || p->in_copy || (p->parent != SIZE_MAX && m->pairs[p->parent].role_refused))
        return false;
    uint16_t bits = p->roles ? p->roles : p->roles_below;
    int r = 0;
    while (r < FLOW_ROLE_KIND && !(bits & ROLE(r))) r++;
    *role = (flow_role_t)r;
    return true;
}

void *f_reuse_pointer(const FReuseMatch *m, const void *old)
{
    if (!m || !old) return NULL;
    size_t at = reuse_hash(old, m->pointer_slots);
    while (m->pointers[at].from) {
        if (m->pointers[at].from == old) return m->pointers[at].to;
        at = (at + 1) & (m->pointer_slots - 1);
    }
    return NULL;
}
static void reuse_bind(FReuseMatch *m, const void *old, void *fresh)
{
    if (!old) return;
    size_t at = reuse_hash(old, m->pointer_slots);
    while (m->pointers[at].from && m->pointers[at].from != old)
        at = (at + 1) & (m->pointer_slots - 1);
    m->pointers[at] = (FReusePointer){old, fresh};
}

static const os64_html_node_t *reuse_element(const os64_html_document_t *doc, const os64_html_node_t *node)
{
    while (node && node->kind != OS64_HTML_ELEMENT) node = f_flat_parent(doc, node);
    return node;
}

static bool reuse_derived_equal(FReuseMatch *m, const os64_html_node_t *node)
{
    node = reuse_element(m->tree->doc, node);
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
                                            bool *inner, uint16_t *roles)
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
        // An atom's content and an out-of-flow box are boxes of their own,
        // judged as they are matched; a float is refused inside a copy.
        if (x->floated) {
            *inner = false;
            *roles |= ROLE(FLOW_ROLE_FLOAT);
        }
        if (x->inl) {
            const FInline *u = x->inl, *v = y->inl;
            if (u->node != v->node || u->pieces != v->pieces ||
                !f_style_equal(u->style, v->style)) return FLOW_REUSE_STRUCTURE;
            // A split inline's shared spans can escape a candidate root.
            // Keep it fresh until a larger ownership boundary is certified.
            if (u->pos || v->pos || u->pieces != 1 || u->open_in || v->open_in) {
                *inner = false;
                *roles |= ROLE(FLOW_ROLE_INLINE);
            }
        }
    }
    return x || y ? FLOW_REUSE_STRUCTURE : FLOW_REUSE_NREASONS;
}

static void reuse_match_box(FReuseMatch *m, FBox *box, const FBox *ordered,
                            size_t parent, bool atomic)
{
    size_t index = m->count++;
    FReusePair *p = &m->pairs[index];
    *p = (FReusePair){.box = box, .parent = parent, .subtree_count = 1, .reason = FLOW_REUSE_NREASONS};
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
    // Inside a copy may stand (RELAYOUT_GRAFT.md § Roles inside a copy): a
    // block container whatever its role (a flex or grid container or item,
    // a positioned, transformed or filtered box, an atom's content, an
    // out-of-flow box, whose containing block is the subtree's question,
    // `escapes`); not a float, a table or a part of one, a block-level
    // picture, nor a block holding a positioned inline's pieces. The root
    // is in the flow among blocks besides.
    p->inner = box->kind == FB_BLOCK && box->floated == FLOW_FLOAT_NONE && !box->homed;
    p->ordinary = !box->out_of_flow && !atomic;
    if (box->out_of_flow) {
        p->escapes = 1;
        const FPos *cb = box->pos ? box->pos->cb : NULL;
        if (cb && cb->kind == FP_BOX && cb->box) {
            FReusePair *holder = f_reuse_pair(m, cb->box);
            if (holder) holder->cb_held++;
        }
    }
    if (atomic && old && old->reuse_call && old->reuse_call->has_queries) {
        p->inner = false;
        p->atom_floats = true;
    }
    // The role counter's, when the layout is profiled: what keeps the box
    // from being a copy's root. Every box whose position is not static is
    // `positioned`: an absolute or fixed one is counted as that, not as
    // positioned in the flow.
    p->roles = !m->clock ? 0 : (uint16_t)((box->kind != FB_BLOCK ? ROLE(FLOW_ROLE_KIND) : 0) |
               (box->out_of_flow ? ROLE(FLOW_ROLE_OUT_OF_FLOW) : 0) |
               (box->floated != FLOW_FLOAT_NONE ? ROLE(FLOW_ROLE_FLOAT) : 0) |
               (box->homed ? ROLE(FLOW_ROLE_INLINE) : 0) | (atomic ? ROLE(FLOW_ROLE_ATOMIC) : 0) |
               (box->parent && (box->parent->flex || box->parent->grid) ? ROLE(FLOW_ROLE_ITEM) : 0));
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
    else p->reason = reuse_items_equal(m, box, old, &p->inner, &p->roles);

    // Missing-image alt text is processed during pass 3, outside fresh
    // items. Keep that IFC fresh until its processed-byte mapping is shared.
    if (old) for (const FLine *ln = old->lines; ln; ln = ln->next)
        for (const FFrag *fr = ln->frags; fr; fr = fr->next)
            if (fr->kind == FF_TEXT && fr->item && fr->item->kind == FI_ATOMIC) {
                p->inner = false;
                p->roles |= ROLE(FLOW_ROLE_ALT_TEXT);
            }
    const FBox *c = old ? old->first : NULL;
    for (FBox *child = box->first; child; child = child->next) {
        reuse_match_box(m, child, c, index, false);
        if (c) c = c->next;
    }
    for (FItem *it = box->items; it; it = it->next)
        if (it->content) reuse_match_box(m, it->content, NULL, index, true);
}

static FReuseMatch *reuse_match_new(flow_tree_t *tree, const flow_tree_t *previous,
                                   const flow_env_t *env, int64_t (*clock)(void *), void *clock_ctx)
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
    size_t pointers = 2;
    if (tree->reuse->nitems > SIZE_MAX / 4) goto budget;
    while (pointers < tree->reuse->nitems * 4) {
        if (pointers > SIZE_MAX / 2) goto budget;
        pointers *= 2;
    }
    if (pointers > (cap - bytes) / sizeof(FReusePointer)) goto budget;
    bytes += pointers * sizeof(FReusePointer);
    FReuseMatch *m = os64_calloc(1, bytes);
    if (!m) { tree->reuse_stats.refused[FLOW_REUSE_NO_MEMORY]++; return NULL; }
    *m = (FReuseMatch){.pairs = (void *)(m + 1), .capacity = count, .nslots = slots,
        .bytes = bytes, .staging_left = cap - bytes, .pointer_slots = pointers,
        .tree = tree, .previous = previous, .clock = clock, .clock_ctx = clock_ctx};
    m->slots = (void *)(m->pairs + count);
    m->pointers = (void *)(m->slots + slots);
    tree->reuse_stats.metadata_peak = tree->reuse->bytes + bytes;
    if (tree->boxes->root) reuse_match_box(m, tree->boxes->root, previous->boxes->root, SIZE_MAX, false);
    // Compare ordered children once, then fold each child's result into its
    // parent. A dirty/excluded ancestor does not prevent a local child match.
    for (size_t i = m->count; i-- > 0;) {
        FReusePair *p = &m->pairs[i];
        const FBox *a = p->box->first, *b = p->old ? p->old->first : NULL;
        for (; a && b; a = a->next, b = b->next) {
            FReusePair *child = f_reuse_pair(m, a);
            if (!child || child->old != b || !child->equal) {
                if (p->reason == FLOW_REUSE_NREASONS) p->reason = FLOW_REUSE_STRUCTURE;
            }
        }
        if ((a || b) && p->reason == FLOW_REUSE_NREASONS) p->reason = FLOW_REUSE_STRUCTURE;
        p->equal = p->reason == FLOW_REUSE_NREASONS;
        if (p->equal) {
            FItem *fresh = p->box->items;
            for (const FItem *old = p->old->items; old; old = old->next, fresh = fresh->next) {
                reuse_bind(m, old, fresh);
                reuse_bind(m, old->inl, fresh->inl);
            }
        }
        tree->reuse_stats.boxes_compared++;
        tree->reuse_stats.subtrees_equal += p->equal;
        bool item = p->box->parent && (p->box->parent->flex || p->box->parent->grid);
        // Its own containing-block entries are met: what is left escapes.
        p->escapes -= p->cb_held;
        // A box that is no root whatever its subtree holds (an item, an
        // out-of-flow box, an atom's content) is refused for that; a root's
        // other refusals are for what its subtree holds.
        bool root_kind = p->ordinary && !item;
        p->ordinary = p->ordinary && p->inner && p->escapes == 0;
        tree->reuse_stats.geometry_candidates += p->equal && p->ordinary && !item;
        if (!p->equal) tree->reuse_stats.refused[p->reason]++;
        else if (!root_kind) tree->reuse_stats.refused[FLOW_REUSE_BOX_ROLE]++;
        else if (p->atom_floats) tree->reuse_stats.refused[FLOW_REUSE_ATOM_FLOATS]++;
        else if (p->inner && p->escapes) tree->reuse_stats.refused[FLOW_REUSE_ABSOLUTE_OUTSIDE]++;
        else if (!p->ordinary) tree->reuse_stats.refused[FLOW_REUSE_BOX_ROLE]++;
        // The out-of-flow boxes under it whose containing block is outside
        // it keep it from being a root (its own escape, an out-of-flow box's,
        // is its own role): said here, where the containing blocks it holds
        // are already taken off, never passed up from a child, whose escape
        // its parent may hold.
        p->roles_below = (uint16_t)(p->roles_below & ~ROLE(FLOW_ROLE_OUT_OF_FLOW));
        if (p->escapes > (p->box->out_of_flow ? 1u : 0u)) p->roles_below |= ROLE(FLOW_ROLE_OUT_OF_FLOW);
        p->role_refused = p->equal && (!p->ordinary || item);
        if (p->equal && p->ordinary && !item) p->cand_boxes = p->subtree_count;
        if (p->parent != SIZE_MAX) {
            FReusePair *up = &m->pairs[p->parent];
            up->cand_boxes += p->cand_boxes;
            up->inner &= p->inner;
            up->atom_floats |= p->atom_floats;
            up->escapes += p->escapes;
            // What refuses a box above is what may not stand inside a copy:
            // not being a root (an item, an atom's content, an out-of-flow
            // box) is the box's own refusal, not its parent's. An escape
            // past the parent is the parent's to say at its own turn.
            uint16_t inside = (uint16_t)(p->roles & ~(ROLE(FLOW_ROLE_ITEM) | ROLE(FLOW_ROLE_ATOMIC) |
                                                    ROLE(FLOW_ROLE_OUT_OF_FLOW)));
            if (p->atom_floats) inside |= ROLE(FLOW_ROLE_ATOMIC);
            up->roles_below |= (uint16_t)(inside | (p->roles_below & ~ROLE(FLOW_ROLE_OUT_OF_FLOW)));
            up->subtree_count += p->subtree_count;
            if (!p->equal && up->reason == FLOW_REUSE_NREASONS) up->reason = FLOW_REUSE_STRUCTURE;
        }
    }
    // THE ROLE COUNTER'S ROOTS (a record: flow_reuse_stats_t.role_*), when
    // the layout is profiled: each equal subtree refused for a role whose
    // parent is not one too and which no candidate above it takes in (a
    // copy takes in what is under it, refused or not), by its own first
    // role or else the first under it (f_reuse_role_root); its boxes leave
    // out what candidates under it take in. A parent's pair comes before
    // its children's.
    for (size_t i = 0; m->clock && i < m->count; i++) {
        FReusePair *p = &m->pairs[i];
        if (p->parent == SIZE_MAX) continue;
        const FReusePair *up = &m->pairs[p->parent];
        bool up_item = up->box->parent && (up->box->parent->flex || up->box->parent->grid);
        p->in_copy = up->in_copy || (up->equal && up->ordinary && !up_item);
    }
    for (size_t i = 0; m->clock && i < m->count; i++) {
        flow_role_t r;
        const FReusePair *p = &m->pairs[i];
        if (!f_reuse_role_root(m, p, &r)) continue;
        tree->reuse_stats.role_roots[r]++;
        tree->reuse_stats.role_boxes[r] += p->subtree_count - p->cand_boxes;
        for (int k = 0; k < FLOW_ROLES; k++)
            tree->reuse_stats.role_inside[k] += ((p->roles | p->roles_below) & ROLE(k)) != 0;
    }
    return m;
budget:
    tree->reuse_stats.refused[FLOW_REUSE_BUDGET]++;
    return NULL;
}
