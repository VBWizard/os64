// pending.inc — what is pending since a tree was made current (flow_pending;
// BRIEF_LAYOUT_NEEDS.md N2). Included by flow.c, whose tree it reads.
//
// A tree is CURRENT at a version: the document's version when it was laid
// out, when a restyle was last kept in it (flow_restyle_in_place), or when a
// NONE verdict last advanced it (flow_pending_advance). Its pending reader
// reads from that version, and the verdict says what the changes since then
// can have moved:
// - NONE: nothing the tree was styled from has changed (no change at all,
//   or inline styles written back to what the cascade parsed);
// - PAINT: only inline style writes, each of whose changed longhands is one
//   placement and box generation never read (kPending), on the same side of
//   `none` where the side makes a containing block. A border box, a padding
//   box, an offset parent and a scroll position are as the tree has them,
//   and so is a scroll range unless a longhand that moves a transform's
//   reach is pending (flow_pending_t.transforms);
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
    PENDING_ORIGIN,             // as PLAIN, but it moves a transform's matrix and so its reach
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
    [GARB_TRANSFORM_ORIGIN] = PENDING_ORIGIN,
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
    os64_strcopy(tree->pending_why, sizeof(tree->pending_why), tree->pending_reader != NULL ? "" : "restyle: no reader");
    tree->pending_at[0] = '\0';
}

// An element named for the record: its tag, its id and its first class.
static void pending_named(char *out, size_t cap, const os64_html_node_t *n)
{
    out[0] = '\0';
    if (n == NULL || n->kind != OS64_HTML_ELEMENT)
        return;
    const os64_html_attr_t *id = os64_html_attr(n, "id"), *cls = os64_html_attr(n, "class");
    char first[32] = "";
    for (size_t i = 0; cls != NULL && cls->value != NULL && cls->value[i] != '\0' && cls->value[i] != ' ' &&
                       i + 1 < sizeof(first); i++) {
        first[i] = cls->value[i];
        first[i + 1] = '\0';
    }
    os64_snprintf(out, cap, "%s%s%s%s%s", n->name != NULL ? n->name : "?", id != NULL && id->value ? "#" : "",
                  id != NULL && id->value ? id->value : "", first[0] ? "." : "", first);
}

// One element's inline style, as it is now against the cascade's parse of
// it, which is what the tree was styled from: PAINT, NONE (nothing listed),
// or RESTYLE.
// Whether a box positioned out of flow lies inside `el`'s box (only fixed
// ones when `fixed_only`), or nothing can be said: the boxes a transform or
// a filter on `el` may be the containing block of. libflow keeps a
// transform or a filter it cannot work out as it was (style.c,
// author_transform), so a value other than `none` may still compute to
// none, and that changes a read of boxes only through such a containing
// block: an offset parent follows `position` alone (geometry.c).
static bool pending_holds_out_of_flow(const flow_tree_t *tree, const os64_html_node_t *el, bool fixed_only)
{
    const flow_box_t *top = flow_box_for(tree, el);
    if (top == NULL)
        return true;
    for (const flow_box_t *b = top->first; b != NULL;) {
        if (b->style != NULL && (b->style->position == FLOW_POSITION_FIXED ||
                                 (!fixed_only && b->style->position == FLOW_POSITION_ABSOLUTE)))
            return true;
        if (b->first != NULL) {
            b = b->first;
            continue;
        }
        while (b != top && b->next == NULL)
            b = b->parent;
        b = b == top ? NULL : b->next;
    }
    return false;
}

// A RESTYLE judged, with what made it one at `why` (a longhand's name, or
// the condition) for the record.
static flow_pending_verdict_t pending_restyle(char *why, size_t cap, const char *what)
{
    os64_strcopy(why, cap, what);
    return FLOW_PENDING_RESTYLE;
}

static flow_pending_verdict_t pending_judge(flow_tree_t *tree, const garb_cascade_t *cascade,
                                            const os64_html_node_t *el, char *why, size_t cap)
{
    why[0] = '\0';
    const FStyled *rec = f_map_get(&tree->styles->map, el);
    // An animation may set the same longhand, and its keyframes are the
    // restyle's to judge.
    if (rec == NULL || rec->motion != NULL)
        return pending_restyle(why, cap, rec == NULL ? "no record" : "animated");
    garb_inline_change_t changes[64];
    int32_t n = garb_cascade_inline_changes(cascade, el, changes, 64);
    if (n < 0)
        return pending_restyle(why, cap, "inline changes unknown");
    const flow_style_t *s = &rec->style;
    for (int32_t i = 0; i < n; i++) {
        const garb_inline_change_t *c = &changes[i];
        int kind = flow_pending_longhand((int)c->prop);
        // An absent declaration falls back to the sheets, and var() is
        // known only once substituted: neither can be judged here.
        if (kind == 0 || !c->declared || c->var)
            return pending_restyle(why, cap, kind == 0 ? garb_prop_name(c->prop) : !c->declared ? "removed" : "var()");
        // A CSS-wide keyword's value comes from elsewhere (the initial
        // value, the parent's, the sheets'), which the record does not say,
        // so it is judged by the restyle whatever the longhand's class.
        if (os64_streq(c->keyword, "initial") || os64_streq(c->keyword, "inherit") ||
            os64_streq(c->keyword, "unset") || os64_streq(c->keyword, "revert") ||
            os64_streq(c->keyword, "revert-layer"))
            return pending_restyle(why, cap, "a CSS-wide keyword");
        bool none = os64_streq(c->keyword, "none");
        switch (kind) {
        case PENDING_TRANSFORM:
            // Drawn is a containing block for absolute and fixed boxes, so
            // the side must hold. A value other than none makes a box
            // transformed (f_transformed) unless its display is a plain
            // inline, which f_transformed never grants, so no containing
            // block can come or go there (an inline atom is still drawn
            // through a frame, whose reach `transforms` carries). None is
            // judged only where nothing was transformed, since another
            // longhand of the family might keep it so.
            tree->pending_transforms = true;
            if (none ? f_transformed(s) : f_transformed(s) != (s->display != FLOW_DISPLAY_INLINE))
                return pending_restyle(why, cap, "transform side");
            // A positioned box is a containing block for absolute ones
            // whatever its transform says; fixed ones need the transform.
            if (!none && pending_holds_out_of_flow(tree, el, s->position != FLOW_POSITION_STATIC))
                return pending_restyle(why, cap, "out of flow inside");
            break;
        case PENDING_FILTER:
            // A plain inline's filter is granted to an atom and not to a
            // box of lines, which a record cannot tell apart.
            if (s->display == FLOW_DISPLAY_INLINE || (none ? f_filtered(s, false) : !f_filtered(s, false)))
                return pending_restyle(why, cap, "filter side");
            if (!none && pending_holds_out_of_flow(tree, el, s->position != FLOW_POSITION_STATIC))
                return pending_restyle(why, cap, "out of flow inside");
            break;
        case PENDING_ORIGIN:
            tree->pending_transforms = true;
            break;
        case PENDING_VISIBILITY:
            if (s->visibility == FLOW_COLLAPSE ||
                !(os64_streq(c->keyword, "visible") || os64_streq(c->keyword, "hidden")))
                return pending_restyle(why, cap, "visibility");
            break;
        default:
            break;
        }
    }
    return n > 0 ? FLOW_PENDING_PAINT : FLOW_PENDING_NONE;
}

flow_pending_t flow_pending(flow_tree_t *tree, const os64_html_document_t *doc, const struct garb_cascade *cascade)
{
    flow_pending_t out = {FLOW_PENDING_RESTYLE, true, false, false, "", "", ""};
    // The cascade's parse must be the one the tree was styled from, and
    // complete. A selector that tests the `style` attribute makes an inline
    // write a change to what matches: on any element when the readers
    // narrow to none (garb_cascade_style_readers_bare), else on an element
    // one may match, judged per write below (N2a).
    const char *unasked = tree == NULL || doc == NULL || tree->doc != doc ? "restyle: no tree"
                          : tree->incomplete || tree->styles == NULL       ? "restyle: an incomplete tree"
                          : tree->pending_reader == NULL                   ? "restyle: no reader"
                          : cascade == NULL || garb_cascade_version(cascade) != tree->pending_version
                              ? "restyle: the cascade moved"
                          : !garb_cascade_tracks_changes(cascade)          ? "restyle: no changes tracked"
                          : garb_cascade_style_readers_bare(cascade)      ? "restyle: a selector reads the style attribute"
                                                                           : NULL;
    if (unasked != NULL) {
        os64_strcopy(out.why, sizeof(out.why), unasked);
        return out;
    }
    if (tree->pending_verdict != FLOW_PENDING_RESTYLE) {
        const os64_html_change_entry_t *e = NULL;
        int64_t n = os64_html_changes(tree->pending_reader, &e);
        // LOST means everything is owed: RESTYLE until the tree is made
        // current again (pending_open), never nothing pending.
        if (n < 0) {
            tree->pending_verdict = FLOW_PENDING_RESTYLE;
            os64_strcopy(tree->pending_why, sizeof(tree->pending_why), "restyle: the log was lost");
            tree->pending_at[0] = '\0';
            out.lost = true;
        }
        for (; n > 0 && e != NULL && tree->pending_verdict != FLOW_PENDING_RESTYLE; e = e->next) {
            flow_pending_verdict_t v = FLOW_PENDING_RESTYLE;
            bool style = e->kind == OS64_HTML_CHANGE_ATTR && e->attr_ns == NULL && e->attr_name != NULL &&
                         os64_streq(e->attr_name, "style") && e->target != NULL &&
                         e->target->kind == OS64_HTML_ELEMENT;
            // A selector may test this element's style (N2a): the write may
            // change what matches, whatever it declares.
            bool read = style && garb_cascade_style_may_read(cascade, e->target);
            char judged[32] = "";
            if (style && !read)
                v = pending_judge(tree, cascade, e->target, judged, sizeof(judged));
            if (read && !out.style_read) {
                out.style_read = true;
                pending_named(out.style_read_at, sizeof(out.style_read_at), e->target);
            }
            if (v > tree->pending_verdict) {
                tree->pending_verdict = (uint8_t)v;
                if (read)
                    os64_strcopy(tree->pending_why, sizeof(tree->pending_why),
                                 "restyle: a selector may read its style");
                else if (style && v == FLOW_PENDING_PAINT)
                    os64_strcopy(tree->pending_why, sizeof(tree->pending_why), "paint: a style write");
                else if (style)
                    os64_snprintf(tree->pending_why, sizeof(tree->pending_why), "restyle: a style write: %s", judged);
                else if (e->kind == OS64_HTML_CHANGE_ATTR)
                    os64_snprintf(tree->pending_why, sizeof(tree->pending_why), "restyle: attribute %s",
                                  e->attr_name != NULL ? e->attr_name : "?");
                else
                    os64_strcopy(tree->pending_why, sizeof(tree->pending_why),
                                 e->kind == OS64_HTML_CHANGE_CHILDREN ? "structural: children"
                                 : e->kind == OS64_HTML_CHANGE_TEXT   ? "structural: text"
                                                                      : "structural: state");
                const os64_html_node_t *at = e->target;
                while (at != NULL && at->kind != OS64_HTML_ELEMENT)
                    at = at->parent;
                pending_named(tree->pending_at, sizeof(tree->pending_at), at);
            }
        }
    }
    out.verdict = (flow_pending_verdict_t)tree->pending_verdict;
    out.transforms = tree->pending_transforms;
    os64_strcopy(out.why, sizeof(out.why), tree->pending_why);
    os64_strcopy(out.at, sizeof(out.at), tree->pending_at);
    return out;
}

bool flow_pending_advance(flow_tree_t *tree, const os64_html_document_t *doc, garb_cascade_t *cascade,
                          flow_pending_t *judged)
{
    flow_pending_t pending = flow_pending(tree, doc, cascade);
    if (judged != NULL)
        *judged = pending;
    if (pending.verdict != FLOW_PENDING_NONE)
        return false;
    uint64_t version = os64_html_version(doc);
    if (version == tree->pending_version)
        return true;
    if (!garb_cascade_advance(cascade))
        return false;
    tree->pending_version = version;
    return true;
}
