// restyle.inc — a restyle kept in the tree it changes (flow_restyle_in_place;
// BRIEF_LAYOUT_NEEDS.md N1). Included by flow.c, whose tree, frames and
// ink it writes.
//
// The styles are rebuilt as a layout would rebuild them. When every element
// whose style came out different differs only in what the painter reads
// (f_style_paint_only), and nothing else about it moved, the new values are
// written into the tree's own styles in place: the boxes, the fragments and
// the public tree keep pointing at the same structs, which now hold the new
// values, and no box is built or placed. The lists those values point into
// are copied into the tree's restyle arena first, so that nothing points
// into the build, which is freed. The animation tick is the precedent: it
// writes a ticking element's style in place the same way, and the frames,
// the ink and the drawn stamps follow as they do there. Where a transform
// took a new value, the page's extent and the scroll containers' ranges
// are reached again from where placement left them, which the tick does
// not do.

// Whether two records' anchors (or fixed anchors) are the same element:
// the same node, or each record's own. A pseudo-element that makes a
// containing block is its own anchor, and its node is the style pass's,
// made again by every pass, so `o_self` and `n_self` (the node each record
// is for) are what say its two anchors are one.
static bool keep_same_anchor(const os64_html_node_t *o, const os64_html_node_t *o_self,
                             const os64_html_node_t *n, const os64_html_node_t *n_self)
{
    return o == n || (o == o_self && n == n_self);
}

// The first derived part of two records of one element that differs, by
// name, or NULL when they agree (the decoration is keep_judge's to judge
// apart): what boxes, lists, links, anchors and pseudo-elements were built
// from. `o_self` and `n_self`: the node each record is for (keep_same_anchor).
static const char *keep_derived_differs(const FStyled *o, const os64_html_node_t *o_self,
                                        const FStyled *n, const os64_html_node_t *n_self)
{
    return o->holds_block != n->holds_block ? "holds_block"
         : o->link != n->link ? "link"
         : o->lists != n->lists || o->lists_or_dls != n->lists_or_dls || o->items != n->items ? "lists"
         : o->in_nobr != n->in_nobr ? "in_nobr"
         : o->quotes != n->quotes ? "quotes"
         : o->has_rp != n->has_rp ? "has_rp"
         : !keep_same_anchor(o->anchor, o_self, n->anchor, n_self) ||
           !keep_same_anchor(o->fixed_anchor, o_self, n->fixed_anchor, n_self) ? "anchor"
         : o->transparent != n->transparent ? "transparent"
         : o->lays_out_items != n->lays_out_items ? "lays_out_items"
         : o->pseudo_declared != n->pseudo_declared || (o->pseudo[0] != NULL) != (n->pseudo[0] != NULL) ||
           (o->pseudo[1] != NULL) != (n->pseudo[1] != NULL) ? "pseudo-elements"
         : NULL;
}

// Whether two pseudo-elements' nodes hold the same content text (or both
// none): a different text is a different box, which is placement's.
static bool keep_same_text(const os64_html_node_t *a, const os64_html_node_t *b)
{
    const os64_html_node_t *x = a != NULL ? a->first_child : NULL, *y = b != NULL ? b->first_child : NULL;
    if ((x == NULL) != (y == NULL))
        return false;
    return x == NULL || (x->text_len == y->text_len && os64_memcmp(x->text, y->text, x->text_len) == 0);
}

typedef struct {
    FStyled *was;
    const FStyled *is;
} FKeep;

typedef struct {
    FKeep *pairs;
    int32_t n, cap;
    flow_restyle_reason_t why;
    // Where the restyle declined, and what was found there (flow_restyle_t).
    const os64_html_node_t *at;
    char detail[48];
} FKeeps;

// A decline at `node`, said in `what`.
static bool keep_declined(FKeeps *k, flow_restyle_reason_t why, const os64_html_node_t *node, const char *what)
{
    k->why = why;
    k->at = node;
    size_t n = 0;
    for (; what != NULL && what[n] != '\0' && n + 1 < sizeof(k->detail); n++)
        k->detail[n] = what[n];
    k->detail[n] = '\0';
    return false;
}

// Judges one element's (or pseudo-element's) two records: nothing to do,
// one more pair to write, or the reason the restyle declines. `node` is
// the element it is told at; `was_self` and `is_self` the nodes the two
// records are for, which differ for a pseudo-element (keep_same_anchor).
static bool keep_judge(FKeeps *k, const os64_html_node_t *node, FStyled *was, const FStyled *is,
                       const os64_html_node_t *was_self, const os64_html_node_t *is_self)
{
    if (was == NULL || is == NULL)
        return keep_declined(k, FLOW_RESTYLE_MEMBERSHIP, node, was == NULL ? "no record before" : "no record after");
    // An animated element's sampled style may come out the same while what
    // its keyframes resolve against moved: an `inherit` endpoint, or an
    // implicit keyframe's underlying value. It is kept only when its
    // underlying style (the motion's `base`, which inherited values reach)
    // came out the same as well.
    if ((was->motion != NULL) != (is->motion != NULL) ||
        (was->motion != NULL && (was->motion->nanims != is->motion->nanims ||
                                 !f_style_equal(&was->motion->base, &is->motion->base))))
        return keep_declined(k, FLOW_RESTYLE_MOTION, node, "an animation's underlying style");
    bool same_style = f_style_equal(&was->style, &is->style);
    bool same_decoration = was->decoration == is->decoration &&
                           (was->decoration == 0 || os64_memcmp(&was->decoration_colors, &is->decoration_colors,
                                                                sizeof(was->decoration_colors)) == 0);
    const char *derived = keep_derived_differs(was, was_self, is, is_self);
    if (same_style && same_decoration && derived == NULL)
        return true;
    if (was->motion != NULL || is->motion != NULL)
        return keep_declined(k, FLOW_RESTYLE_MOTION, node, "an animated element");
    if (derived != NULL) {
        char said[48] = "derived ";
        os64_strcopy(said + 8, sizeof(said) - 8, derived);
        return keep_declined(k, FLOW_RESTYLE_GEOMETRY, node, said);
    }
    if (!f_style_paint_only(&was->style, &is->style)) {
        int64_t at = f_style_placement_byte(&was->style, &is->style);
        if (at < 0)
            return keep_declined(k, FLOW_RESTYLE_GEOMETRY, node,
                                 at == -2 ? "a transform or filter came or went" : "a placement list");
        // "placement byte N", N in decimal (the offset in flow_style_t).
        char said[48] = "placement byte ", digits[24];
        size_t n = 0, used = os64_strlen(said);
        do {
            digits[n++] = (char)('0' + at % 10);
            at /= 10;
        } while (at > 0 && n < sizeof(digits));
        while (n > 0 && used + 1 < sizeof(said))
            said[used++] = digits[--n];
        said[used] = '\0';
        return keep_declined(k, FLOW_RESTYLE_GEOMETRY, node, said);
    }
    // Whether a box comes to be a group or a context, which changes the
    // layers, is asked of the boxes themselves before anything is written
    // (flow_restyle_in_place). An opacity's side of 0 is the record's
    // `transparent` (an unpainted box), which keep_derived_differs compares.
    // The fragments hold a copy of their element's decoration and its
    // colours (layout.c), which no restyle in place refreshes.
    if (!same_decoration)
        return keep_declined(k, FLOW_RESTYLE_DECORATION, node, "a decoration or its colours");
    if (k->n == k->cap) {
        int32_t cap = k->cap != 0 ? k->cap * 2 : 16;
        FKeep *grown = os64_realloc(k->pairs, (size_t)cap * sizeof(*grown));
        if (grown == NULL)
            return keep_declined(k, FLOW_RESTYLE_ROOM, node, "no memory for the pairs");
        k->pairs = grown;
        k->cap = cap;
    }
    k->pairs[k->n++] = (FKeep){was, is};
    return true;
}

// The node `key` of a build is a pseudo-element's: its element's record
// there names it.
static bool keep_is_pseudo(const FStyles *b, const os64_html_node_t *key)
{
    const FStyled *up = key->parent != NULL ? f_style_of(b, key->parent) : NULL;
    return up != NULL && (up->pseudo[0] == key || up->pseudo[1] == key);
}

// A decline found at `node` because its anchor moved is a descendant's
// echo of an ancestor that came to make a containing block, or ceased to
// (the elements are met in the map's order, not the tree's), so the record
// is told that ancestor, the nearest in the flat tree whose being its own
// anchor turned, as the cause. The verdict stays the echo's: geometry.
static void keep_explain_anchor(FKeeps *k, const FStyles *old, const FStyles *b, const os64_html_node_t *node)
{
    for (const os64_html_node_t *a = node != NULL ? f_flat_parent(old->doc, node) : NULL; a != NULL;
         a = f_flat_parent(old->doc, a)) {
        const FStyled *was = f_style_of(old, a), *is = f_style_of(b, a);
        if (was == NULL || is == NULL)
            continue;
        if ((was->anchor == a) != (is->anchor == a) || (was->fixed_anchor == a) != (is->fixed_anchor == a)) {
            k->at = a;
            os64_strcopy(k->detail, sizeof(k->detail), "derived anchor, a containing block came or went");
            return;
        }
    }
    // No ancestor turned, yet the anchor did: the two builds computed it
    // otherwise, which is the builder's to explain.
    os64_strcopy(k->detail, sizeof(k->detail), "derived anchor, no ancestor turned");
}

// The pairs to write for every element and pseudo-element of the new build
// `b` against the tree's `old`, or false with the reason.
static bool keep_collect(FKeeps *k, FStyles *old, const FStyles *b)
{
    if (b->map.count != old->map.count)
        return keep_declined(k, FLOW_RESTYLE_MEMBERSHIP, NULL, "the number of records");
    for (size_t i = 0; i < b->map.cap; i++) {
        const os64_html_node_t *node = b->map.keys[i];
        if (node == NULL || keep_is_pseudo(b, node))
            continue;
        FStyled *was = f_map_get(&old->map, node);
        const FStyled *is = b->map.vals[i];
        if (!keep_judge(k, node, was, is, node, node)) {
            if (k->why == FLOW_RESTYLE_GEOMETRY && os64_streq(k->detail, "derived anchor"))
                keep_explain_anchor(k, old, b, node);
            return false;
        }
        for (int p = 0; p < 2; p++) {
            if (is->pseudo[p] == NULL)
                continue;
            if (!keep_same_text(was->pseudo[p], is->pseudo[p]))
                return keep_declined(k, FLOW_RESTYLE_GEOMETRY, node, p ? "::after's text" : "::before's text");
            if (!keep_judge(k, node, f_map_get(&old->map, was->pseudo[p]), f_style_of(b, is->pseudo[p]),
                            was->pseudo[p], is->pseudo[p]))
                return false;
        }
    }
    return true;
}

// The styles written, by the struct a box points at: the records' (an
// element's, which its fragments share, or a pseudo-element's) and the
// anonymous boxes' taken again under them.
static bool keep_written(const FMap *written, const flow_style_t *s)
{
    return s != NULL && f_map_get(written, s) != NULL;
}

flow_restyle_t flow_restyle_in_place(flow_tree_t *tree, const os64_html_document_t *doc,
                                     const os64_page_t *model, const flow_env_t *env,
                                     const flow_reuse_inputs_t *inputs, int32_t width)
{
    flow_restyle_t out = {FLOW_RESTYLE_DECLINED, FLOW_RESTYLE_NO_TREE, 0, NULL, ""};
    if (tree == NULL || doc == NULL || env == NULL || inputs == NULL || tree->doc != doc || tree->incomplete ||
        tree->styles == NULL || tree->restyle_spent)
        return out;
    // What changed since the tree, and whether it was laid out as this
    // layout would be: the graft's own source test. Its cascade's
    // generation among them: a tree's records point into the cascade they
    // were computed in (a family's names are that cascade's strings), and a
    // host frees a cascade with the tree laid out in it, so a tree kept
    // under another cascade would outlive what its unwritten records read.
    FReuseSnapshot *r = tree->reuse;
    flow_reuse_stats_t ignored = {0};
    out.why = FLOW_RESTYLE_NO_HISTORY;
    if (r == NULL || !inputs->resource_owner || !reuse_read(r, &ignored))
        return out;
    out.why = FLOW_RESTYLE_STRUCTURE;
    if (r->structural) {
        out.at = r->structural_at;
        os64_strcopy(out.detail, sizeof(out.detail), r->structural_what);
        // Whether the element it is told at has a box: one with none is in a
        // part of the page no box was made for (`display: none` above it,
        // say), which the record says.
        if (out.at != NULL && flow_box_for(tree, out.at) == NULL) {
            size_t n = os64_strlen(out.detail);
            os64_strcopy(out.detail + n, sizeof(out.detail) - n, ", no box");
        }
        return out;
    }
    out.why = FLOW_RESTYLE_INPUTS;
    const char *input = r->width != width || r->doc != doc ? "another width"
                      : r->generation != garb_cascade_generation(env->cascade) ? "another cascade"
                      : doc->quirks == OS64_HTML_QUIRKS ? "a quirks-mode page"
                      : !reuse_inputs_equal(&r->inputs, inputs) ? "fonts, pictures or form state"
                      : !reuse_environment_equal(&r->env, env) ? "the environment" : NULL;
    if (input != NULL) {
        os64_strcopy(out.detail, sizeof(out.detail), input);
        return out;
    }
    FStyles *b = f_style_build_reusing(doc, model, env, tree->styles, false);
    out.why = FLOW_RESTYLE_STYLES;
    if (b == NULL || b->lists_refused) {
        f_style_free(b);
        return out;
    }
    FKeeps k = {NULL, 0, 0, FLOW_RESTYLE_NONE, NULL, ""};
    if (!keep_collect(&k, tree->styles, b)) {
        out.why = k.why;
        out.at = k.at;
        os64_strcopy(out.detail, sizeof(out.detail), k.detail);
        os64_free(k.pairs);
        f_style_free(b);
        return out;
    }
    // Every value first, into the restyle arena, before any is written: a
    // room that runs out leaves the tree's styles as they were, and marks
    // the room spent, so that no later restyle is kept in this tree.
    flow_style_t *values = k.n > 0 ? os64_malloc((size_t)k.n * sizeof(*values)) : NULL;
    bool room = k.n == 0 || values != NULL;
    for (int32_t i = 0; room && i < k.n; i++) {
        values[i] = k.pairs[i].is->style;
        room = f_style_copy_lists(&values[i], &tree->restyle);
    }
    if (!room) {
        tree->restyle_spent = true;
        out.why = FLOW_RESTYLE_ROOM;
        os64_free(values);
        os64_free(k.pairs);
        f_style_free(b);
        return out;
    }
    FMap written = {0};
    for (int32_t i = 0; room && i < k.n; i++)
        room = f_map_put(&written, &k.pairs[i].was->style, &k.pairs[i]);
    if (!room) {
        f_map_free(&written);
        out.why = FLOW_RESTYLE_ROOM;
        os64_free(values);
        os64_free(k.pairs);
        f_style_free(b);
        return out;
    }
    // A box that is a group (is_group) has layers opened and closed round
    // it, and one that makes a context (makes_context) paints its own: a
    // new value that changes either changes the public tree's layers, which
    // no restyle in place rebuilds. A transform drawn through its matrix is
    // a group (own_through), and one drawn upright or moved is not, so the
    // copy's frame is filled from the new value for is_group to ask. Judged
    // on a copy of each written box with its new value before anything is
    // written.
    bool transformed = false;
    for (const flow_box_t *box = tree->root; k.n > 0 && box != NULL; box = box_after(box)) {
        FKeep *kp = f_map_get(&written, box->style);
        if (kp == NULL)
            continue;
        // The copy's own frame is a copy too, naming the copy as its box, as
        // makes_context and own_through ask of a box's own frame.
        const flow_style_t *value = &values[kp - k.pairs];
        const flow_frame_t *f = box->frame;
        bool framed = f != NULL && f->box == box && f->kind == F_FRAME_TRANSFORM;
        flow_frame_t tried = framed ? *f : (flow_frame_t){0};
        flow_box_t tried_box = *box;
        tried_box.style = value;
        if (framed) {
            transform_fill(&tried, value);
            tried.box = &tried_box;
            tried_box.frame = &tried;
        }
        bool layers_same = is_group(&tried_box) == is_group(box) && makes_context(&tried_box) == makes_context(box);
        if (!layers_same) {
            f_map_free(&written);
            out.why = FLOW_RESTYLE_STACKING;
            // A pseudo-element's box is told at its host: its own node is
            // the style pass's, which never leaves libflow.
            out.at = box->node != NULL && f_pseudo_of(box->node) != GARB_PSEUDO_NONE ? box->node->parent : box->node;
            os64_strcopy(out.detail, sizeof(out.detail),
                         is_group(&tried_box) != is_group(box) ? "a group came or went" : "a context came or went");
            os64_free(values);
            os64_free(k.pairs);
            f_style_free(b);
            return out;
        }
        transformed = transformed || framed;
    }
    // A transform moves how far the page and its scroll containers reach,
    // which is worked again from where placement left them.
    if (transformed && tree->nscrollers > 0 && tree->reach_base_ranges == NULL) {
        f_map_free(&written);
        out.why = FLOW_RESTYLE_ROOM;
        os64_free(values);
        os64_free(k.pairs);
        f_style_free(b);
        return out;
    }
    for (int32_t i = 0; i < k.n; i++) {
        k.pairs[i].was->style = values[i];
        k.pairs[i].was->decoration_colors = k.pairs[i].is->decoration_colors;
    }
    // The boxes that draw a written style: an anonymous box under one takes
    // its parent's inherited values again (in tree order, so a chain of them
    // follows); a transform's frame is filled again, the ink grows to the
    // new shadows and filters, and the drawn stamps move, as an animation
    // tick's do. An anonymous box past the map's room keeps its old values,
    // which is said: the restyle answers as declined, after its writes, and
    // the layout that follows makes the tree afresh.
    uint64_t stamp = k.n > 0 ? ++tree->drawn : 0;
    bool whole = true;
    for (flow_box_t *box = tree->root; k.n > 0 && box != NULL; box = (flow_box_t *)box_after(box)) {
        bool own = keep_written(&written, box->style);
        if (!own && box->node == NULL && box->parent != NULL && box->style != NULL &&
            keep_written(&written, box->parent->style)) {
            f_style_anonymous(tree->styles, box->parent->style, box->style->display, (flow_style_t *)box->style);
            whole = whole && f_map_put(&written, box->style, box);
            own = true;
        }
        if (!own)
            continue;
        flow_frame_t *f = (flow_frame_t *)box->frame;
        if (f != NULL && f->box == box && f->kind == F_FRAME_TRANSFORM)
            transform_fill(f, box->style);
        ink_grown(box);
        drawn_changed(box, true, stamp);
    }
    f_map_free(&written);
    // The page's extent and the scroll containers' ranges reached again,
    // from where placement left them, now the transforms draw elsewhere.
    if (transformed) {
        tree->layout->width = tree->reach_base_w;
        tree->layout->height = tree->reach_base_h;
        for (int32_t i = 0; i < tree->nscrollers; i++)
            tree->scrollers[i]->range = tree->reach_base_ranges[i];
        reach_stacked(tree);
        sticky_reach_scrolled(tree);
        // A range that shrank holds its scroll within it, as a layout's
        // restored scroll is held (flow_scroll_set).
        for (int32_t i = 0; i < tree->nscrollers; i++) {
            flow_frame_t *s = tree->scrollers[i];
            if (s->at.x > s->range.x) s->at.x = s->range.x;
            if (s->at.y > s->range.y) s->at.y = s->range.y;
        }
    }
    tree->styles->cascade_updates = b->cascade_updates;
    out = (flow_restyle_t){k.n > 0 ? FLOW_RESTYLE_PAINT : FLOW_RESTYLE_NOTHING, FLOW_RESTYLE_NONE, (uint64_t)k.n, NULL, ""};
    if (!whole) {
        tree->restyle_spent = true;
        out = (flow_restyle_t){FLOW_RESTYLE_DECLINED, FLOW_RESTYLE_ROOM, (uint64_t)k.n, NULL, "an anonymous box past the map's room"};
    }
    os64_free(values);
    os64_free(k.pairs);
    f_style_free(b);
    return out;
}

// Whether the kept tree `a` answers as the whole layout `b` of the same
// document does, for what a restyle kept in place may write or must leave:
// - the same boxes in the same order, each of the same kind and node, with
//   the same border box, overflow, clip, and stacking and painting flags;
// - drawing styles equal in every field (f_style_equal);
// - the same decoration lines and, where there are lines, the same colours
//   (a box with none keeps colours it never draws, which a kept tree does
//   not refresh);
// - ink that covers the whole layout's, and an outline held wherever the
//   whole layout holds one (a kept tree's ink and outlines only grow,
//   ink_grown);
// - the same paint layers, in order, by box and part;
// - the same page extent, and each scroll container's range and scroll.
// The first difference is named: a box's at `node` (its element's, for an
// anonymous box), and what differed at `what`.
bool flow_restyle_check(const flow_tree_t *a, const flow_tree_t *b, const os64_html_node_t **node, const char **what)
{
    *node = NULL;
    *what = NULL;
    if (a == NULL || b == NULL) {
        *what = "a tree";
        return false;
    }
    const flow_box_t *x = a->root, *y = b->root;
    for (; x != NULL && y != NULL; x = box_after(x), y = box_after(y)) {
        bool ink_covers = true;
        for (int k = 0; k < 4; k++)
            ink_covers = ink_covers && x->ink[k] >= y->ink[k];
        const char *differs = x->kind != y->kind || x->node != y->node ? "the boxes"
                            : !same_rect(x->rect, y->rect) ? "a border box"
                            : !same_rect(x->overflow, y->overflow) ? "an overflow"
                            : x->clipped != y->clipped || (x->clipped && !same_rect(x->clip, y->clip)) ? "a clip"
                            : x->stacked != y->stacked || x->positioned != y->positioned || x->fixed != y->fixed ||
                              x->unpainted != y->unpainted || x->scroller != y->scroller ? "a box's stacking"
                            : (x->style == NULL) != (y->style == NULL) ? "a style"
                            : x->style != NULL && !f_style_equal(x->style, y->style) ? "a style"
                            : x->decoration != y->decoration ||
                              (x->decoration != 0 && (x->underline_color != y->underline_color ||
                                                      x->line_through_color != y->line_through_color))
                                  ? "a decoration"
                            : !ink_covers ? "an ink that does not cover the whole layout's"
                            : y->holds_outline && !x->holds_outline ? "an outline not held"
                            : NULL;
        if (differs != NULL) {
            *node = box_named(x);
            *what = differs;
            return false;
        }
    }
    if (x != NULL || y != NULL) {
        *what = "the number of boxes";
        return false;
    }
    if (a->nlayers != b->nlayers) {
        *what = "the paint layers";
        return false;
    }
    for (int32_t i = 0; i < a->nlayers; i++)
        if (a->layers[i].part != b->layers[i].part || a->layers[i].box->kind != b->layers[i].box->kind ||
            a->layers[i].box->node != b->layers[i].box->node) {
            *node = box_named(a->layers[i].box);
            *what = "the paint layers";
            return false;
        }
    if (a->layout->width != b->layout->width || a->layout->height != b->layout->height) {
        *what = "the page's extent";
        return false;
    }
    if (a->nscrollers != b->nscrollers) {
        *what = "the scroll containers";
        return false;
    }
    for (int32_t i = 0; i < a->nscrollers; i++) {
        const flow_frame_t *s = a->scrollers[i], *t = b->scrollers[i];
        if (s->range.x != t->range.x || s->range.y != t->range.y || s->at.x != t->at.x || s->at.y != t->at.y) {
            *node = box_named(s->box);
            *what = "a scroll container's range";
            return false;
        }
    }
    return true;
}
