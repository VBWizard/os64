// pending.inc — what is pending since a tree was made current (flow_pending;
// BRIEF_LAYOUT_NEEDS.md N2). Included by flow.c, whose tree it reads.
//
// A tree is CURRENT at a version: the document's version when it was laid
// out, or when a restyle was last kept in it (flow_restyle_in_place). Its
// pending reader is opened at that version, and the verdict says what the
// changes since then can have moved:
// - NONE: nothing has changed;
// - PAINT: only inline style writes, each of whose changed longhands is one
//   placement and box generation never read (kPending), on the same side of
//   `none` where the side makes a containing block. A border box, a padding
//   box, an offset parent and a scroll range are as the tree has them;
// - RESTYLE: anything else, or anything that cannot be said.
// The verdict only rises until the tree is made current again: a write that
// undoes an earlier one leaves RESTYLE standing, which costs a restyle and
// never a wrong answer.
//
// A READ ANSWERED FROM A TREE WITH PAINT PENDING IS A STYLE CHANGE EVENT FOR
// CSS TRANSITIONS. Other browsers recompute style at such a read, and that
// style is the "before-change style" a transition starts from (`opacity: 0`,
// a read, `opacity: 1` fades from 0). yonder runs no CSS transitions
// (PILE3.md § Transitions), so nothing can tell today. When transitions are
// built, a read answered from the tree must leave a checkpoint of the judged
// elements' inline styles, or restyle, so that the before-change style is
// the one at the read.

// What a longhand is to a read of boxes: not listed (it may move a box), or
// one of these.
enum {
    PENDING_PLAIN = 1,          // placement and boxes never read it
    PENDING_TRANSFORM,          // as PLAIN on its side of `none` (f_transformed)
    PENDING_FILTER,             // as PLAIN on its side of `none` (f_filtered)
    PENDING_VISIBILITY,         // as PLAIN between `visible` and `hidden`
};

// The longhands placement and box generation never read
// (layout_needs/FIELD_TABLE.md's rows read only by the paint, the public
// tree and hit testing). Wider than what a restyle may keep in place: a
// stacking change is nothing to a border box. A longhand not listed costs a
// restyle, never a wrong answer.
static const uint8_t kPending[GARB_NPROPS] = {
    [GARB_COLOR] = PENDING_PLAIN,
    [GARB_BACKGROUND_COLOR] = PENDING_PLAIN,
    [GARB_BACKGROUND_IMAGE] = PENDING_PLAIN,
    [GARB_BACKGROUND_REPEAT] = PENDING_PLAIN,
    [GARB_BACKGROUND_POSITION_X] = PENDING_PLAIN,
    [GARB_BACKGROUND_POSITION_Y] = PENDING_PLAIN,
    [GARB_BACKGROUND_SIZE] = PENDING_PLAIN,
    [GARB_BACKGROUND_ORIGIN] = PENDING_PLAIN,
    [GARB_BACKGROUND_CLIP] = PENDING_PLAIN,
    [GARB_BORDER_TOP_COLOR] = PENDING_PLAIN,
    [GARB_BORDER_RIGHT_COLOR] = PENDING_PLAIN,
    [GARB_BORDER_BOTTOM_COLOR] = PENDING_PLAIN,
    [GARB_BORDER_LEFT_COLOR] = PENDING_PLAIN,
    [GARB_BORDER_TOP_LEFT_RADIUS] = PENDING_PLAIN,
    [GARB_BORDER_TOP_RIGHT_RADIUS] = PENDING_PLAIN,
    [GARB_BORDER_BOTTOM_RIGHT_RADIUS] = PENDING_PLAIN,
    [GARB_BORDER_BOTTOM_LEFT_RADIUS] = PENDING_PLAIN,
    [GARB_BOX_SHADOW] = PENDING_PLAIN,
    [GARB_TEXT_SHADOW] = PENDING_PLAIN,
    [GARB_OUTLINE_WIDTH] = PENDING_PLAIN,
    [GARB_OUTLINE_STYLE] = PENDING_PLAIN,
    [GARB_OUTLINE_COLOR] = PENDING_PLAIN,
    [GARB_OUTLINE_OFFSET] = PENDING_PLAIN,
    [GARB_OPACITY] = PENDING_PLAIN,
    [GARB_Z_INDEX] = PENDING_PLAIN,
    [GARB_ISOLATION] = PENDING_PLAIN,
    [GARB_MIX_BLEND_MODE] = PENDING_PLAIN,
    [GARB_MASK_IMAGE] = PENDING_PLAIN,
    [GARB_MASK_REPEAT] = PENDING_PLAIN,
    [GARB_MASK_POSITION_X] = PENDING_PLAIN,
    [GARB_MASK_POSITION_Y] = PENDING_PLAIN,
    [GARB_MASK_SIZE] = PENDING_PLAIN,
    [GARB_MASK_ORIGIN] = PENDING_PLAIN,
    [GARB_MASK_CLIP] = PENDING_PLAIN,
    [GARB_MASK_COMPOSITE] = PENDING_PLAIN,
    [GARB_MASK_MODE] = PENDING_PLAIN,
    [GARB_CLIP_PATH] = PENDING_PLAIN,
    [GARB_OBJECT_FIT] = PENDING_PLAIN,
    [GARB_OBJECT_POSITION] = PENDING_PLAIN,
    [GARB_IMAGE_RENDERING] = PENDING_PLAIN,
    [GARB_CURSOR] = PENDING_PLAIN,
    [GARB_POINTER_EVENTS] = PENDING_PLAIN,
    [GARB_TRANSFORM_ORIGIN] = PENDING_PLAIN,
    [GARB_FILL] = PENDING_PLAIN,
    [GARB_TRANSFORM] = PENDING_TRANSFORM,
    [GARB_TRANSLATE] = PENDING_TRANSFORM,
    [GARB_ROTATE] = PENDING_TRANSFORM,
    [GARB_SCALE] = PENDING_TRANSFORM,
    [GARB_FILTER] = PENDING_FILTER,
    [GARB_BACKDROP_FILTER] = PENDING_FILTER,
    [GARB_VISIBILITY] = PENDING_VISIBILITY,
};

int flow_pending_longhand(int prop)
{
    return prop >= 0 && prop < GARB_NPROPS ? kPending[prop] : 0;
}

static void pending_open(flow_tree_t *tree)
{
    os64_html_reader_free(tree->pending_reader);
    tree->pending_version = os64_html_version(tree->doc);
    tree->pending_reader = os64_html_reader_new((os64_html_document_t *)tree->doc, tree->pending_version);
    tree->pending_verdict = tree->pending_reader != NULL ? FLOW_PENDING_NONE : FLOW_PENDING_RESTYLE;
    tree->pending_transforms = false;
}

// One element's inline style, as it is now against the cascade's parse of
// it, which is what the tree was styled from: PAINT, NONE (nothing listed),
// or RESTYLE.
static flow_pending_verdict_t pending_judge(flow_tree_t *tree, const garb_cascade_t *cascade,
                                            const os64_html_node_t *el)
{
    const FStyled *rec = f_map_get(&tree->styles->map, el);
    // An animation may set the same longhand, and its keyframes are the
    // restyle's to judge.
    if (rec == NULL || rec->motion != NULL)
        return FLOW_PENDING_RESTYLE;
    garb_inline_change_t changes[64];
    int32_t n = garb_cascade_inline_changes(cascade, el, changes, 64);
    if (n < 0)
        return FLOW_PENDING_RESTYLE;
    const flow_style_t *s = &rec->style;
    for (int32_t i = 0; i < n; i++) {
        const garb_inline_change_t *c = &changes[i];
        int kind = flow_pending_longhand((int)c->prop);
        // An absent declaration falls back to the sheets, and var() is
        // known only once substituted: neither can be judged here.
        if (kind == 0 || !c->declared || c->var)
            return FLOW_PENDING_RESTYLE;
        bool none = os64_streq(c->keyword, "none");
        switch (kind) {
        case PENDING_TRANSFORM:
            // Drawn is a containing block for absolute and fixed boxes, so
            // the side must hold: a value other than none is drawn unless
            // the box is a plain inline, which no transform reaches; none
            // is judged only where nothing was drawn, since another
            // longhand of the family might keep it drawn.
            tree->pending_transforms = true;
            if (none ? f_transformed(s) : f_transformed(s) != (s->display != FLOW_DISPLAY_INLINE))
                return FLOW_PENDING_RESTYLE;
            break;
        case PENDING_FILTER:
            // A plain inline's filter is granted to an atom and not to a
            // box of lines, which a record cannot tell apart.
            if (s->display == FLOW_DISPLAY_INLINE || (none ? f_filtered(s, false) : !f_filtered(s, false)))
                return FLOW_PENDING_RESTYLE;
            break;
        case PENDING_VISIBILITY:
            if (s->visibility == FLOW_COLLAPSE ||
                !(os64_streq(c->keyword, "visible") || os64_streq(c->keyword, "hidden")))
                return FLOW_PENDING_RESTYLE;
            break;
        default:
            break;
        }
    }
    return n > 0 ? FLOW_PENDING_PAINT : FLOW_PENDING_NONE;
}

flow_pending_t flow_pending(flow_tree_t *tree, const os64_html_document_t *doc, const struct garb_cascade *cascade)
{
    flow_pending_t out = {FLOW_PENDING_RESTYLE, true};
    // The cascade's parse must be the one the tree was styled from, and
    // complete; a selector that reads the `style` attribute makes any
    // inline write a change to what matches.
    if (tree == NULL || doc == NULL || tree->doc != doc || tree->incomplete || tree->styles == NULL ||
        tree->pending_reader == NULL || cascade == NULL ||
        garb_cascade_version(cascade) != tree->pending_version || !garb_cascade_tracks_changes(cascade) ||
        garb_cascade_reads_style_attribute(cascade))
        return out;
    if (tree->pending_verdict != FLOW_PENDING_RESTYLE) {
        const os64_html_change_entry_t *e = NULL;
        int64_t n = os64_html_changes(tree->pending_reader, &e);
        if (n < 0)
            tree->pending_verdict = FLOW_PENDING_RESTYLE;
        for (; n > 0 && e != NULL && tree->pending_verdict != FLOW_PENDING_RESTYLE; e = e->next) {
            flow_pending_verdict_t v = FLOW_PENDING_RESTYLE;
            if (e->kind == OS64_HTML_CHANGE_ATTR && e->attr_ns == NULL && e->attr_name != NULL &&
                os64_streq(e->attr_name, "style") && e->target != NULL && e->target->kind == OS64_HTML_ELEMENT)
                v = pending_judge(tree, cascade, e->target);
            if (v > tree->pending_verdict)
                tree->pending_verdict = (uint8_t)v;
        }
    }
    out.verdict = (flow_pending_verdict_t)tree->pending_verdict;
    out.transforms = tree->pending_transforms;
    return out;
}
