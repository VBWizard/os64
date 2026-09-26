// paint.c — a laid-out page drawn through the four verbs (paint.h).
//
// libflow's walk hands the boxes over in CSS 2.1 Appendix E's order
// (backgrounds and borders of the block-level boxes, then the inline
// content), so painting is one decision per box: what that kind of box
// looks like. Nothing here knows about a surface.

#include "paint.h"
#include "html/html.h"

typedef struct {
    const yonder_verbs_t *v;
    os64_gui_rect_t view;
    // The box whose background became the canvas's: it does not paint it
    // a second time over its own border box (CSS 2.1 §14.2).
    const flow_box_t *canvas_owner;
} Painter;

static int32_t px(flow_unit_t u)
{
    return (u + FLOW_UNITS_PER_PX / 2) / FLOW_UNITS_PER_PX;
}

static int32_t min32(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

static int32_t max32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

// Every fill is cut to the viewport here, so a verb never sees one that
// reaches past it and an empty one is never drawn.
static void fill(const Painter *p, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t colour)
{
    int32_t x0 = max32(x, p->view.x), y0 = max32(y, p->view.y);
    int32_t x1 = min32(x + w, p->view.x + p->view.w), y1 = min32(y + h, p->view.y + p->view.h);
    if (x1 <= x0 || y1 <= y0)
        return;
    p->v->fill(p->v->ctx, (os64_gui_rect_t){x0, y0, x1 - x0, y1 - y0}, colour);
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
    int32_t t = w[FLOW_TOP], rt = w[FLOW_RIGHT], bt = w[FLOW_BOTTOM], l = w[FLOW_LEFT];
    for (int32_t i = 0; i < t && i < r.h; i++) {
        // ceil(i * l / t): the columns of the left side this row gives up.
        int32_t x0 = r.x + (i * l + t - 1) / t, x1 = r.x + r.w - (i * rt + t - 1) / t;
        fill(p, x0, r.y + i, x1 - x0, 1,
             tone(s->border_style[FLOW_TOP], s->border_color[FLOW_TOP], FLOW_TOP, i, t));
    }
    for (int32_t i = 0; i < bt && i < r.h; i++) {
        int32_t x0 = r.x + (i * l + bt - 1) / bt, x1 = r.x + r.w - (i * rt + bt - 1) / bt;
        fill(p, x0, r.y + r.h - 1 - i, x1 - x0, 1,
             tone(s->border_style[FLOW_BOTTOM], s->border_color[FLOW_BOTTOM], FLOW_BOTTOM, i, bt));
    }
    for (int32_t j = 0; j < l && j < r.w; j++) {
        // How many top rows reach column j: min(t, floor(j * t / l) + 1).
        int32_t y0 = r.y + min32(t, j * t / l + 1), y1 = r.y + r.h - min32(bt, j * bt / l + 1);
        fill(p, r.x + j, y0, 1, y1 - y0,
             tone(s->border_style[FLOW_LEFT], s->border_color[FLOW_LEFT], FLOW_LEFT, j, l));
    }
    for (int32_t j = 0; j < rt && j < r.w; j++) {
        int32_t y0 = r.y + min32(t, j * t / rt + 1), y1 = r.y + r.h - min32(bt, j * bt / rt + 1);
        fill(p, r.x + r.w - 1 - j, y0, 1, y1 - y0,
             tone(s->border_style[FLOW_RIGHT], s->border_color[FLOW_RIGHT], FLOW_RIGHT, j, rt));
    }
}

// A box's background and borders: every block-level box, and an atom.
static void frame(const Painter *p, const flow_box_t *b)
{
    const flow_style_t *s = b->style;
    if (s->has_background && b != p->canvas_owner)
        fill(p, b->rect.x, b->rect.y, b->rect.w, b->rect.h, s->background);
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
    os64_gui_rect_t r = b->rect;
    r.x += e[FLOW_LEFT];
    r.y += e[FLOW_TOP];
    r.w = max32(0, r.w - e[FLOW_LEFT] - e[FLOW_RIGHT]);
    r.h = max32(0, r.h - e[FLOW_TOP] - e[FLOW_BOTTOM]);
    return r;
}

static void decorations(const Painter *p, const flow_box_t *b)
{
    if (b->decoration == 0)
        return;
    int32_t size = px(b->style->font_size);
    int32_t thick = max32(1, size / 16);
    if (b->decoration & FLOW_DECORATION_UNDERLINE)
        fill(p, b->rect.x, b->baseline + thick, b->rect.w, thick, b->decoration_color);
    if (b->decoration & FLOW_DECORATION_LINE_THROUGH)
        fill(p, b->rect.x, b->baseline - size * 3 / 10, b->rect.w, thick, b->decoration_color);
}

// Disc, circle and square are drawn as shapes, the way browsers draw them,
// not as the characters the marker text holds: a face without the
// geometric shapes would draw a missing-glyph box, and the Western profile
// the text engine speaks does not reach them. `d` px across, a pixel in
// the disc when its centre is: (2c+1-d)^2 + (2r+1-d)^2 <= d^2 in doubled
// units, and in the circle when it is in the disc and not in the one of
// diameter d-2 inside it. Centred on the height a line-through would take,
// in the part of the marker left of its trailing space.
static void bullet(const Painter *p, const flow_box_t *b, flow_list_style_type_t type)
{
    int32_t size = px(b->style->font_size);
    int32_t d = max32(3, size / 3);
    int32_t x = b->rect.x + max32(0, (b->rect.w - size / 4 - d) / 2);
    int32_t y = b->baseline - size * 3 / 10 - d / 2;
    uint32_t colour = b->style->color;
    if (type == FLOW_LIST_SQUARE) {
        fill(p, x, y, d, d, colour);
        return;
    }
    int32_t in = d - 2;
    for (int32_t r = 0; r < d; r++) {
        int32_t dy = 2 * r + 1 - d;
        int32_t run = -1;
        for (int32_t c = 0; c <= d; c++) {
            int32_t dx = 2 * c + 1 - d;
            bool on = c < d && dx * dx + dy * dy <= d * d &&
                      (type == FLOW_LIST_DISC || dx * dx + dy * dy > in * in);
            if (on && run < 0)
                run = c;
            if (!on && run >= 0) {
                fill(p, x + run, y + r, c - run, 1, colour);
                run = -1;
            }
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
    const Painter *p = ctx;
    if (b->style->visibility != FLOW_VISIBLE)
        return;
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
            p->v->text(p->v->ctx, b, p->view, b->style->color);
        decorations(p, b);
        return;
    case FLOW_BOX_ATOMIC:
    case FLOW_BOX_REPLACED:
        frame(p, b);
        if (b->control >= 0)
            p->v->control(p->v->ctx, b, content_of(b), p->view);
        else if (is_picture(b))
            p->v->image(p->v->ctx, b, content_of(b), p->view);
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

// The root's background, or else the body's, is the canvas's.
static const flow_box_t *canvas_owner(const flow_box_t *root)
{
    if (root->style->has_background)
        return root;
    for (const flow_box_t *c = root->first; c != NULL; c = c->next)
        if (c->node != NULL && c->node->kind == OS64_HTML_ELEMENT &&
            c->node->tag == OS64_HTML_TAG_BODY)
            return c->style->has_background ? c : NULL;
    return NULL;
}

void yonder_paint(const flow_tree_t *tree, os64_gui_rect_t viewport, uint32_t paper,
                  const yonder_verbs_t *verbs)
{
    Painter p = {verbs, viewport, NULL};
    const flow_box_t *root = flow_root(tree);
    if (root != NULL)
        p.canvas_owner = canvas_owner(root);
    fill(&p, viewport.x, viewport.y, viewport.w, viewport.h,
         p.canvas_owner != NULL ? p.canvas_owner->style->background : paper);
    if (root != NULL)
        flow_visit(tree, viewport, paint_box, &p);
}
