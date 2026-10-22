// computed.c — an element's style as a script reads it (CSSOM §
// getComputedStyle): each of libgarb's properties as its RESOLVED value,
// serialised as the browsers serialise it. The resolved value is the
// computed one, but for the properties CSSOM names — the sizes, the
// margins and padding, the insets of a positioned box, a line height that
// is not `normal`, a transform and its origin — which read the laid-out
// box when the element has one. Everything here is read from the style
// pass 1 made (flow_style_t, in device pixels at the layout's zoom) and
// written in CSS pixels.

#include <float.h>

#include "garb/cascade.h"
#include "garb/values.h"
#include "internal.h"
#include "os64/fmt.h"
#include "os64/mem.h"
#include "os64/str.h"

struct flow_computed {
    const flow_tree_t *tree;
    flow_env_t env;                 // the caller's, its timeline taken away
    flow_computed_options_t options;
    FStyles *extra;                 // f_style_beyond's, when pass 1 did not style it
    const os64_html_node_t *element;
    const flow_style_t *style;      // NULL: it has no computed style
    const flow_box_t *box;          // its first box; NULL for none
    double zoom;                    // device pixels per CSS pixel
};

flow_computed_t *flow_computed_new(const flow_tree_t *tree, const flow_env_t *env,
                                   const os64_html_node_t *element,
                                   const flow_computed_options_t *options)
{
    if (tree == NULL || env == NULL || element == NULL)
        return NULL;
    flow_computed_t *c = os64_calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;
    c->tree = tree;
    c->env = *env;
    c->env.timeline = NULL;
    if (options != NULL)
        c->options = *options;
    c->element = element;
    c->zoom = env->zoom != 0 ? env->zoom / 1000.0 : 1.0;
    const FStyles *styles = f_tree_styles(tree);
    const FStyled *own = f_style_of(styles, element);
    if (own == NULL && element->kind == OS64_HTML_ELEMENT && styles != NULL) {
        c->extra = os64_calloc(1, sizeof(*c->extra));
        if (c->extra == NULL) {
            os64_free(c);
            return NULL;
        }
        c->extra->doc = styles->doc;
        c->extra->env = &c->env;
        c->extra->arena.cap = SIZE_MAX;
        c->extra->lists_left = F_STYLE_LISTS_BUDGET;
        own = f_style_beyond(styles, c->extra, flow_model(tree), element);
        // An element whose ancestors reach a styled one and still came back
        // without a style ran out of memory on the way.
        const os64_html_node_t *up = element->parent;
        while (up != NULL && up->kind == OS64_HTML_ELEMENT && f_style_of(styles, up) == NULL)
            up = up->parent;
        if (own == NULL && up != NULL && up->kind == OS64_HTML_ELEMENT) {
            flow_computed_free(c);
            return NULL;
        }
    }
    c->style = own != NULL ? &own->style : NULL;
    c->box = own != NULL ? flow_box_for(tree, element) : NULL;
    return c;
}

bool flow_computed_styled(const flow_computed_t *c)
{
    return c != NULL && c->style != NULL;
}

void flow_computed_free(flow_computed_t *c)
{
    if (c == NULL)
        return;
    f_style_free(c->extra);
    os64_free(c);
}

// ── Writing ─────────────────────────────────────────────────────────────

typedef struct {
    char *out;
    size_t cap, len;
} Out;

static void put(Out *o, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++, o->len++)
        if (o->len + 1 < o->cap)
            o->out[o->len] = s[i];
    if (o->cap > 0)
        o->out[o->len < o->cap ? o->len : o->cap - 1] = '\0';
}

static void puts_(Out *o, const char *s)
{
    put(o, s, os64_strlen(s));
}

static void number(Out *o, double v)
{
    char b[DBL_MAX_10_EXP + 24];
    garb_css_number(v, b, sizeof(b));
    puts_(o, b);
}

static void integer(Out *o, int64_t v)
{
    char b[24];
    os64_snprintf(b, sizeof(b), "%ld", (long)v);
    puts_(o, b);
}

// Device pixels as CSS pixels.
static void px(const flow_computed_t *c, Out *o, double device)
{
    number(o, device / c->zoom);
    puts_(o, "px");
}

static void units(const flow_computed_t *c, Out *o, int64_t u)
{
    px(c, o, (double)u / FLOW_UNITS_PER_PX);
}

// A length as computed: a percentage stays one (with what a calc() added
// to it), the rest are pixels or keywords.
static void length(const flow_computed_t *c, Out *o, flow_length_t l, const char *auto_word)
{
    switch (l.kind) {
    case FLOW_LENGTH_PX: units(c, o, l.value); return;
    case FLOW_LENGTH_PERCENT:
        if (l.offset != 0) {
            puts_(o, "calc(");
            number(o, l.value / (double)FLOW_UNITS_PER_PX);
            puts_(o, l.offset < 0 ? "% - " : "% + ");
            units(c, o, l.offset < 0 ? -(int64_t)l.offset : l.offset);
            puts_(o, ")");
            return;
        }
        number(o, l.value / (double)FLOW_UNITS_PER_PX);
        puts_(o, "%");
        return;
    case FLOW_LENGTH_FIT_CONTENT: puts_(o, "fit-content"); return;
    case FLOW_LENGTH_CONTENT: puts_(o, "content"); return;
    default: puts_(o, auto_word); return;
    }
}

// sRGB as CSS Color 4 serialises it: rgb() when opaque, else rgba() with
// the alpha the fewest decimals that come back to the same eighth bit.
static void colour(Out *o, uint32_t c)
{
    uint8_t a = flow_alpha(c);
    char b[64];
    unsigned r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, bl = c & 0xFF;
    if (a == 255) {
        os64_snprintf(b, sizeof(b), "rgb(%u, %u, %u)", r, g, bl);
        puts_(o, b);
        return;
    }
    os64_snprintf(b, sizeof(b), "rgba(%u, %u, %u, ", r, g, bl);
    puts_(o, b);
    double alpha = a / 255.0;
    double scale = 100;
    double rounded = (double)(int64_t)(alpha * scale + 0.5) / scale;
    if ((int)(rounded * 255 + 0.5) != a)
        rounded = (double)(int64_t)(alpha * 1000 + 0.5) / 1000;
    number(o, rounded);
    puts_(o, ")");
}

// ── Keywords, in the order of libflow's enums ───────────────────────────

static const char *const kDisplay[] = {
    "inline", "block", "list-item", "inline-block", "table", "table-caption",
    "table-row-group", "table-header-group", "table-footer-group", "table-row",
    "table-cell", "table-column-group", "table-column", "flex", "inline-flex", "grid",
    "inline-grid", "contents", "none",
};
static const char *const kBorder[] = {"none", "hidden", "solid", "inset", "outset",
                                      "groove", "ridge", "dotted", "dashed", "double"};
// The HTML alignments are Chrome's -webkit- keywords, which is how a
// `<center>` reads to a script; `left` is the initial value's `start`,
// which is what the two share here.
static const char *const kAlign[] = {"start", "right", "center", "justify", "-webkit-left",
                                     "-webkit-right", "-webkit-center", "justify"};
static const char *const kValign[] = {"baseline", "sub", "super", "top", "text-top",
                                      "middle", "bottom", "middle"};
static const char *const kWhiteSpace[] = {"normal", "pre", "nowrap", "pre-wrap", "pre-line"};
static const char *const kTransform[] = {"none", "uppercase", "lowercase", "capitalize"};
static const char *const kList[] = {"disc", "circle", "square", "decimal", "lower-alpha",
                                    "upper-alpha", "lower-roman", "upper-roman",
                                    "disclosure-closed", "disclosure-open", "none"};
static const char *const kVisibility[] = {"visible", "hidden", "collapse"};
static const char *const kFloat[] = {"none", "left", "right"};
static const char *const kClear[] = {"none", "left", "right", "both"};
static const char *const kPosition[] = {"static", "relative", "absolute", "fixed", "sticky"};
static const char *const kOverflow[] = {"visible", "hidden", "clip", "scroll", "auto"};
static const char *const kDirection[] = {"row", "row-reverse", "column", "column-reverse"};
static const char *const kWrap[] = {"nowrap", "wrap", "wrap-reverse"};
static const char *const kPlace[] = {"normal", "auto", "stretch", "flex-start", "flex-end",
                                     "center", "baseline", "start", "end", "self-start",
                                     "self-end", "left", "right", "space-between",
                                     "space-around", "space-evenly"};
static const char *const kRepeat[] = {"repeat", "repeat-x", "repeat-y", "no-repeat"};
static const char *const kEdge[] = {"border-box", "padding-box", "content-box", "text"};
static const char *const kGeneric[] = {"serif", "sans-serif", "monospace"};
static const char *const kAnimDirection[] = {"normal", "reverse", "alternate", "alternate-reverse"};
static const char *const kFill[] = {"none", "forwards", "backwards", "both"};
static const char *const kJump[] = {"jump-end", "jump-start", "jump-none", "jump-both"};
static const char *const kCursor[] = {
    "auto", "default", "none", "context-menu", "help", "pointer", "progress", "wait", "cell",
    "crosshair", "text", "vertical-text", "alias", "copy", "move", "no-drop", "not-allowed", "grab",
    "grabbing", "e-resize", "n-resize", "ne-resize", "nw-resize", "s-resize", "se-resize", "sw-resize",
    "w-resize", "ew-resize", "ns-resize", "nesw-resize", "nwse-resize", "col-resize", "row-resize",
    "all-scroll", "zoom-in", "zoom-out"};
static const char *const kUserSelect[] = {"auto", "text", "none", "contain", "all"};
static const char *const kWordBreak[] = {"normal", "keep-all", "break-all", "break-word"};
static const char *const kOverflowWrap[] = {"normal", "break-word", "anywhere"};
static const char *const kTextOverflow[] = {"clip", "ellipsis"};
static const char *const kObjectFit[] = {"fill", "contain", "cover", "none", "scale-down"};

// One name per value: an enum that grows without its table stops the build.
_Static_assert(F_ARRAY(kDisplay) == FLOW_DISPLAY_NONE + 1, "kDisplay");
_Static_assert(F_ARRAY(kBorder) == FLOW_BORDER_DOUBLE + 1, "kBorder");
_Static_assert(F_ARRAY(kAlign) == FLOW_ALIGN_HTML_JUSTIFY + 1, "kAlign");
_Static_assert(F_ARRAY(kValign) == FLOW_VALIGN_HTML_MIDDLE + 1, "kValign");
_Static_assert(F_ARRAY(kWhiteSpace) == FLOW_WS_PRE_LINE + 1, "kWhiteSpace");
_Static_assert(F_ARRAY(kTransform) == FLOW_TRANSFORM_CAPITALIZE + 1, "kTransform");
_Static_assert(F_ARRAY(kList) == FLOW_LIST_NONE + 1, "kList");
_Static_assert(F_ARRAY(kVisibility) == FLOW_COLLAPSE + 1, "kVisibility");
_Static_assert(F_ARRAY(kFloat) == FLOW_FLOAT_RIGHT + 1, "kFloat");
_Static_assert(F_ARRAY(kClear) == FLOW_CLEAR_BOTH + 1, "kClear");
_Static_assert(F_ARRAY(kPosition) == FLOW_POSITION_STICKY + 1, "kPosition");
_Static_assert(F_ARRAY(kOverflow) == FLOW_OVERFLOW_AUTO + 1, "kOverflow");
_Static_assert(F_ARRAY(kDirection) == FLOW_FLEX_COLUMN_REVERSE + 1, "kDirection");
_Static_assert(F_ARRAY(kWrap) == FLOW_FLEX_WRAP_REVERSE + 1, "kWrap");
_Static_assert(F_ARRAY(kPlace) == FLOW_PLACE_SPACE_EVENLY + 1, "kPlace");
_Static_assert(F_ARRAY(kRepeat) == FLOW_NO_REPEAT + 1, "kRepeat");
_Static_assert(F_ARRAY(kEdge) == FLOW_EDGE_TEXT + 1, "kEdge");
_Static_assert(F_ARRAY(kGeneric) == FLOW_GENERIC_MONO + 1, "kGeneric");
_Static_assert(F_ARRAY(kAnimDirection) == FLOW_DIRECTION_ALTERNATE_REVERSE + 1, "kAnimDirection");
_Static_assert(F_ARRAY(kFill) == FLOW_FILL_BOTH + 1, "kFill");
_Static_assert(F_ARRAY(kJump) == FLOW_JUMP_BOTH + 1, "kJump");
_Static_assert(F_ARRAY(kCursor) == FLOW_CURSOR_ZOOM_OUT + 1, "kCursor");
_Static_assert(F_ARRAY(kUserSelect) == FLOW_USER_SELECT_ALL + 1, "kUserSelect");
_Static_assert(F_ARRAY(kWordBreak) == FLOW_WORD_BREAK_BREAK_WORD + 1, "kWordBreak");
_Static_assert(F_ARRAY(kOverflowWrap) == FLOW_OVERFLOW_WRAP_ANYWHERE + 1, "kOverflowWrap");
_Static_assert(F_ARRAY(kTextOverflow) == FLOW_TEXT_OVERFLOW_ELLIPSIS + 1, "kTextOverflow");
_Static_assert(F_ARRAY(kObjectFit) == FLOW_OBJECT_SCALE_DOWN + 1, "kObjectFit");

// `table[v]`, or "" for a value past the table (a style is only ever
// written by libflow, so that is a broken invariant, not a page's doing).
static void table_word(Out *o, const char *const *table, int32_t n, int32_t v)
{
    puts_(o, v >= 0 && v < n ? table[v] : "");
}
#define WORD(o, table, v) table_word((o), (table), F_ARRAY(table), (int32_t)(v))

// ── The box, for the resolved values ────────────────────────────────────

// Whether the element is RENDERED as CSSOM means it: it has a box, and its
// display makes one.
static bool rendered(const flow_computed_t *c)
{
    return c->box != NULL && c->style->display != FLOW_DISPLAY_NONE &&
           c->style->display != FLOW_DISPLAY_CONTENTS;
}

// Whether width and height APPLY to its box: every box but an inline one
// that is not replaced, whose size its line gives it.
static bool sized(const flow_computed_t *c)
{
    return rendered(c) && c->box->kind != FLOW_BOX_SPAN && c->box->kind != FLOW_BOX_TEXT &&
           c->box->kind != FLOW_BOX_LINE;
}

// The box a percentage of the element's resolves against: the nearest
// ancestor box that is a block container, a table's part or an atom — its
// content box. NULL when there is none.
static const flow_box_t *containing(const flow_computed_t *c)
{
    for (const flow_box_t *b = c->box != NULL ? c->box->parent : NULL; b != NULL; b = b->parent)
        if (b->kind != FLOW_BOX_LINE && b->kind != FLOW_BOX_SPAN && b->kind != FLOW_BOX_TEXT &&
            b->kind != FLOW_BOX_MARKER)
            return b;
    return NULL;
}

// A length resolved against `base` device pixels: a percentage of it
// worked out, the rest as computed.
static void used_length(const flow_computed_t *c, Out *o, flow_length_t l, int64_t base,
                        const char *auto_word)
{
    if (l.kind == FLOW_LENGTH_PERCENT && base >= 0) {
        units(c, o, f_len(l, base * FLOW_UNITS_PER_PX));
        return;
    }
    length(c, o, l, auto_word);
}

static int64_t cb_width(const flow_computed_t *c)
{
    const flow_box_t *cb = rendered(c) ? containing(c) : NULL;
    return cb != NULL ? cb->content.w : -1;
}

static int64_t cb_height(const flow_computed_t *c)
{
    const flow_box_t *cb = rendered(c) ? containing(c) : NULL;
    return cb != NULL ? cb->content.h : -1;
}

static bool leaves_flow(const flow_style_t *s)
{
    return s->position == FLOW_POSITION_ABSOLUTE || s->position == FLOW_POSITION_FIXED;
}

// An absolute or fixed box's containing block, in device pixels: its
// nearest positioned ancestor's padding box, or the initial containing
// block (the page's origin, the root's width, the viewport's height) —
// which a fixed box is always in, the viewport's own coordinates being a
// fixed box's (flow_box_t.fixed).
static os64_gui_rect_t positioned_block(const flow_computed_t *c)
{
    const flow_box_t *root = flow_root(c->tree);
    double height = (double)c->env.viewport_height * c->zoom;
    os64_gui_rect_t initial = {0, 0, root != NULL ? 2 * root->rect.x + root->rect.w : 0,
                               (int32_t)(height + 0.5)};
    if (c->style->position == FLOW_POSITION_FIXED)
        return initial;
    for (const flow_box_t *b = c->box->parent; b != NULL; b = b->parent) {
        if (!b->positioned || b->style == NULL)
            continue;
        const flow_unit_t *bw = b->style->border_width;
        int32_t l = (bw[FLOW_LEFT] + 32) / 64, t = (bw[FLOW_TOP] + 32) / 64;
        return (os64_gui_rect_t){b->rect.x + l, b->rect.y + t,
                                 b->rect.w - l - (bw[FLOW_RIGHT] + 32) / 64,
                                 b->rect.h - t - (bw[FLOW_BOTTOM] + 32) / 64};
    }
    return initial;
}

// A length in device pixels, resolved against `base` (a percentage's), or
// `fallback` for one that is `auto`.
static double used_px(flow_length_t l, int64_t base, double fallback)
{
    if (l.kind == FLOW_LENGTH_PX) return (double)l.value / FLOW_UNITS_PER_PX;
    if (l.kind == FLOW_LENGTH_PERCENT && base >= 0) return (double)f_len(l, base * FLOW_UNITS_PER_PX) / FLOW_UNITS_PER_PX;
    return fallback;
}

// A relative or sticky box's offset on an axis, in device pixels, as CSS
// 2.1 § 9.4.3 uses it: an auto inset is the other's negation, both auto
// are 0, and over-constrained the left (top) wins. `first` asks for the
// left or top one.
static double relative_offset(const flow_computed_t *c, bool across, bool first)
{
    const flow_length_t *in = c->style->inset;
    flow_length_t near = in[across ? FLOW_LEFT : FLOW_TOP], far = in[across ? FLOW_RIGHT : FLOW_BOTTOM];
    int64_t base = across ? cb_width(c) : cb_height(c);
    double offset = near.kind != FLOW_LENGTH_AUTO ? used_px(near, base, 0)
                  : far.kind != FLOW_LENGTH_AUTO ? -used_px(far, base, 0) : 0;
    return first ? offset : -offset;
}

// A margin's used value (CSSOM § Resolved values): a percentage of the
// containing block's width, and an `auto` margin across what the box left
// over on that side of where it was placed — in the flow, of its
// containing block's content box, its relative offset taken back; out of
// it, of the containing block past the inset — `auto` anywhere else 0.
static void margin(const flow_computed_t *c, Out *o, int side)
{
    flow_length_t m = c->style->margin[side];
    if (!rendered(c)) {
        length(c, o, m, "auto");
        return;
    }
    if (m.kind != FLOW_LENGTH_AUTO) {
        used_length(c, o, m, leaves_flow(c->style) ? positioned_block(c).w : cb_width(c), "auto");
        return;
    }
    bool across = side == FLOW_LEFT || side == FLOW_RIGHT;
    const flow_box_t *b = c->box;
    double gap = 0;
    if (across && leaves_flow(c->style)) {
        os64_gui_rect_t cb = positioned_block(c);
        flow_length_t at = c->style->inset[side];
        if (at.kind != FLOW_LENGTH_AUTO) {
            double inset_px = used_px(at, cb.w, 0);
            gap = side == FLOW_LEFT ? b->rect.x - cb.x - inset_px
                                    : (double)cb.x + cb.w - ((double)b->rect.x + b->rect.w) - inset_px;
        }
    } else if (across && b->floated == FLOW_FLOAT_NONE && b->kind != FLOW_BOX_ATOMIC) {
        const flow_box_t *cb = containing(c);
        double shift = c->style->position == FLOW_POSITION_RELATIVE || c->style->position == FLOW_POSITION_STICKY
            ? relative_offset(c, true, true) : 0;
        if (cb != NULL)
            gap = side == FLOW_LEFT ? b->rect.x - shift - cb->content.x
                                    : (double)cb->content.x + cb->content.w - (b->rect.x - shift + b->rect.w);
    }
    px(c, o, gap > 0 ? gap : 0);
}

// The used width or height: the content box's, or the border box's when
// box-sizing says the page measures that one.
static void size(const flow_computed_t *c, Out *o, bool across)
{
    const flow_style_t *s = c->style;
    if (!sized(c)) {
        length(c, o, across ? s->width : s->height, "auto");
        return;
    }
    const flow_box_t *b = c->box;
    bool border = s->box_sizing == FLOW_BORDER_BOX;
    int32_t v = across ? (border ? b->rect.w : b->content.w) : (border ? b->rect.h : b->content.h);
    px(c, o, (double)(v > 0 ? v : 0));
}

// An inset (CSSOM § Resolved values): a positioned box's used value. A
// relative one's offsets by CSS 2.1 § 9.4.3 (relative_offset); an absolute
// or fixed one's percentage of its containing block, and its `auto` where
// the box was placed in that block, its margin taken off; a sticky one's
// percentage worked out and its `auto` left; a static box's, and one with
// no box, as computed.
static void inset(const flow_computed_t *c, Out *o, int side)
{
    const flow_style_t *s = c->style;
    flow_length_t l = s->inset[side];
    bool across = side == FLOW_LEFT || side == FLOW_RIGHT;
    if (s->position == FLOW_POSITION_STATIC || !rendered(c)) {
        length(c, o, l, "auto");
        return;
    }
    if (s->position == FLOW_POSITION_RELATIVE) {
        px(c, o, relative_offset(c, across, side == FLOW_LEFT || side == FLOW_TOP));
        return;
    }
    if (!leaves_flow(s)) {
        used_length(c, o, l, across ? cb_width(c) : cb_height(c), "auto");
        return;
    }
    os64_gui_rect_t cb = positioned_block(c);
    if (l.kind != FLOW_LENGTH_AUTO) {
        used_length(c, o, l, across ? cb.w : cb.h, "auto");
        return;
    }
    const flow_box_t *b = c->box;
    double m = used_px(s->margin[side], cb.w, 0);
    double v;
    switch (side) {
    case FLOW_LEFT: v = b->rect.x - cb.x - m; break;
    case FLOW_RIGHT: v = (double)cb.x + cb.w - ((double)b->rect.x + b->rect.w) - m; break;
    case FLOW_TOP: v = b->rect.y - cb.y - m; break;
    default: v = (double)cb.y + cb.h - ((double)b->rect.y + b->rect.h) - m; break;
    }
    px(c, o, v);
}

// ── The lists ───────────────────────────────────────────────────────────

// The names as the page wrote them, then the generic family libflow falls
// back to: the one the page wrote, or, when it wrote none, the one the
// first name guessed (which a browser would not write).
static void families(Out *o, const flow_family_list_t *f)
{
    for (uint32_t i = 0; i < f->count; i++) {
        // A name that is not one identifier is quoted, as Chrome does.
        bool plain = f->names[i].len > 0;
        for (uint32_t k = 0; plain && k < f->names[i].len; k++) {
            char ch = f->names[i].name[k];
            plain = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '-' || ch == '_' ||
                    (k > 0 && ch >= '0' && ch <= '9') || (unsigned char)ch >= 0x80;
        }
        if (!plain)
            puts_(o, "\"");
        for (uint32_t k = 0; k < f->names[i].len; k++) {
            if (!plain && (f->names[i].name[k] == '"' || f->names[i].name[k] == '\\'))
                puts_(o, "\\");
            put(o, &f->names[i].name[k], 1);
        }
        if (!plain)
            puts_(o, "\"");
        puts_(o, ", ");
    }
    WORD(o, kGeneric, f->generic);
}

static void breadth(const flow_computed_t *c, Out *o, const flow_breadth_t *b)
{
    switch (b->kind) {
    case FLOW_TRACK_FIXED: length(c, o, b->len, "auto"); return;
    case FLOW_TRACK_FR: number(o, b->fr / 1000.0); puts_(o, "fr"); return;
    case FLOW_TRACK_MIN_CONTENT: puts_(o, "min-content"); return;
    case FLOW_TRACK_MAX_CONTENT: puts_(o, "max-content"); return;
    default: puts_(o, "auto"); return;
    }
}

static bool breadth_same(const flow_breadth_t *a, const flow_breadth_t *b)
{
    return a->kind == b->kind && a->fr == b->fr && a->len.kind == b->len.kind &&
           a->len.value == b->len.value && a->len.offset == b->len.offset;
}

static void track(const flow_computed_t *c, Out *o, const flow_track_t *t)
{
    if (t->fit) {
        puts_(o, "fit-content(");
        length(c, o, t->max.len, "auto");
        puts_(o, ")");
    } else if (breadth_same(&t->min, &t->max) ||
               (t->min.kind == FLOW_TRACK_AUTO && t->max.kind == FLOW_TRACK_FR)) {
        // A flexible size alone is minmax(auto, it) (Grid 2 § 7.2.4), and is
        // written as itself.
        breadth(c, o, &t->max);
    } else {
        puts_(o, "minmax(");
        breadth(c, o, &t->min);
        puts_(o, ", ");
        breadth(c, o, &t->max);
        puts_(o, ")");
    }
}

// A track list as computed: its tracks, the auto-repeated run in its
// repeat(); `none` when it has none.
static void tracks(const flow_computed_t *c, Out *o, const flow_tracks_t *t, const char *none)
{
    if (t->n <= 0) {
        puts_(o, none);
        return;
    }
    for (int32_t i = 0; i < t->n; i++) {
        if (i > 0)
            puts_(o, " ");
        if (t->repeat_n > 0 && i == t->repeat_at) {
            puts_(o, t->repeat_fit ? "repeat(auto-fit, " : "repeat(auto-fill, ");
            for (int32_t k = 0; k < t->repeat_n && i + k < t->n; k++) {
                if (k > 0)
                    puts_(o, " ");
                track(c, o, &t->tracks[i + k]);
            }
            puts_(o, ")");
            i += t->repeat_n - 1;
            continue;
        }
        track(c, o, &t->tracks[i]);
    }
}

static void areas(Out *o, const flow_style_t *s)
{
    if (s->ngrid_areas <= 0 || s->grid_area_rows <= 0 || s->grid_area_cols <= 0) {
        puts_(o, "none");
        return;
    }
    for (int32_t r = 0; r < s->grid_area_rows; r++) {
        puts_(o, r > 0 ? " \"" : "\"");
        for (int32_t col = 0; col < s->grid_area_cols; col++) {
            if (col > 0)
                puts_(o, " ");
            const flow_grid_area_t *named = NULL;
            for (int32_t a = 0; a < s->ngrid_areas && named == NULL; a++)
                if (r >= s->grid_areas[a].row0 && r < s->grid_areas[a].row1 &&
                    col >= s->grid_areas[a].col0 && col < s->grid_areas[a].col1)
                    named = &s->grid_areas[a];
            if (named != NULL)
                put(o, named->name, named->name_len);
            else
                puts_(o, ".");
        }
        puts_(o, "\"");
    }
}

static void grid_line(Out *o, const flow_grid_line_t *l)
{
    switch (l->kind) {
    case FLOW_GRID_LINE_NUMBER: integer(o, l->n); return;
    case FLOW_GRID_LINE_SPAN: puts_(o, "span "); integer(o, l->n); return;
    case FLOW_GRID_LINE_NAME: put(o, l->name, l->name_len); return;
    default: puts_(o, "auto"); return;
    }
}

// A shadow list: each its colour, then its offsets, blur and — a box's —
// spread, then `inset`, as Chrome writes them.
static void shadows(const flow_computed_t *c, Out *o, const flow_shadow_t *sh, int32_t n, bool box)
{
    if (n <= 0 || sh == NULL) {
        puts_(o, "none");
        return;
    }
    for (int32_t i = 0; i < n; i++) {
        if (i > 0)
            puts_(o, ", ");
        colour(o, sh[i].current ? c->style->color : sh[i].colour);
        int32_t v[4] = {sh[i].x, sh[i].y, sh[i].blur, sh[i].spread};
        for (int k = 0; k < (box ? 4 : 3); k++) {
            puts_(o, " ");
            px(c, o, v[k]);
        }
        if (box && sh[i].inset)
            puts_(o, " inset");
    }
}

static void gradient(const flow_computed_t *c, Out *o, const flow_gradient_t *g)
{
    if (g->repeating)
        puts_(o, "repeating-");
    bool any = false;
    if (g->kind == FLOW_GRADIENT_LINEAR) {
        puts_(o, "linear-gradient(");
        if (g->to_x != 0 || g->to_y != 0) {
            puts_(o, "to");
            if (g->to_y != 0)
                puts_(o, g->to_y < 0 ? " top" : " bottom");
            if (g->to_x != 0)
                puts_(o, g->to_x < 0 ? " left" : " right");
            any = true;
        } else if (g->angle != 180000) {
            number(o, g->angle / 1000.0);
            puts_(o, "deg");
            any = true;
        }
    } else {
        static const char *const kExtent[] = {"farthest-corner", "closest-side", "farthest-side",
                                              "closest-corner", ""};
        _Static_assert(F_ARRAY(kExtent) == FLOW_EXTENT_SIZE + 1, "kExtent");
        puts_(o, "radial-gradient(");
        if (g->circle)
            puts_(o, "circle ");
        if (g->extent == FLOW_EXTENT_SIZE) {
            length(c, o, g->radii[0], "auto");
            if (!g->circle) {
                puts_(o, " ");
                length(c, o, g->radii[1], "auto");
            }
        } else {
            WORD(o, kExtent, g->extent);
        }
        puts_(o, " at ");
        length(c, o, g->centre[0], "auto");
        puts_(o, " ");
        length(c, o, g->centre[1], "auto");
        any = true;
    }
    for (int32_t i = 0; i < g->nstops; i++) {
        const flow_gradient_stop_t *st = &g->stops[i];
        if (any)
            puts_(o, ", ");
        any = true;
        if (!st->hint) {
            colour(o, st->current ? c->style->color : st->colour);
            if (st->at.kind != FLOW_LENGTH_AUTO)
                puts_(o, " ");
        }
        if (st->at.kind != FLOW_LENGTH_AUTO)
            length(c, o, st->at, "");
    }
    puts_(o, ")");
}

static void image(const flow_computed_t *c, Out *o, const flow_bg_image_t *im)
{
    if (im->gradient != NULL) {
        gradient(c, o, im->gradient);
        return;
    }
    if (im->url == NULL) {
        puts_(o, "none");
        return;
    }
    char resolved[2048];
    bool done = c->options.url != NULL &&
                c->options.url(c->options.ctx, im->url, im->len, im->sheet, resolved, sizeof(resolved));
    puts_(o, "url(\"");
    const char *u = done ? resolved : im->url;
    size_t n = done ? os64_strlen(resolved) : im->len;
    for (size_t i = 0; i < n; i++) {
        if (u[i] == '"' || u[i] == '\\')
            puts_(o, "\\");
        put(o, &u[i], 1);
    }
    puts_(o, "\")");
}

// An easing as Chrome writes one: a keyword when it was one, else the
// function.
static void easing(Out *o, const flow_easing_t *e)
{
    if (e->kind == FLOW_EASE_LINEAR) {
        puts_(o, "linear");
        return;
    }
    if (e->kind == FLOW_EASE_CUBIC) {
        static const struct { double p[4]; const char *name; } kNamed[] = {
            {{0.25, 0.1, 0.25, 1}, "ease"}, {{0.42, 0, 1, 1}, "ease-in"},
            {{0, 0, 0.58, 1}, "ease-out"}, {{0.42, 0, 0.58, 1}, "ease-in-out"},
        };
        for (int32_t k = 0; e->named && k < F_ARRAY(kNamed); k++)
            if (os64_memcmp(kNamed[k].p, e->p, sizeof(e->p)) == 0) {
                puts_(o, kNamed[k].name);
                return;
            }
        puts_(o, "cubic-bezier(");
        for (int k = 0; k < 4; k++) {
            if (k > 0)
                puts_(o, ", ");
            number(o, e->p[k]);
        }
        puts_(o, ")");
        return;
    }
    puts_(o, "steps(");
    integer(o, e->steps);
    if (e->jump != FLOW_JUMP_END) {
        puts_(o, ", ");
        WORD(o, kJump, e->jump);
    }
    puts_(o, ")");
}

static void seconds(Out *o, double ms)
{
    number(o, ms / 1000.0);
    puts_(o, "s");
}

// One `animation-*` longhand's list; `none`'s one item when it has none.
static void animations(Out *o, const flow_animations_t *a, int32_t prop)
{
    int32_t n = 0;
    switch (prop) {
    case GARB_ANIMATION_NAME: n = a->nname; break;
    case GARB_ANIMATION_DURATION: n = a->nduration; break;
    case GARB_ANIMATION_DELAY: n = a->ndelay; break;
    case GARB_ANIMATION_TIMING_FUNCTION: n = a->neasing; break;
    case GARB_ANIMATION_ITERATION_COUNT: n = a->niterations; break;
    case GARB_ANIMATION_DIRECTION: n = a->ndirection; break;
    case GARB_ANIMATION_FILL_MODE: n = a->nfill; break;
    case GARB_ANIMATION_PLAY_STATE: n = a->npaused; break;
    default: break;
    }
    if (n <= 0) {
        switch (prop) {
        case GARB_ANIMATION_NAME: puts_(o, "none"); return;
        case GARB_ANIMATION_DURATION: case GARB_ANIMATION_DELAY: puts_(o, "0s"); return;
        case GARB_ANIMATION_TIMING_FUNCTION: puts_(o, "ease"); return;
        case GARB_ANIMATION_ITERATION_COUNT: puts_(o, "1"); return;
        case GARB_ANIMATION_DIRECTION: puts_(o, "normal"); return;
        case GARB_ANIMATION_FILL_MODE: puts_(o, "none"); return;
        default: puts_(o, "running"); return;
        }
    }
    for (int32_t i = 0; i < n; i++) {
        if (i > 0)
            puts_(o, ", ");
        switch (prop) {
        case GARB_ANIMATION_NAME:
            if (a->name[i].text == NULL)
                puts_(o, "none");
            else
                put(o, a->name[i].text, a->name[i].len);
            break;
        case GARB_ANIMATION_DURATION: seconds(o, a->duration[i]); break;
        case GARB_ANIMATION_DELAY: seconds(o, a->delay[i]); break;
        case GARB_ANIMATION_TIMING_FUNCTION: easing(o, &a->easing[i]); break;
        case GARB_ANIMATION_ITERATION_COUNT:
            if (a->iterations[i] > 1e300)
                puts_(o, "infinite");
            else
                number(o, a->iterations[i]);
            break;
        case GARB_ANIMATION_DIRECTION: WORD(o, kAnimDirection, a->direction[i]); break;
        case GARB_ANIMATION_FILL_MODE: WORD(o, kFill, a->fill[i]); break;
        default: puts_(o, a->paused[i] ? "paused" : "running"); break;
        }
    }
}

// One function of `translate`, `rotate` or `scale` as computed; `none`
// for none.
static void individual(const flow_computed_t *c, Out *o, const flow_transform_t *f)
{
    if (f == NULL) {
        puts_(o, "none");
        return;
    }
    switch (f->fn) {
    case FLOW_FN_TRANSLATE:
        length(c, o, f->x, "0px");
        if (f->y.kind != FLOW_LENGTH_PX || f->y.value != 0 || f->a[2] != 0) {
            puts_(o, " ");
            length(c, o, f->y, "0px");
        }
        if (f->a[2] != 0) {
            puts_(o, " ");
            units(c, o, (int64_t)f->a[2]);
        }
        return;
    case FLOW_FN_SCALE:
        number(o, f->a[0]);
        if (f->a[1] != f->a[0] || f->a[2] != 1) {
            puts_(o, " ");
            number(o, f->a[1]);
        }
        if (f->a[2] != 1) {
            puts_(o, " ");
            number(o, f->a[2]);
        }
        return;
    case FLOW_FN_ROTATE: {
        static const char *const kUnit[] = {"deg", "grad", "rad", "turn"};
        _Static_assert(F_ARRAY(kUnit) == FLOW_ANGLE_TURN + 1, "kUnit");
        if (f->a[0] != 0 || f->a[1] != 0 || f->a[2] != 1) {
            for (int k = 0; k < 3; k++) {
                number(o, f->a[k]);
                puts_(o, " ");
            }
        }
        number(o, f->a[3]);
        WORD(o, kUnit, f->unit);
        return;
    }
    default:
        puts_(o, "none");
        return;
    }
}

// `transform`'s resolved value (CSSOM § Resolved values): its list
// multiplied out in the border box, as matrix(), or matrix3d() when it
// leaves the plane; the list as no box gives its percentages a size.
static void transform(const flow_computed_t *c, Out *o)
{
    const flow_style_t *s = c->style;
    if (s->ntransform <= 0 || s->transform == NULL) {
        puts_(o, "none");
        return;
    }
    int64_t w = rendered(c) ? (int64_t)c->box->rect.w * FLOW_UNITS_PER_PX : 0;
    int64_t h = rendered(c) ? (int64_t)c->box->rect.h * FLOW_UNITS_PER_PX : 0;
    double m[16];
    f_tail_matrix(s->transform, s->ntransform, w, h, m);
    // The translation is in 26.6 device units; the rest has none.
    double to_css = 1.0 / (FLOW_UNITS_PER_PX * c->zoom);
    bool flat = m[2] == 0 && m[3] == 0 && m[6] == 0 && m[7] == 0 && m[8] == 0 && m[9] == 0 &&
                m[10] == 1 && m[11] == 0 && m[14] == 0 && m[15] == 1;
    if (flat) {
        double v[6] = {m[0], m[1], m[4], m[5], m[12] * to_css, m[13] * to_css};
        puts_(o, "matrix(");
        for (int k = 0; k < 6; k++) {
            if (k > 0)
                puts_(o, ", ");
            number(o, v[k]);
        }
        puts_(o, ")");
        return;
    }
    puts_(o, "matrix3d(");
    for (int k = 0; k < 16; k++) {
        if (k > 0)
            puts_(o, ", ");
        number(o, k >= 12 && k <= 14 ? m[k] * to_css : m[k]);
    }
    puts_(o, ")");
}

// ── A property ──────────────────────────────────────────────────────────

// A value as libgarb writes it, whole: written in place at the length
// libgarb reports, so a value of any length reads complete, or reports the
// length a caller needs to read it again with room (flow_computed_text's
// snprintf contract).
static void put_value(Out *o, const garb_val_t *v)
{
    size_t need = garb_val_dump(v, NULL, 0);
    if (o->len < o->cap)
        garb_val_dump(v, o->out + o->len, o->cap - o->len);
    o->len += need;
    if (o->cap > 0)
        o->out[o->len < o->cap ? o->len : o->cap - 1] = '\0';
}

// The winner the page's own sheets gave the property on the element, or
// on the ancestors its value comes from: what a property libflow does not
// model computes to, written as libgarb writes a value. The CSS-wide
// keywords mean what the cascade gives them (style.c's lay_sets):
// `inherit`, and `unset` on an inherited property, take the parent's; no
// winner on an inherited property does too; `initial`, `unset` on a
// property that is not inherited, and no winner on one, are the initial
// value. `revert` and `revert-layer` go back to the user-agent origin,
// which declares none of these properties, so they are `unset`. False for
// the initial value, which the caller writes (its own word, or nothing).
static bool declared(const flow_computed_t *c, int32_t prop, Out *o)
{
    const garb_cascade_t *cascade = c->env.cascade;
    if (cascade == NULL)
        return false;
    bool inherited = garb_prop_inherited((garb_prop_t)prop);
    for (const os64_html_node_t *n = c->element; n != NULL && n->kind == OS64_HTML_ELEMENT; n = n->parent) {
        garb_style_t st = garb_style_for(cascade, n);
        const garb_val_t *won = NULL;
        for (int32_t i = 0; i < st.n && won == NULL; i++)
            if ((int32_t)st.sets[i].prop == prop)
                won = &st.sets[i].value;
        if (won == NULL) {
            if (!inherited)
                return false;
            continue;
        }
        if (won->kind != GARB_V_WIDE) {
            put_value(o, won);
            return true;
        }
        bool from_parent = os64_streq(won->keyword, "inherit") ||
                           (!os64_streq(won->keyword, "initial") && inherited);
        if (!from_parent)
            return false;
    }
    return false;
}

size_t flow_computed_text(const flow_computed_t *c, int32_t prop, char *out, size_t cap)
{
    Out o = {out, cap, 0};
    if (cap > 0)
        out[0] = '\0';
    if (c == NULL || c->style == NULL || prop < 0 || prop >= GARB_NPROPS)
        return 0;
    const flow_style_t *s = c->style;
    int side = 0;
    switch (prop) {
    case GARB_DISPLAY:
        if (s->display == FLOW_DISPLAY_BLOCK && s->flow_root)
            puts_(&o, "flow-root");
        else
            WORD(&o, kDisplay, s->display);
        break;
    case GARB_COLOR: colour(&o, s->color); break;
    case GARB_BACKGROUND_COLOR:
        if (s->has_background)
            colour(&o, s->background);
        else
            puts_(&o, "rgba(0, 0, 0, 0)");
        break;
    case GARB_BACKGROUND_IMAGE:
        if (s->backgrounds.nimage <= 0)
            puts_(&o, "none");
        for (int32_t i = 0; i < s->backgrounds.nimage; i++) {
            if (i > 0)
                puts_(&o, ", ");
            image(c, &o, &s->backgrounds.image[i]);
        }
        break;
    case GARB_BACKGROUND_REPEAT:
        if (s->backgrounds.nrepeat <= 0)
            puts_(&o, "repeat");
        for (int32_t i = 0; i < s->backgrounds.nrepeat; i++) {
            if (i > 0)
                puts_(&o, ", ");
            WORD(&o, kRepeat, s->backgrounds.repeat[i]);
        }
        break;
    case GARB_BACKGROUND_POSITION_X:
    case GARB_BACKGROUND_POSITION_Y: {
        bool x = prop == GARB_BACKGROUND_POSITION_X;
        const flow_length_t *list = x ? s->backgrounds.x : s->backgrounds.y;
        int32_t n = x ? s->backgrounds.nx : s->backgrounds.ny;
        if (n <= 0)
            puts_(&o, "0%");
        for (int32_t i = 0; i < n; i++) {
            if (i > 0)
                puts_(&o, ", ");
            length(c, &o, list[i], "0%");
        }
        break;
    }
    case GARB_BACKGROUND_SIZE:
        if (s->backgrounds.nsize <= 0)
            puts_(&o, "auto");
        for (int32_t i = 0; i < s->backgrounds.nsize; i++) {
            const flow_bg_size_t *z = &s->backgrounds.size[i];
            if (i > 0)
                puts_(&o, ", ");
            if (z->fit == FLOW_FIT_COVER) {
                puts_(&o, "cover");
            } else if (z->fit == FLOW_FIT_CONTAIN) {
                puts_(&o, "contain");
            } else {
                length(c, &o, z->size[0], "auto");
                if (z->size[1].kind != FLOW_LENGTH_AUTO) {
                    puts_(&o, " ");
                    length(c, &o, z->size[1], "auto");
                }
            }
        }
        break;
    case GARB_BACKGROUND_ORIGIN:
    case GARB_BACKGROUND_CLIP: {
        bool origin = prop == GARB_BACKGROUND_ORIGIN;
        const flow_edge_t *list = origin ? s->backgrounds.origin : s->backgrounds.clip;
        int32_t n = origin ? s->backgrounds.norigin : s->backgrounds.nclip;
        if (n <= 0)
            puts_(&o, origin ? "padding-box" : "border-box");
        for (int32_t i = 0; i < n; i++) {
            if (i > 0)
                puts_(&o, ", ");
            WORD(&o, kEdge, list[i]);
        }
        break;
    }
    case GARB_MARGIN_LEFT: side++; /* fall through */
    case GARB_MARGIN_BOTTOM: side++; /* fall through */
    case GARB_MARGIN_RIGHT: side++; /* fall through */
    case GARB_MARGIN_TOP: margin(c, &o, side); break;
    case GARB_PADDING_LEFT: side++; /* fall through */
    case GARB_PADDING_BOTTOM: side++; /* fall through */
    case GARB_PADDING_RIGHT: side++; /* fall through */
    case GARB_PADDING_TOP: used_length(c, &o, s->padding[side], cb_width(c), "0px"); break;
    case GARB_BORDER_LEFT_WIDTH: side++; /* fall through */
    case GARB_BORDER_BOTTOM_WIDTH: side++; /* fall through */
    case GARB_BORDER_RIGHT_WIDTH: side++; /* fall through */
    case GARB_BORDER_TOP_WIDTH: units(c, &o, s->border_width[side]); break;
    case GARB_BORDER_LEFT_STYLE: side++; /* fall through */
    case GARB_BORDER_BOTTOM_STYLE: side++; /* fall through */
    case GARB_BORDER_RIGHT_STYLE: side++; /* fall through */
    case GARB_BORDER_TOP_STYLE: WORD(&o, kBorder, s->border_style[side]); break;
    case GARB_BORDER_LEFT_COLOR: side++; /* fall through */
    case GARB_BORDER_BOTTOM_COLOR: side++; /* fall through */
    case GARB_BORDER_RIGHT_COLOR: side++; /* fall through */
    case GARB_BORDER_TOP_COLOR: colour(&o, s->border_color[side]); break;
    case GARB_WIDTH: size(c, &o, true); break;
    case GARB_HEIGHT: size(c, &o, false); break;
    case GARB_MIN_WIDTH: length(c, &o, s->min_width, "auto"); break;
    case GARB_MIN_HEIGHT: length(c, &o, s->min_height, "auto"); break;
    case GARB_MAX_WIDTH: length(c, &o, s->max_width, "none"); break;
    case GARB_MAX_HEIGHT: length(c, &o, s->max_height, "none"); break;
    case GARB_BOX_SIZING: puts_(&o, s->box_sizing == FLOW_BORDER_BOX ? "border-box" : "content-box"); break;
    case GARB_FONT_FAMILY: families(&o, &s->family); break;
    case GARB_FONT_SIZE: units(c, &o, s->font_size); break;
    case GARB_FONT_WEIGHT: integer(&o, s->font_weight); break;
    case GARB_FONT_STYLE: puts_(&o, s->font_style == FLOW_FONT_ITALIC ? "italic" : "normal"); break;
    case GARB_LINE_HEIGHT:
        if (s->line_height.kind == FLOW_LINE_NORMAL)
            puts_(&o, "normal");
        else if (s->line_height.kind == FLOW_LINE_NUMBER)
            units(c, &o, (int64_t)s->font_size * s->line_height.value / 1000);
        else
            units(c, &o, s->line_height.value);
        break;
    case GARB_TEXT_ALIGN: WORD(&o, kAlign, s->text_align); break;
    case GARB_VERTICAL_ALIGN: WORD(&o, kValign, s->vertical_align); break;
    case GARB_WHITE_SPACE: WORD(&o, kWhiteSpace, s->white_space); break;
    case GARB_TEXT_DECORATION_LINE:
        if ((s->text_decoration & (FLOW_DECORATION_UNDERLINE | FLOW_DECORATION_LINE_THROUGH)) == 0)
            puts_(&o, "none");
        if (s->text_decoration & FLOW_DECORATION_UNDERLINE)
            puts_(&o, "underline");
        if (s->text_decoration & FLOW_DECORATION_LINE_THROUGH)
            puts_(&o, s->text_decoration & FLOW_DECORATION_UNDERLINE ? " line-through" : "line-through");
        break;
    case GARB_TEXT_INDENT: length(c, &o, s->text_indent, "0px"); break;
    case GARB_TEXT_TRANSFORM: WORD(&o, kTransform, s->text_transform); break;
    case GARB_VISIBILITY: WORD(&o, kVisibility, s->visibility); break;
    case GARB_LIST_STYLE_TYPE: WORD(&o, kList, s->list_style_type); break;
    case GARB_LIST_STYLE_POSITION:
        puts_(&o, s->list_style_position == FLOW_LIST_INSIDE ? "inside" : "outside");
        break;
    case GARB_BORDER_SPACING:
        units(c, &o, s->border_spacing[0]);
        puts_(&o, " ");
        units(c, &o, s->border_spacing[1]);
        break;
    case GARB_BORDER_COLLAPSE:
        puts_(&o, s->border_collapse == FLOW_COLLAPSE_BORDERS ? "collapse" : "separate");
        break;
    case GARB_CAPTION_SIDE: puts_(&o, s->caption_side == FLOW_CAPTION_BOTTOM ? "bottom" : "top"); break;
    case GARB_FLOAT: WORD(&o, kFloat, s->float_side); break;
    case GARB_CLEAR: WORD(&o, kClear, s->clear); break;
    case GARB_OVERFLOW_X: WORD(&o, kOverflow, s->overflow_x); break;
    case GARB_OVERFLOW_Y: WORD(&o, kOverflow, s->overflow_y); break;
    case GARB_POSITION: WORD(&o, kPosition, s->position); break;
    case GARB_LEFT: side++; /* fall through */
    case GARB_BOTTOM: side++; /* fall through */
    case GARB_RIGHT: side++; /* fall through */
    case GARB_TOP: inset(c, &o, side); break;
    case GARB_Z_INDEX:
        if (s->has_z_index)
            integer(&o, s->z_index);
        else
            puts_(&o, "auto");
        break;
    case GARB_OPACITY: number(&o, s->opacity / 1000.0); break;
    case GARB_POINTER_EVENTS: puts_(&o, s->pointer_events_none ? "none" : "auto"); break;
    case GARB_FLEX_DIRECTION: WORD(&o, kDirection, s->flex_direction); break;
    case GARB_FLEX_WRAP: WORD(&o, kWrap, s->flex_wrap); break;
    case GARB_JUSTIFY_CONTENT: WORD(&o, kPlace, s->justify_content); break;
    case GARB_ALIGN_ITEMS: WORD(&o, kPlace, s->align_items); break;
    case GARB_ALIGN_SELF: WORD(&o, kPlace, s->align_self); break;
    case GARB_ALIGN_CONTENT: WORD(&o, kPlace, s->align_content); break;
    case GARB_JUSTIFY_ITEMS: WORD(&o, kPlace, s->justify_items); break;
    case GARB_JUSTIFY_SELF: WORD(&o, kPlace, s->justify_self); break;
    case GARB_FLEX_GROW: number(&o, s->flex_grow / 1000.0); break;
    case GARB_FLEX_SHRINK: number(&o, s->flex_shrink / 1000.0); break;
    case GARB_FLEX_BASIS: length(c, &o, s->flex_basis, "auto"); break;
    case GARB_ORDER: integer(&o, s->order); break;
    case GARB_ROW_GAP: length(c, &o, s->row_gap, "normal"); break;
    case GARB_COLUMN_GAP: length(c, &o, s->column_gap, "normal"); break;
    case GARB_GRID_TEMPLATE_COLUMNS: tracks(c, &o, &s->grid_template_columns, "none"); break;
    case GARB_GRID_TEMPLATE_ROWS: tracks(c, &o, &s->grid_template_rows, "none"); break;
    case GARB_GRID_TEMPLATE_AREAS: areas(&o, s); break;
    case GARB_GRID_AUTO_COLUMNS: tracks(c, &o, &s->grid_auto_columns, "auto"); break;
    case GARB_GRID_AUTO_ROWS: tracks(c, &o, &s->grid_auto_rows, "auto"); break;
    case GARB_GRID_AUTO_FLOW:
        puts_(&o, s->grid_auto_flow_column ? "column" : "row");
        if (s->grid_dense)
            puts_(&o, " dense");
        break;
    case GARB_GRID_COLUMN_START: grid_line(&o, &s->grid_column_start); break;
    case GARB_GRID_COLUMN_END: grid_line(&o, &s->grid_column_end); break;
    case GARB_GRID_ROW_START: grid_line(&o, &s->grid_row_start); break;
    case GARB_GRID_ROW_END: grid_line(&o, &s->grid_row_end); break;
    case GARB_BORDER_BOTTOM_LEFT_RADIUS: side++; /* fall through */
    case GARB_BORDER_BOTTOM_RIGHT_RADIUS: side++; /* fall through */
    case GARB_BORDER_TOP_RIGHT_RADIUS: side++; /* fall through */
    case GARB_BORDER_TOP_LEFT_RADIUS: {
        const flow_length_t *r = s->radius[side];
        length(c, &o, r[0], "0px");
        if (r[0].kind != r[1].kind || r[0].value != r[1].value || r[0].offset != r[1].offset) {
            puts_(&o, " ");
            length(c, &o, r[1], "0px");
        }
        break;
    }
    case GARB_BOX_SHADOW: shadows(c, &o, s->box_shadows, s->nbox_shadows, true); break;
    case GARB_TEXT_SHADOW: shadows(c, &o, s->text_shadows, s->ntext_shadows, false); break;
    case GARB_IMAGE_RENDERING: puts_(&o, s->pixelated ? "pixelated" : "auto"); break;
    case GARB_CLIP:
        if (!s->legacy_clip_set) {
            puts_(&o, "auto");
            break;
        }
        puts_(&o, "rect(");
        for (int k = 0; k < 4; k++) {
            if (k > 0)
                puts_(&o, ", ");
            length(c, &o, s->legacy_clip[k], "auto");
        }
        puts_(&o, ")");
        break;
    case GARB_TRANSFORM: transform(c, &o); break;
    case GARB_TRANSFORM_ORIGIN: {
        int64_t w = rendered(c) ? c->box->rect.w : -1, h = rendered(c) ? c->box->rect.h : -1;
        used_length(c, &o, s->transform_origin[0], w, "50%");
        puts_(&o, " ");
        used_length(c, &o, s->transform_origin[1], h, "50%");
        if (s->transform_origin_z != 0) {
            puts_(&o, " ");
            units(c, &o, s->transform_origin_z);
        }
        break;
    }
    case GARB_TRANSLATE: individual(c, &o, s->translate); break;
    case GARB_ROTATE: individual(c, &o, s->rotate); break;
    case GARB_SCALE: individual(c, &o, s->scale); break;
    case GARB_TEXT_FILL_COLOR: colour(&o, s->fill); break;
    case GARB_ANIMATION_NAME: case GARB_ANIMATION_DURATION: case GARB_ANIMATION_TIMING_FUNCTION:
    case GARB_ANIMATION_ITERATION_COUNT: case GARB_ANIMATION_DIRECTION: case GARB_ANIMATION_PLAY_STATE:
    case GARB_ANIMATION_DELAY: case GARB_ANIMATION_FILL_MODE:
        animations(&o, &s->animations, prop);
        break;
    case GARB_WILL_CHANGE:
        if ((s->will_change & (FLOW_WILL_TRANSFORM | FLOW_WILL_OPACITY)) == 0)
            puts_(&o, "auto");
        if (s->will_change & FLOW_WILL_TRANSFORM)
            puts_(&o, "transform");
        if (s->will_change & FLOW_WILL_OPACITY)
            puts_(&o, s->will_change & FLOW_WILL_TRANSFORM ? ", opacity" : "opacity");
        break;
    case GARB_CURSOR: WORD(&o, kCursor, s->cursor); break;
    case GARB_USER_SELECT: WORD(&o, kUserSelect, s->user_select); break;
    case GARB_WORD_BREAK: WORD(&o, kWordBreak, s->word_break); break;
    case GARB_OVERFLOW_WRAP: WORD(&o, kOverflowWrap, s->overflow_wrap); break;
    case GARB_LETTER_SPACING:
        if (s->letter_spacing == 0)
            puts_(&o, "normal");
        else
            units(c, &o, s->letter_spacing);
        break;
    case GARB_TEXT_OVERFLOW: WORD(&o, kTextOverflow, s->text_overflow); break;
    case GARB_OUTLINE_WIDTH: units(c, &o, s->outline_width); break;
    case GARB_OUTLINE_STYLE:
        if (s->outline_auto)
            puts_(&o, "auto");
        else
            WORD(&o, kBorder, s->outline_style);
        break;
    case GARB_OUTLINE_COLOR: colour(&o, s->outline_color); break;
    case GARB_OUTLINE_OFFSET: units(c, &o, s->outline_offset); break;
    case GARB_OBJECT_FIT: WORD(&o, kObjectFit, s->object_fit); break;
    case GARB_OBJECT_POSITION:
        length(c, &o, s->object_position[0], "50%");
        puts_(&o, " ");
        length(c, &o, s->object_position[1], "50%");
        break;
    case GARB_ASPECT_RATIO:
        // The style keeps the ratio as one number; the pair the page wrote
        // is its declaration's.
        if (!declared(c, prop, &o))
            puts_(&o, "auto");
        break;
    case GARB_FONT_VARIANT:
        if (!declared(c, prop, &o))
            puts_(&o, "normal");
        break;
    case GARB_LIST_STYLE_IMAGE:
        if (!declared(c, prop, &o))
            puts_(&o, "none");
        break;
    default:
        // A property libgarb reads and libflow does not model yet: what
        // the page's sheets declared, which is what an engine without the
        // property's computation can say; nothing when they declared none.
        declared(c, prop, &o);
        break;
    }
    return o.len;
}
