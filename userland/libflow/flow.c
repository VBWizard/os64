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
// is still the document's, but it is STACKED — reached from the layers
// and never through its tree ancestors, whose walks skip it — and their
// overflow rects leave it out, and its clip is what its containing blocks
// allow. A block-level box in the flow below full opacity is stacked too
// (PILE3.md § The blend), and keeps its parent's overflow, clip and frame.
// A STICKY box is where the flow put it, and the door works out, at each
// scroll, how far to draw it moved (flow_box_doc_offset). So is a SCROLL
// CONTAINER's content, drawn moved back by the container's scroll position,
// which is the one thing a face may change here (flow_scroll_set). Both
// are FRAMES (flow_frame_t): every box carries the innermost one that
// moves it.

#include "internal.h"
#include "garb/cascade.h"

// The public kinds of the block-level boxes are pass 2's, in pass 2's order.
_Static_assert(FLOW_BOX_BLOCK == (int)FB_BLOCK && FLOW_BOX_REPLACED == (int)FB_REPLACED &&
               FLOW_BOX_TABLE == (int)FB_TABLE && FLOW_BOX_CAPTION == (int)FB_CAPTION &&
               FLOW_BOX_COLUMN_GROUP == (int)FB_COLUMN_GROUP &&
               FLOW_BOX_COLUMN == (int)FB_COLUMN && FLOW_BOX_ROW_GROUP == (int)FB_ROW_GROUP &&
               FLOW_BOX_ROW == (int)FB_ROW && FLOW_BOX_CELL == (int)FB_CELL,
               "flow_box_kind_t and f_box_kind_t disagree");

// One step of painting (CSS 2.1 Appendix E): a box's WHOLE two-phase walk,
// or, for a stacking context with members under it, its SELF — its own
// background and borders — and its BODY — everything else of its own —
// with the members of negative z-index between the two. A GROUP — a
// stacking context below full opacity — is bracketed by an OPEN and a
// CLOSE around everything it paints, which a face composites whole.
typedef enum { PART_WHOLE = 0, PART_SELF, PART_BODY, PART_OPEN, PART_CLOSE } LayerPart;

typedef struct {
    const flow_box_t *box;
    LayerPart part;
} Layer;

struct flow_tree {
    const os64_html_document_t *doc;
    os64_html_pin_t pin;
    const os64_page_t *model;
    FStyles *styles;
    FBoxes *boxes;
    FLayout *layout;
    FArena arena;           // the public boxes
    flow_box_t *root;
    FMap first;             // node -> its first public box
    const flow_box_t **images, **controls, **positioned, **stacked;
    int32_t nimages, ncontrols, npositioned, nstacked;
    int32_t cap_images, cap_controls, cap_positioned, cap_stacked;
    // The paint order: the root's layers and every stacked box's, as the
    // stacking contexts sort them (stack_build). `stacked` is left in the
    // same order, and `positioned`, the stacked boxes that are positioned,
    // too.
    Layer *layers;
    int32_t nlayers;
    // Each scroll container's frame, in tree order: what flow_scroll_set
    // changes, and where a box's `scroller` points.
    flow_frame_t **scrollers;
    int32_t nscrollers, cap_scrollers;
    bool incomplete;
};

typedef struct {
    flow_tree_t *t;
    bool failed;
    // The viewport a sticky box sticks to when no scroll container is
    // nearer: the width laid out at, and the initial containing block's
    // height.
    int32_t view_w, view_h;
    // Each containing block's entry (FPos) to the frame its boxes move
    // with, when there is one; and each scroll container's public box to
    // its own, which its padding box is read from.
    FMap frames, ports;
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
    b->link = b->control = b->scroller = -1;
    b->parent = parent;
    // Transparent: its element's opacity, or an ancestor's, is 0; a box
    // with no element of its own is as its parent is.
    const os64_html_node_t *e = node;
    while (e != NULL && e->kind != OS64_HTML_ELEMENT)
        e = e->parent;
    const FStyled *st = e != NULL ? f_style_of(bd->t->styles, e) : NULL;
    b->unpainted = st != NULL ? st->transparent : parent != NULL && parent->unpainted;
    b->fixed = parent != NULL && parent->fixed;
    // A scroll container's content is in its scroll; everything else is in
    // the frame its parent is in.
    b->frame = parent == NULL ? NULL
             : parent->scroller >= 0 ? bd->t->scrollers[parent->scroller] : parent->frame;
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

// A clip as a rectangle: an axis nothing clips is as wide as a rectangle
// can say.
static os64_gui_rect_t clip_rect(Clip c)
{
    os64_gui_rect_t r = c.r;
    if (!c.x) {
        r.x = INT32_MIN / 2;
        r.w = INT32_MAX;
    }
    if (!c.y) {
        r.y = INT32_MIN / 2;
        r.h = INT32_MAX;
    }
    return r;
}

// A box's clip: what `c` says, and, in a frame, clipped as well when
// something outside the frame clips (flow_frame_t.clip), which
// flow_box_doc_clip meets with this one.
static void set_clip(flow_box_t *b, Clip c)
{
    b->clipped = c.x || c.y || (b->frame != NULL && b->frame->clipped);
    if (b->clipped)
        b->clip = clip_rect(c);
}

static void meet_axis(int32_t *at, int32_t *len, int32_t other_at, int32_t other_len)
{
    int64_t a0 = *at > other_at ? *at : other_at;
    int64_t a1 = (int64_t)*at + *len < (int64_t)other_at + other_len ? (int64_t)*at + *len
                                                                    : (int64_t)other_at + other_len;
    *at = (int32_t)a0;
    *len = (int32_t)(a1 > a0 ? a1 - a0 : 0);
}

static Clip inner_clip(const FBox *src, Clip c);
static Clip content_clip(const FBox *b);

static bool is_sticky(const FBox *b)
{
    return b->positioned && !b->out_of_flow && b->style->position == FLOW_POSITION_STICKY;
}

// What a box may draw into, walked up rather than handed down: the clip
// its tree parent hands its content, or, out of flow, what its containing
// block hands its — an inline's being its home's, since an inline clips
// nothing. Only an out-of-flow box asks: every other box is handed its
// parent's by the build, which is the same answer. What clips a frame from
// outside does not move with it, so it is the frame's (flow_frame_t.clip):
// a sticky box's own is nothing, and what it hands its content is only
// what clips inside it; a scroll container hands its content nothing.
static Clip outer_clip(const FBox *b)
{
    if (is_sticky(b))
        return kNoClip;
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
    return f_box_scrolls(b) ? kNoClip : inner_clip(b, outer_clip(b));
}

// The clip a box hands its content: its own, met with its padding box on
// the axes it clips — for a scroll container, what its frame keeps.
static Clip inner_clip(const FBox *src, Clip c)
{
    bool x = f_box_clips(src, true), y = f_box_clips(src, false);
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

// ── Sticky boxes (CSS Position 3 § 3.4) ─────────────────────────────────

static bool map_put(Build *bd, FMap *map, const void *key, void *val)
{
    if (!f_map_put(map, key, val))
        bd->failed = true;
    return !bd->failed;
}

// A box's padding box, in its own coordinates.
static os64_gui_rect_t padding_box(const FBox *src)
{
    return rect_of(src->x + src->border[FLOW_LEFT], src->y + src->border[FLOW_TOP],
                   src->w - src->border[FLOW_LEFT] - src->border[FLOW_RIGHT],
                   src->h - src->border[FLOW_TOP] - src->border[FLOW_BOTTOM]);
}

// How far a box whose edges are [b0, b1] along an axis may move and stay
// inside [c0, c1] with its margins m0 and m1 outside it: [lo, hi], each
// held to 0 on the side the box already reaches past.
static void room(int64_t b0, int64_t b1, int64_t m0, int64_t m1, int64_t c0, int64_t c1,
                 int32_t *lo, int32_t *hi)
{
    int64_t l = round_px(c0 - (b0 - m0)), h = round_px(c1 - (b1 + m1));
    *lo = l > 0 ? 0 : edge32(l);
    *hi = h < 0 ? 0 : edge32(h);
}

// The frames that move and that `s`'s frame is worked from, added to
// `seen[0..n)` once each; F_FRAME_DEPS + 1 once there are more than that.
static int32_t reach(const flow_frame_t *s, const flow_frame_t **seen, int32_t n)
{
    for (s = s != NULL ? s->mover : NULL; s != NULL && n <= F_FRAME_DEPS;
         s = s->outer != NULL ? s->outer->mover : NULL) {
        for (int32_t i = 0; i < n; i++)
            if (seen[i] == s)
                return n;       // it is here, and so is everything it needs
        seen[n++] = s;
        if (s->port != NULL)
            n = reach(s->port->frame, seen, n);
    }
    return n;
}

// The frame a sticky box is drawn moved by. Its scrollport is the nearest
// scroll container above it, found along the public parents, which go on
// past an inline-block's edge where the box tree's stop. Its containing
// block is its parent's content box — a cell's, its table's — reaching as
// far down as the content does when the parent scrolls
// (sticky_reach_scrolled); a block-level box fills its containing block
// across, margins and all, so only a cell can move sideways.
static const flow_frame_t *sticky_new(Build *bd, const FBox *src, const flow_box_t *b, Clip clip)
{
    flow_frame_t *st = f_arena_alloc(&bd->t->arena, sizeof(*st));
    if (st == NULL) {
        bd->failed = true;
        return NULL;
    }
    st->kind = F_FRAME_STICKY;
    st->box = b;
    st->outer = b->frame;
    st->clips = clip.x || clip.y;
    st->clip = clip_rect(clip);
    st->clipped = st->clips || (st->outer != NULL && st->outer->clipped);
    st->view_w = bd->view_w;
    st->view_h = bd->view_h;
    for (const flow_box_t *a = b->parent; a != NULL && st->port == NULL; a = a->parent) {
        const FBox *port = f_map_get(&bd->ports, a);
        if (port != NULL) {
            st->port = a;
            st->port_rect = padding_box(port);
        }
    }
    const flow_frame_t *seen[F_FRAME_DEPS + 1];
    int32_t n = reach(st->outer, seen, 0);
    if (st->port != NULL)
        n = reach(st->port->frame, seen, n);
    st->moves = n < F_FRAME_DEPS;
    st->mover = st->moves ? st : st->outer != NULL ? st->outer->mover : NULL;
    if (!st->moves)
        return st;
    int64_t pw = st->port != NULL ? st->port_rect.w : st->view_w;
    int64_t ph = st->port != NULL ? st->port_rect.h : st->view_h;
    const flow_style_t *s = src->style;
    for (int k = 0; k < 4; k++) {
        st->has[k] = s->inset[k].kind != FLOW_LENGTH_AUTO;
        int64_t base = (k == FLOW_TOP || k == FLOW_BOTTOM ? ph : pw) * 64;
        st->inset[k] = st->has[k] ? edge32(round_px(f_len(s->inset[k], base))) : 0;
    }
    const FBox *cb = src->parent;
    while (src->kind == FB_CELL && cb != NULL && cb->kind != FB_TABLE)
        cb = cb->parent;
    if (cb == NULL)
        return st;      // the root: nothing to stay inside, so it stays
    int64_t cx0 = cb->x + cb->border[FLOW_LEFT] + cb->padding[FLOW_LEFT];
    int64_t cx1 = cb->x + cb->w - cb->border[FLOW_RIGHT] - cb->padding[FLOW_RIGHT];
    int64_t cy0 = cb->y + cb->border[FLOW_TOP] + cb->padding[FLOW_TOP];
    int64_t cy1 = cb->y + cb->h - cb->border[FLOW_BOTTOM] - cb->padding[FLOW_BOTTOM];
    int64_t mt = 0, mb = 0;
    if (src->kind == FB_CELL) {
        room(src->x, src->x + src->w, 0, 0, cx0, cx1, &st->lo[0], &st->hi[0]);
    } else {
        // A vertical margin's `auto` is 0; a percentage is of the width.
        mt = f_len(s->margin[FLOW_TOP], cx1 - cx0);
        mb = f_len(s->margin[FLOW_BOTTOM], cx1 - cx0);
    }
    room(src->y, src->y + src->h, mt, mb, cy0, cy1, &st->lo[1], &st->hi[1]);
    if (src->kind != FB_CELL && f_box_scrolls(cb)) {
        st->cb_scrolls = true;
        st->room_down = edge32(round_px(cy1 - (src->y + src->h + mb)));
    }
    return st;
}

// A sticky box whose containing block is a scroll container may move as
// far down as that container's content reaches, which is its foot plus
// how far it scrolls: known once every box is built.
static void sticky_reach_scrolled(flow_tree_t *t)
{
    for (int32_t i = 0; i < t->npositioned; i++) {
        const flow_box_t *p = t->positioned[i];
        flow_frame_t *st = (flow_frame_t *)p->frame;
        if (st == NULL || st->kind != F_FRAME_STICKY || st->box != p || !st->cb_scrolls ||
            st->outer == NULL || st->outer->kind != F_FRAME_SCROLL)
            continue;
        int64_t h = (int64_t)st->room_down + st->outer->range.y;
        st->hi[1] = h < 0 ? 0 : edge32(h);
    }
}

// What a box's own frame is: an out-of-flow box moves with its containing
// block, every other with its parent (add), and a sticky box with itself,
// `clip` being what clips it from outside; a scroll container is
// remembered for the sticky boxes inside it. False on no memory.
static bool frame_box(Build *bd, const FBox *src, flow_box_t *b, Clip clip)
{
    if (src->out_of_flow) {
        const FPos *cb = src->pos != NULL ? src->pos->cb : NULL;
        b->frame = cb != NULL ? f_map_get(&bd->frames, cb) : NULL;
    }
    if (is_sticky(src)) {
        b->frame = sticky_new(bd, src, b, clip);
        if (b->frame == NULL)
            return false;
    }
    if (f_box_scrolls(src))
        return map_put(bd, &bd->ports, b, (void *)src);
    return true;
}

// A scroll container's frame: its content is drawn moved back by `at`,
// starting at 0,0, and `in` — the clip it hands its content — is what
// clips that content from outside. Its `range` grows as its content is
// built (scroll_reach).
static flow_frame_t *scroll_new(Build *bd, const FBox *src, flow_box_t *b, Clip in)
{
    flow_frame_t *f = f_arena_alloc(&bd->t->arena, sizeof(*f));
    flow_tree_t *t = bd->t;
    if (f != NULL && t->nscrollers == t->cap_scrollers) {
        int32_t cap2 = t->cap_scrollers != 0 ? t->cap_scrollers * 2 : 16;
        flow_frame_t **grown = os64_realloc(t->scrollers, (size_t)cap2 * sizeof(*grown));
        if (grown == NULL)
            f = NULL;
        else
            t->scrollers = grown, t->cap_scrollers = cap2;
    }
    if (f == NULL) {
        bd->failed = true;
        return NULL;
    }
    f->kind = F_FRAME_SCROLL;
    f->box = b;
    f->moves = true;
    f->mover = f;
    f->outer = b->frame;
    f->clips = in.x || in.y;
    f->clip = clip_rect(in);
    f->clipped = f->clips || (f->outer != NULL && f->outer->clipped);
    f->pad = padding_box(src);
    f->index = t->nscrollers;
    t->scrollers[t->nscrollers++] = f;
    b->scroller = f->index;
    return f;
}

// A scroll container's range grown to reach `right` and `bottom`: its
// scrollable overflow (CSS Overflow 3 § 2.2) is everything of its content,
// and how far that reaches past its padding box is how far it can scroll.
// What is left of or above the padding box cannot be scrolled to.
static void scroll_reach(flow_frame_t *f, int64_t right, int64_t bottom)
{
    int64_t dx = right - ((int64_t)f->pad.x + f->pad.w);
    int64_t dy = bottom - ((int64_t)f->pad.y + f->pad.h);
    if (dx > f->range.x)
        f->range.x = edge32(dx);
    if (dy > f->range.y)
        f->range.y = edge32(dy);
}

// `r` met with `c` on the axes `c` holds; empty when they do not meet.
static os64_gui_rect_t cut_to(os64_gui_rect_t r, os64_gui_rect_t c)
{
    meet_axis(&r.x, &r.w, c.x, c.w);
    meet_axis(&r.y, &r.h, c.y, c.h);
    return r;
}

// The positioned boxes a scroll container's content holds, which its own
// walk leaves out — a relative box in its flow, a box its content is the
// containing block of, and either inside a sticky box in it — reach as far
// as what of them can be SEEN: their overflow rects, cut by every clip
// between them and the container — their own, and each sticky frame's on
// the way out — but not the container's, which is what scrolling reaches
// past. Sticky boxes are counted where the flow put them, as the content
// is.
static void scroll_reach_positioned(flow_tree_t *t)
{
    for (int32_t i = 0; i < t->npositioned; i++) {
        const flow_box_t *p = t->positioned[i];
        os64_gui_rect_t r = p->clipped ? cut_to(p->overflow, p->clip) : p->overflow;
        const flow_frame_t *f = p->frame;
        for (; f != NULL && f->kind == F_FRAME_STICKY; f = f->outer)
            if (f->clips)
                r = cut_to(r, f->clip);
        if (f != NULL && r.w > 0 && r.h > 0)
            scroll_reach(t->scrollers[f->index], (int64_t)r.x + r.w, (int64_t)r.y + r.h);
    }
}

// A piece of a scroll container's own content, `r`, reached, and the
// container's end padding past it; nothing when `f` is NULL.
static void content_reach(flow_frame_t *f, const FBox *src, os64_gui_rect_t r)
{
    if (f != NULL)
        scroll_reach(f, (int64_t)r.x + r.w + round_px(src->padding[FLOW_RIGHT]),
                     (int64_t)r.y + r.h + round_px(src->padding[FLOW_BOTTOM]));
}

// What a box lends the boxes whose containing block it is, and those of the
// inline containing blocks its lines hold: the frame its content is in.
static bool lend_frame(Build *bd, const FBox *src, const flow_box_t *b)
{
    const flow_frame_t *f = b->scroller >= 0 ? bd->t->scrollers[b->scroller] : b->frame;
    if (f == NULL)
        return true;
    if (src->pos != NULL && !map_put(bd, &bd->frames, src->pos, (void *)f))
        return false;
    for (const FPos *p = src->homed; p != NULL; p = p->home_next)
        if (!map_put(bd, &bd->frames, p, (void *)f))
            return false;
    return true;
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
    b->control = src->control;
    if (b->control >= 0 &&
        !push_list(bd, &bd->t->controls, &bd->t->ncontrols, &bd->t->cap_controls, b))
        return b;
    b->positioned = src->positioned;
    b->fixed |= src->out_of_flow && src->style->position == FLOW_POSITION_FIXED;
    if (src->positioned &&
        !push_list(bd, &bd->t->positioned, &bd->t->npositioned, &bd->t->cap_positioned, b))
        return b;
    // Below full opacity, a box its block-level parent's walks would reach
    // is a stacking context of its own, painted whole. The root is the
    // first context already, and an inline-block's content is painted where
    // its atom is, on the line.
    b->stacked = src->positioned ||
                 (src->style->opacity < 1000 && parent != NULL && parent->kind <= FLOW_BOX_CELL);
    if (b->stacked &&
        !push_list(bd, &bd->t->stacked, &bd->t->nstacked, &bd->t->cap_stacked, b))
        return b;
    b->rect = rect_of(src->x, src->y, src->w, src->h);
    b->overflow = b->rect;
    b->link = src->link;
    b->unfinished = src->unfinished;
    if (!frame_box(bd, src, b, clip))
        return b;
    if (is_sticky(src))
        clip = kNoClip;         // its frame's, where it stays as the box moves
    set_clip(b, clip);
    Clip in = inner_clip(src, clip);
    flow_frame_t *scroll = NULL;
    if (f_box_scrolls(src)) {
        scroll = scroll_new(bd, src, b, in);
        if (scroll == NULL)
            return b;
        in = kNoClip;           // its frame's, where it stays as the content moves
    }
    if (!lend_frame(bd, src, b))
        return b;
    // A scroll container's range is what its content reaches, piece by
    // piece: its own overflow rect also holds its border box, which would
    // hold the range at 0.
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
        content_reach(scroll, src, line->overflow);
    }
    for (const FBox *c = src->first; c != NULL && !bd->failed; c = c->next) {
        if (!c->placed)
            continue;
        flow_box_t *child = public_box(bd, c, b, &kids, c->out_of_flow ? outer_clip(c) : in);
        if (child != NULL && !c->positioned) {
            b->overflow = join(b->overflow, child->overflow);
            content_reach(scroll, src, child->overflow);
        }
    }
    // A marker comes after the box's content, so an unfinished box, whose
    // content stopped short, has not reached it.
    if (src->marker_frag != NULL && !src->unfinished && !bd->failed) {
        public_frag(bd, src->marker_frag, b, &kids, in);
        if (kids != NULL) {
            b->overflow = join(b->overflow, kids->overflow);
            content_reach(scroll, src, kids->overflow);
        }
    }
    // What it clips does not reach past its border box on that axis.
    if (f_box_clips(src, true)) {
        b->overflow.x = b->rect.x;
        b->overflow.w = b->rect.w;
    }
    if (f_box_clips(src, false)) {
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
    os64_free(tree->stacked);
    os64_free(tree->layers);
    os64_free(tree->scrollers);
    os64_page_free((os64_page_t *)tree->model);
    if (tree->pin != 0)
        os64_html_unpin(tree->doc, tree->pin);
    os64_free(tree);
}

const os64_page_t *flow_model(const flow_tree_t *tree)
{
    return tree != NULL ? tree->model : NULL;
}

// ── Stacking (CSS 2.1 Appendix E, § 9.9.1) ───────────────────────────────
//
// A STACKING CONTEXT is the root, a positioned box with a z-index, a fixed
// or sticky one, or any box painted at less than full opacity (CSS
// Position 3, Color 4) — which is why such a box is stacked even in the
// flow, where it paints as a positioned box of z-index 0 would. Each
// stacked box belongs to the nearest one that is its ancestor; within a
// context its members paint after the context's own background — those of
// negative z-index first, then the context's own content, then the rest by
// z-index, auto and 0 in tree order, the positive lowest first — and a
// member that is a context paints whole, its own members with it. A
// positioned box that is not a context lends its descendants to the
// context it is in. A context below full opacity is a GROUP: everything it
// paints is bracketed by an OPEN and a CLOSE.

static bool makes_context(const flow_box_t *b)
{
    const flow_style_t *s = b->style;
    return s->has_z_index || s->position == FLOW_POSITION_FIXED ||
           s->position == FLOW_POSITION_STICKY || s->opacity < 1000;
}

// z-index applies to a positioned box; a box stacked in the flow is 0.
static int32_t z_of(const flow_box_t *b)
{
    return b->positioned && b->style->has_z_index ? b->style->z_index : 0;
}

// Composited whole: below full opacity, and painted at all.
static bool is_group(const flow_box_t *b)
{
    return b->style->opacity < 1000 && !b->unpainted;
}

typedef struct {
    flow_tree_t *t;
    const flow_box_t **entry;   // the stacked boxes, in tree order
    bool *context;              // each entry's makes_context
    int32_t *order;             // every context's members, grouped, each group sorted
    int32_t *first, *count;     // a context's group in `order`: [first, first + count); -1 is the root
    Layer *layers;
    const flow_box_t **painted; // the entries again, in paint order
    int32_t nlayers, npainted;
} Stack;

// A stable sort of a group by z-index: a merge through `tmp`, so members
// of one z-index keep their tree order.
static void sort_by_z(const Stack *st, int32_t *v, int32_t *tmp, int32_t n)
{
    if (n < 2)
        return;
    int32_t h = n / 2;
    sort_by_z(st, v, tmp, h);
    sort_by_z(st, v + h, tmp, n - h);
    int32_t i = 0, j = h, k = 0;
    while (i < h && j < n)
        tmp[k++] = z_of(st->entry[v[j]]) < z_of(st->entry[v[i]]) ? v[j++] : v[i++];
    while (i < h)
        tmp[k++] = v[i++];
    while (j < n)
        tmp[k++] = v[j++];
    os64_memcpy(v, tmp, (size_t)n * sizeof(*v));
}

static void layer(Stack *st, const flow_box_t *box, LayerPart part)
{
    st->layers[st->nlayers++] = (Layer){box, part};
    if ((part == PART_WHOLE || part == PART_BODY) && box->stacked)
        st->painted[st->npainted++] = box;
}

// Context `c` (-1 for the root) and everything painted within it.
static void emit_context(Stack *st, int32_t c)
{
    const flow_box_t *box = c < 0 ? st->t->root : st->entry[c];
    const int32_t *m = st->order + st->first[c + 1];
    int32_t n = st->count[c + 1], neg = 0;
    if (is_group(box))
        layer(st, box, PART_OPEN);
    while (neg < n && z_of(st->entry[m[neg]]) < 0)
        neg++;
    if (neg == 0) {
        layer(st, box, PART_WHOLE);
    } else {
        layer(st, box, PART_SELF);
        for (int32_t k = 0; k < neg; k++)
            emit_context(st, m[k]);     // a negative z-index is always a context
        layer(st, box, PART_BODY);
    }
    for (int32_t k = neg; k < n; k++) {
        if (st->context[m[k]])
            emit_context(st, m[k]);
        else
            layer(st, st->entry[m[k]], PART_WHOLE);
    }
    if (is_group(box))
        layer(st, box, PART_CLOSE);
}

// The paint order, once per tree. False on no memory.
static bool stack_build(flow_tree_t *t)
{
    int32_t n = t->nstacked;
    Stack st = {.t = t, .entry = t->stacked};
    FMap index = {0};
    int32_t *ctx = os64_calloc((size_t)n + 1, sizeof(int32_t));
    int32_t *tmp = os64_calloc((size_t)n + 1, sizeof(int32_t));
    st.context = os64_calloc((size_t)n + 1, sizeof(bool));
    st.order = os64_calloc((size_t)n + 1, sizeof(int32_t));
    st.first = os64_calloc((size_t)n + 2, sizeof(int32_t));
    st.count = os64_calloc((size_t)n + 2, sizeof(int32_t));
    // Each context two layers at most, and an open and a close.
    st.layers = os64_calloc((size_t)n * 4 + 4, sizeof(Layer));
    st.painted = os64_calloc((size_t)n + 1, sizeof(*st.painted));
    bool ok = ctx != NULL && tmp != NULL && st.context != NULL && st.order != NULL &&
              st.first != NULL && st.count != NULL && st.layers != NULL && st.painted != NULL;
    for (int32_t i = 0; ok && i < n; i++) {
        st.context[i] = makes_context(st.entry[i]);
        ok = f_map_put(&index, st.entry[i], (void *)(intptr_t)(i + 1));
    }
    // Each entry's context: the nearest ancestor entry that is one. An
    // ancestor comes before in tree order, so it is indexed already.
    for (int32_t i = 0; ok && i < n; i++) {
        ctx[i] = -1;
        for (const flow_box_t *p = st.entry[i]->parent; p != NULL; p = p->parent) {
            intptr_t j = p->stacked ? (intptr_t)f_map_get(&index, p) - 1 : -1;
            if (j >= 0 && st.context[j]) {
                ctx[i] = (int32_t)j;
                break;
            }
        }
        st.count[ctx[i] + 1]++;
    }
    for (int32_t c = 1; ok && c <= n; c++)
        st.first[c] = st.first[c - 1] + st.count[c - 1];
    if (ok) {
        int32_t *fill = tmp;            // how many of each group are placed
        os64_memset(fill, 0, ((size_t)n + 1) * sizeof(int32_t));
        for (int32_t i = 0; i < n; i++)
            st.order[st.first[ctx[i] + 1] + fill[ctx[i] + 1]++] = i;
        for (int32_t c = 0; c <= n; c++)
            sort_by_z(&st, st.order + st.first[c], tmp, st.count[c]);
        emit_context(&st, -1);
        os64_memcpy(t->stacked, st.painted, (size_t)n * sizeof(*st.painted));
        int32_t k = 0;
        for (int32_t i = 0; i < n; i++)
            if (st.painted[i]->positioned)
                t->positioned[k++] = st.painted[i];
        t->layers = st.layers;
        t->nlayers = st.nlayers;
        st.layers = NULL;
    }
    f_map_free(&index);
    os64_free(ctx);
    os64_free(tmp);
    os64_free(st.context);
    os64_free(st.order);
    os64_free(st.first);
    os64_free(st.count);
    os64_free(st.layers);
    os64_free(st.painted);
    return ok;
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
    tree->doc = doc;
    tree->pin = os64_html_pin(doc);
    if (tree->pin == 0 || (model != NULL && !os64_page_retain(model))) {
        flow_free(tree);
        return NULL;
    }
    tree->model = model;
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
        Build bd = {.t = tree, .view_w = width,
                    .view_h = (int32_t)((f_css_units(env, env->viewport_height) + 32) / 64)};
        if (bd.view_h <= 0)
            bd.view_h = flow_height(tree);
        flow_box_t *none = NULL;
        tree->root = public_box(&bd, root, NULL, &none, kNoClip);
        f_map_free(&bd.frames);
        f_map_free(&bd.ports);
        // Out of memory for the public boxes is out of memory for the
        // page: nothing half-built is handed over.
        if (bd.failed || !stack_build(tree)) {
            flow_free(tree);
            return NULL;
        }
        scroll_reach_positioned(tree);
        sticky_reach_scrolled(tree);
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
    return block_level(c->kind) && !c->stacked;
}

typedef struct {
    const flow_tree_t *t;
    os64_gui_rect_t view;   // in the coordinates of the boxes being walked
    void (*visit)(void *ctx, const flow_box_t *box);
    void *ctx;
} Visit;

// A rect moved by an offset, its edges held to what an int32_t says.
static os64_gui_rect_t moved(os64_gui_rect_t r, int64_t dx, int64_t dy)
{
    return rect_from((int64_t)r.x + dx, (int64_t)r.y + dy, (int64_t)r.x + r.w + dx,
                     (int64_t)r.y + r.h + dy);
}

// The frames already worked out in one lookup, each once: a sticky box's
// push is worked from its outer one's frame and its scrollport's, which are
// often the same, so without this a nest of them costs twice as much per
// level. The build bounds how many a sticky box's lookup meets
// (F_FRAME_DEPS); past that a lookup is remembered no further, which costs
// a chain of scroll containers nothing, since each is worked from one.
typedef struct {
    const flow_frame_t *s[F_FRAME_DEPS];
    flow_point_t at[F_FRAME_DEPS];
    int32_t n;
} Frames;

static flow_point_t frame_of(Frames *m, const flow_frame_t *s, bool fixed, flow_point_t scroll);

// How far a sticky box is pushed along one axis (CSS Position 3 § 3.4).
// The view is the scrollport's [v0, v1] less the insets that are set, and
// no smaller than the box: where it is, its end edge gives way (possibly
// past the scrollport), so a box taller than its view shows its start.
// The end edge is pushed back inside the view, then the start edge, which
// wins where both cannot hold — top over bottom, left over right — and the
// push is held to the room its containing block leaves.
static int64_t push(int64_t b0, int64_t b1, int64_t v0, int64_t v1, const flow_frame_t *s,
                    int start, int end, int axis)
{
    int64_t lo = v0 + (s->has[start] ? s->inset[start] : 0);
    int64_t hi = v1 - (s->has[end] ? s->inset[end] : 0);
    if (hi - lo < b1 - b0)
        hi = lo + (b1 - b0);
    int64_t d = 0;
    if (s->has[end] && b1 > hi)
        d = hi - b1;
    if (s->has[start] && b0 + d < lo)
        d = lo - b0;
    return d < s->lo[axis] ? s->lo[axis] : d > s->hi[axis] ? s->hi[axis] : d;
}

// Where a sticky box's scrollport is, in the coordinates of the frame it is
// in — the frame being moved by `at` — and how far that pushes the box.
static flow_point_t shift(Frames *m, const flow_frame_t *s, flow_point_t at, flow_point_t scroll)
{
    int64_t vx = scroll.x, vy = scroll.y, vw = s->view_w, vh = s->view_h;
    if (s->port != NULL) {
        flow_point_t o = frame_of(m, s->port->frame, s->port->fixed, scroll);
        vx = (int64_t)s->port_rect.x + o.x;
        vy = (int64_t)s->port_rect.y + o.y;
        vw = s->port_rect.w;
        vh = s->port_rect.h;
    }
    vx -= at.x;
    vy -= at.y;
    os64_gui_rect_t r = s->box->rect;
    int64_t dx = push(r.x, (int64_t)r.x + r.w, vx, vx + vw, s, FLOW_LEFT, FLOW_RIGHT, 0);
    int64_t dy = push(r.y, (int64_t)r.y + r.h, vy, vy + vh, s, FLOW_TOP, FLOW_BOTTOM, 1);
    return (flow_point_t){edge32(dx), edge32(dy)};
}

// A frame's offset: the scroll under a fixed box, then each frame's move,
// outermost first, each worked in the frame the one outside it moved — a
// sticky box's push, a scroll container's scroll position taken back. A
// sticky box that does not move is where the frame it is in puts it.
static flow_point_t frame_of(Frames *m, const flow_frame_t *s, bool fixed, flow_point_t scroll)
{
    s = s != NULL ? s->mover : NULL;
    if (s == NULL)
        return fixed ? scroll : (flow_point_t){0, 0};
    for (int32_t i = 0; i < m->n; i++)
        if (m->s[i] == s)
            return m->at[i];
    flow_point_t at = frame_of(m, s->outer, fixed, scroll);
    flow_point_t d = s->kind == F_FRAME_SCROLL ? (flow_point_t){-s->at.x, -s->at.y}
                                               : shift(m, s, at, scroll);
    flow_point_t here = {edge32((int64_t)at.x + d.x), edge32((int64_t)at.y + d.y)};
    if (m->n < F_FRAME_DEPS) {
        m->s[m->n] = s;
        m->at[m->n++] = here;
    }
    return here;
}

flow_point_t flow_box_doc_offset(const flow_box_t *box, flow_point_t scroll)
{
    Frames m = {.n = 0};
    return box != NULL ? frame_of(&m, box->frame, box->fixed, scroll) : (flow_point_t){0, 0};
}

int32_t flow_background_layers(const flow_style_t *s)
{
    return s->backgrounds.nimage;
}

flow_layer_t flow_background_layer(const flow_style_t *s, int32_t i)
{
    const flow_backgrounds_t *b = &s->backgrounds;
    const flow_bg_image_t *im = &b->image[i % b->nimage];
    const flow_bg_size_t *sz = &b->size[i % b->nsize];
    return (flow_layer_t){im->url,
                          im->len,
                          im->sheet,
                          im->gradient,
                          b->repeat[i % b->nrepeat],
                          {b->x[i % b->nx], b->y[i % b->ny]},
                          sz->fit,
                          {sz->size[0], sz->size[1]},
                          b->origin[i % b->norigin],
                          b->clip[i % b->nclip]};
}

bool flow_background_has_image(const flow_style_t *s)
{
    for (int32_t i = 0; i < s->backgrounds.nimage; i++)
        if (s->backgrounds.image[i].url != NULL || s->backgrounds.image[i].gradient != NULL)
            return true;
    return false;
}

void flow_box_radii(const flow_box_t *box, int32_t radii[4][2])
{
    // Each radius as written, never capped on its own: a cap would change
    // the corner's shape before the common reduction below, which must
    // scale the radii a page wrote (§ 5.5's overlapping-curves rule).
    int64_t r[4][2], size[2] = {box != NULL ? box->rect.w : 0, box != NULL ? box->rect.h : 0};
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 2; k++) {
            int64_t v = box != NULL ? round_px(f_len(box->style->radius[c][k], size[k] * 64)) : 0;
            r[c][k] = v < 0 ? 0 : v;
        }
    // Along each side, the two radii that meet it: top (TL, TR across),
    // right (TR, BR down), bottom (BR, BL across), left (BL, TL down). The
    // smallest side over its sum, when below 1, scales every radius.
    static const int kSide[4][3] = {{0, 1, 0}, {1, 2, 1}, {2, 3, 0}, {3, 0, 1}};
    int64_t num = 1, den = 1;
    for (int s = 0; s < 4; s++) {
        int axis = kSide[s][2];
        int64_t sum = r[kSide[s][0]][axis] + r[kSide[s][1]][axis];
        // f_len holds a length to LEN_MAX, so a radius is at most ~2^31
        // pixels, a side's sum 2^32 and a side 2^31: each product is under
        // 2^63, and unsigned 64 bits hold it — no 128-bit arithmetic, whose
        // division is a libgcc helper the freestanding link has not got.
        if (sum > 0 && (uint64_t)size[axis] * (uint64_t)den < (uint64_t)sum * (uint64_t)num) {
            num = size[axis];
            den = sum;
        }
    }
    // Scaled, a radius is no longer than its side, so it fits an int32_t
    // as the box does — and one written longer than its side, with no other
    // along it, is scaled to the side (a 100px corner of a 50px box is 50).
    // r * num is under 2^62.
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 2; k++)
            radii[c][k] = (int32_t)(r[c][k] * num / den);
}

os64_gui_rect_t flow_box_doc_rect(const flow_box_t *box, flow_point_t scroll)
{
    flow_point_t o = flow_box_doc_offset(box, scroll);
    return box != NULL ? moved(box->rect, o.x, o.y) : (os64_gui_rect_t){0, 0, 0, 0};
}

static os64_gui_rect_t meet(os64_gui_rect_t a, os64_gui_rect_t b)
{
    meet_axis(&a.x, &a.w, b.x, b.w);
    meet_axis(&a.y, &a.h, b.y, b.h);
    return a;
}

// Its own clip where its frame puts it, met with what clips each frame it
// is inside from outside that frame, where the frame outside puts it.
os64_gui_rect_t flow_box_doc_clip(const flow_box_t *box, flow_point_t scroll)
{
    if (box == NULL)
        return (os64_gui_rect_t){0, 0, 0, 0};
    Frames m = {.n = 0};
    flow_point_t o = frame_of(&m, box->frame, box->fixed, scroll);
    os64_gui_rect_t r = moved(box->clip, o.x, o.y);
    for (const flow_frame_t *s = box->frame; s != NULL; s = s->outer)
        if (s->clips) {
            flow_point_t at = frame_of(&m, s->outer, s->box->fixed, scroll);
            r = meet(r, moved(s->clip, at.x, at.y));
        }
    return r;
}

// Whether what clips the frames `b` is inside, from outside each, holds
// the document point (x, y) at this scroll: a box in one is drawn, and
// hit, only there.
static bool outside_clips_hold(const flow_box_t *b, int64_t x, int64_t y, flow_point_t scroll)
{
    Frames m = {.n = 0};
    for (const flow_frame_t *s = b->frame; s != NULL && s->clipped; s = s->outer)
        if (s->clips) {
            flow_point_t at = frame_of(&m, s->outer, s->box->fixed, scroll);
            if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX ||
                !holds(moved(s->clip, at.x, at.y), (int32_t)x, (int32_t)y))
                return false;
        }
    return true;
}

static void visit_inline(const Visit *v, const flow_box_t *b);

// The walk of a box's content: a scroll container's is in its scroll, so
// the view is where the content stands, `*in` filled; any other box's is
// the box's own, `v`.
static const Visit *content_view(const Visit *v, const flow_box_t *b, Visit *in)
{
    if (b->scroller < 0)
        return v;
    flow_point_t at = v->t->scrollers[b->scroller]->at;
    *in = *v;
    in->view = moved(v->view, at.x, at.y);
    return in;
}

// Appendix E's step 4: the block-level boxes, in tree order. Nothing of an
// unpainted box is painted, and nothing inside it either.
static void visit_blocks(const Visit *v, const flow_box_t *b)
{
    if (b->unpainted || !meets(b->overflow, v->view))
        return;
    v->visit(v->ctx, b);
    Visit scrolled;
    const Visit *cv = content_view(v, b, &scrolled);
    for (const flow_box_t *c = b->first; c != NULL; c = c->next)
        if (in_tree(c))
            visit_blocks(cv, c);
}

// Step 7: the inline content — each line's spans and fragments, an atom's
// own content painted where the atom is — and the markers.
static void visit_inline(const Visit *outer, const flow_box_t *b)
{
    if (b->unpainted || !meets(b->overflow, outer->view))
        return;
    Visit scrolled;
    const Visit *v = content_view(outer, b, &scrolled);
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

// What a group paints, from its OPEN at layer `i`: every box of every layer
// up to its CLOSE, each overflow rect where its frame puts it at `scroll`.
static os64_gui_rect_t group_bounds(const flow_tree_t *tree, int32_t i, flow_point_t scroll)
{
    os64_gui_rect_t r = {0, 0, 0, 0};
    bool any = false;
    for (int32_t k = i + 1, depth = 1; k < tree->nlayers; k++) {
        const Layer *l = &tree->layers[k];
        depth += l->part == PART_OPEN ? 1 : l->part == PART_CLOSE ? -1 : 0;
        if (depth == 0)
            break;
        if (l->part == PART_OPEN || l->part == PART_CLOSE)
            continue;
        flow_point_t o = flow_box_doc_offset(l->box, scroll);
        os64_gui_rect_t at = moved(l->box->overflow, o.x, o.y);
        r = any ? join(r, at) : at;
        any = true;
    }
    return r;
}

// The layers in paint order (stack_build), each against the viewport in
// its box's own coordinates: a fixed box's are the viewport's. A WHOLE box
// is its two-phase walk; a SELF, the box alone; a BODY, the rest of that
// walk — its block-level descendants, then its inline content; an OPEN and
// a CLOSE, the face's to hear.
void flow_visit(const flow_tree_t *tree, os64_gui_rect_t viewport, flow_point_t scroll,
                void (*visit)(void *ctx, const flow_box_t *box), void *ctx)
{
    flow_visitor_t v = {visit, NULL, NULL, ctx};
    flow_visit_groups(tree, viewport, scroll, &v);
}

void flow_visit_groups(const flow_tree_t *tree, os64_gui_rect_t viewport, flow_point_t scroll,
                       const flow_visitor_t *visitor)
{
    if (tree == NULL || tree->root == NULL || visitor == NULL || visitor->visit == NULL)
        return;
    void (*visit)(void *, const flow_box_t *) = visitor->visit;
    void *ctx = visitor->ctx;
    for (int32_t i = 0; i < tree->nlayers; i++) {
        const flow_box_t *b = tree->layers[i].box;
        if (tree->layers[i].part == PART_OPEN) {
            if (visitor->open != NULL)
                visitor->open(ctx, b, group_bounds(tree, i, scroll));
            continue;
        }
        if (tree->layers[i].part == PART_CLOSE) {
            if (visitor->close != NULL)
                visitor->close(ctx, b);
            continue;
        }
        flow_point_t o = flow_box_doc_offset(b, scroll);
        Visit v = {tree, moved(viewport, -(int64_t)o.x, -(int64_t)o.y), visit, ctx};
        switch (tree->layers[i].part) {
        case PART_WHOLE:
            visit_blocks(&v, b);
            visit_inline(&v, b);
            break;
        case PART_SELF:
            if (!b->unpainted && meets(b->overflow, v.view))
                visit(ctx, b);
            break;
        case PART_BODY: {
            if (b->unpainted || !meets(b->overflow, v.view))
                break;
            Visit scrolled;
            const Visit *cv = content_view(&v, b, &scrolled);
            for (const flow_box_t *c = b->first; c != NULL; c = c->next)
                if (in_tree(c))
                    visit_blocks(cv, c);
            visit_inline(&v, b);
            break;
        }
        case PART_OPEN: case PART_CLOSE: break;     // heard above
        }
    }
}

// What is clipped away is not there to be pointed at, and neither is a box
// that lets the pointer through or is not drawn at all — asked of each
// box, since both are inherited and a descendant may undo them.
static const flow_box_t *hit_self(const flow_box_t *b, int32_t x, int32_t y)
{
    return holds(b->rect, x, y) && (!b->clipped || holds(b->clip, x, y)) &&
                   !b->style->pointer_events_none && b->style->visibility == FLOW_VISIBLE
               ? b : NULL;
}

static const flow_box_t *hit(const flow_tree_t *t, const flow_box_t *b, int32_t x, int32_t y);

// The deepest of a box's descendants the tree reaches (its stacked ones
// are layers of their own) holding the point, the last painted winning.
// Pruned on the overflow rect alone, and each box's clip asked of its own
// rect: a clip is a box's own, so no box is pruned by its parent's — a
// positioned one may have a wider clip than its tree parent. A scroll
// container's content is a frame inside the one being walked: what clips
// it from outside is asked here, where the container stands, and the point
// is then taken to where the content stands.
static const flow_box_t *hit_kids(const flow_tree_t *t, const flow_box_t *b, int32_t x, int32_t y)
{
    if (!holds(b->overflow, x, y))
        return NULL;
    if (b->scroller >= 0) {
        const flow_frame_t *f = t->scrollers[b->scroller];
        int64_t cx = (int64_t)x + f->at.x, cy = (int64_t)y + f->at.y;
        if ((f->clips && !holds(f->clip, x, y)) || cx > INT32_MAX || cy > INT32_MAX)
            return NULL;
        x = (int32_t)cx;
        y = (int32_t)cy;
    }
    const flow_box_t *found = NULL;
    for (const flow_box_t *c = b->first; c != NULL; c = c->next) {
        if (c->stacked)
            continue;
        const flow_box_t *h = hit(t, c, x, y);
        if (h != NULL)
            found = h;
    }
    return found;
}

static const flow_box_t *hit(const flow_tree_t *t, const flow_box_t *b, int32_t x, int32_t y)
{
    const flow_box_t *h = hit_kids(t, b, x, y);
    return h != NULL ? h : hit_self(b, x, y);
}

// What is painted last is on top: the layers backwards, each asked in its
// own coordinates. What clips the frames a layer's box is in from outside
// is asked once, of the layer; the scroll containers inside it, as the
// walk enters each (hit_kids).
const flow_box_t *flow_hit(const flow_tree_t *tree, int32_t x, int32_t y, flow_point_t scroll)
{
    if (tree == NULL || tree->root == NULL)
        return NULL;
    for (int32_t i = tree->nlayers; i > 0; i--) {
        const flow_box_t *b = tree->layers[i - 1].box;
        if (!outside_clips_hold(b, x, y, scroll))
            continue;
        flow_point_t o = flow_box_doc_offset(b, scroll);
        int64_t px = (int64_t)x - o.x, py = (int64_t)y - o.y;
        if (px < INT32_MIN || px > INT32_MAX || py < INT32_MIN || py > INT32_MAX)
            continue;
        const flow_box_t *h = NULL;
        switch (tree->layers[i - 1].part) {
        case PART_WHOLE: h = hit(tree, b, (int32_t)px, (int32_t)py); break;
        case PART_SELF: h = hit_self(b, (int32_t)px, (int32_t)py); break;
        case PART_BODY: h = hit_kids(tree, b, (int32_t)px, (int32_t)py); break;
        case PART_OPEN: case PART_CLOSE: break;     // a group is no box of its own
        }
        if (h != NULL)
            return h;
    }
    return NULL;
}

bool flow_box_covered(const flow_tree_t *tree, const flow_box_t *box, flow_point_t scroll)
{
    if (tree == NULL || box == NULL)
        return false;
    os64_gui_rect_t r = flow_box_doc_rect(box, scroll);
    const flow_box_t *h = flow_hit(tree, (int32_t)((int64_t)r.x + r.w / 2),
                                   (int32_t)((int64_t)r.y + r.h / 2), scroll);
    while (h != NULL && h != box)
        h = h->parent;
    return h == NULL;
}

// Everything in the flow — a relative box is, an out-of-flow one is not — the
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
    // place, not a target, so pointer-events is not asked here. Places are
    // where the flow put them, sticky boxes unpushed and scroll containers
    // unscrolled, so what clips a frame from outside is asked where it
    // stands.
    if (!holds(b->rect, x, y) || (b->clipped && !holds(b->clip, x, y)))
        return NULL;
    for (const flow_frame_t *s = b->frame; s != NULL && s->clipped; s = s->outer)
        if (s->clips && !holds(s->clip, x, y))
            return NULL;
    return b;
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

int32_t flow_nscrollers(const flow_tree_t *tree)
{
    return tree != NULL ? tree->nscrollers : 0;
}

static const flow_frame_t *scroller_at(const flow_tree_t *tree, int32_t i)
{
    return tree != NULL && i >= 0 && i < tree->nscrollers ? tree->scrollers[i] : NULL;
}

const flow_box_t *flow_scroller(const flow_tree_t *tree, int32_t i)
{
    const flow_frame_t *f = scroller_at(tree, i);
    return f != NULL ? f->box : NULL;
}

flow_point_t flow_scroll_range(const flow_tree_t *tree, int32_t i)
{
    const flow_frame_t *f = scroller_at(tree, i);
    return f != NULL ? f->range : (flow_point_t){0, 0};
}

flow_point_t flow_scroll_at(const flow_tree_t *tree, int32_t i)
{
    const flow_frame_t *f = scroller_at(tree, i);
    return f != NULL ? f->at : (flow_point_t){0, 0};
}

static int32_t held(int32_t v, int32_t most)
{
    return v < 0 ? 0 : v > most ? most : v;
}

flow_point_t flow_scroll_set(flow_tree_t *tree, int32_t i, flow_point_t at)
{
    if (scroller_at(tree, i) == NULL)
        return (flow_point_t){0, 0};
    flow_frame_t *f = tree->scrollers[i];
    f->at = (flow_point_t){held(at.x, f->range.x), held(at.y, f->range.y)};
    return f->at;
}

int32_t flow_box_scroller(const flow_box_t *box)
{
    for (const flow_frame_t *f = box != NULL ? box->frame : NULL; f != NULL; f = f->outer)
        if (f->kind == F_FRAME_SCROLL)
            return f->index;
    return -1;
}

int32_t flow_scroller_for(const flow_tree_t *tree, const os64_html_node_t *node)
{
    for (int32_t i = 0; node != NULL && i < flow_nscrollers(tree); i++)
        if (tree->scrollers[i]->box->node == node)
            return i;
    return -1;
}

// Where along one axis a container's view [at, at + view) must start to
// show [b0, b1), both in its content's coordinates from its padding edge:
// the start edge at the view's start (`start`), or the least move that
// shows it (nearest), the start edge winning when it does not fit.
static int64_t reveal_axis(int64_t at, int64_t view, int64_t b0, int64_t b1, bool start)
{
    if (start || b0 < at || b1 - b0 > view)
        return b0;
    return b1 > at + view ? b1 - view : at;
}

// Each container is asked to show the TARGET, never the container inside
// it (CSSOM View's walk): the target's rect taken into the container's
// content coordinates as it stands now, after the containers inside have
// moved — its own frame offset less the one the container's content is
// drawn moved by, at the page unscrolled — then measured from the padding
// edge.
void flow_scroll_reveal(flow_tree_t *tree, const flow_box_t *box)
{
    if (box == NULL)
        return;
    flow_point_t still = {0, 0};
    for (int32_t i = flow_box_scroller(box); scroller_at(tree, i) != NULL;
         i = flow_box_scroller(tree->scrollers[i]->box)) {
        flow_frame_t *f = tree->scrollers[i];
        Frames m = {.n = 0}, mc = {.n = 0};
        flow_point_t ot = frame_of(&m, box->frame, box->fixed, still);
        flow_point_t oc = frame_of(&mc, f, box->fixed, still);
        int64_t x0 = (int64_t)box->rect.x + ot.x - oc.x - f->pad.x;
        int64_t y0 = (int64_t)box->rect.y + ot.y - oc.y - f->pad.y;
        int64_t x = reveal_axis(f->at.x, f->pad.w, x0, x0 + box->rect.w, false);
        int64_t y = reveal_axis(f->at.y, f->pad.h, y0, y0 + box->rect.h, true);
        flow_scroll_set(tree, i, (flow_point_t){edge32(x), edge32(y)});
    }
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
    return f_tree_dump(tree->root, (const flow_frame_t *const *)tree->scrollers, flow_width(tree),
                       flow_height(tree), tree->incomplete, out,
                       cap);
}
