// paint.c — a laid-out page drawn through the four verbs (paint.h).
//
// libflow's walk hands the boxes over in CSS 2.1 Appendix E's order
// (backgrounds and borders of the block-level boxes, then the inline
// content), so painting is one decision per box: what that kind of box
// looks like. Nothing here knows about a surface.

#include "paint.h"
#include "html/html.h"
#include "os64/mem.h"
#include <math.h>

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

// Each colour channel through `f`; how transparent the colour is stays.
static uint32_t channelwise(uint32_t c, uint32_t (*f)(uint32_t))
{
    return (c & 0xff000000u) | f((c >> 16) & 0xff) << 16 | f((c >> 8) & 0xff) << 8 |
           f(c & 0xff);
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
static int64_t nearest(int64_t n, int64_t d)
{
    int64_t q = n + d / 2;
    return q >= 0 ? q / d : -((-q + d - 1) / d);
}

// A position from `start`: pixels, or a percentage of `room` plus a
// calc()'s fixed part. In units of 1/6400 px: a percentage is 64ths.
// Held to what an int32_t says.
static int32_t place_axis(flow_length_t at, int32_t start, int32_t room)
{
    int64_t by = at.kind == FLOW_LENGTH_PERCENT
                     ? nearest((int64_t)room * at.value + (int64_t)at.offset * 100, 100 * 64)
                 : at.kind == FLOW_LENGTH_PX ? nearest(at.value, 64) : 0;
    return (int32_t)max64(INT32_MIN, min64(INT32_MAX, start + by));
}

os64_gui_rect_t yonder_box_edge(const flow_box_t *box, os64_gui_rect_t rect, flow_edge_t edge)
{
    if (edge != FLOW_EDGE_PADDING && edge != FLOW_EDGE_CONTENT)
        return rect;
    const flow_style_t *s = box->style;
    int64_t e[4];
    for (int k = 0; k < 4; k++) {
        e[k] = px(s->border_width[k]);
        if (edge == FLOW_EDGE_CONTENT && s->padding[k].kind == FLOW_LENGTH_PX)
            e[k] += px(s->padding[k].value);
    }
    int64_t w = max64(0, (int64_t)rect.w - e[FLOW_LEFT] - e[FLOW_RIGHT]);
    int64_t h = max64(0, (int64_t)rect.h - e[FLOW_TOP] - e[FLOW_BOTTOM]);
    int64_t x = min64((int64_t)rect.x + e[FLOW_LEFT], INT32_MAX);
    int64_t y = min64((int64_t)rect.y + e[FLOW_TOP], INT32_MAX);
    return (os64_gui_rect_t){(int32_t)x, (int32_t)y, (int32_t)min64(w, INT32_MAX - x),
                             (int32_t)min64(h, INT32_MAX - y)};
}

// A size written for one axis of a `room`-px origin box, in px up to what
// an int32_t says (so that scaling it by a picture's side fits 64 bits);
// -1 for auto.
static int64_t size_axis(flow_length_t l, int32_t room)
{
    int64_t n;
    if (l.kind == FLOW_LENGTH_PX)
        n = nearest(l.value, 64);
    else if (l.kind == FLOW_LENGTH_PERCENT)
        n = nearest((int64_t)room * l.value + (int64_t)l.offset * 100, 100 * 64);
    else
        return -1;
    return max64(0, min64(n, INT32_MAX));
}

bool yonder_background_tile(const flow_style_t *s, os64_gui_rect_t area, uint32_t iw, uint32_t ih,
                            yonder_tile_t *out)
{
    int64_t w = area.w, h = area.h;         // no size of its own: the origin box's
    bool ratio = iw > 0 && ih > 0;
    if (s->background_fit != FLOW_FIT_LENGTHS) {
        // Scaled to the box's width or to its height, whichever covers it
        // (or fits inside it): the width when the box is the wider shape.
        bool wider = (int64_t)area.w * ih >= (int64_t)area.h * iw;
        if (ratio && wider == (s->background_fit == FLOW_FIT_COVER))
            h = nearest((int64_t)area.w * ih, iw);
        else if (ratio)
            w = nearest((int64_t)area.h * iw, ih);
    } else {
        int64_t gw = size_axis(s->background_size[0], area.w);
        int64_t gh = size_axis(s->background_size[1], area.h);
        // An auto side keeps the picture's shape against the other, or is
        // its own size, or the box's (§ 3.9).
        if (gw >= 0 && gh >= 0) {
            w = gw;
            h = gh;
        } else if (gw >= 0) {
            w = gw;
            h = ratio ? nearest(gw * ih, iw) : area.h;
        } else if (gh >= 0) {
            h = gh;
            w = ratio ? nearest(gh * iw, ih) : area.w;
        } else if (ratio) {
            w = iw;
            h = ih;
        }
    }
    if (w <= 0 || h <= 0)
        return false;
    w = min64(w, INT32_MAX);
    h = min64(h, INT32_MAX);
    out->at = (os64_gui_rect_t){place_axis(s->background_position[0], area.x, (int32_t)(area.w - w)),
                                place_axis(s->background_position[1], area.y, (int32_t)(area.h - h)),
                                (int32_t)w, (int32_t)h};
    out->repeat_x = s->background_repeat == FLOW_REPEAT || s->background_repeat == FLOW_REPEAT_X;
    out->repeat_y = s->background_repeat == FLOW_REPEAT || s->background_repeat == FLOW_REPEAT_Y;
    return true;
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
    case FLOW_BORDER_RIDGE:
        sunk = i >= (width + 1) / 2;
        break;
    default:
        return c;
    }
    return top_left == sunk ? yonder_darker(c) : yonder_lighter(c);
}

// a / b rounded down, for an a that may be negative (b > 0).
static int64_t floor_div(int64_t a, int64_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

// One row (along x) or column of a side, `depth` rows in from its outer
// edge of a side `width` deep, starting at (x, y) and `len` long. A DOUBLE
// side is two lines a third of its width each, the middle third left; a
// DOTTED one is squares a width across with a width between, and a DASHED
// one dashes three widths long with as much between — both counted from
// `origin`, the box's edge along the side, so every row's pattern lines up
// with the next. Only what meets the view is walked, however long the side.
static void stroke(const Painter *p, flow_border_style_t style, int64_t x, int64_t y, int64_t len,
                   bool along_x, int64_t depth, int64_t width, int64_t origin, uint32_t colour)
{
    if (style == FLOW_BORDER_DOUBLE && width >= 3) {
        int64_t band = (width + 1) / 3;
        if (depth >= band && depth < width - band)
            return;
    }
    if (style != FLOW_BORDER_DOTTED && style != FLOW_BORDER_DASHED) {
        fill(p, x, y, along_x ? len : 1, along_x ? 1 : len, colour);
        return;
    }
    int64_t on = max64(1, style == FLOW_BORDER_DOTTED ? width : 3 * width), period = 2 * on;
    int64_t at = along_x ? x : y;
    int64_t v0 = along_x ? p->view.x : p->view.y;
    int64_t a = max64(at, v0), b = min64(at + len, v0 + (along_x ? p->view.w : p->view.h));
    for (int64_t s = origin + floor_div(a - origin, period) * period; s < b; s += period) {
        int64_t s0 = max64(s, a), s1 = min64(s + on, b);
        if (s1 <= s0)
            continue;
        if (along_x)
            fill(p, s0, y, s1 - s0, 1, colour);
        else
            fill(p, x, s0, 1, s1 - s0, colour);
    }
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
        stroke(p, s->border_style[FLOW_TOP], x0, ry + i, x1 - x0, true, i, t, rx,
               tone(s->border_style[FLOW_TOP], s->border_color[FLOW_TOP], FLOW_TOP, (int32_t)i,
                    (int32_t)t));
    }
    for (int64_t i = max64(0, ry + rh - vy1), end = min64(min64(bt, rh), ry + rh - vy0); i < end;
         i++) {
        int64_t x0 = rx + (i * l + bt - 1) / bt, x1 = rx + rw - (i * rt + bt - 1) / bt;
        stroke(p, s->border_style[FLOW_BOTTOM], x0, ry + rh - 1 - i, x1 - x0, true, i, bt, rx,
               tone(s->border_style[FLOW_BOTTOM], s->border_color[FLOW_BOTTOM], FLOW_BOTTOM,
                    (int32_t)i, (int32_t)bt));
    }
    for (int64_t j = max64(0, vx0 - rx), end = min64(min64(l, rw), vx1 - rx); j < end; j++) {
        // How many top rows reach column j: min(t, floor(j * t / l) + 1).
        int64_t y0 = ry + min64(t, j * t / l + 1), y1 = ry + rh - min64(bt, j * bt / l + 1);
        stroke(p, s->border_style[FLOW_LEFT], rx + j, y0, y1 - y0, false, j, l, ry,
               tone(s->border_style[FLOW_LEFT], s->border_color[FLOW_LEFT], FLOW_LEFT, (int32_t)j,
                    (int32_t)l));
    }
    for (int64_t j = max64(0, rx + rw - vx1), end = min64(min64(rt, rw), rx + rw - vx0); j < end;
         j++) {
        int64_t y0 = ry + min64(t, j * t / rt + 1), y1 = ry + rh - min64(bt, j * bt / rt + 1);
        stroke(p, s->border_style[FLOW_RIGHT], rx + rw - 1 - j, y0, y1 - y0, false, j, rt, ry,
               tone(s->border_style[FLOW_RIGHT], s->border_color[FLOW_RIGHT], FLOW_RIGHT,
                    (int32_t)j, (int32_t)rt));
    }
}

// ── Rounded corners (Backgrounds 3 § 5) ─────────────────────────────────
//
// A box with any radius is drawn row by row in 1/256 px: each row's span
// through the corner ellipses, taken at the row's middle, its whole pixels
// filled in runs and the pixel at each end laid over at the share of it
// the span covers — so a curve is smooth across, which is where the eye
// sees steps. A square box's background and borders never come here; its
// shadows use the same shapes, with no radius.

#define SUB 256

static int64_t isqrt64(int64_t v);

typedef struct {
    int64_t l, t, r, b;         // the edges, in 1/256 px
    int64_t rad[4][2];          // TL, TR, BR, BL; horizontal, vertical; 1/256 px
} Round;

// How far in from its side a corner's ellipse is, `dy` from where it
// starts curving: rx * (1 - sqrt(1 - (dy/ry)^2)), worked with dy/ry as a
// fraction of FRAC, so that no product passes 64 bits whatever size a box
// has (a radius in 1/256 px reaches 2^40; squaring it would not fit).
#define FRAC ((int64_t)65536)

static int64_t corner_in(int64_t rx, int64_t ry, int64_t dy)
{
    if (rx <= 0 || ry <= 0 || dy <= 0)
        return 0;
    if (dy >= ry)
        return rx;
    int64_t t = dy * FRAC / ry;                         // < FRAC
    return rx - rx * isqrt64(FRAC * FRAC - t * t) / FRAC;
}

// Row `yc` (its middle, 1/256 px) of the shape: [*x0, *x1); false when it
// misses the row.
static bool round_row(const Round *s, int64_t yc, int64_t *x0, int64_t *x1)
{
    if (yc < s->t || yc >= s->b)
        return false;
    *x0 = s->l + max64(corner_in(s->rad[0][0], s->rad[0][1], s->t + s->rad[0][1] - yc),
                       corner_in(s->rad[3][0], s->rad[3][1], yc - (s->b - s->rad[3][1])));
    *x1 = s->r - max64(corner_in(s->rad[1][0], s->rad[1][1], s->t + s->rad[1][1] - yc),
                       corner_in(s->rad[2][0], s->rad[2][1], yc - (s->b - s->rad[2][1])));
    return *x1 > *x0;
}

// How much of pixel column x the span [a, b) covers, of SUB.
static int64_t cover(int64_t x, int64_t a, int64_t b)
{
    return max64(0, min64((x + 1) * SUB, b) - max64(x * SUB, a));
}

// `colour` laid on at `share` of SUB of its own alpha.
static uint32_t at_share(uint32_t colour, int64_t share)
{
    uint32_t alpha = (uint32_t)(flow_alpha(colour) * share / SUB);
    return (colour & 0xffffffu) | (255u - alpha) << 24;
}

// A shape `in` px inside `outer` on each side: each radius its outer one
// less the inset beside it (§ 5.2).
static Round round_inset_by(const Round *outer, const int64_t in[4])
{
    Round s = {outer->l + in[FLOW_LEFT] * SUB, outer->t + in[FLOW_TOP] * SUB,
               outer->r - in[FLOW_RIGHT] * SUB, outer->b - in[FLOW_BOTTOM] * SUB, {{0}}};
    // Each corner's horizontal side and vertical side.
    static const int kH[4] = {FLOW_LEFT, FLOW_RIGHT, FLOW_RIGHT, FLOW_LEFT};
    static const int kV[4] = {FLOW_TOP, FLOW_TOP, FLOW_BOTTOM, FLOW_BOTTOM};
    for (int c = 0; c < 4; c++) {
        s.rad[c][0] = max64(0, outer->rad[c][0] - in[kH[c]] * SUB);
        s.rad[c][1] = max64(0, outer->rad[c][1] - in[kV[c]] * SUB);
    }
    return s;
}

// The shape of `inner`, a box inside the border box `rect` whose shape is
// `outer`.
static Round round_inset(const Round *outer, os64_gui_rect_t rect, os64_gui_rect_t inner)
{
    int64_t in[4];
    in[FLOW_LEFT] = (int64_t)inner.x - rect.x;
    in[FLOW_TOP] = (int64_t)inner.y - rect.y;
    in[FLOW_RIGHT] = ((int64_t)rect.x + rect.w) - ((int64_t)inner.x + inner.w);
    in[FLOW_BOTTOM] = ((int64_t)rect.y + rect.h) - ((int64_t)inner.y + inner.h);
    return round_inset_by(outer, in);
}

// The border box's shape, and the padding box's inside it.
static void round_shapes(const flow_box_t *b, const int32_t radii[4][2], const int32_t w[4],
                         Round *outer, Round *inner)
{
    os64_gui_rect_t r = b->rect;
    *outer = (Round){(int64_t)r.x * SUB, (int64_t)r.y * SUB, ((int64_t)r.x + r.w) * SUB,
                     ((int64_t)r.y + r.h) * SUB, {{0}}};
    for (int c = 0; c < 4; c++) {
        outer->rad[c][0] = (int64_t)radii[c][0] * SUB;
        outer->rad[c][1] = (int64_t)radii[c][1] * SUB;
    }
    int64_t in[4];
    for (int k = 0; k < 4; k++)
        in[k] = w[k];
    *inner = round_inset_by(outer, in);
}

// The rows of the view the box reaches.
static void view_rows(const Painter *p, const flow_box_t *b, int64_t *y0, int64_t *y1)
{
    *y0 = max64(b->rect.y, p->view.y);
    *y1 = min64((int64_t)b->rect.y + b->rect.h, (int64_t)p->view.y + p->view.h);
}

static void round_background(const Painter *p, const flow_box_t *b, const Round *shape,
                             uint32_t colour)
{
    int64_t y0, y1;
    view_rows(p, b, &y0, &y1);
    for (int64_t y = y0; y < y1; y++) {
        int64_t a, e;
        if (!round_row(shape, y * SUB + SUB / 2, &a, &e))
            continue;
        int64_t first = a / SUB, last = (e - 1) / SUB;     // the pixels it touches
        int64_t whole0 = (a + SUB - 1) / SUB, whole1 = e / SUB;
        if (whole1 > whole0)
            fill(p, whole0, y, whole1 - whole0, 1, colour);
        if (first < whole0)
            fill(p, first, y, 1, 1, at_share(colour, cover(first, a, e)));
        if (last >= whole1 && last >= whole0)
            fill(p, last, y, 1, 1, at_share(colour, cover(last, a, e)));
    }
}

// Which side owns a pixel of the ring: of the two sides of the quarter it
// is in, the top or bottom one at or above the diagonal through its outer
// corner, sloped by the two widths — the square corners' rule.
static int ring_side(const flow_box_t *b, const int32_t w[4], int64_t x, int64_t y)
{
    os64_gui_rect_t r = b->rect;
    bool top = y * 2 < (int64_t)r.y * 2 + r.h, left = x * 2 < (int64_t)r.x * 2 + r.w;
    int v = top ? FLOW_TOP : FLOW_BOTTOM, h = left ? FLOW_LEFT : FLOW_RIGHT;
    if (w[h] == 0)
        return v;
    if (w[v] == 0)
        return h;
    int64_t dv = top ? y - r.y : (int64_t)r.y + r.h - 1 - y;
    int64_t dh = left ? x - r.x : (int64_t)r.x + r.w - 1 - x;
    return dv * w[h] <= dh * w[v] ? v : h;
}

// The ring between the border box's shape and the padding box's, each
// pixel in its side's colour at the share of it the ring covers. Every
// style is drawn solid here, in its side's tone (PILE3.md § Booked).
static void round_borders(const Painter *p, const flow_box_t *b, const Round *outer,
                          const Round *inner, const int32_t w[4])
{
    const flow_style_t *s = b->style;
    int64_t y0, y1;
    view_rows(p, b, &y0, &y1);
    for (int64_t y = y0; y < y1; y++) {
        int64_t oa, oe, ia = 0, ie = 0;
        int64_t yc = y * SUB + SUB / 2;
        if (!round_row(outer, yc, &oa, &oe))
            continue;
        if (!round_row(inner, yc, &ia, &ie))
            ia = ie = oa;
        int64_t x0 = max64(oa / SUB, p->view.x), x1 = min64((oe + SUB - 1) / SUB,
                                                            (int64_t)p->view.x + p->view.w);
        // The whole pixels inside the padding box's span hold nothing of
        // the ring, and are stepped over at once.
        int64_t hole0 = (ia + SUB - 1) / SUB, hole1 = ie / SUB;
        int64_t run = x0;
        uint32_t run_colour = 0;
        bool in_run = false;
        for (int64_t x = x0; x <= x1; x++) {
            if (x >= hole0 && x < hole1 && x < x1) {
                if (in_run)
                    fill(p, run, y, x - run, 1, run_colour);
                in_run = false;
                x = hole1 - 1;
                continue;
            }
            int64_t share = x < x1 ? cover(x, oa, oe) - cover(x, ia, ie) : 0;
            uint32_t colour = 0;
            if (share > 0) {
                int side = ring_side(b, w, x, y);
                colour = at_share(tone(s->border_style[side], s->border_color[side], side, 0,
                                       w[side]),
                                  share);
            }
            if (in_run && (share != SUB || colour != run_colour)) {
                fill(p, run, y, x - run, 1, run_colour);
                in_run = false;
            }
            if (share == SUB && !in_run) {
                run = x;
                run_colour = colour;
                in_run = true;
            } else if (share > 0 && share < SUB) {
                fill(p, x, y, 1, 1, colour);
            }
        }
    }
}

// ── Shadows (Backgrounds 3 § 7) ─────────────────────────────────────────
//
// A shadow is its shape — the border box moved by its offset, grown by its
// spread (the padding box's, shrunk, for an inset one) — as a mask of how
// much of each pixel it covers, blurred, and laid on in its colour only
// where it may fall: OUTSIDE the border box for an outer shadow, inside the
// padding box for an inset one. The blur is three box blurs, whose sum is
// near enough the Gaussian CSS asks for (standard deviation half the blur
// radius). Only the view's part, and the blur's reach around it, is made.

typedef struct {
    int64_t x, y, w, h;         // page pixels
    uint16_t *a;                // of SUB, w a row
} Mask;

// The shape's coverage of every pixel of the mask, or what it leaves.
static void mask_cover(Mask *m, const Round *s, bool leaves)
{
    for (int64_t r = 0; r < m->h; r++) {
        int64_t a = 0, e = 0;
        bool row = round_row(s, (m->y + r) * SUB + SUB / 2, &a, &e);
        uint16_t *out = m->a + r * m->w;
        for (int64_t c = 0; c < m->w; c++) {
            int64_t cov = row ? cover(m->x + c, a, e) : 0;
            out[c] = (uint16_t)(leaves ? SUB - cov : cov);
        }
    }
}

// Each pixel times the shape's coverage of it, or what the shape leaves.
static void mask_times(Mask *m, const Round *s, bool leaves)
{
    for (int64_t r = 0; r < m->h; r++) {
        int64_t a = 0, e = 0;
        bool row = round_row(s, (m->y + r) * SUB + SUB / 2, &a, &e);
        uint16_t *out = m->a + r * m->w;
        for (int64_t c = 0; c < m->w; c++) {
            int64_t cov = row ? cover(m->x + c, a, e) : 0;
            out[c] = (uint16_t)(out[c] * (leaves ? SUB - cov : cov) / SUB);
        }
    }
}

// One box blur of radius `r` along rows (or columns), past the edges
// reading `edge`.
static void blur_pass(Mask *m, uint16_t *tmp, int64_t r, bool rows, uint16_t edge)
{
    int64_t lines = rows ? m->h : m->w, len = rows ? m->w : m->h;
    int64_t step = rows ? 1 : m->w, span = 2 * r + 1;
    for (int64_t l = 0; l < lines; l++) {
        uint16_t *v = m->a + (rows ? l * m->w : l);
        int64_t sum = 0;
        for (int64_t i = -r; i <= r; i++)
            sum += i < 0 || i >= len ? edge : v[i * step];
        for (int64_t i = 0; i < len; i++) {
            tmp[i] = (uint16_t)((sum + span / 2) / span);
            int64_t out = i - r, in = i + r + 1;
            sum += (in >= len ? edge : v[in * step]) - (out < 0 ? edge : v[out * step]);
        }
        for (int64_t i = 0; i < len; i++)
            v[i * step] = tmp[i];
    }
}

static bool mask_blur(Mask *m, int64_t blur, uint16_t edge)
{
    int64_t r = blur / 2;
    if (r <= 0)
        return true;
    uint16_t *tmp = os64_malloc((size_t)max64(m->w, m->h) * sizeof(*tmp));
    if (tmp == NULL)
        return false;
    for (int k = 0; k < 3; k++) {
        blur_pass(m, tmp, r, true, edge);
        blur_pass(m, tmp, r, false, edge);
    }
    os64_free(tmp);
    return true;
}

// The view's part of the mask, in `colour` at its alpha times each pixel's.
static void mask_emit(const Painter *p, const Mask *m, uint32_t colour)
{
    int64_t x0 = max64(m->x, p->view.x), y0 = max64(m->y, p->view.y);
    int64_t x1 = min64(m->x + m->w, (int64_t)p->view.x + p->view.w);
    int64_t y1 = min64(m->y + m->h, (int64_t)p->view.y + p->view.h);
    if (x1 <= x0 || y1 <= y0 || p->v->mask == NULL)
        return;
    uint8_t *alpha = os64_malloc((size_t)((x1 - x0) * (y1 - y0)));
    if (alpha == NULL)
        return;
    uint32_t a = flow_alpha(colour);
    for (int64_t y = y0; y < y1; y++)
        for (int64_t x = x0; x < x1; x++)
            alpha[(y - y0) * (x1 - x0) + (x - x0)] =
                (uint8_t)(m->a[(y - m->y) * m->w + (x - m->x)] * a / SUB);
    p->v->mask(p->v->ctx,
               (os64_gui_rect_t){(int32_t)(x0 + p->off.x), (int32_t)(y0 + p->off.y),
                                 (int32_t)(x1 - x0), (int32_t)(y1 - y0)},
               alpha, colour & 0xffffffu);
    os64_free(alpha);
}

// A shape moved by (dx, dy) and grown by `by` on every side, its radii
// with it (a radius grown from 0 stays 0: a square corner stays square).
static Round round_moved(const Round *s, int64_t dx, int64_t dy, int64_t by)
{
    Round r = {s->l + dx - by, s->t + dy - by, s->r + dx + by, s->b + dy + by, {{0}}};
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 2; k++)
            r.rad[c][k] = s->rad[c][k] > 0 ? max64(0, s->rad[c][k] + by) : 0;
    return r;
}

static void shadow(const Painter *p, const flow_box_t *b, const Round *outer, const Round *inner,
                   const flow_shadow_t *sh, bool square)
{
    // How far a pixel's shade reads: three box blurs of radius blur/2 each
    // (mask_blur), and a pixel. A mask cut any closer to the view would
    // shade a pixel by where the dirty part happened to end (Quinn, #203).
    int64_t blur = sh->blur, reach = 3 * (blur / 2) + 1;
    Round shape = sh->inset ? round_moved(inner, (int64_t)sh->x * SUB, (int64_t)sh->y * SUB,
                                          -(int64_t)sh->spread * SUB)
                            : round_moved(outer, (int64_t)sh->x * SUB, (int64_t)sh->y * SUB,
                                          (int64_t)sh->spread * SUB);
    // An outer square shadow with no blur is rectangles: the shape less the
    // border box, which is how pixel art draws hundreds of them cheaply.
    if (!sh->inset && blur == 0 && square) {
        int64_t l = shape.l / SUB, t = shape.t / SUB, r = shape.r / SUB, e = shape.b / SUB;
        os64_gui_rect_t bx = b->rect;
        int64_t bl = bx.x, bt = bx.y, br = (int64_t)bx.x + bx.w, bb = (int64_t)bx.y + bx.h;
        if (r <= l || e <= t)
            return;
        fill(p, l, t, r - l, max64(0, min64(e, bt) - t), sh->colour);                // above
        fill(p, l, max64(t, bb), r - l, e - max64(t, bb), sh->colour);               // below
        int64_t mt = max64(t, bt), mb = min64(e, bb);
        if (mb > mt) {
            fill(p, l, mt, max64(0, min64(r, bl) - l), mb - mt, sh->colour);         // left
            fill(p, max64(l, br), mt, r - max64(l, br), mb - mt, sh->colour);        // right
        }
        return;
    }
    // Where it can fall: around its shape by the blur's reach, outside the
    // border box (or inside the padding box), and in the view or the blur's
    // reach of it.
    const Round *where = sh->inset ? inner : &shape;
    int64_t x0 = where->l / SUB - reach, y0 = where->t / SUB - reach;
    int64_t x1 = (where->r + SUB - 1) / SUB + reach, y1 = (where->b + SUB - 1) / SUB + reach;
    x0 = max64(x0, (int64_t)p->view.x - reach);
    y0 = max64(y0, (int64_t)p->view.y - reach);
    x1 = min64(x1, (int64_t)p->view.x + p->view.w + reach);
    y1 = min64(y1, (int64_t)p->view.y + p->view.h + reach);
    if (x1 <= x0 || y1 <= y0)
        return;
    Mask m = {x0, y0, x1 - x0, y1 - y0, NULL};
    m.a = os64_malloc((size_t)(m.w * m.h) * sizeof(*m.a));
    if (m.a == NULL)
        return;
    // An inset shadow is what its shape leaves, inside the padding box.
    mask_cover(&m, &shape, sh->inset);
    if (mask_blur(&m, blur, sh->inset ? SUB : 0)) {
        mask_times(&m, sh->inset ? inner : outer, !sh->inset);
        mask_emit(p, &m, sh->colour);
    }
    os64_free(m.a);
}

// ── Gradients (Images 3 § 3) ────────────────────────────────────────────
//
// A gradient is a picture the size of the box's padding box — its
// positioning area — drawn across its border box, repeated as
// `background-repeat` says, and cut to its corners' curves. Each pixel of
// it that the view shows is worked out on its own: where its middle falls
// along the gradient's line (or ray), and the colour the stops give there,
// mixed in premultiplied sRGB as CSS mixes colours with alpha.

#define GRADIENT_STOPS_MAX 128

typedef struct {
    double at[GRADIENT_STOPS_MAX];      // along the line, 0 its start, 1 its end
    uint32_t colour[GRADIENT_STOPS_MAX];
    bool hint[GRADIENT_STOPS_MAX];
    int32_t n;
} Stops;

// A stop's position on a line `len` px long, or NAN for none written.
static double stop_at(flow_length_t l, double len)
{
    if (l.kind == FLOW_LENGTH_PERCENT)
        return l.value / 6400.0 + (len > 0 ? l.offset / 64.0 / len : 0);
    if (l.kind == FLOW_LENGTH_PX)
        return len > 0 ? l.value / 64.0 / len : 0;
    return NAN;
}

// A length-percentage in px against `base`: what the box's own sizes make
// of a radial gradient's centre and radii.
static double lp_px(flow_length_t l, double base)
{
    if (l.kind == FLOW_LENGTH_PERCENT)
        return l.value / 6400.0 * base + l.offset / 64.0;
    return l.kind == FLOW_LENGTH_PX ? l.value / 64.0 : 0;
}

// § 3.5.3's fix-up: the first stop at 0 and the last at 1 when left out,
// each no earlier than the one before it, and a run left out spread evenly
// between the stops either side of it.
static bool stops_resolve(const flow_gradient_t *g, double len, Stops *out)
{
    out->n = g->nstops < GRADIENT_STOPS_MAX ? g->nstops : GRADIENT_STOPS_MAX;
    if (out->n < 2)
        return false;
    for (int32_t i = 0; i < out->n; i++) {
        out->at[i] = stop_at(g->stops[i].at, len);
        out->colour[i] = g->stops[i].colour;
        out->hint[i] = g->stops[i].hint;
    }
    if (isnan(out->at[0]))
        out->at[0] = 0;
    if (isnan(out->at[out->n - 1]))
        out->at[out->n - 1] = 1;
    double most = out->at[0];
    for (int32_t i = 1; i < out->n; i++) {
        if (!isnan(out->at[i]) && out->at[i] < most)
            out->at[i] = most;
        if (!isnan(out->at[i]))
            most = out->at[i];
    }
    for (int32_t i = 1; i < out->n; i++) {
        if (!isnan(out->at[i]))
            continue;
        int32_t j = i;
        while (isnan(out->at[j]))
            j++;
        for (int32_t k = i; k < j; k++)
            out->at[k] = out->at[i - 1] + (out->at[j] - out->at[i - 1]) * (k - i + 1) / (j - i + 1);
        i = j;
    }
    return true;
}

// Colour `a` toward `b` by `t` (0..1), premultiplied, as a flow colour.
static uint32_t mix(uint32_t a, uint32_t b, double t)
{
    double aa = flow_alpha(a) / 255.0, ba = flow_alpha(b) / 255.0;
    double alpha = aa + (ba - aa) * t;
    uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        double ca = ((a >> shift) & 0xff) * aa, cb = ((b >> shift) & 0xff) * ba;
        double c = alpha > 0 ? (ca + (cb - ca) * t) / alpha : 0;
        out |= (uint32_t)(c + 0.5 > 255 ? 255 : c + 0.5) << shift;
    }
    uint32_t al = (uint32_t)(alpha * 255 + 0.5);
    return out | (255u - (al > 255 ? 255 : al)) << 24;
}

// The colour at `t` along the line: the first stop's before it, the last's
// after, and between two stops their mix — bent by a hint between them so
// that the two meet half and half at the hint.
static uint32_t stops_colour(const Stops *s, double t, bool repeating)
{
    double first = s->at[0], last = s->at[s->n - 1];
    if (repeating && last > first) {
        double span = last - first;
        t = first + (t - first - span * floor((t - first) / span));
    }
    if (t <= first)
        return s->colour[0];
    for (int32_t i = 1; i < s->n; i++) {
        if (s->hint[i] || t > s->at[i])
            continue;
        int32_t p = i - 1;
        bool hinted = p > 0 && s->hint[p];
        double h = hinted ? s->at[p] : 0;
        if (hinted)
            p--;
        double a = s->at[p], b = s->at[i];
        double f = b > a ? (t - a) / (b - a) : 1;
        if (hinted && b > a) {
            double hf = (h - a) / (b - a);
            f = hf <= 0 ? 1 : hf >= 1 ? 0 : pow(f, log(0.5) / log(hf));
        }
        return mix(s->colour[p], s->colour[i], f);
    }
    return s->colour[s->n - 1];
}

// `x` taken into [0, size), as a repeating tile is.
static double wrap(double x, double size)
{
    return x - size * floor(x / size);
}

// Gradient `g` in `tile`, drawn over `area` (and the view), cut to `shape`
// when `round`.
static void gradient(const Painter *p, const flow_gradient_t *g, const yonder_tile_t *tile,
                     os64_gui_rect_t area, const Round *shape, bool round)
{
    double tx = tile->at.x, ty = tile->at.y, w = tile->at.w, h = tile->at.h;
    if (w <= 0 || h <= 0)
        return;
    int64_t x0 = max64(area.x, p->view.x), y0 = max64(area.y, p->view.y);
    int64_t x1 = min64((int64_t)area.x + area.w, (int64_t)p->view.x + p->view.w);
    int64_t y1 = min64((int64_t)area.y + area.h, (int64_t)p->view.y + p->view.h);
    if (x1 <= x0 || y1 <= y0 || p->v->pixels == NULL)
        return;
    // The line (linear) or the ray (radial) and its length in px.
    double dx = 0, dy = 0, len, cx = w / 2, cy = h / 2, rx = 1, ry = 1;
    if (g->kind == FLOW_GRADIENT_LINEAR) {
        if (g->to_x != 0 || g->to_y != 0) {
            // Toward a corner, the line is square to the diagonal between
            // its two neighbours (§ 3.1.1).
            dx = g->to_x * h;
            dy = g->to_y * w;
            double n = sqrt(dx * dx + dy * dy);
            dx /= n;
            dy /= n;
        } else {
            double a = g->angle / 1000.0 * 3.14159265358979323846 / 180;
            dx = sin(a);
            dy = -cos(a);
        }
        len = fabs(w * dx) + fabs(h * dy);
    } else {
        cx = lp_px(g->centre[0], w);
        cy = lp_px(g->centre[1], h);
        double sx_near = fmin(fabs(cx), fabs(w - cx)), sx_far = fmax(fabs(cx), fabs(w - cx));
        double sy_near = fmin(fabs(cy), fabs(h - cy)), sy_far = fmax(fabs(cy), fabs(h - cy));
        switch (g->extent) {
        case FLOW_EXTENT_CLOSEST_SIDE: rx = sx_near; ry = sy_near; break;
        case FLOW_EXTENT_FARTHEST_SIDE: rx = sx_far; ry = sy_far; break;
        case FLOW_EXTENT_CLOSEST_CORNER: rx = sx_near * sqrt(2); ry = sy_near * sqrt(2); break;
        case FLOW_EXTENT_FARTHEST_CORNER: rx = sx_far * sqrt(2); ry = sy_far * sqrt(2); break;
        case FLOW_EXTENT_SIZE:
            rx = lp_px(g->radii[0], w);
            ry = lp_px(g->radii[1], h);
            break;
        }
        if (g->circle) {
            // A circle's sides are the nearer (or farther) of the two; its
            // corners the distance to that corner.
            double r = g->extent == FLOW_EXTENT_CLOSEST_SIDE ? fmin(sx_near, sy_near)
                     : g->extent == FLOW_EXTENT_FARTHEST_SIDE ? fmax(sx_far, sy_far)
                     : g->extent == FLOW_EXTENT_CLOSEST_CORNER ? hypot(sx_near, sy_near)
                     : g->extent == FLOW_EXTENT_FARTHEST_CORNER ? hypot(sx_far, sy_far) : rx;
            rx = ry = r;
        }
        len = rx;
    }
    Stops stops;
    if (!(len > 0) || !(rx > 0) || !(ry > 0) || !stops_resolve(g, len, &stops))
        return;
    bool rep_x = tile->repeat_x, rep_y = tile->repeat_y;
    uint32_t *out = os64_malloc((size_t)((x1 - x0) * (y1 - y0)) * sizeof(*out));
    if (out == NULL)
        return;
    for (int64_t y = y0; y < y1; y++) {
        int64_t a = 0, e = 0;
        bool row = !round || round_row(shape, y * SUB + SUB / 2, &a, &e);
        for (int64_t x = x0; x < x1; x++) {
            uint32_t *px_out = &out[(y - y0) * (x1 - x0) + (x - x0)];
            double u = x + 0.5 - tx, v = y + 0.5 - ty;
            if ((!rep_x && (u < 0 || u >= w)) || (!rep_y && (v < 0 || v >= h)) || !row) {
                *px_out = 0;
                continue;
            }
            u = wrap(u, w);
            v = wrap(v, h);
            double t = g->kind == FLOW_GRADIENT_LINEAR
                           ? ((u - cx) * dx + (v - cy) * dy) / len + 0.5
                           : hypot((u - cx) / rx, (v - cy) / ry);
            uint32_t c = stops_colour(&stops, t, g->repeating);
            uint32_t alpha = flow_alpha(c);
            if (round)
                alpha = (uint32_t)(alpha * cover(x, a, e) / SUB);
            *px_out = alpha << 24 | (c & 0xffffffu);
        }
    }
    p->v->pixels(p->v->ctx,
                 (os64_gui_rect_t){(int32_t)(x0 + p->off.x), (int32_t)(y0 + p->off.y),
                                   (int32_t)(x1 - x0), (int32_t)(y1 - y0)},
                 out);
    os64_free(out);
}

static bool rounded(const int32_t radii[4][2])
{
    for (int c = 0; c < 4; c++)
        if (radii[c][0] > 0 && radii[c][1] > 0)
            return true;
    return false;
}

// A box's background and borders: every block-level box, and an atom. Its
// outer shadows go under its colour, the last first so the first is on
// top; its picture over its colour, sized and placed in its origin box and
// cut, with the colour, to its clip box; its inset shadows over those and
// under its borders (Backgrounds 3 § 7.1). Rounded, its colour, gradient,
// shadows and borders follow the curves.
static void frame(const Painter *p, const flow_box_t *b)
{
    const flow_style_t *s = b->style;
    int32_t radii[4][2];
    flow_box_radii(b, radii);
    bool round = rounded(radii);
    int32_t w[4];
    Round outer, inner;
    if (round || s->nbox_shadows > 0) {
        for (int k = 0; k < 4; k++)
            w[k] = px(s->border_width[k]);
        round_shapes(b, round ? radii : (const int32_t[4][2]){{0}}, w, &outer, &inner);
    }
    for (int32_t i = s->nbox_shadows - 1; i >= 0; i--)
        if (!s->box_shadows[i].inset)
            shadow(p, b, &outer, &inner, &s->box_shadows[i], !round);
    // The colour and the picture are cut to the clip box, rounded as the
    // box is; the picture is placed in the origin box. A clip to the text
    // is not drawn (PILE3.md § Booked).
    if (b != p->canvas_owner && s->background_clip != FLOW_EDGE_TEXT) {
        os64_gui_rect_t cut = yonder_box_edge(b, b->rect, s->background_clip);
        Round shape = {0};
        if (round)
            shape = round_inset(&outer, b->rect, cut);
        if (s->has_background && round)
            round_background(p, b, &shape, s->background);
        else if (s->has_background)
            fill(p, cut.x, cut.y, cut.w, cut.h, s->background);
        yonder_tile_t tile;
        if (s->background_gradient != NULL) {
            if (yonder_background_tile(s, yonder_box_edge(b, b->rect, s->background_origin), 0, 0,
                                       &tile))
                gradient(p, s->background_gradient, &tile, cut, &shape, round);
        } else {
            os64_gui_rect_t area = on_page(p, cut);
            (void)p->v->backdrop(p->v->ctx, b, &area, area.x, area.y, on_page(p, p->view));
        }
    }
    for (int32_t i = s->nbox_shadows - 1; i >= 0; i--)
        if (s->box_shadows[i].inset)
            shadow(p, b, &outer, &inner, &s->box_shadows[i], !round);
    if (round)
        round_borders(p, b, &outer, &inner, w);
    else
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
        // Its shadows under it, the last first: the run again in each
        // shadow's colour at its offset — sharp, whatever its blur
        // (PILE3.md § Booked).
        for (int32_t i = b->style->ntext_shadows - 1; b->run != NULL && i >= 0; i--) {
            const flow_shadow_t *sh = &b->style->text_shadows[i];
            p->v->text(p->v->ctx, b, (int32_t)((int64_t)b->rect.x + p->off.x + sh->x),
                       (int32_t)((int64_t)b->baseline + p->off.y + sh->y), on_page(p, p->view),
                       sh->colour);
        }
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
        // An inline-block (or inline-flex, inline-grid) is two boxes of one
        // element: the atom on its line, which paints the element's frame
        // where the line is painted, and its content box inside it, which
        // must not paint it again — a translucent background or border laid
        // twice is twice as opaque (Quinn, #202).
        if (b->parent != NULL && b->parent->kind == FLOW_BOX_ATOMIC && b->parent->node == b->node)
            return;
        frame(p, b);
        return;
    }
}

// A group (flow_visit_groups) opens: its bounds, cut to what is painted,
// are the verb's to remember what was under them; closes: everything it
// painted is laid over that at the group's opacity, in 255ths.
static void group_open(void *ctx, const flow_box_t *box, os64_gui_rect_t bounds)
{
    (void)box;
    const Painter *p = ctx;
    int64_t x0 = max64(bounds.x, p->view.x), y0 = max64(bounds.y, p->view.y);
    int64_t x1 = min64((int64_t)bounds.x + bounds.w, (int64_t)p->view.x + p->view.w);
    int64_t y1 = min64((int64_t)bounds.y + bounds.h, (int64_t)p->view.y + p->view.h);
    p->v->group_open(p->v->ctx, (os64_gui_rect_t){(int32_t)x0, (int32_t)y0,
                                                  (int32_t)max64(0, x1 - x0),
                                                  (int32_t)max64(0, y1 - y0)});
}

static void group_close(void *ctx, const flow_box_t *box)
{
    const Painter *p = ctx;
    p->v->group_close(p->v->ctx, (uint8_t)(((uint32_t)box->style->opacity * 255 + 500) / 1000));
}

// Whether a box has any background: a colour, a gradient, or a picture
// behind it.
static bool has_background(const Painter *p, const flow_box_t *b)
{
    return b->style->has_background || b->style->background_gradient != NULL ||
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

const flow_box_t *yonder_canvas_owner(const flow_tree_t *tree, const yonder_verbs_t *verbs)
{
    Painter p = {.v = verbs};
    const flow_box_t *root = flow_root(tree);
    return root != NULL ? canvas_owner(&p, root) : NULL;
}

void yonder_paint(const flow_tree_t *tree, os64_gui_rect_t viewport, flow_point_t scroll,
                  uint32_t paper, const yonder_verbs_t *verbs)
{
    Painter p = {.v = verbs, .view = viewport, .scroll = scroll};
    const flow_box_t *root = flow_root(tree);
    if (root != NULL)
        p.canvas_owner = canvas_owner(&p, root);
    const flow_box_t *owner = p.canvas_owner;
    // A translucent canvas colour is laid over the paper: under the page
    // there is nothing else.
    uint32_t canvas = owner != NULL && owner->style->has_background ? owner->style->background
                                                                    : paper;
    if (flow_alpha(canvas) != 255)
        fill(&p, viewport.x, viewport.y, viewport.w, viewport.h, paper);
    fill(&p, viewport.x, viewport.y, viewport.w, viewport.h, canvas);
    // The canvas's picture is placed as its owner's box places it (an
    // attribute's from the page's own corner), so it scrolls with the
    // page; its gradient's origin box is the root's, so a page shorter
    // than the view repeats it, as the browsers do.
    yonder_tile_t tile;
    if (owner != NULL && owner->style->background_gradient != NULL) {
        if (yonder_background_tile(owner->style, root->rect, 0, 0, &tile))
            gradient(&p, owner->style->background_gradient, &tile, viewport, NULL, false);
    } else if (owner != NULL)
        (void)verbs->backdrop(verbs->ctx, owner, &viewport, 0, 0, viewport);
    if (root != NULL) {
        bool groups = verbs->group_open != NULL && verbs->group_close != NULL;
        flow_visitor_t v = {paint_box, groups ? group_open : NULL, groups ? group_close : NULL,
                            &p};
        flow_visit_groups(tree, viewport, scroll, &v);
    }
}
