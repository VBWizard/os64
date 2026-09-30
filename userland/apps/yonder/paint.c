// paint.c — a laid-out page drawn through the four verbs (paint.h).
//
// libflow's walk hands the boxes over in CSS 2.1 Appendix E's order
// (backgrounds and borders of the block-level boxes, then the inline
// content), so painting is one decision per box: what that kind of box
// looks like. Nothing here knows about a surface.

#include "paint.h"
#include "html/html.h"

// A painter works in the coordinates of the box it paints — a fixed box's
// are the viewport's — and `off` is what takes them to the page's, added
// wherever a rectangle is handed to a verb. `view` is in the box's.
typedef struct {
    const yonder_verbs_t *v;
    os64_gui_rect_t view;
    flow_point_t scroll, off;
    // The box whose background became the canvas's: it does not paint it
    // a second time over its own border box (CSS 2.1 §14.2).
    const flow_box_t *canvas_owner;
} Painter;

static int32_t px(flow_unit_t u)
{
    return (u + FLOW_UNITS_PER_PX / 2) / FLOW_UNITS_PER_PX;
}

static int64_t min64(int64_t a, int64_t b)
{
    return a < b ? a : b;
}

static int64_t max64(int64_t a, int64_t b)
{
    return a > b ? a : b;
}

// A PAINT COSTS THE VIEWPORT IT PAINTS, WHATEVER SIZES THE PAGE ASKS FOR: a
// border can be a million pixels thick and a bullet a third of a font a
// million pixels high, so every loop below walks only the rows or columns
// that meet the viewport, and every coordinate is added in 64 bits.

// Every fill is cut to the viewport here, so a verb never sees one that
// reaches past it and an empty one is never drawn.
static void fill(const Painter *p, int64_t x, int64_t y, int64_t w, int64_t h, uint32_t colour)
{
    int64_t x0 = max64(x, p->view.x), y0 = max64(y, p->view.y);
    int64_t x1 = min64(x + w, (int64_t)p->view.x + p->view.w);
    int64_t y1 = min64(y + h, (int64_t)p->view.y + p->view.h);
    if (x1 <= x0 || y1 <= y0)
        return;
    p->v->fill(p->v->ctx,
               (os64_gui_rect_t){(int32_t)(x0 + p->off.x), (int32_t)(y0 + p->off.y),
                                 (int32_t)(x1 - x0), (int32_t)(y1 - y0)},
               colour);
}

// A rectangle in the box's coordinates, in the page's, held to int32.
static os64_gui_rect_t on_page(const Painter *p, os64_gui_rect_t r)
{
    int64_t x = (int64_t)r.x + p->off.x, y = (int64_t)r.y + p->off.y;
    x = x > INT32_MAX ? INT32_MAX : x < INT32_MIN ? INT32_MIN : x;
    y = y > INT32_MAX ? INT32_MAX : y < INT32_MIN ? INT32_MIN : y;
    return (os64_gui_rect_t){(int32_t)x, (int32_t)y, r.w, r.h};
}

static uint32_t channelwise(uint32_t c, uint32_t (*f)(uint32_t))
{
    return f((c >> 16) & 0xff) << 16 | f((c >> 8) & 0xff) << 8 | f(c & 0xff);
}

static uint32_t half_to_white(uint32_t c)
{
    return c + (255 - c) / 2;
}

static uint32_t half_to_black(uint32_t c)
{
    return c / 2;
}

// n / d to nearest, ties up, for an n that may be negative (d > 0).
static int32_t nearest(int64_t n, int64_t d)
{
    int64_t q = n + d / 2;
    return (int32_t)(q >= 0 ? q / d : -((-q + d - 1) / d));
}

// A position from `start`: pixels, or a percentage of `room` plus a
// calc()'s fixed part. In units of 1/6400 px: a percentage is 64ths.
static int32_t place_axis(flow_length_t at, int32_t start, int32_t room)
{
    if (at.kind == FLOW_LENGTH_PERCENT)
        return start + nearest((int64_t)room * at.value + (int64_t)at.offset * 100, 100 * 64);
    return start + (at.kind == FLOW_LENGTH_PX ? nearest(at.value, 64) : 0);
}

void yonder_background_place(const flow_box_t *box, os64_gui_rect_t rect, uint32_t iw,
                             uint32_t ih, int32_t *ox, int32_t *oy, bool *repeat_x,
                             bool *repeat_y)
{
    const flow_style_t *s = box->style;
    *ox = place_axis(s->background_position[0], rect.x, rect.w - (int32_t)iw);
    *oy = place_axis(s->background_position[1], rect.y, rect.h - (int32_t)ih);
    *repeat_x = s->background_repeat == FLOW_REPEAT || s->background_repeat == FLOW_REPEAT_X;
    *repeat_y = s->background_repeat == FLOW_REPEAT || s->background_repeat == FLOW_REPEAT_Y;
}

uint32_t yonder_lighter(uint32_t colour)
{
    return channelwise(colour, half_to_white);
}

uint32_t yonder_darker(uint32_t colour)
{
    return channelwise(colour, half_to_black);
}

// The colour of row `i` of a side `width` rows deep, counted from the
// outside. A bevel's top and left take one tone and its bottom and right
// the other; a groove is an inset outer half round an outset inner one.
static uint32_t tone(flow_border_style_t style, uint32_t c, int side, int32_t i, int32_t width)
{
    bool top_left = side == FLOW_TOP || side == FLOW_LEFT;
    bool sunk;
    switch (style) {
    case FLOW_BORDER_INSET:
        sunk = true;
        break;
    case FLOW_BORDER_OUTSET:
        sunk = false;
        break;
    case FLOW_BORDER_GROOVE:
        sunk = i < (width + 1) / 2;
        break;
    default:
        return c;
    }
    return top_left == sunk ? yonder_darker(c) : yonder_lighter(c);
}

// The four sides, one row or column at a time, meeting on the diagonals:
// where two sides share a corner square, the top or bottom side owns the
// pixel at or above the diagonal from the outer corner to the inner one,
// and the side beside it the rest, so every pixel is drawn exactly once.
static void borders(const Painter *p, const flow_box_t *b)
{
    const flow_style_t *s = b->style;
    os64_gui_rect_t r = b->rect;
    int32_t w[4];
    for (int k = 0; k < 4; k++)
        w[k] = s->border_style[k] == FLOW_BORDER_NONE || s->border_style[k] == FLOW_BORDER_HIDDEN
                   ? 0 : px(s->border_width[k]);
    int64_t t = w[FLOW_TOP], rt = w[FLOW_RIGHT], bt = w[FLOW_BOTTOM], l = w[FLOW_LEFT];
    int64_t rx = r.x, ry = r.y, rw = r.w, rh = r.h;
    int64_t vx0 = p->view.x, vy0 = p->view.y;
    int64_t vx1 = vx0 + p->view.w, vy1 = vy0 + p->view.h;
    // Row i of the top is y = ry + i, of the bottom y = ry + rh - 1 - i;
    // column j of the left is x = rx + j, of the right x = rx + rw - 1 - j.
    for (int64_t i = max64(0, vy0 - ry), end = min64(min64(t, rh), vy1 - ry); i < end; i++) {
        // ceil(i * l / t): the columns of the left side this row gives up.
        int64_t x0 = rx + (i * l + t - 1) / t, x1 = rx + rw - (i * rt + t - 1) / t;
        fill(p, x0, ry + i, x1 - x0, 1,
             tone(s->border_style[FLOW_TOP], s->border_color[FLOW_TOP], FLOW_TOP, (int32_t)i,
                  (int32_t)t));
    }
    for (int64_t i = max64(0, ry + rh - vy1), end = min64(min64(bt, rh), ry + rh - vy0); i < end;
         i++) {
        int64_t x0 = rx + (i * l + bt - 1) / bt, x1 = rx + rw - (i * rt + bt - 1) / bt;
        fill(p, x0, ry + rh - 1 - i, x1 - x0, 1,
             tone(s->border_style[FLOW_BOTTOM], s->border_color[FLOW_BOTTOM], FLOW_BOTTOM,
                  (int32_t)i, (int32_t)bt));
    }
    for (int64_t j = max64(0, vx0 - rx), end = min64(min64(l, rw), vx1 - rx); j < end; j++) {
        // How many top rows reach column j: min(t, floor(j * t / l) + 1).
        int64_t y0 = ry + min64(t, j * t / l + 1), y1 = ry + rh - min64(bt, j * bt / l + 1);
        fill(p, rx + j, y0, 1, y1 - y0,
             tone(s->border_style[FLOW_LEFT], s->border_color[FLOW_LEFT], FLOW_LEFT, (int32_t)j,
                  (int32_t)l));
    }
    for (int64_t j = max64(0, rx + rw - vx1), end = min64(min64(rt, rw), rx + rw - vx0); j < end;
         j++) {
        int64_t y0 = ry + min64(t, j * t / rt + 1), y1 = ry + rh - min64(bt, j * bt / rt + 1);
        fill(p, rx + rw - 1 - j, y0, 1, y1 - y0,
             tone(s->border_style[FLOW_RIGHT], s->border_color[FLOW_RIGHT], FLOW_RIGHT, (int32_t)j,
                  (int32_t)rt));
    }
}

// A box's background and borders: every block-level box, and an atom. Its
// picture goes over its colour and under its borders, tiled from the
// border box's corner.
static void frame(const Painter *p, const flow_box_t *b)
{
    const flow_style_t *s = b->style;
    if (b != p->canvas_owner) {
        if (s->has_background)
            fill(p, b->rect.x, b->rect.y, b->rect.w, b->rect.h, s->background);
        os64_gui_rect_t area = on_page(p, b->rect);
        (void)p->v->backdrop(p->v->ctx, b, &area, area.x, area.y, on_page(p, p->view));
    }
    borders(p, b);
}

// The picture or control inside an atom's borders and padding. A percentage
// padding resolved against a width the public tree does not carry, so it
// counts as none here; pictures and controls rarely have one.
static os64_gui_rect_t content_of(const flow_box_t *b)
{
    const flow_style_t *s = b->style;
    int32_t e[4];
    for (int k = 0; k < 4; k++) {
        int32_t pad = s->padding[k].kind == FLOW_LENGTH_PX ? px(s->padding[k].value) : 0;
        bool drawn = s->border_style[k] != FLOW_BORDER_NONE && s->border_style[k] != FLOW_BORDER_HIDDEN;
        e[k] = pad + (drawn ? px(s->border_width[k]) : 0);
    }
    // An edge held to what an int32_t says, as the door holds the box's.
    int64_t x = min64((int64_t)b->rect.x + e[FLOW_LEFT], INT32_MAX);
    int64_t y = min64((int64_t)b->rect.y + e[FLOW_TOP], INT32_MAX);
    int64_t w = max64(0, (int64_t)b->rect.w - e[FLOW_LEFT] - e[FLOW_RIGHT]);
    int64_t h = max64(0, (int64_t)b->rect.h - e[FLOW_TOP] - e[FLOW_BOTTOM]);
    return (os64_gui_rect_t){(int32_t)x, (int32_t)y, (int32_t)min64(w, INT32_MAX - x),
                             (int32_t)min64(h, INT32_MAX - y)};
}

static void decorations(const Painter *p, const flow_box_t *b)
{
    if (b->decoration == 0)
        return;
    int64_t size = px(b->style->font_size);
    int64_t thick = max64(1, size / 16);
    if (b->decoration & FLOW_DECORATION_UNDERLINE)
        fill(p, b->rect.x, (int64_t)b->baseline + thick, b->rect.w, thick, b->underline_color);
    if (b->decoration & FLOW_DECORATION_LINE_THROUGH)
        fill(p, b->rect.x, (int64_t)b->baseline - size * 3 / 10, b->rect.w, thick,
             b->line_through_color);
}

// Disc, circle and square are drawn as shapes, the way browsers draw them,
// not as the characters the marker text holds: a face without the
// geometric shapes would draw a missing-glyph box, and the Western profile
// the text engine speaks does not reach them. `d` px across, a pixel in
// the disc when its centre is: (2c+1-d)^2 + (2r+1-d)^2 <= d^2 in doubled
// units, and in the circle when it is in the disc and not in the one of
// diameter d-2 inside it. Centred on the height a line-through would take,
// in the part of the marker left of its trailing space.
static int64_t isqrt64(int64_t v)
{
    if (v <= 0)
        return 0;
    int64_t r = 0, hi = 3037000499;    // floor(sqrt(INT64_MAX))
    while (r < hi) {
        int64_t mid = r + (hi - r + 1) / 2;
        if (mid <= v / mid)
            r = mid;
        else
            hi = mid - 1;
    }
    return r;
}

// Row `row` of a disc `d` across: the columns c (0 <= c < d) whose doubled
// offset dx = 2c+1-d has dx^2 <= limit, as [*lo, *hi]; false when none.
static bool span_within(int64_t d, int64_t limit, int64_t *lo, int64_t *hi)
{
    if (limit < 0)
        return false;
    int64_t m = isqrt64(limit);
    // dx >= -m and dx <= m, solved for c, rounding inwards.
    int64_t a = d - 1 - m, b = d - 1 + m;
    *lo = max64(0, a <= 0 ? -((-a) / 2) : (a + 1) / 2);
    *hi = min64(d - 1, b / 2);
    return *lo <= *hi;
}

static void bullet(const Painter *p, const flow_box_t *b, flow_list_style_type_t type)
{
    int64_t size = px(b->style->font_size);
    int64_t d = max64(3, size / 3);
    int64_t x = (int64_t)b->rect.x + max64(0, ((int64_t)b->rect.w - size / 4 - d) / 2);
    int64_t y = (int64_t)b->baseline - size * 3 / 10 - d / 2;
    uint32_t colour = b->style->color;
    if (type == FLOW_LIST_SQUARE) {
        fill(p, x, y, d, d, colour);
        return;
    }
    // Each row's run is solved, not searched, and only the rows the
    // viewport shows are.
    int64_t in = d - 2;
    int64_t first = max64(0, p->view.y - y), last = min64(d, (int64_t)p->view.y + p->view.h - y);
    for (int64_t r = first; r < last; r++) {
        int64_t dy = 2 * r + 1 - d;
        int64_t lo, hi, glo, ghi;
        if (!span_within(d, d * d - dy * dy, &lo, &hi))
            continue;
        if (type == FLOW_LIST_CIRCLE && span_within(d, in * in - dy * dy, &glo, &ghi)) {
            // The ring: the disc's run less the inner disc's.
            if (glo > lo)
                fill(p, x + lo, y + r, min64(glo, hi + 1) - lo, 1, colour);
            if (ghi < hi)
                fill(p, x + max64(ghi + 1, lo), y + r, hi - max64(ghi + 1, lo) + 1, 1, colour);
        } else {
            fill(p, x + lo, y + r, hi - lo + 1, 1, colour);
        }
    }
}

static bool is_picture(const flow_box_t *b)
{
    return b->node != NULL && b->node->kind == OS64_HTML_ELEMENT &&
           b->node->tag == OS64_HTML_TAG_IMG;
}

static void paint_box(void *ctx, const flow_box_t *b)
{
    const Painter *outer = ctx;
    if (b->style->visibility != FLOW_VISIBLE)
        return;
    // The box's own coordinates: a fixed one's view is the page's less the
    // scroll, a sticky one's less its push. A box an ancestor's `overflow`
    // clips is drawn only inside the clip, asked in document coordinates —
    // what clips a sticky box from outside stays put as the box moves — and
    // every verb is handed the view, so the view is narrowed.
    Painter clipped = *outer;
    clipped.off = flow_box_doc_offset(b, outer->scroll);
    os64_gui_rect_t v = outer->view;
    if (b->clipped) {
        os64_gui_rect_t c = flow_box_doc_clip(b, outer->scroll);
        int64_t x0 = max64(v.x, c.x), y0 = max64(v.y, c.y);
        int64_t x1 = min64((int64_t)v.x + v.w, (int64_t)c.x + c.w);
        int64_t y1 = min64((int64_t)v.y + v.h, (int64_t)c.y + c.h);
        if (x1 <= x0 || y1 <= y0)
            return;
        v = (os64_gui_rect_t){(int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0)};
    }
    clipped.view = (os64_gui_rect_t){(int32_t)((int64_t)v.x - clipped.off.x),
                                     (int32_t)((int64_t)v.y - clipped.off.y), v.w, v.h};
    const Painter *p = &clipped;
    switch (b->kind) {
    case FLOW_BOX_LINE:
        return;
    case FLOW_BOX_MARKER:
        if (b->style->list_style_type == FLOW_LIST_DISC ||
            b->style->list_style_type == FLOW_LIST_CIRCLE ||
            b->style->list_style_type == FLOW_LIST_SQUARE) {
            bullet(p, b, b->style->list_style_type);
            return;
        }
        // A numbered marker is its text.
        __attribute__((fallthrough));
    case FLOW_BOX_TEXT:
        if (b->run != NULL)
            p->v->text(p->v->ctx, b, (int32_t)((int64_t)b->rect.x + p->off.x),
                       (int32_t)((int64_t)b->baseline + p->off.y), on_page(p, p->view),
                       b->style->color);
        decorations(p, b);
        return;
    case FLOW_BOX_ATOMIC:
    case FLOW_BOX_REPLACED:
        frame(p, b);
        if (b->control >= 0)
            p->v->control(p->v->ctx, b, on_page(p, content_of(b)), on_page(p, p->view));
        else if (is_picture(b))
            p->v->image(p->v->ctx, b, on_page(p, content_of(b)), on_page(p, p->view));
        return;
    case FLOW_BOX_SPAN:
        // An inline box's borders open on its first piece and close on its
        // last, which the public tree does not say; its background is right
        // on every piece.
        if (b->style->has_background)
            fill(p, b->rect.x, b->rect.y, b->rect.w, b->rect.h, b->style->background);
        return;
    default:
        frame(p, b);
        return;
    }
}

// Whether a box has any background: a colour, or a picture behind it.
static bool has_background(const Painter *p, const flow_box_t *b)
{
    return b->style->has_background ||
           p->v->backdrop(p->v->ctx, b, NULL, 0, 0, p->view);
}

// The root's background, or else the body's, is the canvas's.
static const flow_box_t *canvas_owner(const Painter *p, const flow_box_t *root)
{
    if (has_background(p, root))
        return root;
    for (const flow_box_t *c = root->first; c != NULL; c = c->next)
        if (c->node != NULL && c->node->kind == OS64_HTML_ELEMENT &&
            c->node->tag == OS64_HTML_TAG_BODY)
            return has_background(p, c) ? c : NULL;
    return NULL;
}

void yonder_paint(const flow_tree_t *tree, os64_gui_rect_t viewport, flow_point_t scroll,
                  uint32_t paper, const yonder_verbs_t *verbs)
{
    Painter p = {.v = verbs, .view = viewport, .scroll = scroll};
    const flow_box_t *root = flow_root(tree);
    if (root != NULL)
        p.canvas_owner = canvas_owner(&p, root);
    const flow_box_t *owner = p.canvas_owner;
    fill(&p, viewport.x, viewport.y, viewport.w, viewport.h,
         owner != NULL && owner->style->has_background ? owner->style->background : paper);
    // The canvas's picture is tiled from the page's own corner, so it
    // scrolls with the page.
    if (owner != NULL)
        (void)verbs->backdrop(verbs->ctx, owner, &viewport, 0, 0, viewport);
    if (root != NULL)
        flow_visit(tree, viewport, scroll, paint_box, &p);
}
