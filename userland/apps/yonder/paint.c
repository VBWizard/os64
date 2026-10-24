// paint.c — a laid-out page drawn through the four verbs (paint.h).
//
// libflow's walk hands the boxes over in CSS 2.1 Appendix E's order
// (backgrounds and borders of the block-level boxes, then the inline
// content), so painting is one decision per box: what that kind of box
// looks like. Nothing here knows about a surface.

#include "paint.h"
#include "paint_cache.h"
#include "html/html.h"
#include "os64/mem.h"
#include "os64/str.h"
#include <math.h>

// A painter works in the coordinates of the box it paints — a fixed box's
// are the viewport's — and `off` is what takes them to the page's, added
// wherever a rectangle is handed to a verb. `view` is in the box's.
// The groups open while a page is painted (group_open): what to put back
// at the close, and what the open asked of the verbs.
#define PAINT_GROUPS 64
typedef enum { G_ASKED, G_OPACITY, G_THROUGH } PaintGroupKind;
typedef struct {
    struct {
        os64_gui_rect_t view;
        flow_point_t shift;
        PaintGroupKind kind;
    } open[PAINT_GROUPS];
    int32_t depth;                  // may pass PAINT_GROUPS; those are not kept
    // Past the deepest kept, a group drawn through its matrix is not painted
    // at all: the depth it opened at, plus one, or 0.
    int32_t hidden;
} PaintGroups;

typedef struct {
    const yonder_verbs_t *v;
    // `view` is what is painted, and `shift` what to add to a box's
    // flow_box_doc_offset, both in the coordinates being painted: inside a
    // group laid through its matrix, the group's own; inside one drawn
    // upright instead, the outside's, `shift` taking the box there.
    os64_gui_rect_t view;
    flow_point_t scroll, off, shift;
    PaintGroups *groups;
    // The box whose background became the canvas's: it does not paint it
    // a second time over its own border box (CSS 2.1 §14.2).
    const flow_box_t *canvas_owner;
    // Dark pages (paint.h), NULL for none.
    const yonder_dark_t *dark;
    // The paint cache (paint.h), NULL for none.
    yonder_paint_cache_t *cache;
    // Where the working buffers come from (scratch.h), NULL for the heap.
    yonder_scratch_t *scratch;
    // A TEXT CLIP's text (PILE3.md § The text clip): when `penned`, a text
    // box draws its run and its decorations in `pen` — no shadows, no
    // relighting — and every other box draws nothing.
    bool penned;
    uint32_t pen;
    // The paint's cuts of the lines that end in an ellipsis
    // (flow_cut_memo_t), each line worked once a paint; NULL draws no
    // line as cut.
    flow_cut_memo_t *cuts;
} Painter;

// A coordinate and a move added, held to what a 32-bit coordinate can say,
// so a far shift never wraps — and one that fits is left where it is.
static int32_t sum32(int64_t a, int64_t b)
{
    int64_t v = a + b;
    return (int32_t)(v < INT32_MIN ? INT32_MIN : v > INT32_MAX ? INT32_MAX : v);
}

static double hue_part(double p, double q, double t)
{
    if (t < 0)
        t += 1;
    if (t > 1)
        t -= 1;
    return t < 1.0 / 6 ? p + (q - p) * 6 * t : t < 0.5 ? q : t < 2.0 / 3 ? p + (q - p) * (2.0 / 3 - t) * 6 : p;
}

// How bright a colour looks, 0 to 1: Rec. 709's weights over each channel
// squared, a near enough linearisation for the one decision it makes and
// cheap enough for every pixel of a gradient.
static double luminance(double r, double g, double b)
{
    return 0.2126 * r * r + 0.7152 * g * g + 0.0722 * b * b;
}

// The colour at HSL lightness `l` with hue `h` and saturation `s`.
static void from_hsl(double h, double s, double l, double *r, double *g, double *b)
{
    if (s == 0) {
        *r = *g = *b = l;
        return;
    }
    double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    *r = hue_part(p, q, h + 1.0 / 3);
    *g = hue_part(p, q, h);
    *b = hue_part(p, q, h - 1.0 / 3);
}

// Where a dark page splits light from dark: a colour that looks brighter
// than this is light (mid grey, #6c6c6c, is about here).
#define DARK_SPLIT 0.18

// `c` as a dark page draws it, hue, saturation and transparency kept, its
// LUMINANCE moved: PAPER brighter than the split goes dark, to between 0.4%
// (white) and 7% (the split), and INK darker than the split goes light, to
// between 80% (black) and 62% (the split); each other colour is as it was.
// Luminance and not HSL lightness decides, because yellow and green are
// "half light" by lightness and look nearly white (Quinn, #238). Any moved
// ink on any moved paper is at least 5.3 to 1 in WCAG's contrast (searched
// over a 4,096-colour grid), about 14.5 to 1 for black on white. A
// saturated dark blue keeps only a tint: blue is so dark to the eye that
// the luminance an ink needs is near white. The order within each side is
// kept, so a paler box stays paler than a darker one.
static uint32_t relight(uint32_t c, bool paper)
{
    double r = ((c >> 16) & 0xff) / 255.0, g = ((c >> 8) & 0xff) / 255.0, b = (c & 0xff) / 255.0;
    double y = luminance(r, g, b);
    if (paper ? y <= DARK_SPLIT : y >= DARK_SPLIT)
        return c;
    double want = paper ? 0.004 + (1 - y) / (1 - DARK_SPLIT) * (0.07 - 0.004)
                        : 0.80 - y / DARK_SPLIT * (0.80 - 0.62);
    double mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    double mn = r < g ? (r < b ? r : b) : (g < b ? g : b), l = (mx + mn) / 2;
    double d = mx - mn, s = 0, h = 0;
    if (d > 0) {
        s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
        h = mx == r ? (g - b) / d + (g < b ? 6 : 0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
        h /= 6;
    }
    // Luminance rises with lightness at a fixed hue and saturation, so the
    // lightness that gives `want` is found by halving.
    double lo = 0, hi = 1;
    for (int i = 0; i < 24; i++) {
        double mid = (lo + hi) / 2;
        from_hsl(h, s, mid, &r, &g, &b);
        if (luminance(r, g, b) < want)
            lo = mid;
        else
            hi = mid;
    }
    from_hsl(h, s, (lo + hi) / 2, &r, &g, &b);
    // Each part is from 0 to 1, so adding a half rounds it.
    return (c & 0xff000000u) | (uint32_t)(r * 255 + 0.5) << 16 | (uint32_t)(g * 255 + 0.5) << 8 |
           (uint32_t)(b * 255 + 0.5);
}

// A colour as a dark page draws it (paint.h): a background or a gradient
// is PAPER, text, a decoration, a bullet or a border is INK.
// The answers already worked out, by colour: a gradient asks once a pixel,
// and its neighbours are mostly colours it has asked about. One window
// paints on one thread.
#define RELIT_SLOTS 1024
static struct {
    uint32_t in, out;
    bool paper, used;
} s_relit[RELIT_SLOTS];

static uint32_t relit(uint32_t colour, bool paper)
{
    uint32_t k = ((colour * 2654435761u) >> 22) ^ (paper ? 1u : 0u);
    k %= RELIT_SLOTS;
    if (s_relit[k].used && s_relit[k].in == colour && s_relit[k].paper == paper)
        return s_relit[k].out;
    uint32_t out = relight(colour, paper);
    s_relit[k].in = colour;
    s_relit[k].out = out;
    s_relit[k].paper = paper;
    s_relit[k].used = true;
    return out;
}

static uint32_t paper_of(const Painter *p, uint32_t colour)
{
    return p->dark != NULL ? relit(colour, true) : colour;
}

static uint32_t ink_of(const Painter *p, uint32_t colour)
{
    return p->dark != NULL ? relit(colour, false) : colour;
}

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

os64_gui_rect_t yonder_object_rect(const flow_style_t *s, os64_gui_rect_t box, uint32_t iw,
                                   uint32_t ih)
{
    flow_object_fit_t fit = s->object_fit;
    if (iw == 0 || ih == 0)
        return box;
    if (fit == FLOW_OBJECT_SCALE_DOWN)
        fit = iw <= (uint32_t)max64(0, box.w) && ih <= (uint32_t)max64(0, box.h)
                  ? FLOW_OBJECT_NONE : FLOW_OBJECT_CONTAIN;
    int64_t w = box.w, h = box.h;
    if (fit == FLOW_OBJECT_NONE) {
        w = iw;
        h = ih;
    } else if (fit == FLOW_OBJECT_CONTAIN || fit == FLOW_OBJECT_COVER) {
        // The picture wider in shape than the box fits by its width, and
        // covers by its height.
        bool wider = (int64_t)iw * box.h > (int64_t)ih * box.w;
        if (wider == (fit == FLOW_OBJECT_CONTAIN))
            h = nearest((int64_t)box.w * ih, iw);
        else
            w = nearest((int64_t)box.h * iw, ih);
    }
    // Held to a quarter of what an int32_t says, size and place alike, so
    // that a picture's edge less any point of a surface still fits one: a
    // 1 x 16384 picture covering a wide box is billions of pixels tall.
    const int64_t most = INT32_MAX / 4;
    w = min64(w, most);
    h = min64(h, most);
    int64_t x = place_axis(s->object_position[0], box.x, (int32_t)(box.w - w));
    int64_t y = place_axis(s->object_position[1], box.y, (int32_t)(box.h - h));
    return (os64_gui_rect_t){(int32_t)max64(-most, min64(most, x)),
                             (int32_t)max64(-most, min64(most, y)), (int32_t)w, (int32_t)h};
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

bool yonder_background_tile(const flow_layer_t *l, os64_gui_rect_t area, uint32_t iw, uint32_t ih,
                            yonder_tile_t *out)
{
    int64_t w = area.w, h = area.h;         // no size of its own: the origin box's
    bool ratio = iw > 0 && ih > 0;
    if (l->fit != FLOW_FIT_LENGTHS) {
        // Scaled to the box's width or to its height, whichever covers it
        // (or fits inside it): the width when the box is the wider shape.
        bool wider = (int64_t)area.w * ih >= (int64_t)area.h * iw;
        if (ratio && wider == (l->fit == FLOW_FIT_COVER))
            h = nearest((int64_t)area.w * ih, iw);
        else if (ratio)
            w = nearest((int64_t)area.h * iw, ih);
    } else {
        int64_t gw = size_axis(l->size[0], area.w);
        int64_t gh = size_axis(l->size[1], area.h);
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
    out->at = (os64_gui_rect_t){place_axis(l->position[0], area.x, (int32_t)(area.w - w)),
                                place_axis(l->position[1], area.y, (int32_t)(area.h - h)),
                                (int32_t)w, (int32_t)h};
    out->repeat_x = l->repeat == FLOW_REPEAT || l->repeat == FLOW_REPEAT_X;
    out->repeat_y = l->repeat == FLOW_REPEAT || l->repeat == FLOW_REPEAT_Y;
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
    uint32_t colour[4];
    for (int k = 0; k < 4; k++)
        colour[k] = ink_of(p, s->border_color[k]);
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
               tone(s->border_style[FLOW_TOP], colour[FLOW_TOP], FLOW_TOP, (int32_t)i,
                    (int32_t)t));
    }
    for (int64_t i = max64(0, ry + rh - vy1), end = min64(min64(bt, rh), ry + rh - vy0); i < end;
         i++) {
        int64_t x0 = rx + (i * l + bt - 1) / bt, x1 = rx + rw - (i * rt + bt - 1) / bt;
        stroke(p, s->border_style[FLOW_BOTTOM], x0, ry + rh - 1 - i, x1 - x0, true, i, bt, rx,
               tone(s->border_style[FLOW_BOTTOM], colour[FLOW_BOTTOM], FLOW_BOTTOM,
                    (int32_t)i, (int32_t)bt));
    }
    for (int64_t j = max64(0, vx0 - rx), end = min64(min64(l, rw), vx1 - rx); j < end; j++) {
        // How many top rows reach column j: min(t, floor(j * t / l) + 1).
        int64_t y0 = ry + min64(t, j * t / l + 1), y1 = ry + rh - min64(bt, j * bt / l + 1);
        stroke(p, s->border_style[FLOW_LEFT], rx + j, y0, y1 - y0, false, j, l, ry,
               tone(s->border_style[FLOW_LEFT], colour[FLOW_LEFT], FLOW_LEFT, (int32_t)j,
                    (int32_t)l));
    }
    for (int64_t j = max64(0, rx + rw - vx1), end = min64(min64(rt, rw), rx + rw - vx0); j < end;
         j++) {
        int64_t y0 = ry + min64(t, j * t / rt + 1), y1 = ry + rh - min64(bt, j * bt / rt + 1);
        stroke(p, s->border_style[FLOW_RIGHT], rx + rw - 1 - j, y0, y1 - y0, false, j, rt, ry,
               tone(s->border_style[FLOW_RIGHT], colour[FLOW_RIGHT], FLOW_RIGHT,
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
                colour = at_share(tone(s->border_style[side], ink_of(p, s->border_color[side]), side, 0,
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
// radius). Only the view's part, and the blur's reach around it, is made —
// or, when the paint cache keeps it, the blocks of the whole mask that part
// reads, which are the same pixels (shadow_kept).

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
    uint8_t *alpha = yonder_scratch_take(p->scratch, (size_t)((x1 - x0) * (y1 - y0)));
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
    yonder_scratch_give(p->scratch, alpha);
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

// ── The paint cache's entries (YONDER_PERF.md § perf-2) ────────────────
//
// A gradient's tile, or a shadow's whole mask, kept in BLOCKs of 128 x 128
// pixels (less at the picture's right and bottom edges), only the blocks a
// paint's part reads, and only all of them: a picture whose blocks do not
// all fit is worked out as it always was.

#define BLOCK 128

// An entry's key: the values that make its pixels, written out as bytes
// (no padding, so equal values are equal keys) — the picture's, then the
// block's place.
typedef struct {
    uint8_t b[64 + 16 * 8 + 128 * 13];
    uint32_t n;
} Key;

static void key_put(Key *k, const void *v, uint32_t n)
{
    os64_memcpy(k->b + k->n, v, n);
    k->n += n;
}

static void key_i64(Key *k, int64_t v)
{
    key_put(k, &v, sizeof(v));
}

static void key_round(Key *k, const Round *s)
{
    key_i64(k, s->l);
    key_i64(k, s->t);
    key_i64(k, s->r);
    key_i64(k, s->b);
    for (int c = 0; c < 4; c++)
        for (int i = 0; i < 2; i++)
            key_i64(k, s->rad[c][i]);
}

static int64_t floor_mod(int64_t a, int64_t b)
{
    return a - b * floor_div(a, b);
}

// The blocks of a picture `pw` x `ph`, `pixel` bytes a pixel, that a part
// of a paint reads: for each column of the part, which block along x it
// reads (an index into `bx`, -1 for none: off the picture) and where in it,
// and the same down the rows; then every block the part reads, `nby` rows
// of `nbx`, found or made by blocks_get.
//
// Making is in two steps so that an entry is published only once nothing
// can fail before it is filled: PREPARE works out, once, what every block
// the paint is missing needs — the picture's columns `c0` to `c1` and rows
// `r0` to `r1` hold them all — and may fail for memory; FILL then writes
// one block from that into its entry, and cannot. NULL prepare: the fill
// works its block out on its own.
typedef struct Blocks Blocks;
typedef bool (*BlockPrepare)(const Painter *p, Blocks *bk, int64_t c0, int64_t r0, int64_t c1,
                             int64_t r1);
typedef void (*BlockFill)(const Painter *p, const Blocks *bk, int64_t bx, int64_t by, int64_t w,
                          int64_t h, void *dst);
struct Blocks {
    int64_t pw, ph;
    size_t pixel;
    const void *what;               // what the fill works the pixels out from
    Key key;
    uint32_t klen;                  // the picture's part of the key
    uint64_t base;                  // its hash
    int32_t *xb, *xin, *yb, *yin;
    int64_t *bx, *by;
    int32_t *bw;                    // each block's width along x
    int32_t nbx, nby;
    void **blocks;
    void *maps;                     // the allocation the arrays above live in
    // What a prepare worked out: `scratch`, `sw` x `sh` pixels of the
    // picture from (sx, sy), or NULL.
    void *scratch;
    int64_t sx, sy, sw, sh;
};

// One axis of the part, `n` from `from`, on a picture `size` long placed
// at `origin`, taken into it when `wrap` (a repeating tile): each
// position's block, an index into `dense`, which this fills, or -1 off the
// picture, and its place in the block. How many blocks it reads.
static int32_t axis_map(int64_t from, int64_t n, int64_t origin, int64_t size, bool wrap,
                        int32_t *blk, int32_t *in, int64_t *dense)
{
    int32_t nd = 0, at = -1;
    for (int64_t i = 0; i < n; i++) {
        int64_t pos = from + i - origin;
        if (wrap) {
            pos = floor_mod(pos, size);
        } else if (pos < 0 || pos >= size) {
            blk[i] = -1;
            in[i] = 0;
            continue;
        }
        int64_t b = pos / BLOCK;
        if (at < 0 || dense[at] != b) {
            at = -1;
            for (int32_t k = 0; k < nd && at < 0; k++)
                if (dense[k] == b)
                    at = k;
            if (at < 0) {
                dense[nd] = b;
                at = nd++;
            }
        }
        blk[i] = at;
        in[i] = (int32_t)(pos - b * BLOCK);
    }
    return nd;
}

static void blocks_free(const Painter *p, Blocks *bk)
{
    yonder_scratch_give(p->scratch, bk->scratch);
    os64_free(bk->blocks);
    os64_free(bk->maps);
}

// The part `x0`.. (`nx` columns) by `y0`.. (`ny` rows), on the picture
// placed at (ox, oy). False when there is no memory for the maps.
static bool blocks_map(Blocks *bk, int64_t x0, int64_t nx, int64_t ox, bool wrap_x, int64_t y0,
                       int64_t ny, int64_t oy, bool wrap_y)
{
    size_t ints = (size_t)(2 * nx + 2 * ny + nx), longs = (size_t)(nx + ny);
    bk->maps = os64_malloc(longs * sizeof(int64_t) + ints * sizeof(int32_t));
    bk->blocks = NULL;
    if (bk->maps == NULL)
        return false;
    bk->bx = bk->maps;
    bk->by = bk->bx + nx;
    bk->xb = (int32_t *)(bk->by + ny);
    bk->xin = bk->xb + nx;
    bk->yb = bk->xin + nx;
    bk->yin = bk->yb + ny;
    bk->bw = bk->yin + ny;
    bk->nbx = axis_map(x0, nx, ox, bk->pw, wrap_x, bk->xb, bk->xin, bk->bx);
    bk->nby = axis_map(y0, ny, oy, bk->ph, wrap_y, bk->yb, bk->yin, bk->by);
    for (int32_t i = 0; i < bk->nbx; i++)
        bk->bw[i] = (int32_t)min64(BLOCK, bk->pw - bk->bx[i] * BLOCK);
    bk->blocks = os64_calloc((size_t)bk->nbx * (size_t)bk->nby, sizeof(void *));
    return bk->blocks != NULL || bk->nbx * bk->nby == 0;
}

// Block (i, j)'s key and its hash, written after the picture's.
static uint64_t block_key(Blocks *bk, int32_t i, int32_t j)
{
    bk->key.n = bk->klen;
    key_i64(&bk->key, bk->bx[i]);
    key_i64(&bk->key, bk->by[j]);
    return paint_cache_hash_from(bk->base, bk->key.b + bk->klen, bk->key.n - bk->klen);
}

static void blocks_release(const Painter *p, Blocks *bk)
{
    for (int32_t k = 0; k < bk->nbx * bk->nby; k++)
        if (bk->blocks[k] != NULL)
            paint_cache_release(p->cache, bk->blocks[k]);
}

// Every block the part reads, found or made with `prepare` and `fill`: the
// ones held are found first, and the rest made only when they all fit
// (paint_cache_room) and what they need has been worked out. False, with
// none of them kept for the paint and the picture counted as refused, when
// they do not fit or there is no memory.
static bool blocks_get(const Painter *p, Blocks *bk, BlockPrepare prepare, BlockFill fill)
{
    size_t cost = 0;
    int64_t c0 = INT64_MAX, r0 = INT64_MAX, c1 = 0, r1 = 0;
    for (int32_t j = 0; j < bk->nby; j++)
        for (int32_t i = 0; i < bk->nbx; i++) {
            uint64_t hash = block_key(bk, i, j);
            void **at = &bk->blocks[j * bk->nbx + i];
            *at = paint_cache_find(p->cache, bk->key.b, bk->key.n, hash);
            if (*at != NULL)
                continue;
            int64_t h = min64(BLOCK, bk->ph - bk->by[j] * BLOCK);
            cost += paint_cache_cost(bk->key.n, (size_t)(bk->bw[i] * h) * bk->pixel);
            c0 = min64(c0, bk->bx[i] * BLOCK);
            c1 = max64(c1, bk->bx[i] * BLOCK + bk->bw[i]);
            r0 = min64(r0, bk->by[j] * BLOCK);
            r1 = max64(r1, bk->by[j] * BLOCK + h);
        }
    if (cost == 0)
        return true;
    if (!paint_cache_room(p->cache, cost)) {
        blocks_release(p, bk);
        paint_cache_refused(p->cache, false);
        return false;
    }
    if (prepare != NULL && !prepare(p, bk, c0, r0, c1, r1)) {
        blocks_release(p, bk);
        paint_cache_refused(p->cache, true);
        return false;
    }
    for (int32_t j = 0; j < bk->nby; j++)
        for (int32_t i = 0; i < bk->nbx; i++) {
            void **at = &bk->blocks[j * bk->nbx + i];
            if (*at != NULL)
                continue;
            int64_t w = bk->bw[i], h = min64(BLOCK, bk->ph - bk->by[j] * BLOCK);
            uint64_t hash = block_key(bk, i, j);
            *at = paint_cache_make(p->cache, bk->key.b, bk->key.n, hash, (size_t)(w * h) * bk->pixel);
            if (*at == NULL) {
                blocks_release(p, bk);
                paint_cache_refused(p->cache, true);
                return false;
            }
            fill(p, bk, bk->bx[i], bk->by[j], w, h, *at);
        }
    return true;
}

// The run from column `c` of the part's row `r` (its `n` columns, the row
// on the picture): how many columns from `c` read one block's row in order
// — a run ends where its block does, which is also where a repeating
// tile wraps — with `*src` its first pixel, or NULL for a run off the
// picture.
static int64_t blocks_run(const Blocks *bk, int64_t r, int64_t c, int64_t n, const void **src)
{
    int32_t i = bk->xb[c];
    if (i < 0) {
        int64_t k = 1;
        while (c + k < n && bk->xb[c + k] < 0)
            k++;
        *src = NULL;
        return k;
    }
    const uint8_t *block = bk->blocks[bk->yb[r] * bk->nbx + i];
    *src = block + ((size_t)bk->yin[r] * (size_t)bk->bw[i] + (size_t)bk->xin[c]) * bk->pixel;
    return min64(bk->bw[i] - bk->xin[c], n - c);
}

// A shadow's mask, kept: its shape and the shape it is cut to (`times`,
// whose coverage it is multiplied by — or what that leaves, when outer),
// moved so the whole mask's corner is (0, 0), which by whole pixels
// changes no pixel.
typedef struct {
    Round shape, times;
    bool inset;
    int64_t blur, reach;
} ShadowMask;

// The mask the missing blocks need, columns `c0` to `c1` and rows `r0` to
// `r1`, made once as the view's part is (shadow_cast): those pixels and the
// blur's reach around them, blurred together, so a block costs what its
// share of the uncached path's work does.
static bool mask_prepare(const Painter *p, Blocks *bk, int64_t c0, int64_t r0, int64_t c1,
                         int64_t r1)
{
    const ShadowMask *sm = bk->what;
    int64_t sc0 = max64(0, c0 - sm->reach), sc1 = min64(bk->pw, c1 + sm->reach);
    int64_t sr0 = max64(0, r0 - sm->reach), sr1 = min64(bk->ph, r1 + sm->reach);
    Mask m = {sc0, sr0, sc1 - sc0, sr1 - sr0, NULL};
    m.a = yonder_scratch_take(p->scratch, (size_t)(m.w * m.h) * sizeof(*m.a));
    if (m.a == NULL)
        return false;
    mask_cover(&m, &sm->shape, sm->inset);
    if (!mask_blur(&m, sm->blur, sm->inset ? SUB : 0)) {
        yonder_scratch_give(p->scratch, m.a);
        return false;
    }
    mask_times(&m, &sm->times, !sm->inset);
    bk->scratch = m.a;
    bk->sx = m.x;
    bk->sy = m.y;
    bk->sw = m.w;
    bk->sh = m.h;
    return true;
}

// Block (bx, by) of the mask, cut from what mask_prepare made: every pixel
// of it is the reach or more inside that region's cut edges, so it is the
// whole mask's.
static void mask_fill(const Painter *p, const Blocks *bk, int64_t bx, int64_t by, int64_t w,
                      int64_t h, void *dst)
{
    (void)p;
    const uint16_t *from = bk->scratch;
    uint16_t *out = dst;
    for (int64_t r = 0; r < h; r++)
        os64_memcpy(out + r * w, from + (by * BLOCK - bk->sy + r) * bk->sw + (bx * BLOCK - bk->sx),
                    (size_t)w * sizeof(*out));
}

// The view's part of the shadow whose whole mask is (fx0, fy0) to (fx1,
// fy1), drawn from the cache in `colour`. False when its blocks could not
// be kept: the caller works it out as it always did.
static bool shadow_kept(const Painter *p, const Round *shape, const Round *times, bool inset,
                        int64_t blur, int64_t reach, int64_t fx0, int64_t fy0, int64_t fx1,
                        int64_t fy1, uint32_t colour)
{
    int64_t x0 = max64(fx0, p->view.x), y0 = max64(fy0, p->view.y);
    int64_t x1 = min64(fx1, (int64_t)p->view.x + p->view.w);
    int64_t y1 = min64(fy1, (int64_t)p->view.y + p->view.h);
    if (x1 <= x0 || y1 <= y0 || p->v->mask == NULL)
        return true;
    ShadowMask sm = {round_moved(shape, -fx0 * SUB, -fy0 * SUB, 0),
                     round_moved(times, -fx0 * SUB, -fy0 * SUB, 0), inset, blur, reach};
    Blocks bk = {.pw = fx1 - fx0, .ph = fy1 - fy0, .pixel = sizeof(uint16_t), .what = &sm};
    key_put(&bk.key, "S", 1);
    key_put(&bk.key, &sm.inset, 1);
    key_i64(&bk.key, blur);
    key_i64(&bk.key, bk.pw);
    key_i64(&bk.key, bk.ph);
    key_round(&bk.key, &sm.shape);
    key_round(&bk.key, &sm.times);
    bk.klen = bk.key.n;
    bk.base = paint_cache_hash(bk.key.b, bk.klen);
    if (!blocks_map(&bk, x0, x1 - x0, fx0, false, y0, y1 - y0, fy0, false)) {
        blocks_free(p, &bk);
        paint_cache_refused(p->cache, true);
        return false;
    }
    if (!blocks_get(p, &bk, mask_prepare, mask_fill)) {
        blocks_free(p, &bk);
        return false;
    }
    uint8_t *alpha = yonder_scratch_take(p->scratch, (size_t)((x1 - x0) * (y1 - y0)));
    if (alpha != NULL) {
        // The part lies inside the mask, so no run is off it.
        uint32_t a = flow_alpha(colour);
        for (int64_t r = 0; r < y1 - y0; r++) {
            uint8_t *row = alpha + r * (x1 - x0);
            for (int64_t c = 0, n; c < x1 - x0; c += n) {
                const void *src;
                n = blocks_run(&bk, r, c, x1 - x0, &src);
                const uint16_t *m = src;
                for (int64_t k = 0; k < n; k++)
                    row[c + k] = (uint8_t)(m[k] * a / SUB);
            }
        }
        p->v->mask(p->v->ctx,
                   (os64_gui_rect_t){(int32_t)(x0 + p->off.x), (int32_t)(y0 + p->off.y),
                                     (int32_t)(x1 - x0), (int32_t)(y1 - y0)},
                   alpha, colour & 0xffffffu);
        yonder_scratch_give(p->scratch, alpha);
    }
    blocks_free(p, &bk);
    return true;
}

void (*yonder_paint_stage)(yonder_paint_stage_t stage, bool begin);
uint64_t yonder_paint_cuts;

static void shadow_cast(const Painter *p, const flow_box_t *b, const Round *outer, const Round *inner,
                        const flow_shadow_t *sh, bool square);

static void shadow(const Painter *p, const flow_box_t *b, const Round *outer, const Round *inner,
                   const flow_shadow_t *sh, bool square)
{
    if (yonder_paint_stage != NULL)
        yonder_paint_stage(YONDER_PAINT_SHADOW, true);
    shadow_cast(p, b, outer, inner, sh, square);
    if (yonder_paint_stage != NULL)
        yonder_paint_stage(YONDER_PAINT_SHADOW, false);
}

static void shadow_cast(const Painter *p, const flow_box_t *b, const Round *outer, const Round *inner,
                        const flow_shadow_t *sh, bool square)
{
    // An outer shadow a negative spread collapses casts nothing — libflow's
    // rule (flow_shadow_casts), which a group's bounds follow.
    if (!sh->inset && !flow_shadow_casts(b, sh))
        return;
    // How far a pixel's shade reads: three box blurs of radius blur/2 each
    // (mask_blur), 3 (blur/2) — libflow's rule (flow_shadow_blur_reach),
    // which a group's bounds hold too. A pass wrongs only the r pixels
    // nearest the mask's edge, where it reads past it, so after three every
    // pixel 3 (blur/2) inside the edge is right; a mask cut any closer to
    // the view would shade a pixel by where the dirty part happened to end
    // (Quinn, #203).
    int64_t blur = sh->blur, reach = flow_shadow_blur_reach(sh->blur);
    uint32_t colour = sh->current ? b->style->color : sh->colour;
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
        fill(p, l, t, r - l, max64(0, min64(e, bt) - t), colour);                // above
        fill(p, l, max64(t, bb), r - l, e - max64(t, bb), colour);               // below
        int64_t mt = max64(t, bt), mb = min64(e, bb);
        if (mb > mt) {
            fill(p, l, mt, max64(0, min64(r, bl) - l), mb - mt, colour);         // left
            fill(p, max64(l, br), mt, r - max64(l, br), mb - mt, colour);        // right
        }
        return;
    }
    // Where it can fall: around its shape by the blur's reach, outside the
    // border box (or inside the padding box), and in the view or the blur's
    // reach of it — or, from the cache, all of it.
    const Round *where = sh->inset ? inner : &shape;
    const Round *times = sh->inset ? inner : outer;
    int64_t x0 = where->l / SUB - reach, y0 = where->t / SUB - reach;
    int64_t x1 = (where->r + SUB - 1) / SUB + reach, y1 = (where->b + SUB - 1) / SUB + reach;
    if (p->cache != NULL &&
        shadow_kept(p, &shape, times, sh->inset, blur, reach, x0, y0, x1, y1, colour))
        return;
    x0 = max64(x0, (int64_t)p->view.x - reach);
    y0 = max64(y0, (int64_t)p->view.y - reach);
    x1 = min64(x1, (int64_t)p->view.x + p->view.w + reach);
    y1 = min64(y1, (int64_t)p->view.y + p->view.h + reach);
    if (x1 <= x0 || y1 <= y0)
        return;
    Mask m = {x0, y0, x1 - x0, y1 - y0, NULL};
    m.a = yonder_scratch_take(p->scratch, (size_t)(m.w * m.h) * sizeof(*m.a));
    if (m.a == NULL)
        return;
    // An inset shadow is what its shape leaves, inside the padding box.
    mask_cover(&m, &shape, sh->inset);
    if (mask_blur(&m, blur, sh->inset ? SUB : 0)) {
        mask_times(&m, times, !sh->inset);
        mask_emit(p, &m, colour);
    }
    yonder_scratch_give(p->scratch, m.a);
}

// ── Gradients (Images 3 § 3) ────────────────────────────────────────────
//
// A gradient is a picture the size of the box's padding box — its
// positioning area — drawn across its border box, repeated as
// `background-repeat` says, and cut to its corners' curves. Each pixel of
// it that the view shows is worked out on its own: where its middle falls
// along the gradient's line (or ray), and the colour the stops give there,
// mixed in premultiplied sRGB as CSS mixes colours with alpha — once a
// tile, when the paint cache keeps it (tile_fill).

#define GRADIENT_STOPS_MAX 128

typedef struct {
    double at[GRADIENT_STOPS_MAX];      // along the line, 0 its start, 1 its end
    uint32_t colour[GRADIENT_STOPS_MAX];
    bool hint[GRADIENT_STOPS_MAX];
    int32_t n;
} Stops;

// A tile's key holds every stop (tile_kept).
_Static_assert(sizeof(((Key *)0)->b) >= 100 + GRADIENT_STOPS_MAX * 13, "a gradient's key must fit its stops");

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
// each no earlier than the one before it, and a run of colour stops left
// out spread evenly between the colour stops either side of it — a hint
// among them is not one of those, and keeps its own place. A currentColor
// stop is the colour of the box it is drawn on, `current`.
static bool stops_resolve(const flow_gradient_t *g, double len, uint32_t current, Stops *out)
{
    out->n = g->nstops < GRADIENT_STOPS_MAX ? g->nstops : GRADIENT_STOPS_MAX;
    if (out->n < 2)
        return false;
    for (int32_t i = 0; i < out->n; i++) {
        out->at[i] = stop_at(g->stops[i].at, len);
        out->colour[i] = g->stops[i].current ? current : g->stops[i].colour;
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
    // A hint always has a place, and the first and last stops have one by
    // now, so every run below ends at a colour stop with one.
    for (int32_t i = 1; i < out->n; i++) {
        if (!isnan(out->at[i]))
            continue;
        int32_t before = i - 1;
        while (out->hint[before])
            before--;
        int32_t after = i, unset = 0;
        while (out->hint[after] || isnan(out->at[after])) {
            unset += !out->hint[after];
            after++;
        }
        int32_t k = 0;
        for (int32_t m = i; m < after; m++)
            if (!out->hint[m])
                out->at[m] = out->at[before] +
                             (out->at[after] - out->at[before]) * ++k / (unset + 1);
        i = after;
    }
    return true;
}

// The gradient's average colour (Images 3 § 3.6): each pair of
// neighbouring colour stops lends each of its two colours half its
// distance over the whole length, premultiplied. A gradient with no
// length is averaged as its colours spread evenly from first to last,
// so each pair weighs the same. Hints are not colour stops and lend
// nothing.
static uint32_t stops_average(const Stops *s)
{
    double total = s->at[s->n - 1] - s->at[0];
    bool even = !(total > 0);
    double sum[4] = {0, 0, 0, 0}, weights = 0;
    int32_t prev = -1;
    for (int32_t i = 0; i < s->n; i++) {
        if (s->hint[i])
            continue;
        if (prev >= 0) {
            double w = even ? 1 : (s->at[i] - s->at[prev]) / total;
            const uint32_t two[2] = {s->colour[prev], s->colour[i]};
            for (int k = 0; k < 2; k++) {
                double a = flow_alpha(two[k]) / 255.0;
                sum[3] += a * w / 2;
                for (int c = 0; c < 3; c++)
                    sum[c] += ((two[k] >> (8 * c)) & 0xff) * a * w / 2;
            }
            weights += w;
        }
        prev = i;
    }
    if (!(weights > 0) || sum[3] <= 0)
        return 0xff000000u;                 // wholly transparent
    uint32_t out = 0;
    for (int c = 0; c < 3; c++) {
        double v = sum[c] / sum[3];
        out |= (uint32_t)(v + 0.5 > 255 ? 255 : v + 0.5) << (8 * c);
    }
    uint32_t al = (uint32_t)(sum[3] / weights * 255 + 0.5);
    return out | (255u - (al > 255 ? 255 : al)) << 24;
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

// What works a gradient's pixels out, its stops resolved: the line through
// the tile's middle (cx, cy) along (dx, dy), `len` long, or the ray from
// the centre (cx, cy) with radii rx and ry.
typedef struct {
    flow_gradient_kind_t kind;
    bool repeating;
    double dx, dy, len, cx, cy, rx, ry;
    Stops stops;
} Line;

// The tile's pixel whose middle is (u, v) in the tile, as the verb takes
// it: straight alpha on top.
static uint32_t line_at(const Painter *p, const Line *l, double u, double v)
{
    double t = l->kind == FLOW_GRADIENT_LINEAR ? ((u - l->cx) * l->dx + (v - l->cy) * l->dy) / l->len + 0.5
                                               : hypot((u - l->cx) / l->rx, (v - l->cy) / l->ry);
    uint32_t c = paper_of(p, stops_colour(&l->stops, t, l->repeating));
    return (uint32_t)flow_alpha(c) << 24 | (c & 0xffffffu);
}

// Block (bx, by) of a gradient's tile: each pixel the colour at its
// middle, which is where the uncached path asks (x + 0.5 - tx, wrapped, is
// the column plus a half).
static void tile_fill(const Painter *p, const Blocks *bk, int64_t bx, int64_t by, int64_t w,
                      int64_t h, void *dst)
{
    const Line *line = bk->what;
    uint32_t *out = dst;
    for (int64_t r = 0; r < h; r++)
        for (int64_t c = 0; c < w; c++)
            out[r * w + c] = line_at(p, line, bx * BLOCK + c + 0.5, by * BLOCK + r + 0.5);
}

// Pixel `px` with its alpha times `share` of SUB.
static uint32_t alpha_times(uint32_t px, int64_t share)
{
    return (uint32_t)((px >> 24) * share / SUB) << 24 | (px & 0xffffffu);
}

// Row `row`, columns `x0` to `x1`, cut to the span [a, e) (1/256 px): each
// pixel's alpha times how much of it the span covers. The columns wholly
// inside it keep theirs, so only those its edges cross are worked.
static void row_cut(uint32_t *row, int64_t x0, int64_t x1, int64_t a, int64_t e)
{
    int64_t whole = min64(x1, max64(x0, -floor_div(-a, SUB)));     // the first wholly in
    int64_t past = max64(whole, min64(x1, floor_div(e, SUB)));     // the first after them
    for (int64_t x = x0; x < whole; x++)
        row[x - x0] = alpha_times(row[x - x0], cover(x, a, e));
    for (int64_t x = past; x < x1; x++)
        row[x - x0] = alpha_times(row[x - x0], cover(x, a, e));
}

// The blocks of `line`'s tile that the part `x0`..`x1` by `y0`..`y1` reads,
// found or made (YONDER_PERF.md § perf-2). False when they could not be
// kept, and `bk` holds nothing to free.
static bool tile_kept(const Painter *p, const Line *line, const yonder_tile_t *tile, int64_t x0,
                      int64_t x1, int64_t y0, int64_t y1, Blocks *bk)
{
    *bk = (Blocks){.pw = tile->at.w, .ph = tile->at.h, .pixel = sizeof(uint32_t), .what = line};
    uint8_t kind = (uint8_t)line->kind, dark = p->dark != NULL;
    key_put(&bk->key, "G", 1);
    key_put(&bk->key, &kind, 1);
    key_put(&bk->key, &line->repeating, 1);
    key_put(&bk->key, &dark, 1);
    key_i64(&bk->key, bk->pw);
    key_i64(&bk->key, bk->ph);
    const double geometry[7] = {line->dx, line->dy, line->len, line->cx, line->cy, line->rx, line->ry};
    key_put(&bk->key, geometry, sizeof(geometry));
    key_i64(&bk->key, line->stops.n);
    for (int32_t i = 0; i < line->stops.n; i++) {
        key_put(&bk->key, &line->stops.at[i], sizeof(line->stops.at[i]));
        key_put(&bk->key, &line->stops.colour[i], sizeof(line->stops.colour[i]));
        key_put(&bk->key, &line->stops.hint[i], 1);
    }
    bk->klen = bk->key.n;
    bk->base = paint_cache_hash(bk->key.b, bk->klen);
    if (!blocks_map(bk, x0, x1 - x0, tile->at.x, tile->repeat_x, y0, y1 - y0, tile->at.y, tile->repeat_y)) {
        blocks_free(p, bk);
        paint_cache_refused(p->cache, true);
        return false;
    }
    if (!blocks_get(p, bk, NULL, tile_fill)) {
        blocks_free(p, bk);
        return false;
    }
    return true;
}

// Gradient `g` in `tile`, drawn over `area` (and the view), cut to `shape`
// when `round`; a currentColor stop is `current`, the colour of the box it
// is drawn on. False only when its pixels found no memory and nothing was
// drawn — a text clip must know (text_clip); true when it is drawn, or
// there is nothing of it to draw.
static bool gradient_paint(const Painter *p, const flow_gradient_t *g, const yonder_tile_t *tile,
                           os64_gui_rect_t area, const Round *shape, bool round, uint32_t current);

static bool gradient(const Painter *p, const flow_gradient_t *g, const yonder_tile_t *tile,
                     os64_gui_rect_t area, const Round *shape, bool round, uint32_t current)
{
    if (yonder_paint_stage != NULL)
        yonder_paint_stage(YONDER_PAINT_GRADIENT, true);
    bool drawn = gradient_paint(p, g, tile, area, shape, round, current);
    if (yonder_paint_stage != NULL)
        yonder_paint_stage(YONDER_PAINT_GRADIENT, false);
    return drawn;
}

static bool gradient_paint(const Painter *p, const flow_gradient_t *g, const yonder_tile_t *tile,
                           os64_gui_rect_t area, const Round *shape, bool round, uint32_t current)
{
    double tx = tile->at.x, ty = tile->at.y, w = tile->at.w, h = tile->at.h;
    if (w <= 0 || h <= 0)
        return true;
    int64_t x0 = max64(area.x, p->view.x), y0 = max64(area.y, p->view.y);
    int64_t x1 = min64((int64_t)area.x + area.w, (int64_t)p->view.x + p->view.w);
    int64_t y1 = min64((int64_t)area.y + area.h, (int64_t)p->view.y + p->view.h);
    if (x1 <= x0 || y1 <= y0 || p->v->pixels == NULL)
        return true;
    // The line (linear) or the ray (radial) and its length in px.
    double dx = 0, dy = 0, len, cx = w / 2, cy = h / 2, rx = 1, ry = 1;
    bool flat = false;
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
            // A radius is never negative where it is used: a calc() that
            // works out below 0 is 0 (Values 4 § 10.12), then the
            // degenerate shapes below.
            rx = fmax(0, lp_px(g->radii[0], w));
            ry = fmax(0, lp_px(g->radii[1], h));
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
        // § 3.2.3's degenerate shapes, which still paint: a circle of no
        // radius is a very small one; an ellipse of no width a very narrow
        // and very tall one, so its stops spread across x as a linear
        // gradient mirrored about the centre (and a percentage stop sits at
        // 0); one of no height a very wide and flat one, which is the last
        // stop's colour everywhere, or the average if it repeats — the
        // look § 3.2.3 says that shape gives, and what Chrome draws even on
        // a box one pixel high, whose pixel middles lie ON the flat line,
        // where sampling the limiting shape would give the first colour.
        const double kSmall = 1e-6, kLarge = 1e9;
        if (g->circle && !(rx > 0)) {
            rx = ry = kSmall;
        } else if (!(rx > 0)) {
            rx = kSmall;
            ry = kLarge;
        } else if (!(ry > 0)) {
            ry = kSmall;
            flat = true;
        }
        len = rx;
    }
    Line line = {g->kind, g->repeating, dx, dy, len, cx, cy, rx, ry, {{0}, {0}, {0}, 0}};
    if (!(len > 0) || !stops_resolve(g, len, current, &line.stops))
        return true;
    const Stops *stops = &line.stops;
    // One colour everywhere: the flat ellipse, and a repeating gradient
    // whose repeat is shorter than a pixel along the way it runs — none
    // at all, or one no pixel can show (§ 3.6) — which is its average. A
    // radial's repeat is shortest along its shorter radius.
    double pixel_period = (stops->at[stops->n - 1] - stops->at[0]) *
                          (g->kind == FLOW_GRADIENT_LINEAR ? len : fmin(rx, ry));
    bool solid = flat || (g->repeating && !(pixel_period >= 1));
    uint32_t solid_colour = paper_of(p, g->repeating ? stops_average(stops) : stops->colour[stops->n - 1]);
    uint32_t solid_px = (uint32_t)flow_alpha(solid_colour) << 24 | (solid_colour & 0xffffffu);
    bool rep_x = tile->repeat_x, rep_y = tile->repeat_y;
    // One colour is a fill, already cheap; anything else is drawn from the
    // tile when the cache keeps it.
    Blocks bk;
    bool kept = p->cache != NULL && !solid && tile_kept(p, &line, tile, x0, x1, y0, y1, &bk);
    uint32_t *out = yonder_scratch_take(p->scratch, (size_t)((x1 - x0) * (y1 - y0)) * sizeof(*out));
    if (out == NULL) {
        if (kept)
            blocks_free(p, &bk);
        return false;
    }
    for (int64_t y = y0; y < y1; y++) {
        int64_t a = 0, e = 0;
        uint32_t *row = &out[(y - y0) * (x1 - x0)];
        double v = y + 0.5 - ty;
        if ((round && !round_row(shape, y * SUB + SUB / 2, &a, &e)) || (!rep_y && (v < 0 || v >= h))) {
            os64_memset(row, 0, (size_t)(x1 - x0) * sizeof(*row));
            continue;
        }
        if (kept) {
            // The tile is placed at whole pixels, so a pixel whose middle
            // is off it is one the maps leave off the picture.
            for (int64_t c = 0, n; c < x1 - x0; c += n) {
                const void *src;
                n = blocks_run(&bk, y - y0, c, x1 - x0, &src);
                if (src != NULL)
                    os64_memcpy(row + c, src, (size_t)n * sizeof(*row));
                else
                    os64_memset(row + c, 0, (size_t)n * sizeof(*row));
            }
        } else {
            for (int64_t x = x0; x < x1; x++) {
                double u = x + 0.5 - tx;
                row[x - x0] = !rep_x && (u < 0 || u >= w) ? 0
                            : solid ? solid_px : line_at(p, &line, wrap(u, w), wrap(v, h));
            }
        }
        if (round)
            row_cut(row, x0, x1, a, e);
    }
    p->v->pixels(p->v->ctx,
                 (os64_gui_rect_t){(int32_t)(x0 + p->off.x), (int32_t)(y0 + p->off.y),
                                   (int32_t)(x1 - x0), (int32_t)(y1 - y0)},
                 out);
    yonder_scratch_give(p->scratch, out);
    if (kept)
        blocks_free(p, &bk);
    return true;
}

// Layers `from` down to `to` of a box's background (`n` of them; at `n`,
// its colour, under the bottom layer and cut by its clip), the bottom
// first: a block-level box's frame (`outer` its shape when rounded), or an
// inline box's piece (`piece`).
typedef struct {
    const flow_box_t *b;
    const Round *outer;
    int32_t from, to, n;
    bool piece;
} LayerRun;

static void text_clip(const Painter *p, const LayerRun *run);
static void round_backdrop(const Painter *p, const flow_box_t *b, const Round *shape,
                           int32_t layer, os64_gui_rect_t area, os64_gui_rect_t origin);

// Whether layer `i` of `s`, or at `i == n` its colour, is cut to the text.
static bool clipped_to_text(const flow_style_t *s, int32_t i, int32_t n)
{
    return flow_background_layer(s, i < n ? i : n - 1).clip == FLOW_EDGE_TEXT;
}

// One layer of a block-level box's frame, placed in its origin box and cut
// to its clip box — rounded as the box is when `outer` is its shape (a
// clip to the text is the border box: yonder_box_edge). With no picture or
// gradient from a sheet, a `background` attribute's picture is drawn as
// the top layer. False only when a gradient's pixels found no memory
// (gradient).
static bool frame_layer(const Painter *p, const flow_box_t *b, const Round *outer, int32_t i,
                        int32_t n)
{
    const flow_style_t *s = b->style;
    bool sheet = flow_background_has_image(s);
    flow_layer_t l = flow_background_layer(s, i < n ? i : n - 1);
    if (i == n && !s->has_background)
        return true;
    os64_gui_rect_t cut = yonder_box_edge(b, b->rect, l.clip);
    Round shape = {0};
    if (outer != NULL)
        shape = round_inset(outer, b->rect, cut);
    yonder_tile_t tile;
    if (i == n && outer != NULL) {
        round_background(p, b, &shape, paper_of(p, s->background));
    } else if (i == n) {
        fill(p, cut.x, cut.y, cut.w, cut.h, paper_of(p, s->background));
    } else if (l.gradient != NULL) {
        if (yonder_background_tile(&l, yonder_box_edge(b, b->rect, l.origin), 0, 0, &tile))
            return gradient(p, l.gradient, &tile, cut, &shape, outer != NULL, s->color);
    } else if (l.image != NULL || (!sheet && i == 0)) {
        os64_gui_rect_t origin = on_page(p, yonder_box_edge(b, b->rect, l.origin));
        if (outer != NULL) {
            round_backdrop(p, b, &shape, sheet ? i : -1, cut, origin);
        } else {
            os64_gui_rect_t area = on_page(p, cut);
            (void)p->v->backdrop(p->v->ctx, b, sheet ? i : -1, &area, origin, area.x, area.y,
                                 on_page(p, p->view));
        }
    }
    return true;
}

// One layer of an inline box's piece. A piece is its content area, with no
// padding or border laid out round it, so its origin and clip boxes are
// all the piece; its colour is right on every piece, and its pictures and
// gradients are placed on each — the browsers lay the pieces end to end
// and draw one along them, which needs knowledge the public tree does not
// have (PILE3.md § Booked). False as frame_layer's is.
static bool piece_layer(const Painter *p, const flow_box_t *b, int32_t i, int32_t n)
{
    if (i == n) {
        if (b->style->has_background)
            fill(p, b->rect.x, b->rect.y, b->rect.w, b->rect.h, paper_of(p, b->style->background));
        return true;
    }
    flow_layer_t l = flow_background_layer(b->style, i);
    yonder_tile_t tile;
    if (l.gradient != NULL && yonder_background_tile(&l, b->rect, 0, 0, &tile))
        return gradient(p, l.gradient, &tile, b->rect, NULL, false, b->style->color);
    if (l.image != NULL) {
        os64_gui_rect_t area = on_page(p, b->rect);
        (void)p->v->backdrop(p->v->ctx, b, i, &area, area, area.x, area.y, on_page(p, p->view));
    }
    return true;
}

// Every layer of the run, each drawn whatever came of the last; false when
// one of them found no memory.
static bool run_layers(const Painter *p, const LayerRun *run)
{
    bool drawn = true;
    for (int32_t i = run->from; i >= run->to; i--) {
        if (run->piece)
            drawn = piece_layer(p, run->b, i, run->n) && drawn;
        else
            drawn = frame_layer(p, run->b, run->outer, i, run->n) && drawn;
    }
    return drawn;
}

// A box's background over its outer shadows, its colour then its layers
// from the bottom up, each layer as frame_layer or piece_layer draws it —
// but a run of them cut to the text is drawn through a text clip
// (text_clip, PILE3.md § The text clip).
static void background_of(const Painter *p, const flow_box_t *b, const Round *outer, bool piece)
{
    const flow_style_t *s = b->style;
    int32_t n = flow_background_layers(s);
    for (int32_t i = n; i >= 0; i--) {
        LayerRun run = {b, outer, i, i, n, piece};
        while (clipped_to_text(s, run.from, n) && run.to > 0 && clipped_to_text(s, run.to - 1, n))
            run.to--;
        if (clipped_to_text(s, i, n))
            text_clip(p, &run);
        else
            (void)run_layers(p, &run);
        i = run.to;
    }
}

static void layers(const Painter *p, const flow_box_t *b, const Round *outer)
{
    background_of(p, b, outer, false);
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
// top; its background's layers over its colour (layers, above); its inset
// shadows over those and under its borders (Backgrounds 3 § 7.1). Rounded,
// its colour, gradients, shadows and borders follow the curves.
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
    if (b != p->canvas_owner)
        layers(p, b, round ? &outer : NULL);
    for (int32_t i = s->nbox_shadows - 1; i >= 0; i--)
        if (s->box_shadows[i].inset)
            shadow(p, b, &outer, &inner, &s->box_shadows[i], !round);
    if (round)
        round_borders(p, b, &outer, &inner, w);
    else
        borders(p, b);
}

// A box's outline (CSS UI 4 § 3): a ring of the outline's width, style and
// colour round where libflow says it is (flow_outline_rect), drawn as a
// square border is — so its styles are a border's, dotted to outset — when
// the walk hands it over, last in its stacking context, over everything
// painted in it (paint_outline). Square whatever the box's radius:
// following the corners is booked.
static void outline(const Painter *p, const flow_box_t *b)
{
    os64_gui_rect_t ring;
    if (p->penned || !flow_outline_rect(b, &ring))
        return;
    flow_style_t s = *b->style;
    for (int k = 0; k < 4; k++) {
        s.border_width[k] = b->style->outline_width;
        s.border_style[k] = b->style->outline_style;
        s.border_color[k] = b->style->outline_color;
    }
    flow_box_t drawn = *b;
    drawn.style = &s;
    drawn.rect = ring;
    borders(p, &drawn);
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
    // Where each falls is libflow's (flow_decoration_rect), which a
    // group's bounds hold too. A text clip's pen draws them as the text's.
    if (b->decoration & FLOW_DECORATION_UNDERLINE) {
        os64_gui_rect_t r = flow_decoration_rect(b, FLOW_DECORATION_UNDERLINE);
        fill(p, r.x, r.y, r.w, r.h, p->penned ? p->pen : ink_of(p, b->underline_color));
    }
    if (b->decoration & FLOW_DECORATION_LINE_THROUGH) {
        os64_gui_rect_t r = flow_decoration_rect(b, FLOW_DECORATION_LINE_THROUGH);
        fill(p, r.x, r.y, r.w, r.h, p->penned ? p->pen : ink_of(p, b->line_through_color));
    }
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
    // Where it falls is libflow's (flow_marker_shape_rect), which a
    // group's bounds hold too.
    os64_gui_rect_t at = flow_marker_shape_rect(b);
    int64_t x = at.x, y = at.y, d = at.w;
    // A marker's symbol is the marker's text, so it is FILLED as text is
    // (-webkit-text-fill-color), as Chrome draws it.
    uint32_t colour = ink_of(p, b->style->fill);
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

static bool is_frame(const flow_box_t *b)
{
    return b->node != NULL && b->node->kind == OS64_HTML_ELEMENT && b->node->ns == OS64_HTML_NS_HTML &&
           (b->node->tag == OS64_HTML_TAG_IFRAME || b->node->tag == OS64_HTML_TAG_FRAME);
}

// A canvas that is a replaced box (libflow makes it one where scripting is
// on): nothing here draws what its script draws.
static bool is_canvas(const flow_box_t *b)
{
    return b->node != NULL && b->node->kind == OS64_HTML_ELEMENT && b->node->ns == OS64_HTML_NS_HTML &&
           b->node->tag == OS64_HTML_TAG_CANVAS;
}

// ── The ellipsis (CSS Overflow 4 § 4.1) ─────────────────────────────────
//
// Where a line ends in its block's ellipsis, and what of each box on it is
// kept, is libflow's (flow_line_cut, flow_line_cut_keeps), because the
// rule has two doors: what is not kept is neither drawn here nor hit by
// flow_hit. The painter draws a piece as far as it is kept, leaves out a
// box cut away (flow_box_cut_away: an atom past the limit with everything
// in it, a box stacked out of one), and draws the ellipsis after what is
// kept, on the line's baseline, as the block's text is drawn (text_run:
// its shadows, then its fill), when the walk hands it the line after the
// line's pieces — and in a text clip's pen pass too, where the walk of
// the clip hands it the line (flow_visit_text_clip). Its cuts are kept in
// the paint's memo (Painter.cuts), each line worked once a paint.

static flow_line_cut_t line_cut(const Painter *p, const flow_box_t *line)
{
    return p->cuts != NULL ? flow_cut_memo_get(p->cuts, line) : (flow_line_cut_t){false, 0, 0};
}

// Whether nothing of a box is drawn for a line's ellipsis: the rule
// flow_hit reads too (flow_box_cut_away).
static bool past_ellipsis(const Painter *p, const flow_box_t *b)
{
    return p->cuts != NULL && flow_box_cut_away(p->cuts, b);
}

// What a text's glyphs are filled with: the clip's pen inside a text clip,
// else the style's fill (-webkit-text-fill-color, or the colour), as the
// page's ink.
static uint32_t text_fill(const Painter *p, const flow_style_t *s)
{
    return p->penned ? p->pen : ink_of(p, s->fill);
}

// A text box's run with its pen at (`x`, `y`), the baseline's start in the
// coordinates being painted: its shadows under it, the last first — the
// run again in each shadow's colour at its offset, sharp whatever its blur
// (PILE3.md § Booked) — and then the run in its fill (text_fill). A text
// clip's pen draws the run alone. The page's text and a line's ellipsis
// both go through it.
static void text_run(const Painter *p, const flow_box_t *b, int64_t x, int64_t y)
{
    os64_gui_rect_t view = on_page(p, p->view);
    for (int32_t i = b->style->ntext_shadows - 1; !p->penned && i >= 0; i--) {
        const flow_shadow_t *sh = &b->style->text_shadows[i];
        p->v->text(p->v->ctx, b, (int32_t)max64(INT32_MIN, min64(INT32_MAX, x + sh->x)),
                   (int32_t)max64(INT32_MIN, min64(INT32_MAX, y + sh->y)), view,
                   sh->current ? ink_of(p, b->style->color) : sh->colour);
    }
    p->v->text(p->v->ctx, b, (int32_t)max64(INT32_MIN, min64(INT32_MAX, x)),
               (int32_t)max64(INT32_MIN, min64(INT32_MAX, y)), view, text_fill(p, b->style));
}

// A line's ellipsis, where its cut puts it, drawn as the block's text is
// (text_run): its shadows, and its fill — in the pen alone when a text
// clip's pass draws it.
static void ellipsis(const Painter *p, const flow_box_t *line)
{
    flow_line_cut_t c = line_cut(p, line);
    if (!c.on)
        return;
    const flow_box_t *block = line->parent;
    flow_box_t e = *line;
    e.kind = FLOW_BOX_TEXT;
    e.style = block->style;
    e.run = block->ellipsis;
    e.text = NULL;
    e.length = 0;
    text_run(p, &e, c.at + p->off.x, (int64_t)line->baseline + p->off.y);
}

// A stand-in is a pale panel, light grey; a frame's reads as a link on it,
// in the default link colour, and a canvas's as plain grey text. All are
// moved for dark pages like the page's own.
#define FRAME_INK 0x0000eeu
#define CANVAS_INK 0x505050u
#define STAND_IN_PAPER 0xf0f0f0u

// The painter for one box, in its own coordinates; false when nothing of
// it is to be drawn. The box's own coordinates: a fixed one's view is the
// page's less the scroll, a sticky one's less its push, and the painter's
// shift (a group drawn upright) applied. A box an ancestor's `overflow`
// clips is drawn only inside the clip, asked in the coordinates being
// painted — what clips a sticky box from outside stays put as the box
// moves — and every verb is handed the view, so the view is narrowed.
static bool box_painter(const Painter *outer, const flow_box_t *b, Painter *out)
{
    if (b->style->visibility != FLOW_VISIBLE || outer->groups->hidden != 0)
        return false;
    Painter clipped = *outer;
    clipped.off = flow_box_doc_offset(b, outer->scroll);
    clipped.off.x = sum32(clipped.off.x, outer->shift.x);
    clipped.off.y = sum32(clipped.off.y, outer->shift.y);
    os64_gui_rect_t v = outer->view;
    if (b->clipped) {
        os64_gui_rect_t c = flow_box_doc_clip(b, outer->scroll);
        c.x = sum32(c.x, outer->shift.x);
        c.y = sum32(c.y, outer->shift.y);
        int64_t x0 = max64(v.x, c.x), y0 = max64(v.y, c.y);
        int64_t x1 = min64((int64_t)v.x + v.w, (int64_t)c.x + c.w);
        int64_t y1 = min64((int64_t)v.y + v.h, (int64_t)c.y + c.h);
        if (x1 <= x0 || y1 <= y0)
            return false;
        v = (os64_gui_rect_t){(int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0)};
    }
    clipped.view = (os64_gui_rect_t){(int32_t)((int64_t)v.x - clipped.off.x),
                                     (int32_t)((int64_t)v.y - clipped.off.y), v.w, v.h};
    *out = clipped;
    return true;
}

// A box's outline, handed over last in its stacking context
// (flow_visitor_t.outline); none for a box left out of a line's ellipsis.
static void paint_outline(void *ctx, const flow_box_t *b)
{
    Painter p;
    if (box_painter(ctx, b, &p) && !past_ellipsis(&p, b))
        outline(&p, b);
}

static void paint_box(void *ctx, const flow_box_t *b)
{
    Painter clipped;
    if (!box_painter(ctx, b, &clipped))
        return;
    // On a line that ends in an ellipsis, a box is drawn as far as it is
    // kept (flow_line_cut_keeps), and not at all when it is cut away.
    if (past_ellipsis(&clipped, b))
        return;
    if (b->parent != NULL && b->parent->kind == FLOW_BOX_LINE) {
        flow_line_cut_t lc = line_cut(&clipped, b->parent);
        int64_t keep;
        if (lc.on && flow_line_cut_keeps(&lc, b, &keep) &&
            keep < (int64_t)b->rect.x + b->rect.w) {
            int64_t room = keep - clipped.view.x;
            if (room <= 0)
                return;
            clipped.view.w = (int32_t)min64(clipped.view.w, room);
        }
    }
    const Painter *p = &clipped;
    // A text clip's pen draws text alone: the texts, and the ellipsis a
    // line ends in.
    if (p->penned && b->kind != FLOW_BOX_TEXT && b->kind != FLOW_BOX_LINE)
        return;
    switch (b->kind) {
    case FLOW_BOX_LINE:
        // The walk hands a line over only after its pieces, and only when
        // its block has an ellipsis; so does a text clip's.
        ellipsis(p, b);
        return;
    case FLOW_BOX_MARKER:
        if (flow_marker_is_shape(b->style)) {
            bullet(p, b, b->style->list_style_type);
            return;
        }
        // A numbered marker is its text.
        __attribute__((fallthrough));
    case FLOW_BOX_TEXT:
        if (b->run != NULL)
            text_run(p, b, (int64_t)b->rect.x + p->off.x, (int64_t)b->baseline + p->off.y);
        decorations(p, b);
        return;
    case FLOW_BOX_ATOMIC:
    case FLOW_BOX_REPLACED:
        frame(p, b);
        if (b->control >= 0)
            p->v->control(p->v->ctx, b, on_page(p, content_of(b)), on_page(p, p->view));
        else if (is_picture(b))
            p->v->image(p->v->ctx, b, on_page(p, content_of(b)), on_page(p, p->view));
        else if ((is_frame(b) || is_canvas(b)) && p->v->stand_in != NULL)
            p->v->stand_in(p->v->ctx, b, on_page(p, content_of(b)), on_page(p, p->view),
                           ink_of(p, is_frame(b) ? FRAME_INK : CANVAS_INK), paper_of(p, STAND_IN_PAPER));
        return;
    case FLOW_BOX_SPAN:
        // An inline box's borders open on its first piece and close on its
        // last, which the public tree does not say; its background is drawn
        // on each piece (piece_layer), a run cut to the text through a text
        // clip of the piece's own text.
        background_of(p, b, NULL, true);
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

// `r` met with `c`; empty, at `r`'s corner, where they do not meet.
static os64_gui_rect_t cut(os64_gui_rect_t r, os64_gui_rect_t c)
{
    int64_t x0 = max64(r.x, c.x), y0 = max64(r.y, c.y);
    int64_t x1 = min64((int64_t)r.x + r.w, (int64_t)c.x + c.w);
    int64_t y1 = min64((int64_t)r.y + r.h, (int64_t)c.y + c.h);
    return (os64_gui_rect_t){(int32_t)x0, (int32_t)y0, (int32_t)max64(0, x1 - x0),
                             (int32_t)max64(0, y1 - y0)};
}

// [x0, x1) by [y0, y1) held as libflow holds a bounds' rect: each edge to a
// 32-bit coordinate, and one too wide for a 32-bit width held to the middle
// of the range, where any view is.
static os64_gui_rect_t held(int64_t x0, int64_t y0, int64_t x1, int64_t y1)
{
    x0 = max64(x0, INT32_MIN);
    y0 = max64(y0, INT32_MIN);
    x1 = min64(x1, INT32_MAX);
    y1 = min64(y1, INT32_MAX);
    if (x1 - x0 > INT32_MAX) {
        x0 = max64(x0, INT32_MIN / 2);
        x1 = min64(x1, INT32_MAX / 2);
    }
    if (y1 - y0 > INT32_MAX) {
        y0 = max64(y0, INT32_MIN / 2);
        y1 = min64(y1, INT32_MAX / 2);
    }
    return (os64_gui_rect_t){(int32_t)x0, (int32_t)y0, (int32_t)max64(0, x1 - x0),
                             (int32_t)max64(0, y1 - y0)};
}

// `r` one pixel wider all round, held (above).
static os64_gui_rect_t widened(os64_gui_rect_t r)
{
    return held((int64_t)r.x - 1, (int64_t)r.y - 1, (int64_t)r.x + r.w + 1,
                (int64_t)r.y + r.h + 1);
}

// ── The text clip (PILE3.md § The text clip) ─────────────────────────────

// Whether the page wrote anything for a run of layers to draw: its colour,
// a gradient, or a picture a sheet or (on a block's frame) the
// `background` attribute puts there. One with nothing draws nothing through
// a clip either, and is not a headline to rescue (text_clip). It asks what
// was written, not whether a tile, once placed, covers a pixel — a
// picture's only the face could say — so a refused clip over a background
// that places nothing (a zero `background-size`) still rescues the words:
// the degraded drawing's over-count (PILE3.md § The text clip, Booked).
static bool run_paints(const Painter *p, const LayerRun *run)
{
    const flow_style_t *s = run->b->style;
    bool sheet = flow_background_has_image(s);
    for (int32_t i = run->from; i >= run->to; i--) {
        if (i == run->n) {
            if (s->has_background)
                return true;
            continue;
        }
        flow_layer_t l = flow_background_layer(s, i);
        if (l.gradient != NULL)
            return true;
        if ((l.image != NULL || (!run->piece && !sheet && i == 0)) &&
            p->v->backdrop(p->v->ctx, run->b, sheet ? i : -1, NULL, (os64_gui_rect_t){0, 0, 0, 0},
                           0, 0, (os64_gui_rect_t){0, 0, 0, 0}))
            return true;
    }
    return false;
}

// Where a text clip's text is painted, in the coordinates being painted
// (`p`'s, taken back off its box): each TEXT box's overflow rect grown by
// its ink (the bounds contract, PILE3.md § Transforms, the painter).
typedef struct {
    const Painter *p;
    int64_t x0, y0, x1, y1;
} ClipInk;

static void clip_ink(void *ctx, const flow_box_t *t)
{
    ClipInk *k = ctx;
    flow_point_t o = flow_box_doc_offset(t, k->p->scroll);
    int64_t ox = (int64_t)o.x + k->p->shift.x, oy = (int64_t)o.y + k->p->shift.y;
    os64_gui_rect_t r = t->overflow;
    int64_t x0 = ox + r.x - t->ink[FLOW_LEFT], y0 = oy + r.y - t->ink[FLOW_TOP];
    int64_t x1 = ox + r.x + r.w + t->ink[FLOW_RIGHT];
    int64_t y1 = oy + r.y + r.h + t->ink[FLOW_BOTTOM];
    if (x1 <= x0 || y1 <= y0)
        return;
    k->x0 = min64(k->x0, x0);
    k->y0 = min64(k->y0, y0);
    k->x1 = max64(k->x1, x1);
    k->y1 = max64(k->y1, y1);
}

static void pen_text(void *ctx, const flow_box_t *t)
{
    paint_box(ctx, t);
}

// The one colour a refused clip's text is drawn in: the background's colour
// if it has one, else the middle of the run's top gradient, else the text's
// colour — each at full opacity, since a transparent one is the case being
// rescued — moved as ink for dark pages.
static uint32_t clip_solid(const Painter *p, const LayerRun *run)
{
    const flow_style_t *s = run->b->style;
    uint32_t c = s->color;
    if (s->has_background) {
        c = s->background;
    } else {
        for (int32_t i = run->to; i <= run->from && i < run->n; i++) {
            const flow_gradient_t *g = flow_background_layer(s, i).gradient;
            Stops stops;
            if (g != NULL && stops_resolve(g, 100, s->color, &stops)) {
                c = stops_colour(&stops, 0.5, false);
                break;
            }
        }
    }
    return ink_of(p, c & 0xffffffu);
}

// A run of a box's layers cut to its text (Backgrounds 4 § 2.8): the text
// libflow says the clip holds (flow_visit_text_clip) is painted in white
// into the face's mask — by paint_box itself, with a pen, so the mask is
// the text's own coverage — then the layers into the clip's group, which
// the face lays over what is under it at each pixel's coverage. A face that
// cannot (no such verbs, past its limits, no memory) gets the text in one
// solid colour instead (clip_solid), and so does a clip whose layers found
// no memory once it was open, over what it laid: never the layers as a box
// over transparent text, nor a headline whose words are gone. A run that
// draws nothing (run_paints) makes no clip and rescues nothing.
static void text_clip(const Painter *p, const LayerRun *run)
{
    if (!run_paints(p, run))
        return;
    ClipInk ink = {p, INT64_MAX, INT64_MAX, INT64_MIN, INT64_MIN};
    flow_visit_text_clip(run->b, clip_ink, &ink);
    if (ink.x1 <= ink.x0 || ink.y1 <= ink.y0)
        return;
    os64_gui_rect_t bounds = cut(cut(held(ink.x0, ink.y0, ink.x1, ink.y1), on_page(p, run->b->rect)),
                                 on_page(p, p->view));
    if (bounds.w <= 0 || bounds.h <= 0)
        return;
    // The pen paints in the coordinates being painted, as the walk does:
    // its view is this box's taken back off it, cut to the border box the
    // clip is cut to.
    Painter pen = *p;
    pen.view = cut(on_page(p, p->view), on_page(p, run->b->rect));
    pen.penned = true;
    if (p->v->clip_open != NULL && p->v->clip_fill != NULL && p->v->clip_close != NULL &&
        p->v->clip_open(p->v->ctx, bounds)) {
        pen.pen = 0x00ffffffu;
        flow_visit_text_clip(run->b, pen_text, &pen);
        p->v->clip_fill(p->v->ctx);
        bool drawn = run_layers(p, run);
        p->v->clip_close(p->v->ctx);
        if (drawn)
            return;
    }
    pen.pen = clip_solid(p, run);
    flow_visit_text_clip(run->b, pen_text, &pen);
}

// The pixels corner `c` of `s` (Round's order) can curve through: the
// rectangle of its two radii, empty for a square corner.
static os64_gui_rect_t corner_square(const Round *s, int c)
{
    int64_t rx = s->rad[c][0], ry = s->rad[c][1];
    if (rx <= 0 || ry <= 0)
        return (os64_gui_rect_t){0, 0, 0, 0};
    int64_t x0 = c == 0 || c == 3 ? s->l : s->r - rx, y0 = c < 2 ? s->t : s->b - ry;
    return held(floor_div(x0, SUB), floor_div(y0, SUB), -floor_div(-(x0 + rx), SUB),
                -floor_div(-(y0 + ry), SUB));
}

static bool meets(os64_gui_rect_t a, os64_gui_rect_t b)
{
    os64_gui_rect_t m = cut(a, b);
    return m.w > 0 && m.h > 0;
}

// Picture layer `layer` behind `b` across `part` of its area: tiled as
// `origin` places it, or from the area's corner (`ox`, `oy`) for an
// attribute's, wherever the part is.
static void backdrop_part(const Painter *p, const flow_box_t *b, int32_t layer,
                          os64_gui_rect_t part, os64_gui_rect_t origin, int32_t ox, int32_t oy)
{
    os64_gui_rect_t on = on_page(p, part);
    (void)p->v->backdrop(p->v->ctx, b, layer, &on, origin, ox, oy, on_page(p, p->view));
}

// One corner's square `sq` of a rounded picture layer (round_backdrop):
// through a clip whose mask is the shape's coverage there, painted as
// round_background paints the shape's colour, so the picture's edge is the
// colour's. A clip the verbs cannot make (none, no memory, too large, one
// already open) leaves each row's pixels whose middle the shape holds.
static void corner_backdrop(const Painter *p, const flow_box_t *b, const Round *shape,
                            int32_t layer, os64_gui_rect_t sq, os64_gui_rect_t origin, int32_t ox,
                            int32_t oy)
{
    if (p->v->clip_open != NULL && p->v->clip_fill != NULL && p->v->clip_close != NULL &&
        p->v->clip_open(p->v->ctx, on_page(p, sq))) {
        Painter mask = *p;
        mask.view = sq;
        round_background(&mask, b, shape, 0x00ffffffu);
        p->v->clip_fill(p->v->ctx);
        backdrop_part(p, b, layer, sq, origin, ox, oy);
        p->v->clip_close(p->v->ctx);
        return;
    }
    for (int64_t y = sq.y; y < (int64_t)sq.y + sq.h; y++) {
        int64_t a, e;
        if (!round_row(shape, y * SUB + SUB / 2, &a, &e))
            continue;
        // Pixel x's middle, x * SUB + SUB / 2, in [a, e).
        int64_t x0 = max64(sq.x, -floor_div(SUB / 2 - a, SUB));
        int64_t x1 = min64((int64_t)sq.x + sq.w, -floor_div(SUB / 2 - e, SUB));
        if (x1 > x0)
            backdrop_part(p, b, layer, held(x0, y, x1, y + 1), origin, ox, oy);
    }
}

// A picture layer cut to `shape`, the layer's clip box `area` rounded as
// the box is (frame_layer). Only a corner's square holds pixels the shape
// leaves out or covers in part, so the rest of the area is drawn as it is,
// in bands of rows each side's squares narrow, and each square through
// corner_backdrop. Squares that meet — radii past the middle — are one
// square, the whole area. Only what meets the view is drawn, and nothing
// at all, no clip made, when the verbs say no picture is there.
static void round_backdrop(const Painter *p, const flow_box_t *b, const Round *shape,
                           int32_t layer, os64_gui_rect_t area, os64_gui_rect_t origin)
{
    os64_gui_rect_t corner = on_page(p, area);
    if (!p->v->backdrop(p->v->ctx, b, layer, NULL, origin, corner.x, corner.y,
                        on_page(p, p->view)))
        return;
    os64_gui_rect_t sq[4];
    for (int c = 0; c < 4; c++)
        sq[c] = cut(corner_square(shape, c), area);
    bool met = false;
    for (int c = 0; c < 4; c++)
        for (int d = c + 1; d < 4; d++)
            met = met || meets(sq[c], sq[d]);
    if (met) {
        sq[0] = area;
        sq[1] = sq[2] = sq[3] = (os64_gui_rect_t){0, 0, 0, 0};
    }
    // The bands: the rows between one square's top or bottom and the next.
    int64_t at[10];
    int n = 0;
    at[n++] = area.y;
    at[n++] = (int64_t)area.y + area.h;
    for (int c = 0; c < 4; c++) {
        if (sq[c].w > 0 && sq[c].h > 0) {
            at[n++] = sq[c].y;
            at[n++] = (int64_t)sq[c].y + sq[c].h;
        }
    }
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && at[j - 1] > at[j]; j--) {
            int64_t t = at[j];
            at[j] = at[j - 1];
            at[j - 1] = t;
        }
    for (int i = 0; i + 1 < n; i++) {
        if (at[i + 1] <= at[i])
            continue;
        // A square narrows the band from the side it starts at.
        int64_t x0 = area.x, x1 = (int64_t)area.x + area.w;
        for (int c = 0; c < 4; c++) {
            if (sq[c].w <= 0 || sq[c].h <= 0 || sq[c].y > at[i] ||
                (int64_t)sq[c].y + sq[c].h < at[i + 1])
                continue;
            if (sq[c].x == area.x)
                x0 = max64(x0, (int64_t)sq[c].x + sq[c].w);
            else
                x1 = min64(x1, sq[c].x);
        }
        os64_gui_rect_t band = cut(held(x0, at[i], x1, at[i + 1]), p->view);
        if (x1 > x0 && band.w > 0 && band.h > 0)
            backdrop_part(p, b, layer, band, origin, corner.x, corner.y);
    }
    for (int c = 0; c < 4; c++) {
        os64_gui_rect_t seen = cut(sq[c], p->view);
        if (seen.w > 0 && seen.h > 0)
            corner_backdrop(p, b, shape, layer, seen, origin, corner.x, corner.y);
    }
}

static os64_gui_rect_t shifted(os64_gui_rect_t r, flow_point_t by)
{
    return (os64_gui_rect_t){sum32(r.x, by.x), sum32(r.y, by.y), r.w, r.h};
}

// Whether the verbs composite a group at its opacity: both, or neither.
static bool opacity_verbs(const Painter *p)
{
    return p->v->group_open != NULL && p->v->group_close != NULL;
}

// A group (flow_visit_groups) opens. One below full opacity hands the
// verbs its bounds, cut to what is painted, to remember what was under
// them. One DRAWN THROUGH its matrix (flow_box_through) is asked of the
// verbs whole: its bounds — in its own coordinates, cut to the view taken
// back through the matrix, which is all of it that can show — the matrix
// to the coordinates being painted, and the clip it is laid inside. Laid
// so, what is inside is painted in its own coordinates. One none of which
// can show — out of view, or a matrix that collapses an axis — is not
// painted at all. A face that cannot lay it (no verb, no memory) has it
// drawn upright, where its origin goes, the shift taking every box there,
// and under its opacity as an ordinary group. Past the deepest kept, an
// opacity group asks nothing of the verbs and is drawn opaque where it is,
// and a group drawn through its matrix is not painted at all: nothing
// remembers where to put it back, and the walk culled it for the turned
// and the upright drawings, not this one.
static void group_open(void *ctx, const flow_box_t *box, os64_gui_rect_t bounds)
{
    Painter *p = ctx;
    PaintGroups *gs = p->groups;
    int32_t d = gs->depth++;
    if (d >= PAINT_GROUPS) {
        if (gs->hidden != 0)
            return;
        if (flow_box_through(box, p->scroll, (double[6]){0}, NULL, NULL))
            gs->hidden = d + 1;
        else if (opacity_verbs(p))
            p->v->group_open(p->v->ctx, cut(shifted(bounds, p->shift), p->view));
        return;
    }
    gs->open[d].view = p->view;
    gs->open[d].shift = p->shift;
    gs->open[d].kind = G_ASKED;
    double m[6];
    os64_gui_rect_t clip;
    flow_point_t upright;
    if (flow_box_through(box, p->scroll, m, &clip, &upright)) {
        m[4] += p->shift.x;
        m[5] += p->shift.y;
        clip = cut(shifted(clip, p->shift), p->view);
        os64_gui_rect_t back = flow_affine_rect_back(m, clip);
        os64_gui_rect_t own = cut(bounds, back);
        // Nothing of it shows — a matrix that collapses an axis (scaleX(0),
        // rotateX(90deg)) draws nothing, and the rest may be out of view —
        // so nothing is painted: the view is empty until its close.
        if (own.w <= 0 || own.h <= 0) {
            p->view = (os64_gui_rect_t){0, 0, 0, 0};
            return;
        }
        // What shows, one pixel wider all round: a bilinear sample at the
        // edge of the clip mixes the source pixel beyond it.
        own = cut(bounds, widened(back));
        if (p->v->through_open != NULL && p->v->through_close != NULL &&
            p->v->through_open(p->v->ctx, own, m, clip)) {
            gs->open[d].kind = G_THROUGH;
            p->view = own;
            p->shift = (flow_point_t){0, 0};
            return;
        }
        // Drawn upright instead, still inside what clips it from outside,
        // which no box inside will meet (flow_box_doc_clip stops here).
        p->view = clip;
        p->shift.x = sum32(p->shift.x, upright.x);
        p->shift.y = sum32(p->shift.y, upright.y);
        if (box->style->opacity >= 1000)
            return;
    }
    if (opacity_verbs(p)) {
        gs->open[d].kind = G_OPACITY;
        p->v->group_open(p->v->ctx, cut(shifted(bounds, p->shift), p->view));
    }
}

// Closes: what it painted is laid over what was under it at the group's
// opacity, in 255ths — through its matrix, when it was laid so — and the
// view and the shift are what they were.
static void group_close(void *ctx, const flow_box_t *box)
{
    Painter *p = ctx;
    PaintGroups *gs = p->groups;
    int32_t d = --gs->depth;
    uint8_t alpha = (uint8_t)(((uint32_t)box->style->opacity * 255 + 500) / 1000);
    if (d >= PAINT_GROUPS) {
        if (gs->hidden != 0) {
            if (gs->hidden == d + 1)
                gs->hidden = 0;
            return;
        }
        if (!flow_box_through(box, p->scroll, (double[6]){0}, NULL, NULL) && opacity_verbs(p))
            p->v->group_close(p->v->ctx, alpha);
        return;
    }
    if (gs->open[d].kind == G_THROUGH)
        p->v->through_close(p->v->ctx, alpha);
    else if (gs->open[d].kind == G_OPACITY)
        p->v->group_close(p->v->ctx, alpha);
    p->view = gs->open[d].view;
    p->shift = gs->open[d].shift;
}

// Whether a box has any background: a colour, a gradient, or a picture
// behind it.
static bool has_background(const Painter *p, const flow_box_t *b)
{
    return b->style->has_background || flow_background_has_image(b->style) ||
           p->v->backdrop(p->v->ctx, b, -1, NULL, (os64_gui_rect_t){0}, 0, 0, p->view);
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
                  uint32_t paper, const yonder_dark_t *dark, yonder_paint_cache_t *cache,
                  yonder_scratch_t *scratch, const yonder_verbs_t *verbs)
{
    PaintGroups groups = {.depth = 0};
    flow_cut_memo_t cuts;
    flow_cut_memo_open(&cuts, tree);
    Painter p = {.v = verbs, .view = viewport, .scroll = scroll, .groups = &groups, .dark = dark,
                 .cache = cache, .scratch = scratch, .cuts = &cuts};
    if (cache != NULL)
        paint_cache_begin(cache);
    if (dark != NULL)
        paper = dark->paper;
    const flow_box_t *root = flow_root(tree);
    if (root != NULL)
        p.canvas_owner = canvas_owner(&p, root);
    const flow_box_t *owner = p.canvas_owner;
    // A translucent canvas colour is laid over the paper: under the page
    // there is nothing else.
    uint32_t canvas = owner != NULL && owner->style->has_background ? paper_of(&p, owner->style->background)
                                                                    : paper;
    if (flow_alpha(canvas) != 255)
        fill(&p, viewport.x, viewport.y, viewport.w, viewport.h, paper);
    fill(&p, viewport.x, viewport.y, viewport.w, viewport.h, canvas);
    // The canvas's layers, from the bottom up, across the whole view, each
    // placed in the ROOT's origin box, whichever element they came from
    // (Backgrounds 3 § 2.11.2: the body's background is drawn as if the
    // root had it), so they scroll with the page and a page shorter than
    // the view repeats them, as the browsers do; an attribute's picture is
    // tiled from the page's own corner. They are drawn across the whole
    // view whatever each layer's background-clip says, `text` included: the
    // canvas's painting area IS the canvas (§ 2.11.1), and Chrome fills it.
    bool sheet = owner != NULL && flow_background_has_image(owner->style);
    for (int32_t i = owner != NULL ? flow_background_layers(owner->style) - 1 : -1; i >= 0; i--) {
        flow_layer_t l = flow_background_layer(owner->style, i);
        os64_gui_rect_t origin = yonder_box_edge(root, root->rect, l.origin);
        yonder_tile_t tile;
        if (l.gradient != NULL) {
            if (yonder_background_tile(&l, origin, 0, 0, &tile))
                (void)gradient(&p, l.gradient, &tile, viewport, NULL, false, owner->style->color);
        } else if (l.image != NULL || (!sheet && i == 0)) {
            (void)verbs->backdrop(verbs->ctx, owner, sheet ? i : -1, &viewport, origin, 0, 0,
                                  viewport);
        }
    }
    if (root != NULL) {
        flow_visitor_t v = {paint_box, group_open, group_close, &p, paint_outline};
        flow_visit_groups(tree, viewport, scroll, &v);
    }
    yonder_paint_cuts += cuts.worked;
    flow_cut_memo_close(&cuts);
}
