// flow.c — the door: a document, its model and a width in, a laid-out
// tree out (LAYOUT.md § The one door).
//
// Pass 3 works in 26.6 on its own records; what a face reads is built here
// once from them — every rectangle rounded ONCE by the painter's rule, to
// nearest with ties up, and a size as round(end) - round(start) so boxes
// that meet in the layout meet on the glass — with each box's overflow
// rect, a node's first box, and the lists of pictures, controls and
// positioned boxes.
//
// A POSITIONED box (POSITION.md) stays under its parent, so the pre-order
// is still the document's, but it is reached from the positioned list and
// never through its tree ancestors: their walks skip it, their overflow
// rects leave it out, and its clip is what its containing blocks allow.

#include "internal.h"
#include "garb/cascade.h"

// The public kinds of the block-level boxes are pass 2's, in pass 2's order.
_Static_assert(FLOW_BOX_BLOCK == (int)FB_BLOCK && FLOW_BOX_REPLACED == (int)FB_REPLACED &&
               FLOW_BOX_TABLE == (int)FB_TABLE && FLOW_BOX_CAPTION == (int)FB_CAPTION &&
               FLOW_BOX_COLUMN_GROUP == (int)FB_COLUMN_GROUP &&
               FLOW_BOX_COLUMN == (int)FB_COLUMN && FLOW_BOX_ROW_GROUP == (int)FB_ROW_GROUP &&
               FLOW_BOX_ROW == (int)FB_ROW && FLOW_BOX_CELL == (int)FB_CELL,
               "flow_box_kind_t and f_box_kind_t disagree");

struct flow_tree {
    FStyles *styles;
    FBoxes *boxes;
    FLayout *layout;
    FArena arena;           // the public boxes
    flow_box_t *root;
    FMap first;             // node -> its first public box
    const flow_box_t **images, **controls, **positioned;
    int32_t nimages, ncontrols, npositioned, cap_images, cap_controls, cap_positioned;
    bool incomplete;
};

typedef struct {
    flow_tree_t *t;
    bool failed;
} Build;

static int64_t round_px(int64_t v)
{
    int64_t q = (v + 32) / 64;
    return (v + 32) % 64 < 0 ? q - 1 : q;
}

// A face reads whole pixels in int32_t, and a page can reach past that
// (flow_height holds it to INT32_MAX): an edge is held to the range, and a
// size between two edges to INT32_MAX, so what lies past the last pixel
// sits on it rather than wrapping round to the top of the page.
static int32_t edge32(int64_t px)
{
    return px > INT32_MAX ? INT32_MAX : px < INT32_MIN ? INT32_MIN : (int32_t)px;
}

static os64_gui_rect_t rect_from(int64_t x0, int64_t y0, int64_t x1, int64_t y1)
{
    int64_t w = (int64_t)edge32(x1) - edge32(x0), h = (int64_t)edge32(y1) - edge32(y0);
    return (os64_gui_rect_t){edge32(x0), edge32(y0), (int32_t)(w > INT32_MAX ? INT32_MAX : w),
                             (int32_t)(h > INT32_MAX ? INT32_MAX : h)};
}

static os64_gui_rect_t rect_of(int64_t x, int64_t y, int64_t w, int64_t h)
{
    return rect_from(round_px(x), round_px(y), round_px(x + w), round_px(y + h));
}

static os64_gui_rect_t join(os64_gui_rect_t a, os64_gui_rect_t b)
{
    int64_t x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int64_t x1 = (int64_t)a.x + a.w > (int64_t)b.x + b.w ? (int64_t)a.x + a.w : (int64_t)b.x + b.w;
    int64_t y1 = (int64_t)a.y + a.h > (int64_t)b.y + b.h ? (int64_t)a.y + a.h : (int64_t)b.y + b.h;
    return rect_from(x0, y0, x1, y1);
}

static bool push_list(Build *bd, const flow_box_t ***v, int32_t *n, int32_t *cap,
                      const flow_box_t *box)
{
    if (*n == *cap) {
        int32_t cap2 = *cap != 0 ? *cap * 2 : 16;
        const flow_box_t **grown = os64_realloc(*v, (size_t)cap2 * sizeof(*grown));
        if (grown == NULL) {
            bd->failed = true;
            return false;
        }
        *v = grown;
        *cap = cap2;
    }
    (*v)[(*n)++] = box;
    return true;
}

static bool is_picture(const os64_html_node_t *n)
{
    if (n == NULL || n->kind != OS64_HTML_ELEMENT || n->ns != OS64_HTML_NS_HTML)
        return false;
    if (n->tag == OS64_HTML_TAG_IMG)
        return true;
    const os64_html_attr_t *type = os64_html_attr(n, "type");
    return n->tag == OS64_HTML_TAG_INPUT && type != NULL && f_eq_nocase(type->value, "image");
}

// A new public box, attached under `parent` after `*last`, and recorded as
// its node's first box — and as a picture or a control — when it is one.
static flow_box_t *add(Build *bd, flow_box_t *parent, flow_box_t **last, flow_box_kind_t kind,
                       const os64_html_node_t *node, const flow_style_t *style)
{
    if (bd->failed)
        return NULL;
    flow_box_t *b = f_arena_alloc(&bd->t->arena, sizeof(*b));
    if (b == NULL) {
        bd->failed = true;
        return NULL;
    }
    b->kind = kind;
    b->node = node;
    b->style = style;
    b->link = b->control = -1;
    b->parent = parent;
    // Transparent: its element's opacity, or an ancestor's, is 0; a box
    // with no element of its own is as its parent is.
    const os64_html_node_t *e = node;
    while (e != NULL && e->kind != OS64_HTML_ELEMENT)
        e = e->parent;
    const FStyled *st = e != NULL ? f_style_of(bd->t->styles, e) : NULL;
    b->unpainted = st != NULL ? st->transparent : parent != NULL && parent->unpainted;
    if (parent != NULL) {
        if (*last != NULL)
            (*last)->next = b;
        else
            parent->first = b;
        *last = b;
    }
    if (node != NULL && f_map_get(&bd->t->first, node) == NULL) {
        if (!f_map_put(&bd->t->first, node, b)) {
            bd->failed = true;
            return b;
        }
        if (is_picture(node))
            push_list(bd, &bd->t->images, &bd->t->nimages, &bd->t->cap_images, b);
    }
    return b;
}

// ── Clipping (CSS Overflow 3) ───────────────────────────────────────────

// What a box's ancestors let it draw: on each axis either everything, or
// the span where their clipping padding boxes meet.
typedef struct {
    os64_gui_rect_t r;
    bool x, y;          // clipped on that axis
} Clip;

static const Clip kNoClip = {{0, 0, 0, 0}, false, false};

static void set_clip(flow_box_t *b, Clip c)
{
    b->clipped = c.x || c.y;
    if (!b->clipped)
        return;
    // An axis nothing clips is as wide as a rectangle can say.
    b->clip = c.r;
    if (!c.x) {
        b->clip.x = INT32_MIN / 2;
        b->clip.w = INT32_MAX;
    }
    if (!c.y) {
        b->clip.y = INT32_MIN / 2;
        b->clip.h = INT32_MAX;
    }
}

static void meet_axis(int32_t *at, int32_t *len, int32_t other_at, int32_t other_len)
{
    int64_t a0 = *at > other_at ? *at : other_at;
    int64_t a1 = (int64_t)*at + *len < (int64_t)other_at + other_len ? (int64_t)*at + *len
                                                                    : (int64_t)other_at + other_len;
    *at = (int32_t)a0;
    *len = (int32_t)(a1 > a0 ? a1 - a0 : 0);
}

// The root's overflow, or the body's when the root's is visible, is the
// viewport's (CSS Overflow 3 § 3.3), and clips neither axis.
static bool viewport_overflow(const FBox *src)
{
    if (src->parent == NULL)
        return true;
    const flow_style_t *root = src->parent->style;
    return src->parent->parent == NULL && src->node != NULL &&
           src->node->kind == OS64_HTML_ELEMENT && src->node->tag == OS64_HTML_TAG_BODY &&
           root->overflow_x == FLOW_OVERFLOW_VISIBLE && root->overflow_y == FLOW_OVERFLOW_VISIBLE;
}

// Whether a box clips its own content on the x axis (or else the y).
static bool clips_own(const FBox *src, bool x_axis)
{
    return src->node != NULL && !viewport_overflow(src) &&
           f_overflow_clips(x_axis ? src->style->overflow_x : src->style->overflow_y);
}

static Clip inner_clip(const FBox *src, Clip c);
static Clip content_clip(const FBox *b);

// What a box may draw into, walked up rather than handed down: the clip
// its tree parent hands its content, or, out of flow, what its containing
// block hands its — an inline's being its home's, since an inline clips
// nothing. Only an out-of-flow box asks: every other box is handed its
// parent's by the build, which is the same answer.
static Clip outer_clip(const FBox *b)
{
    if (b->out_of_flow) {
        const FPos *cb = b->pos != NULL ? b->pos->cb : NULL;
        if (cb == NULL)
            return kNoClip;
        return content_clip(cb->kind == FP_BOX ? cb->box : cb->home);
    }
    return b->parent != NULL ? content_clip(b->parent) : kNoClip;
}

static Clip content_clip(const FBox *b)
{
    return inner_clip(b, outer_clip(b));
}

// The clip a box hands its content: its own, met with its padding box on
// the axes it clips.
static Clip inner_clip(const FBox *src, Clip c)
{
    bool x = clips_own(src, true), y = clips_own(src, false);
    if (!x && !y)
        return c;
    os64_gui_rect_t pad = rect_of(src->x + src->border[FLOW_LEFT], src->y + src->border[FLOW_TOP],
                                  src->w - src->border[FLOW_LEFT] - src->border[FLOW_RIGHT],
                                  src->h - src->border[FLOW_TOP] - src->border[FLOW_BOTTOM]);
    if (x) {
        if (c.x)
            meet_axis(&c.r.x, &c.r.w, pad.x, pad.w);
        else
            c.r.x = pad.x, c.r.w = pad.w;
        c.x = true;
    }
    if (y) {
        if (c.y)
            meet_axis(&c.r.y, &c.r.h, pad.y, pad.h);
        else
            c.r.y = pad.y, c.r.h = pad.h;
        c.y = true;
    }
    return c;
}

static flow_box_t *public_box(Build *bd, const FBox *src, flow_box_t *parent, flow_box_t **last,
                              Clip clip);

static void public_frag(Build *bd, const FFrag *fr, flow_box_t *parent, flow_box_t **last,
                        Clip clip)
{
    flow_box_kind_t kind = fr->kind == FF_TEXT ? FLOW_BOX_TEXT
                         : fr->kind == FF_ATOMIC ? FLOW_BOX_ATOMIC : FLOW_BOX_MARKER;
    flow_box_t *b = add(bd, parent, last, kind, fr->node, fr->style);
    if (b == NULL)
        return;
    set_clip(b, clip);
    b->rect = rect_of(fr->x, fr->y, fr->w, fr->h);
    b->overflow = b->rect;
    b->baseline = edge32(round_px(fr->baseline));
    b->link = fr->link;
    b->decoration = fr->decoration;
    b->underline_color = fr->decoration_colors.underline;
    b->line_through_color = fr->decoration_colors.line_through;
    if (fr->kind != FF_ATOMIC) {
        b->run = fr->run;
        b->text = fr->text + fr->begin;
        b->length = fr->end - fr->begin;
        b->begin = (fr->item != NULL ? fr->item->offset : 0) + fr->begin;
    } else {
        b->control = fr->item->control;
        if (b->control >= 0)
            push_list(bd, &bd->t->controls, &bd->t->ncontrols, &bd->t->cap_controls, b);
        if (fr->item->content != NULL && fr->item->content->placed) {
            flow_box_t *inner_last = NULL;
            flow_box_t *inner = public_box(bd, fr->item->content, b, &inner_last, clip);
            if (inner != NULL)
                b->overflow = join(b->overflow, inner->overflow);
        }
    }
}

// A laid-out box and everything under it: its lines (each line's spans,
// then its fragments), its child boxes, then an outside marker — the
// order a partial layout grows in, so a tree cut short is a prefix of the
// whole one.
static flow_box_t *public_box(Build *bd, const FBox *src, flow_box_t *parent, flow_box_t **last,
                              Clip clip)
{
    flow_box_t *b = add(bd, parent, last, (flow_box_kind_t)src->kind, src->node, src->style);
    if (b == NULL)
        return NULL;
    // In the list at its place in the pre-order, which is tree order: the
    // paint order until z-index sorts it.
    b->positioned = src->positioned;
    if (src->positioned &&
        !push_list(bd, &bd->t->positioned, &bd->t->npositioned, &bd->t->cap_positioned, b))
        return b;
    set_clip(b, clip);
    Clip in = inner_clip(src, clip);
    b->rect = rect_of(src->x, src->y, src->w, src->h);
    b->overflow = b->rect;
    b->link = src->link;
    b->unfinished = src->unfinished;
    flow_box_t *kids = NULL;
    for (const FLine *ln = src->lines; ln != NULL && !bd->failed; ln = ln->next) {
        flow_box_t *line = add(bd, b, &kids, FLOW_BOX_LINE, NULL, src->style);
        if (line == NULL)
            break;
        set_clip(line, in);
        line->rect = rect_of(ln->x, ln->y, ln->w, ln->h);
        line->overflow = line->rect;
        line->baseline = edge32(round_px(ln->baseline));
        line->unfinished = ln->unfinished;
        flow_box_t *pieces = NULL;
        for (const FSpan *sp = ln->spans; sp != NULL && !bd->failed; sp = sp->next) {
            flow_box_t *span = add(bd, line, &pieces, FLOW_BOX_SPAN, sp->inl->node, sp->inl->style);
            if (span == NULL)
                break;
            set_clip(span, in);
            span->rect = rect_of(sp->x0, sp->top, sp->x1 - sp->x0, sp->bottom - sp->top);
            span->overflow = span->rect;
            span->baseline = line->baseline;
            span->link = sp->inl->link;
            line->overflow = join(line->overflow, span->overflow);
        }
        for (const FFrag *fr = ln->frags; fr != NULL && !bd->failed; fr = fr->next) {
            // A placeholder is only where a static position was read.
            if (fr->kind == FF_PLACEHOLDER)
                continue;
            public_frag(bd, fr, line, &pieces, in);
            if (pieces != NULL)
                line->overflow = join(line->overflow, pieces->overflow);
        }
        b->overflow = join(b->overflow, line->overflow);
    }
    for (const FBox *c = src->first; c != NULL && !bd->failed; c = c->next) {
        if (!c->placed)
            continue;
        flow_box_t *child = public_box(bd, c, b, &kids, c->out_of_flow ? outer_clip(c) : in);
        if (child != NULL && !c->positioned)
            b->overflow = join(b->overflow, child->overflow);
    }
    // A marker comes after the box's content, so an unfinished box, whose
    // content stopped short, has not reached it.
    if (src->marker_frag != NULL && !src->unfinished && !bd->failed) {
        public_frag(bd, src->marker_frag, b, &kids, in);
        if (kids != NULL)
            b->overflow = join(b->overflow, kids->overflow);
    }
    // What it clips does not reach past its border box on that axis.
    if (clips_own(src, true)) {
        b->overflow.x = b->rect.x;
        b->overflow.w = b->rect.w;
    }
    if (clips_own(src, false)) {
        b->overflow.y = b->rect.y;
        b->overflow.h = b->rect.h;
    }
    return b;
}

void flow_free(flow_tree_t *tree)
{
    if (tree == NULL)
        return;
    f_layout_free(tree->layout);
    f_boxes_free(tree->boxes);
    f_style_free(tree->styles);
    f_map_free(&tree->first);
    f_arena_free(&tree->arena);
    os64_free(tree->images);
    os64_free(tree->controls);
    os64_free(tree->positioned);
    os64_free(tree);
}

flow_tree_t *flow_layout(const os64_html_document_t *doc, const os64_page_t *model,
                         int32_t width, const flow_env_t *env)
{
    if (doc == NULL || env == NULL || env->text == NULL || env->fonts == NULL || width < 0)
        return NULL;
    // One viewport height for the cascade's `vh` and the initial containing
    // block, or none.
    if (env->cascade != NULL && garb_cascade_env(env->cascade).height != env->viewport_height)
        return NULL;
    flow_tree_t *tree = os64_calloc(1, sizeof(*tree));
    if (tree == NULL)
        return NULL;
    // The public boxes mirror what pass 3 placed, one for each box and
    // fragment, so the budgeted arenas already bound them: no cap of their
    // own, which would turn a page the budget cut short into no page.
    tree->arena.cap = SIZE_MAX;
    tree->styles = f_style_build(doc, model, env);
    tree->boxes = tree->styles != NULL ? f_boxes_build(doc, model, tree->styles, env) : NULL;
    tree->layout = tree->boxes != NULL ? f_layout(tree->boxes, doc, model, env, width) : NULL;
    if (tree->layout == NULL) {
        flow_free(tree);
        return NULL;
    }
    tree->incomplete = tree->layout->incomplete;
    const FBox *root = tree->boxes->root;
    if (root != NULL && root->placed) {
        Build bd = {tree, false};
        flow_box_t *none = NULL;
        tree->root = public_box(&bd, root, NULL, &none, kNoClip);
        // Out of memory for the public boxes is out of memory for the
        // page: nothing half-built is handed over.
        if (bd.failed) {
            flow_free(tree);
            return NULL;
        }
    }
    return tree;
}

bool flow_incomplete(const flow_tree_t *tree)
{
    return tree != NULL && tree->incomplete;
}

// Whole pixels, rounded up, held to what an int32_t can say: a page may be
// taller than that in 26.6 (thousands of height=1000000 cells).
static int32_t whole_px(int64_t u)
{
    int64_t px = (u + 63) / 64;
    return px > INT32_MAX ? INT32_MAX : px < 0 ? 0 : (int32_t)px;
}

int32_t flow_height(const flow_tree_t *tree)
{
    return tree != NULL ? whole_px(tree->layout->height) : 0;
}

int32_t flow_width(const flow_tree_t *tree)
{
    return tree != NULL ? whole_px(tree->layout->width) : 0;
}

const flow_box_t *flow_root(const flow_tree_t *tree)
{
    return tree != NULL ? tree->root : NULL;
}

static bool meets(os64_gui_rect_t a, os64_gui_rect_t b)
{
    return (int64_t)a.x < (int64_t)b.x + b.w && (int64_t)b.x < (int64_t)a.x + a.w &&
           (int64_t)a.y < (int64_t)b.y + b.h && (int64_t)b.y < (int64_t)a.y + a.h;
}

static bool holds(os64_gui_rect_t r, int32_t x, int32_t y)
{
    return x >= r.x && (int64_t)x < (int64_t)r.x + r.w && y >= r.y && (int64_t)y < (int64_t)r.y + r.h;
}

static bool block_level(flow_box_kind_t k)
{
    return k <= FLOW_BOX_CELL;
}

// A child the tree's own walks go into: not one reached from the list.
static bool in_tree(const flow_box_t *c)
{
    return block_level(c->kind) && !c->positioned;
}

typedef struct {
    os64_gui_rect_t view;
    void (*visit)(void *ctx, const flow_box_t *box);
    void *ctx;
} Visit;

static void visit_inline(const Visit *v, const flow_box_t *b);

// Appendix E's step 4: the block-level boxes, in tree order. Nothing of an
// unpainted box is painted, and nothing inside it either.
static void visit_blocks(const Visit *v, const flow_box_t *b)
{
    if (b->unpainted || !meets(b->overflow, v->view))
        return;
    v->visit(v->ctx, b);
    for (const flow_box_t *c = b->first; c != NULL; c = c->next)
        if (in_tree(c))
            visit_blocks(v, c);
}

// Step 7: the inline content — each line's spans and fragments, an atom's
// own content painted where the atom is — and the markers.
static void visit_inline(const Visit *v, const flow_box_t *b)
{
    if (b->unpainted || !meets(b->overflow, v->view))
        return;
    for (const flow_box_t *c = b->first; c != NULL; c = c->next) {
        if (c->kind == FLOW_BOX_LINE) {
            if (!meets(c->overflow, v->view))
                continue;
            for (const flow_box_t *f = c->first; f != NULL; f = f->next) {
                if (f->unpainted || !meets(f->overflow, v->view))
                    continue;
                v->visit(v->ctx, f);
                for (const flow_box_t *inner = f->first; inner != NULL; inner = inner->next) {
                    visit_blocks(v, inner);
                    visit_inline(v, inner);
                }
            }
        } else if (c->kind == FLOW_BOX_MARKER) {
            if (!c->unpainted && meets(c->overflow, v->view))
                v->visit(v->ctx, c);
        } else if (in_tree(c)) {
            visit_inline(v, c);
        }
    }
}

// The ordinary tree, then step 8 — each positioned box, in list order, as a
// small two-phase walk of its own.
void flow_visit(const flow_tree_t *tree, os64_gui_rect_t viewport,
                void (*visit)(void *ctx, const flow_box_t *box), void *ctx)
{
    if (tree == NULL || tree->root == NULL || visit == NULL)
        return;
    Visit v = {viewport, visit, ctx};
    visit_blocks(&v, tree->root);
    visit_inline(&v, tree->root);
    for (int32_t i = 0; i < tree->npositioned; i++) {
        visit_blocks(&v, tree->positioned[i]);
        visit_inline(&v, tree->positioned[i]);
    }
}

// Pruned on the overflow rect alone, and each box's clip asked of its own
// rect: a clip is a box's own, so no box is pruned by its parent's — a
// positioned one may have a wider clip than its tree parent.
static const flow_box_t *hit(const flow_box_t *b, int32_t x, int32_t y)
{
    if (!holds(b->overflow, x, y))
        return NULL;
    const flow_box_t *found = NULL;
    for (const flow_box_t *c = b->first; c != NULL; c = c->next) {
        if (c->positioned)
            continue;
        const flow_box_t *h = hit(c, x, y);
        if (h != NULL)
            found = h;
    }
    if (found != NULL)
        return found;
    // What is clipped away is not there to be pointed at, and a box that
    // lets the pointer through is not either.
    return holds(b->rect, x, y) && (!b->clipped || holds(b->clip, x, y)) &&
                   !b->style->pointer_events_none
               ? b : NULL;
}

// What is painted last is on top: the positioned list backwards, then the
// ordinary tree.
const flow_box_t *flow_hit(const flow_tree_t *tree, int32_t x, int32_t y)
{
    if (tree == NULL || tree->root == NULL)
        return NULL;
    for (int32_t i = tree->npositioned; i > 0; i--) {
        const flow_box_t *h = hit(tree->positioned[i - 1], x, y);
        if (h != NULL)
            return h;
    }
    return hit(tree->root, x, y);
}

// Everything in the flow — a relative box is, an absolute one is not — the
// last painted winning. Not pruned on overflow rects, which leave every
// positioned box out: a relative one is in the flow all the same, and it
// may sit anywhere its tree parent's rect does not reach. A walk of the
// in-flow tree, once per layout, for a face that anchors on it.
static const flow_box_t *hit_in_flow(const flow_box_t *b, int32_t x, int32_t y)
{
    const flow_box_t *found = NULL;
    for (const flow_box_t *c = b->first; c != NULL; c = c->next) {
        if (c->positioned && f_out_of_flow(c->style))
            continue;
        const flow_box_t *h = hit_in_flow(c, x, y);
        if (h != NULL)
            found = h;
    }
    if (found != NULL)
        return found;
    // What the pointer passes through is still where it is: an anchor is a
    // place, not a target, so pointer-events is not asked here.
    return holds(b->rect, x, y) && (!b->clipped || holds(b->clip, x, y)) ? b : NULL;
}

const flow_box_t *flow_hit_in_flow(const flow_tree_t *tree, int32_t x, int32_t y)
{
    return tree != NULL && tree->root != NULL ? hit_in_flow(tree->root, x, y) : NULL;
}

int32_t flow_npositioned(const flow_tree_t *tree)
{
    return tree != NULL ? tree->npositioned : 0;
}

const flow_box_t *flow_positioned(const flow_tree_t *tree, int32_t i)
{
    return tree != NULL && i >= 0 && i < tree->npositioned ? tree->positioned[i] : NULL;
}

const flow_box_t *flow_box_for(const flow_tree_t *tree, const os64_html_node_t *node)
{
    return tree != NULL ? f_map_get(&tree->first, node) : NULL;
}

int32_t flow_nimages(const flow_tree_t *tree)
{
    return tree != NULL ? tree->nimages : 0;
}

const flow_box_t *flow_image(const flow_tree_t *tree, int32_t i)
{
    return tree != NULL && i >= 0 && i < tree->nimages ? tree->images[i] : NULL;
}

int32_t flow_ncontrols(const flow_tree_t *tree)
{
    return tree != NULL ? tree->ncontrols : 0;
}

const flow_box_t *flow_control(const flow_tree_t *tree, int32_t i)
{
    return tree != NULL && i >= 0 && i < tree->ncontrols ? tree->controls[i] : NULL;
}

int64_t flow_dump(const flow_tree_t *tree, char *out, size_t cap)
{
    if (tree == NULL) {
        if (cap > 0)
            out[0] = '\0';
        return 0;
    }
    return f_tree_dump(tree->root, flow_width(tree), flow_height(tree), tree->incomplete, out,
                       cap);
}
