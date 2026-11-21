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
// the ink and the drawn stamps follow as they do there.

// Whether the derived parts of two records of one element agree, but for
// the decoration, which keep_decoration judges: what boxes, lists, links,
// anchors and pseudo-elements were built from.
static bool keep_derived_equal(const FStyled *o, const FStyled *n)
{
    return o->holds_block == n->holds_block && o->link == n->link && o->lists == n->lists &&
           o->lists_or_dls == n->lists_or_dls && o->items == n->items && o->in_nobr == n->in_nobr &&
           o->quotes == n->quotes && o->has_rp == n->has_rp && o->anchor == n->anchor &&
           o->fixed_anchor == n->fixed_anchor && o->transparent == n->transparent &&
           o->lays_out_items == n->lays_out_items && o->pseudo_declared == n->pseudo_declared &&
           (o->pseudo[0] != NULL) == (n->pseudo[0] != NULL) && (o->pseudo[1] != NULL) == (n->pseudo[1] != NULL);
}

// Whether a paint-only change leaves the public tree's stacking as it was:
// an opacity that stays on its side of 1 (a group) and of 0 (unpainted, by
// `transparent`), a box blended and masked as before (as the public tree
// asks, for an atom and for a box that is none), and its visibility. The
// stacking is rebuilt by no restyle in place, so a change to it is declined.
static bool keep_stacking_same(const flow_style_t *a, const flow_style_t *b)
{
    return (a->opacity < 1000) == (b->opacity < 1000) && (a->opacity == 0) == (b->opacity == 0) &&
           f_blended(a, false) == f_blended(b, false) && f_blended(a, true) == f_blended(b, true) &&
           f_masked(a, false) == f_masked(b, false) && f_masked(a, true) == f_masked(b, true) &&
           a->visibility == b->visibility;
}

// A pseudo-element's content text, NULL for none.
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
} FKeeps;

// Judges one element's (or pseudo-element's) two records: nothing to do,
// one more pair to write, or the reason the restyle declines.
static bool keep_judge(FKeeps *k, FStyled *was, const FStyled *is)
{
    if (was == NULL || is == NULL) {
        k->why = FLOW_RESTYLE_MEMBERSHIP;
        return false;
    }
    bool same_style = f_style_equal(&was->style, &is->style);
    bool same_decoration = was->decoration == is->decoration &&
                           (was->decoration == 0 || os64_memcmp(&was->decoration_colors, &is->decoration_colors,
                                                                sizeof(was->decoration_colors)) == 0);
    if (same_style && same_decoration && keep_derived_equal(was, is))
        return true;
    if (was->motion != NULL || is->motion != NULL) {
        k->why = FLOW_RESTYLE_MOTION;
        return false;
    }
    if (!keep_derived_equal(was, is) || !f_style_paint_only(&was->style, &is->style)) {
        k->why = FLOW_RESTYLE_GEOMETRY;
        return false;
    }
    if (!keep_stacking_same(&was->style, &is->style)) {
        k->why = FLOW_RESTYLE_STACKING;
        return false;
    }
    // The fragments hold a copy of their element's decoration and its
    // colours (layout.c), which no restyle in place refreshes.
    if (!same_decoration) {
        k->why = FLOW_RESTYLE_DECORATION;
        return false;
    }
    if (k->n == k->cap) {
        int32_t cap = k->cap != 0 ? k->cap * 2 : 16;
        FKeep *grown = os64_realloc(k->pairs, (size_t)cap * sizeof(*grown));
        if (grown == NULL) {
            k->why = FLOW_RESTYLE_ROOM;
            return false;
        }
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

// The pairs to write for every element and pseudo-element of the new build
// `b` against the tree's `old`, or false with the reason.
static bool keep_collect(FKeeps *k, FStyles *old, const FStyles *b)
{
    if (b->map.count != old->map.count) {
        k->why = FLOW_RESTYLE_MEMBERSHIP;
        return false;
    }
    for (size_t i = 0; i < b->map.cap; i++) {
        const os64_html_node_t *node = b->map.keys[i];
        if (node == NULL || keep_is_pseudo(b, node))
            continue;
        FStyled *was = f_map_get(&old->map, node);
        const FStyled *is = b->map.vals[i];
        if (!keep_judge(k, was, is))
            return false;
        for (int p = 0; p < 2; p++) {
            if (is->pseudo[p] == NULL)
                continue;
            if (!keep_same_text(was->pseudo[p], is->pseudo[p])) {
                k->why = FLOW_RESTYLE_GEOMETRY;
                return false;
            }
            if (!keep_judge(k, f_map_get(&old->map, was->pseudo[p]), f_style_of(b, is->pseudo[p])))
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
    flow_restyle_t out = {FLOW_RESTYLE_DECLINED, FLOW_RESTYLE_NO_TREE, 0};
    if (tree == NULL || doc == NULL || env == NULL || inputs == NULL || tree->doc != doc || tree->incomplete ||
        tree->styles == NULL || tree->restyle_spent)
        return out;
    // What changed since the tree, and whether it was laid out as this
    // layout would be: the graft's own source test.
    FReuseSnapshot *r = tree->reuse;
    flow_reuse_stats_t ignored = {0};
    out.why = FLOW_RESTYLE_NO_HISTORY;
    if (r == NULL || !inputs->resource_owner || !reuse_read(r, &ignored))
        return out;
    out.why = FLOW_RESTYLE_STRUCTURE;
    if (r->structural)
        return out;
    out.why = FLOW_RESTYLE_INPUTS;
    if (r->width != width || r->doc != doc ||
        r->generation != garb_cascade_generation(env->cascade) || doc->quirks == OS64_HTML_QUIRKS ||
        !reuse_inputs_equal(&r->inputs, inputs) || !reuse_environment_equal(&r->env, env))
        return out;
    FStyles *b = f_style_build_reusing(doc, model, env, tree->styles, false);
    out.why = FLOW_RESTYLE_STYLES;
    if (b == NULL || b->lists_refused) {
        f_style_free(b);
        return out;
    }
    FKeeps k = {NULL, 0, 0, FLOW_RESTYLE_NONE};
    if (!keep_collect(&k, tree->styles, b)) {
        out.why = k.why;
        os64_free(k.pairs);
        f_style_free(b);
        return out;
    }
    // Every value first, into the restyle arena, before any is written: a
    // room that runs out leaves the tree as it was.
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
        room = f_map_put(&written, &k.pairs[i].was->style, k.pairs[i].was);
    if (!room) {
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
    tree->styles->cascade_updates = b->cascade_updates;
    out = (flow_restyle_t){k.n > 0 ? FLOW_RESTYLE_PAINT : FLOW_RESTYLE_NOTHING, FLOW_RESTYLE_NONE, (uint64_t)k.n};
    if (!whole) {
        tree->restyle_spent = true;
        out = (flow_restyle_t){FLOW_RESTYLE_DECLINED, FLOW_RESTYLE_ROOM, (uint64_t)k.n};
    }
    os64_free(values);
    os64_free(k.pairs);
    f_style_free(b);
    return out;
}

// Whether two trees of one document answer alike for what a restyle kept
// in place may write or must leave: the same boxes in the same order, each
// of the same kind, node and border box, drawing styles equal in every
// field (f_style_equal), with the same decoration lines and, where there
// are lines, the same colours (a box with none keeps colours it never
// draws, which a kept tree does not refresh). The first box that
// differs is named at `node` (its element's, for an anonymous box) with
// what differed at `what`.
bool flow_restyle_check(const flow_tree_t *a, const flow_tree_t *b, const os64_html_node_t **node, const char **what)
{
    *node = NULL;
    *what = NULL;
    const flow_box_t *x = a != NULL ? a->root : NULL, *y = b != NULL ? b->root : NULL;
    for (; x != NULL && y != NULL; x = box_after(x), y = box_after(y)) {
        const char *differs = x->kind != y->kind || x->node != y->node ? "the boxes"
                            : !same_rect(x->rect, y->rect) ? "a border box"
                            : (x->style == NULL) != (y->style == NULL) ? "a style"
                            : x->style != NULL && !f_style_equal(x->style, y->style) ? "a style"
                            : x->decoration != y->decoration ||
                              (x->decoration != 0 && (x->underline_color != y->underline_color ||
                                                      x->line_through_color != y->line_through_color))
                                  ? "a decoration"
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
    return true;
}
