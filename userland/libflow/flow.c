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
// (PILE3.md § The blend), and keeps its parent's overflow, clip and frame;
// so is a transformed one, block-level or an atom on a line, in a frame of
// its own.
// A STICKY box is where the flow put it, and the door works out, at each
// scroll, how far to draw it moved (flow_box_doc_offset). So is a SCROLL
// CONTAINER's content, drawn moved back by the container's scroll position,
// which is the one thing a face may change here (flow_scroll_set). So is
// a TRANSFORMED box, with everything in it, drawn through its matrix
// (PILE3.md § Transforms). All three are FRAMES (flow_frame_t): every box
// carries the innermost one that moves it.

#include <math.h>

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
    // How far past its box any shadow on the page may be drawn, in pixels
    // (ink_reach): what a group's bounds are widened by, since a shadow is
    // in no box's overflow.
    int64_t ink;
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

// What clips a frame from outside does not move with it, so it is the
// frame's (flow_frame_t.clip): what a sticky or transformed box hands its
// content is only what clips inside it, and a scroll container hands its
// content nothing.
static Clip content_clip(const FBox *b)
{
    if (f_box_scrolls(b))
        return kNoClip;
    return inner_clip(b, is_sticky(b) || b->transformed ? kNoClip : outer_clip(b));
}

// An absolute/fixed rectangle clips its own painting and descendants,
// intersecting the clip inherited through its containing-block chain.
static Clip legacy_clip(const FBox *src, Clip c)
{
    if (!src->out_of_flow || !src->style->legacy_clip_set) return c;
    const flow_length_t *sides = src->style->legacy_clip;
    int64_t top = sides[FLOW_TOP].kind == FLOW_LENGTH_AUTO ? 0 : sides[FLOW_TOP].value;
    int64_t left = sides[FLOW_LEFT].kind == FLOW_LENGTH_AUTO ? 0 : sides[FLOW_LEFT].value;
    int64_t right = sides[FLOW_RIGHT].kind == FLOW_LENGTH_AUTO ? src->w : sides[FLOW_RIGHT].value;
    int64_t bottom = sides[FLOW_BOTTOM].kind == FLOW_LENGTH_AUTO ? src->h : sides[FLOW_BOTTOM].value;
    os64_gui_rect_t rect = rect_of(src->x+left,src->y+top,right > left ? right-left : 0,bottom > top ? bottom-top : 0);
    if (c.x) meet_axis(&c.r.x,&c.r.w,rect.x,rect.w);
    else { c.r.x = rect.x;c.r.w = rect.w; }
    if (c.y) meet_axis(&c.r.y,&c.r.h,rect.y,rect.h);
    else { c.r.y = rect.y;c.r.h = rect.h; }
    c.x = c.y = true;
    return c;
}

static Clip inner_clip(const FBox *src, Clip c)
{
    c = legacy_clip(src,c);
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
// how far it scrolls: known once every box is built. A box also
// transformed has its sticky frame outside its transform one.
static void sticky_reach_scrolled(flow_tree_t *t)
{
    for (int32_t i = 0; i < t->npositioned; i++) {
        const flow_box_t *p = t->positioned[i];
        flow_frame_t *st = (flow_frame_t *)p->frame;
        if (st != NULL && st->kind == F_FRAME_TRANSFORM && st->box == p)
            st = (flow_frame_t *)st->outer;
        if (st == NULL || st->kind != F_FRAME_STICKY || st->box != p || !st->cb_scrolls ||
            st->outer == NULL || st->outer->kind != F_FRAME_SCROLL)
            continue;
        int64_t h = (int64_t)st->room_down + st->outer->range.y;
        st->hi[1] = h < 0 ? 0 : edge32(h);
    }
}

// ── Transforms (CSS Transforms 1 § 3, 2 § 6) ─────────────────────────────

// a · b, column-major.
static void mat_mul(double out[16], const double a[16], const double b[16])
{
    double r[16];
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++) {
            double s = 0;
            for (int i = 0; i < 4; i++)
                s += a[i * 4 + k] * b[c * 4 + i];
            r[c * 4 + k] = s;
        }
    os64_memcpy(out, r, sizeof(r));
}

// m · T(x, y, z): a translation applied before m.
static void mat_translate(double m[16], double x, double y, double z)
{
    for (int k = 0; k < 4; k++)
        m[12 + k] += m[k] * x + m[4 + k] * y + m[8 + k] * z;
}

// m · T(the function's x, y) · its matrix, a percentage of the border box
// `w` by `h`.
static void mat_fn(double m[16], const flow_transform_t *f, int64_t w, int64_t h)
{
    mat_translate(m, (double)f_len(f->x, w), (double)f_len(f->y, h), 0);
    mat_mul(m, m, f->m);
}

void f_transform_matrix(const flow_style_t *s, int64_t x, int64_t y, int64_t w, int64_t h,
                        double m[16])
{
    os64_memset(m, 0, 16 * sizeof(double));
    m[0] = m[5] = m[10] = m[15] = 1;
    double ox = (double)(x + f_len(s->transform_origin[0], w));
    double oy = (double)(y + f_len(s->transform_origin[1], h));
    double oz = s->transform_origin_z;
    mat_translate(m, ox, oy, oz);
    if (s->translate != NULL)
        mat_fn(m, s->translate, w, h);
    if (s->rotate != NULL)
        mat_fn(m, s->rotate, w, h);
    if (s->scale != NULL)
        mat_fn(m, s->scale, w, h);
    for (int32_t i = 0; i < s->ntransform; i++)
        mat_fn(m, &s->transform[i], w, h);
    mat_translate(m, -ox, -oy, -oz);
}

// A move in 26.6 as whole pixels, held to what a point says; one that is
// not a number — a matrix the page wrote with an infinity in it — moves
// nothing.
static int32_t move_px(double d)
{
    if (!(d == d))
        return 0;
    return edge32(round_px((int64_t)fmax(fmin(d, 4e18), -4e18)));
}

// A transformed box's frame, its border box at (x, y), `w` by `h` in 26.6:
// it and everything in it are drawn through its matrix, and `clip`, what
// clips it from outside, stays where it is. Drawn flat, the point (x, y)
// goes where the matrix sends (x, y, 0), over w (Transforms 2 § 4); `by`
// is how far that moves the transform-origin.
static const flow_frame_t *transform_new(Build *bd, const flow_style_t *s, int64_t x, int64_t y,
                                         int64_t w, int64_t h, const flow_box_t *b, Clip clip)
{
    flow_frame_t *f = f_arena_alloc(&bd->t->arena, sizeof(*f));
    if (f == NULL) {
        bd->failed = true;
        return NULL;
    }
    f->kind = F_FRAME_TRANSFORM;
    f->box = b;
    f->moves = true;
    f->mover = f;
    f->outer = b->frame;
    f->clips = clip.x || clip.y;
    f->clip = clip_rect(clip);
    f->clipped = f->clips || (f->outer != NULL && f->outer->clipped);
    double *m = f->matrix;
    f_transform_matrix(s, x, y, w, h, m);
    double ox = (double)(x + f_len(s->transform_origin[0], w));
    double oy = (double)(y + f_len(s->transform_origin[1], h));
    double tx = m[0] * ox + m[4] * oy + m[12], ty = m[1] * ox + m[5] * oy + m[13];
    double tw = m[3] * ox + m[7] * oy + m[15];
    // Behind the eye a point is not drawn at all, nor moved.
    double dx = tw > 0 ? tx / tw - ox : 0, dy = tw > 0 ? ty / tw - oy : 0;
    f->by = (flow_point_t){move_px(dx), move_px(dy)};
    // FLAT, as a flat drawing shows it: x' = (m0 x + m4 y + m12) / w, w =
    // m3 x + m7 y + m15, divides everywhere by the same w > 0 when m3 = m7
    // = 0 — an affine map, in pixels with the 26.6 translation taken down.
    // Exactly 0: every function but perspective() and matrix3d() keeps the
    // w row (0, 0, 0, 1), and so does their product, to the bit. A
    // TRANSLATION besides when what it does to x and y, w divided out, is
    // the identity to within the rounding of a turn's sine and cosine — so
    // rotate(360deg) moves, as drawn; that tolerance chooses only how it
    // is drawn, never what.
    bool flat = m[3] == 0 && m[7] == 0 && m[15] > 0 && isfinite(m[15]);
    const double e = 1e-9;
    bool moves = flat && fabs(m[0] / m[15] - 1) < e && fabs(m[5] / m[15] - 1) < e &&
                 fabs(m[1] / m[15]) < e && fabs(m[4] / m[15]) < e;
    f->upright = !flat;
    f->through = flat && !moves;
    if (f->through) {
        double w = m[15];
        double a[6] = {m[0] / w, m[1] / w, m[4] / w, m[5] / w, m[12] / w / 64, m[13] / w / 64};
        for (int i = 0; i < 6; i++)
            f->through = f->through && isfinite(a[i]);
        os64_memcpy(f->affine, a, sizeof(a));
        if (!f->through)
            f->upright = true;          // what cannot be worked out is drawn upright
    }
    return f;
}

// Whether a box moves in a transform frame of its own: its element is
// transformed (FBox.transformed), and it is not an inline-block's content,
// which its atom's moves.
static bool own_transform(const FBox *src, const flow_box_t *b)
{
    return src->transformed && (b->parent == NULL || b->parent->kind != FLOW_BOX_ATOMIC);
}

// What a box's own frame is: an out-of-flow box moves with its containing
// block, every other with its parent (add), and a sticky or transformed
// box with itself, `clip` being what clips it from outside — a box both
// sticky and transformed, its transform frame inside its sticky one; a
// scroll container is remembered for the sticky boxes inside it. False on
// no memory.
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
        clip = kNoClip;
    }
    if (own_transform(src, b)) {
        b->frame = transform_new(bd, src->style, src->x, src->y, src->w, src->h, b, clip);
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

static os64_gui_rect_t moved(os64_gui_rect_t r, int64_t dx, int64_t dy);
static os64_gui_rect_t affine_bounds(const double m[6], os64_gui_rect_t r);

// The page's extent grown to reach (right, bottom), in pixels.
static void page_reach(flow_tree_t *t, int64_t right, int64_t bottom)
{
    if (right * 64 > t->layout->width)
        t->layout->width = right * 64;
    if (bottom * 64 > t->layout->height)
        t->layout->height = bottom * 64;
}

// The positioned boxes a scroll container's content holds, which its own
// walk leaves out — a relative box in its flow, a box its content is the
// containing block of, and either inside a sticky or transformed box in it
// — reach as far as what of them can be SEEN: their overflow rects, cut by
// every clip between them and the container — their own, and each sticky
// or transform frame's on the way out — but not the container's, which is
// what scrolling reaches past. Sticky boxes are counted where the flow put
// them, as the content is; a transformed box where its frame draws it,
// moved or through its matrix — the bounds of its corners there (Transforms
// 1 § 3). So a stacked box a transform moves reaches
// too, and outside every scroll container it reaches the page's extent
// (flow_width, flow_height) — unless it is fixed to the viewport, which the
// page is never scrolled to reach.
static void reach_stacked(flow_tree_t *t)
{
    for (int32_t i = 0; i < t->nstacked; i++) {
        const flow_box_t *p = t->stacked[i];
        os64_gui_rect_t r = p->clipped ? cut_to(p->overflow, p->clip) : p->overflow;
        bool transformed = false;
        const flow_frame_t *f = p->frame;
        for (; f != NULL && f->kind != F_FRAME_SCROLL; f = f->outer) {
            if (f->kind == F_FRAME_TRANSFORM) {
                r = f->through ? affine_bounds(f->affine, r) : moved(r, f->by.x, f->by.y);
                transformed = true;
            }
            if (f->clips)
                r = cut_to(r, f->clip);
        }
        if ((!p->positioned && !transformed) || r.w <= 0 || r.h <= 0)
            continue;
        int64_t right = (int64_t)r.x + r.w, bottom = (int64_t)r.y + r.h;
        if (f != NULL) {
            scroll_reach(t->scrollers[f->index], right, bottom);
        } else if (transformed && !p->fixed) {
            page_reach(t, right, bottom);
        }
    }
    // The root is no stacked box, being the first context already, but
    // its transform moves the whole page: what it holds reaches the page's
    // extent moved as well as where layout counted it. (It is never fixed:
    // `position` on the root is laid out in the flow, POSITION.md § Booked.)
    const flow_box_t *root = t->root;
    if (root != NULL && root->frame != NULL && root->frame->box == root &&
        root->frame->kind == F_FRAME_TRANSFORM) {
        const flow_frame_t *f = root->frame;
        os64_gui_rect_t r = f->through ? affine_bounds(f->affine, root->overflow)
                                       : moved(root->overflow, f->by.x, f->by.y);
        if (r.w > 0 && r.h > 0)
            page_reach(t, (int64_t)r.x + r.w, (int64_t)r.y + r.h);
    }
}

// How far past its shape one shadow may be drawn: its offset and spread,
// and its blur's reach — twice the blur radius, a Gaussian of standard
// deviation half the radius being nothing past three of them.
static int64_t shadow_reach(const flow_shadow_t *sh)
{
    int64_t dx = sh->x < 0 ? -(int64_t)sh->x : sh->x, dy = sh->y < 0 ? -(int64_t)sh->y : sh->y;
    return (dx > dy ? dx : dy) + (sh->spread > 0 ? sh->spread : 0) + 2 * (int64_t)sh->blur + 1;
}

// The furthest any shadow in the tree under `root` reaches past its box: a
// box's own shadows (an inset one falls inside) and its text's. Walked
// without recursion, the tree being as deep as the page.
static int64_t ink_reach(const flow_box_t *root)
{
    int64_t most = 0;
    for (const flow_box_t *b = root; b != NULL;) {
        const flow_style_t *s = b->style;
        for (int32_t i = 0; i < s->nbox_shadows; i++)
            if (!s->box_shadows[i].inset && shadow_reach(&s->box_shadows[i]) > most)
                most = shadow_reach(&s->box_shadows[i]);
        for (int32_t i = 0; i < s->ntext_shadows; i++)
            if (shadow_reach(&s->text_shadows[i]) > most)
                most = shadow_reach(&s->text_shadows[i]);
        if (b->first != NULL) {
            b = b->first;
            continue;
        }
        while (b != NULL && b != root && b->next == NULL)
            b = b->parent;
        b = b != NULL && b != root ? b->next : NULL;
    }
    return most < INT32_MAX / 4 ? most : INT32_MAX / 4;
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
    // A transformed atom — a replaced box or an inline-block — is a
    // stacking context, painted whole with its content, in a frame of its
    // own.
    if (fr->kind == FF_ATOMIC && f_transforms(fr->style)) {
        b->stacked = true;
        if (!push_list(bd, &bd->t->stacked, &bd->t->nstacked, &bd->t->cap_stacked, b))
            return;
        b->frame = transform_new(bd, fr->style, fr->x, fr->y, fr->w, fr->h, b, clip);
        if (b->frame == NULL)
            return;
        clip = kNoClip;         // its frame's, where it stays as the atom moves
    }
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
    b->floated = src->floated;
    b->clearance = (int32_t)round_px(src->clearance);
    // Fixed to the viewport, unless a transformed box round it is its
    // containing block, which it scrolls with.
    b->fixed |= src->out_of_flow && src->style->position == FLOW_POSITION_FIXED &&
                src->pos != NULL && src->pos->cb == NULL;
    if (src->positioned &&
        !push_list(bd, &bd->t->positioned, &bd->t->npositioned, &bd->t->cap_positioned, b))
        return b;
    // Below full opacity, a box its block-level parent's walks would reach
    // is a stacking context of its own, painted whole; and so is one
    // transformed. The root is the first context already, and an
    // inline-block's content is painted where its atom is.
    b->stacked = src->positioned || (parent != NULL && own_transform(src, b)) ||
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
    if (is_sticky(src) || own_transform(src, b))
        clip = kNoClip;         // its frame's, where it stays as the box moves
    b->content = rect_of(src->x + src->border[FLOW_LEFT] + src->padding[FLOW_LEFT],
        src->y + src->border[FLOW_TOP] + src->padding[FLOW_TOP],
        src->w - src->border[FLOW_LEFT] - src->border[FLOW_RIGHT] - src->padding[FLOW_LEFT] - src->padding[FLOW_RIGHT],
        src->h - src->border[FLOW_TOP] - src->border[FLOW_BOTTOM] - src->padding[FLOW_TOP] - src->padding[FLOW_BOTTOM]);
    set_clip(b, legacy_clip(src,clip));
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
            // A transformed atom reaches where it is drawn (reach_stacked).
            if (pieces != NULL && !pieces->stacked)
                line->overflow = join(line->overflow, pieces->overflow);
        }
        b->overflow = join(b->overflow, line->overflow);
        content_reach(scroll, src, line->overflow);
    }
    for (const FBox *c = src->first; c != NULL && !bd->failed; c = c->next) {
        if (!c->placed)
            continue;
        flow_box_t *child = public_box(bd, c, b, &kids, c->out_of_flow ? outer_clip(c) : in);
        // A positioned or transformed child reaches by itself (reach_stacked).
        if (child != NULL && !c->positioned && !c->transformed) {
            b->overflow = join(b->overflow, child->overflow);
            content_reach(scroll, src, child->overflow);
        }
        if (child != NULL && !child->stacked)
            b->holds_float |= child->floated != FLOW_FLOAT_NONE || child->holds_float;
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
    if (src->out_of_flow && src->style->legacy_clip_set)
        b->overflow = cut_to(b->overflow,b->clip);
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
// or sticky one, or any box painted at less than full opacity or
// transformed (CSS Position 3, Color 4, Transforms 1 § 3) — which is why
// such a box is stacked even in the flow, where it paints as a positioned
// box of z-index 0 would. Each
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
           s->position == FLOW_POSITION_STICKY || s->opacity < 1000 ||
           (b->frame != NULL && b->frame->box == b && b->frame->kind == F_FRAME_TRANSFORM);
}

// z-index applies to a positioned box; a box stacked in the flow is 0.
static int32_t z_of(const flow_box_t *b)
{
    return b->positioned && b->style->has_z_index ? b->style->z_index : 0;
}

// Drawn through its own matrix (flow_box_through).
static bool own_through(const flow_box_t *b)
{
    return b->frame != NULL && b->frame->box == b && b->frame->kind == F_FRAME_TRANSFORM &&
           b->frame->through;
}

// Composited whole: below full opacity, or drawn through its matrix, and
// painted at all.
static bool is_group(const flow_box_t *b)
{
    return (b->style->opacity < 1000 || own_through(b)) && !b->unpainted;
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
        reach_stacked(tree);
        sticky_reach_scrolled(tree);
        tree->ink = ink_reach(tree->root);
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
// `untransformed`: every transform left out, as layout geometry sees a box
// (flow_box_layout_offset).
typedef struct {
    const flow_frame_t *s[F_FRAME_DEPS];
    flow_point_t at[F_FRAME_DEPS];
    int32_t n;
    bool untransformed;
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

// A frame's offset in the coordinates it is PAINTED in: the scroll under a
// fixed box, then each frame's move, outermost first, each worked in the
// frame the one outside it moved — a sticky box's push, a scroll
// container's scroll position taken back, a translation's `by`. A sticky
// box that does not move is where the frame it is in puts it. A frame
// drawn THROUGH its matrix is where those coordinates begin: inside it,
// everything is painted in its own, and nothing outside moves it
// (to_space and from_space cross it).
static flow_point_t frame_of(Frames *m, const flow_frame_t *s, bool fixed, flow_point_t scroll)
{
    s = s != NULL ? s->mover : NULL;
    if (s != NULL && s->kind == F_FRAME_TRANSFORM && s->through && !m->untransformed)
        return (flow_point_t){0, 0};
    if (s == NULL)
        return fixed ? scroll : (flow_point_t){0, 0};
    for (int32_t i = 0; i < m->n; i++)
        if (m->s[i] == s)
            return m->at[i];
    flow_point_t at = frame_of(m, s->outer, fixed, scroll);
    flow_point_t d = s->kind == F_FRAME_SCROLL      ? (flow_point_t){-s->at.x, -s->at.y}
                   : s->kind != F_FRAME_TRANSFORM ? shift(m, s, at, scroll)
                   : m->untransformed || s->through ? (flow_point_t){0, 0}
                                                  : s->by;
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

flow_point_t flow_box_layout_offset(const flow_box_t *box, flow_point_t scroll)
{
    Frames m = {.n = 0, .untransformed = true};
    return box != NULL ? frame_of(&m, box->frame, box->fixed, scroll) : (flow_point_t){0, 0};
}

// The innermost frame drawn through its matrix along `f` and those outside
// it, or NULL: whose coordinates a box in `f` is painted in.
static const flow_frame_t *inner_through(const flow_frame_t *f)
{
    for (; f != NULL; f = f->outer)
        if (f->kind == F_FRAME_TRANSFORM && f->through)
            return f;
    return NULL;
}

// A point painted in the coordinates of frame `f`'s drawn-through frame
// taken out to the page's: through each drawn-through frame's matrix and
// then the offset of the frame outside it, innermost first.
static void from_space(const flow_frame_t *f, flow_point_t scroll, double *x, double *y)
{
    Frames m = {.n = 0};
    for (const flow_frame_t *t = inner_through(f); t != NULL; t = inner_through(t->outer)) {
        const double *a = t->affine;
        double px = *x, py = *y;
        flow_point_t o = frame_of(&m, t->outer, t->box->fixed, scroll);
        *x = a[0] * px + a[2] * py + a[4] + o.x;
        *y = a[1] * px + a[3] * py + a[5] + o.y;
    }
}

// x * y as p + e exactly, Dekker's two-product: each split into halves of
// 26 and 27 bits (Veltkamp), whose products a double holds exactly. Valid
// while the split's 2^27 cannot overflow and no partial underflows.
static void two_product(double x, double y, double *p, double *e)
{
    const double split = 134217729.0;   // 2^27 + 1
    double t = split * x, xh = t - (t - x), xl = x - xh;
    t = split * y;
    double yh = t - (t - y), yl = y - yh;
    *p = x * y;
    *e = ((xh * yh - *p) + xh * yl + xl * yh) + xl * yl;
}

// The 2x2 determinant a d - b c, worked so cancellation does not lose it:
// each product exact as a pair (two_product), and the difference of the
// heads exact where they are close (Sterbenz), the tails added after. In
// a double's middle range; elsewhere in long double, whose exponent
// reaches past a double's, so no product over- or underflows there.
static long double determinant(double a, double b, double c, double d)
{
    const double small = 0x1p-480, large = 0x1p+480;
    double most = fmax(fmax(fabs(a), fabs(b)), fmax(fabs(c), fabs(d)));
    double least = fmin(fmin(a != 0 ? fabs(a) : most, b != 0 ? fabs(b) : most),
                        fmin(c != 0 ? fabs(c) : most, d != 0 ? fabs(d) : most));
    if (most < large && least > small) {
        double p1, e1, p2, e2;
        two_product(a, d, &p1, &e1);
        two_product(b, c, &p2, &e2);
        return (long double)(p1 - p2) + ((long double)e1 - e2);
    }
    return (long double)a * d - (long double)b * c;
}

bool flow_affine_invert(const double m[6], double inv[6])
{
    // The determinant worked without loss (determinant()), the rest in long
    // double: a huge matrix (scale(1e200)), a tiny one, one whose entries
    // span a double's range (matrix(1e300, 1e-300, 1e300, 2e-300, 0, 0)),
    // and one whose determinant is a cancellation (matrix(2^52, 2^52 - 1,
    // 2^52 + 1, 2^52, 0, 0), determinant 1) all come back with a finite
    // inverse.
    long double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5];
    long double det = determinant(m[0], m[1], m[2], m[3]);
    if (det == 0)
        return false;
    long double r[6] = {d / det, -b / det, -c / det, a / det, 0, 0};
    r[4] = -(r[0] * e + r[2] * f);
    r[5] = -(r[1] * e + r[3] * f);
    for (int i = 0; i < 6; i++) {
        inv[i] = (double)r[i];
        if (!isfinite(inv[i]))
            return false;
    }
    return true;
}

// The page's point (x, y) taken in to the coordinates frame `f`'s
// drawn-through frame paints in: outermost first, each frame's outside
// offset taken off and its matrix undone. False where a matrix cannot be
// undone — a scale of 0, which draws nothing to point at.
static bool to_space(const flow_frame_t *f, flow_point_t scroll, double *x, double *y)
{
    const flow_frame_t *t = inner_through(f);
    if (t == NULL)
        return true;
    if (!to_space(t->outer, scroll, x, y))
        return false;
    Frames m = {.n = 0};
    flow_point_t o = frame_of(&m, t->outer, t->box->fixed, scroll);
    double inv[6];
    if (!flow_affine_invert(t->affine, inv))
        return false;
    double px = *x - o.x, py = *y - o.y;
    *x = inv[0] * px + inv[2] * py + inv[4];
    *y = inv[1] * px + inv[3] * py + inv[5];
    return isfinite(*x) && isfinite(*y);
}

// The pixel a point taken through a matrix falls in, a whole pixel within
// a millionth being that pixel (affine_bounds' rule).
static double pixel_of(double v)
{
    return floor(v + 1e-6);
}

bool flow_box_point(const flow_box_t *box, flow_point_t scroll, int32_t x, int32_t y,
                    flow_point_t *out)
{
    if (box == NULL)
        return false;
    double px = x, py = y;
    if (!to_space(box->frame, scroll, &px, &py))
        return false;
    flow_point_t o = flow_box_doc_offset(box, scroll);
    px = pixel_of(px) - o.x;
    py = pixel_of(py) - o.y;
    if (!(px >= INT32_MIN && px <= INT32_MAX && py >= INT32_MIN && py <= INT32_MAX))
        return false;
    *out = (flow_point_t){(int32_t)px, (int32_t)py};
    return true;
}

bool flow_box_through(const flow_box_t *box, flow_point_t scroll, double m[6],
                      os64_gui_rect_t *clip, flow_point_t *upright)
{
    const flow_frame_t *t = box != NULL ? box->frame : NULL;
    if (t == NULL || t->box != box || t->kind != F_FRAME_TRANSFORM || !t->through)
        return false;
    Frames fm = {.n = 0};
    flow_point_t o = frame_of(&fm, t->outer, box->fixed, scroll);
    os64_memcpy(m, t->affine, 6 * sizeof(double));
    m[4] += o.x;
    m[5] += o.y;
    if (clip != NULL)
        *clip = t->clips ? moved(t->clip, o.x, o.y) : clip_rect(kNoClip);
    if (upright != NULL)
        *upright = (flow_point_t){edge32((int64_t)o.x + t->by.x), edge32((int64_t)o.y + t->by.y)};
    return true;
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

// Where it is: in a drawn-through frame, the bounds of its four corners
// taken out through every matrix (CSSOM View's rect of a transformed box),
// whole pixels holding all of them.
os64_gui_rect_t flow_box_doc_rect(const flow_box_t *box, flow_point_t scroll)
{
    if (box == NULL)
        return (os64_gui_rect_t){0, 0, 0, 0};
    flow_point_t o = flow_box_doc_offset(box, scroll);
    os64_gui_rect_t r = moved(box->rect, o.x, o.y);
    if (inner_through(box->frame) == NULL)
        return r;
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (int k = 0; k < 4; k++) {
        double x = (double)r.x + (k & 1 ? r.w : 0), y = (double)r.y + (k & 2 ? r.h : 0);
        from_space(box->frame, scroll, &x, &y);
        x0 = fmin(x0, x);
        y0 = fmin(y0, y);
        x1 = fmax(x1, x);
        y1 = fmax(y1, y);
    }
    // Held to what the rect's 32 bits can say, as edge32 holds an edge. A
    // whole pixel within a millionth is that pixel, so a sine's last bit
    // never grows a rect by one; and an extent that is there, however thin
    // (scaleX(1e-12)), is at least the pixel it is in, as affine_bounds
    // keeps it.
    const double lo = INT32_MIN / 2, hi = INT32_MAX / 2;
    const double snap = 1e-6;
    bool wide = x1 > x0, tall = y1 > y0;
    x0 = floor(fmax(lo, fmin(hi, x0)) + snap);
    y0 = floor(fmax(lo, fmin(hi, y0)) + snap);
    x1 = ceil(fmax(lo, fmin(hi, x1)) - snap);
    y1 = ceil(fmax(lo, fmin(hi, y1)) - snap);
    if (wide && x1 <= x0)
        x1 = x0 + 1;
    if (tall && y1 <= y0)
        y1 = y0 + 1;
    return (os64_gui_rect_t){(int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0)};
}

static os64_gui_rect_t meet(os64_gui_rect_t a, os64_gui_rect_t b)
{
    meet_axis(&a.x, &a.w, b.x, b.w);
    meet_axis(&a.y, &a.h, b.y, b.h);
    return a;
}

// Its own clip where its frame puts it, met with what clips each frame it
// is inside from outside that frame, where the frame outside puts it — in
// the coordinates it is painted in, so out to its drawn-through frame,
// whose own clip the face lays the frame through (flow_box_through).
os64_gui_rect_t flow_box_doc_clip(const flow_box_t *box, flow_point_t scroll)
{
    if (box == NULL)
        return (os64_gui_rect_t){0, 0, 0, 0};
    Frames m = {.n = 0};
    flow_point_t o = frame_of(&m, box->frame, box->fixed, scroll);
    os64_gui_rect_t r = moved(box->clip, o.x, o.y);
    const flow_frame_t *end = inner_through(box->frame);
    for (const flow_frame_t *s = box->frame; s != NULL && s != end; s = s->outer)
        if (s->clips) {
            flow_point_t at = frame_of(&m, s->outer, s->box->fixed, scroll);
            r = meet(r, moved(s->clip, at.x, at.y));
        }
    return r;
}

// Whether what clips the frames `b` is inside, from outside each, holds
// the document point (x, y) at this scroll: a box in one is drawn, and
// hit, only there. Each clip is asked in the coordinates it is painted in,
// the point taken in through the drawn-through frames outside it.
static bool outside_clips_hold(const flow_box_t *b, int64_t x, int64_t y, flow_point_t scroll)
{
    Frames m = {.n = 0};
    for (const flow_frame_t *s = b->frame; s != NULL && s->clipped; s = s->outer)
        if (s->clips) {
            double px = (double)x, py = (double)y;
            if (!to_space(s->outer, scroll, &px, &py))
                return false;
            px = pixel_of(px);
            py = pixel_of(py);
            flow_point_t at = frame_of(&m, s->outer, s->box->fixed, scroll);
            if (!(px >= INT32_MIN && px <= INT32_MAX && py >= INT32_MIN && py <= INT32_MAX) ||
                !holds(moved(s->clip, at.x, at.y), (int32_t)px, (int32_t)py))
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

// Appendix E's step 4: the block-level boxes, in tree order, floats left
// for step 5. Nothing of an unpainted box is painted, and nothing inside
// it either.
static void visit_blocks(const Visit *v, const flow_box_t *b)
{
    if (b->unpainted || !meets(b->overflow, v->view))
        return;
    v->visit(v->ctx, b);
    Visit scrolled;
    const Visit *cv = content_view(v, b, &scrolled);
    for (const flow_box_t *c = b->first; c != NULL; c = c->next)
        if (in_tree(c) && c->floated == FLOW_FLOAT_NONE)
            visit_blocks(cv, c);
}

static void visit_floats(const Visit *v, const flow_box_t *b);

// A box painted whole, as if it were a stacking context of its own (Appendix
// E, steps 4, 5 and 7): its blocks, its floats, its inline content. A
// float is painted so, and an inline-block's content is.
static void visit_whole(const Visit *v, const flow_box_t *b)
{
    visit_blocks(v, b);
    visit_floats(v, b);
    visit_inline(v, b);
}

// Step 5: the floats below a box, in tree order, each painted whole; the
// walk does not go into a float it has painted, nor into a subtree that
// holds none (flow_box_t.holds_float). A float inside an overflow-hidden
// box is painted here with the rest: the step is the stacking context's,
// not the formatting context's.
static void visit_floats(const Visit *v, const flow_box_t *b)
{
    if (!b->holds_float || b->unpainted || !meets(b->overflow, v->view))
        return;
    Visit scrolled;
    const Visit *cv = content_view(v, b, &scrolled);
    for (const flow_box_t *c = b->first; c != NULL; c = c->next) {
        if (!in_tree(c))
            continue;
        if (c->floated != FLOW_FLOAT_NONE)
            visit_whole(cv, c);
        else
            visit_floats(cv, c);
    }
}

// A piece of a line, and an atom's own content painted where the atom is,
// whole.
static void visit_piece(const Visit *v, const flow_box_t *f)
{
    v->visit(v->ctx, f);
    for (const flow_box_t *inner = f->first; inner != NULL; inner = inner->next)
        visit_whole(v, inner);
}

// Step 7: the inline content — each line's spans and fragments, but for a
// stacked atom, which is a layer of its own — and the markers.
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
            for (const flow_box_t *f = c->first; f != NULL; f = f->next)
                if (!f->stacked && !f->unpainted && meets(f->overflow, v->view))
                    visit_piece(v, f);
        } else if (c->kind == FLOW_BOX_MARKER) {
            if (!c->unpainted && meets(c->overflow, v->view))
                v->visit(v->ctx, c);
        } else if (in_tree(c) && c->floated == FLOW_FLOAT_NONE) {
            visit_inline(v, c);
        }
    }
}

// How deep the walk keeps the view of nested drawn-through groups, and
// the view past that: all of the plane a rect can say.
#define F_VIEWS 64
static const os64_gui_rect_t kWholePlane = {INT32_MIN / 4, INT32_MIN / 4, INT32_MAX / 2,
                                            INT32_MAX / 2};

// The bounds of `r` sent through the affine map `m` (flow_box_through),
// whole pixels holding the four corners.
static os64_gui_rect_t affine_bounds(const double m[6], os64_gui_rect_t r)
{
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (int k = 0; k < 4; k++) {
        double x = (double)r.x + (k & 1 ? r.w : 0), y = (double)r.y + (k & 2 ? r.h : 0);
        double tx = m[0] * x + m[2] * y + m[4], ty = m[1] * x + m[3] * y + m[5];
        x0 = fmin(x0, tx);
        y0 = fmin(y0, ty);
        x1 = fmax(x1, tx);
        y1 = fmax(y1, ty);
    }
    const double lo = INT32_MIN / 2, hi = INT32_MAX / 2;
    if (!(x0 <= x1 && y0 <= y1))
        return (os64_gui_rect_t){0, 0, 0, 0};
    // A whole pixel within a millionth is that pixel, so a sine's last bit
    // never grows a rect by one. Nor may the snap, or a double's precision,
    // empty what is not empty: the view taken
    // back through scale(1e200) is far less than a pixel — 50 plus 4e-198
    // is 50 — and still the pixel it is in. Whether an extent is empty is
    // the matrix's to say, not the rounded corners'.
    const double snap = 1e-6;
    bool wide = r.w > 0 && fabs(m[0]) * r.w + fabs(m[2]) * r.h > 0;
    bool tall = r.h > 0 && fabs(m[1]) * r.w + fabs(m[3]) * r.h > 0;
    x0 = floor(fmax(lo, fmin(hi, x0)) + snap);
    y0 = floor(fmax(lo, fmin(hi, y0)) + snap);
    x1 = ceil(fmax(lo, fmin(hi, x1)) - snap);
    y1 = ceil(fmax(lo, fmin(hi, y1)) - snap);
    if (wide && x1 <= x0)
        x1 = x0 + 1;
    if (tall && y1 <= y0)
        y1 = y0 + 1;
    return (os64_gui_rect_t){(int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0)};
}

// The bounds of `r` taken back through `m`: what of a group's own
// coordinates lands in `r` outside it. Empty when `m` cannot be undone.
static os64_gui_rect_t affine_back(const double m[6], os64_gui_rect_t r)
{
    double inv[6];
    if (!flow_affine_invert(m, inv))
        return (os64_gui_rect_t){0, 0, 0, 0};
    return affine_bounds(inv, r);
}

os64_gui_rect_t flow_affine_rect(const double m[6], os64_gui_rect_t r)
{
    return affine_bounds(m, r);
}

os64_gui_rect_t flow_affine_rect_back(const double m[6], os64_gui_rect_t r)
{
    return affine_back(m, r);
}

// `r` grown by `d` on every side, held to what a rect can say.
static os64_gui_rect_t grown(os64_gui_rect_t r, int64_t d)
{
    const int64_t lo = INT32_MIN / 2, hi = INT32_MAX / 2;
    int64_t x0 = (int64_t)r.x - d, y0 = (int64_t)r.y - d;
    int64_t x1 = (int64_t)r.x + r.w + d, y1 = (int64_t)r.y + r.h + d;
    x0 = x0 < lo ? lo : x0;
    y0 = y0 < lo ? lo : y0;
    x1 = x1 > hi ? hi : x1;
    y1 = y1 > hi ? hi : y1;
    return (os64_gui_rect_t){(int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0)};
}

// What a group paints, from its OPEN at layer `i` to its CLOSE, whose
// layer `*close` is set to: every box's overflow rect where its frame puts
// it at `scroll`, grown by how far the page's shadows reach (tree->ink) and
// cut to its clip, in the coordinates the group is painted in — a group
// drawn through its matrix inside it counted as its own bounds sent
// through that matrix, cut to what clips it from outside.
static os64_gui_rect_t group_bounds(const flow_tree_t *tree, int32_t i, flow_point_t scroll,
                                    int32_t *close)
{
    os64_gui_rect_t r = {0, 0, 0, 0};
    bool any = false;
    *close = tree->nlayers;
    for (int32_t k = i + 1; k < tree->nlayers; k++) {
        const Layer *l = &tree->layers[k];
        if (l->part == PART_CLOSE) {
            *close = k;
            break;
        }
        os64_gui_rect_t at;
        if (l->part == PART_OPEN) {
            int32_t end;
            at = group_bounds(tree, k, scroll, &end);
            double m[6];
            os64_gui_rect_t clip;
            if (flow_box_through(l->box, scroll, m, &clip, NULL))
                at = meet(affine_bounds(m, at), clip);
            k = end;
            if (at.w <= 0 || at.h <= 0)
                continue;
        } else {
            flow_point_t o = flow_box_doc_offset(l->box, scroll);
            at = moved(l->box->overflow, o.x, o.y);
            at = grown(at, tree->ink);      // what shadows paint past it
            if (l->box->clipped)
                at = meet(at, flow_box_doc_clip(l->box, scroll));
            if (at.w <= 0 || at.h <= 0)
                continue;       // clipped away: nothing of it to hold
        }
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
    // The view in the coordinates each open group drawn through its matrix
    // is painted in: the one outside taken back through the matrix, joined
    // with it moved back to where an upright drawing puts the group — the
    // face may refuse to lay it through (flow_box_through's `upright`), and
    // the walk is over before it is asked. Past the deepest kept, the whole
    // plane, which culls nothing and so loses nothing.
    os64_gui_rect_t views[F_VIEWS];
    int32_t depth = 0;
    for (int32_t i = 0; i < tree->nlayers; i++) {
        const flow_box_t *b = tree->layers[i].box;
        os64_gui_rect_t view = depth == 0 ? viewport : depth <= F_VIEWS ? views[depth - 1]
                                                                        : kWholePlane;
        if (tree->layers[i].part == PART_OPEN) {
            double m[6];
            flow_point_t up;
            if (flow_box_through(b, scroll, m, NULL, &up)) {
                if (depth < F_VIEWS) {
                    os64_gui_rect_t back = affine_back(m, view);
                    os64_gui_rect_t upright = moved(view, -(int64_t)up.x, -(int64_t)up.y);
                    views[depth] = view.w == kWholePlane.w ? kWholePlane
                                 : back.w > 0 && back.h > 0 ? join(back, upright)
                                                            : upright;
                }
                depth++;
            }
            // Bounds scan the group's layers: only for a visitor that asks.
            if (visitor->open != NULL) {
                int32_t end;
                visitor->open(ctx, b, group_bounds(tree, i, scroll, &end));
            }
            continue;
        }
        if (tree->layers[i].part == PART_CLOSE) {
            if (own_through(b) && depth > 0)
                depth--;
            if (visitor->close != NULL)
                visitor->close(ctx, b);
            continue;
        }
        flow_point_t o = flow_box_doc_offset(b, scroll);
        Visit v = {tree, moved(view, -(int64_t)o.x, -(int64_t)o.y), visit, ctx};
        switch (tree->layers[i].part) {
        case PART_WHOLE:
            if (b->kind != FLOW_BOX_ATOMIC)
                visit_whole(&v, b);
            else if (!b->unpainted && meets(b->overflow, v.view))
                visit_piece(&v, b);
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
                if (in_tree(c) && c->floated == FLOW_FLOAT_NONE)
                    visit_blocks(cv, c);
            for (const flow_box_t *c = b->first; c != NULL; c = c->next)
                if (in_tree(c) && c->floated != FLOW_FLOAT_NONE)
                    visit_whole(cv, c);
                else if (in_tree(c))
                    visit_floats(cv, c);
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

// What a hit is painted with (Appendix E): a block's background (step 4),
// under a float (step 5), under inline content (step 7). A higher rank is
// on top whatever the tree order, so the paragraph written after a float,
// whose border box runs under it, does not take a click on the float.
enum { RANK_BLOCK = 1, RANK_FLOAT, RANK_INLINE };

static const flow_box_t *hit_kids_ranked(const flow_tree_t *t, const flow_box_t *b, int32_t x,
                                         int32_t y, int *rank);

// A child's hit, and its rank: anything on a line is inline content; a
// float, and whatever in it is hit, is the float's; a block is whatever
// was hit inside it, or its own background.
static const flow_box_t *hit_ranked(const flow_tree_t *t, const flow_box_t *c, int32_t x,
                                    int32_t y, int *rank)
{
    if (!block_level(c->kind)) {
        *rank = RANK_INLINE;
        return hit(t, c, x, y);
    }
    if (c->floated != FLOW_FLOAT_NONE) {
        *rank = RANK_FLOAT;
        return hit(t, c, x, y);
    }
    const flow_box_t *h = hit_kids_ranked(t, c, x, y, rank);
    if (h != NULL)
        return h;
    *rank = RANK_BLOCK;
    return hit_self(c, x, y);
}

// The deepest of a box's descendants the tree reaches (its stacked ones
// are layers of their own) holding the point, the last painted winning:
// the highest rank, and among those the last in tree order.
// Pruned on the overflow rect alone, and each box's clip asked of its own
// rect: a clip is a box's own, so no box is pruned by its parent's — a
// positioned one may have a wider clip than its tree parent. A scroll
// container's content is a frame inside the one being walked: what clips
// it from outside is asked here, where the container stands, and the point
// is then taken to where the content stands.
static const flow_box_t *hit_kids_ranked(const flow_tree_t *t, const flow_box_t *b, int32_t x,
                                         int32_t y, int *rank)
{
    *rank = 0;
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
        int r = 0;
        const flow_box_t *h = hit_ranked(t, c, x, y, &r);
        if (h != NULL && r >= *rank) {
            found = h;
            *rank = r;
        }
    }
    return found;
}

static const flow_box_t *hit_kids(const flow_tree_t *t, const flow_box_t *b, int32_t x, int32_t y)
{
    int rank;
    return hit_kids_ranked(t, b, x, y, &rank);
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
        flow_point_t at;
        if (!flow_box_point(b, scroll, x, y, &at))
            continue;
        int64_t px = at.x, py = at.y;
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
