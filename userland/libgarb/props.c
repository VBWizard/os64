// props.c — each property's grammar, and a declaration read against it
// (garb/values.h). The grammars are the specifications' own, section named
// at each; where one is read short of its whole grammar the missing part is
// named at the reader, and a value using it is invalid rather than guessed.

#include "values_internal.h"
#include "os64/fmt.h"
#include "os64/mem.h"
#include "os64/str.h"

// ── The table ───────────────────────────────────────────────────────────

typedef struct {
    const char *name;
    bool inherited;
} Prop;

static const Prop kProps[GARB_NPROPS] = {
    [GARB_DISPLAY] = {"display", false},
    [GARB_COLOR] = {"color", true},
    [GARB_BACKGROUND_COLOR] = {"background-color", false},
    [GARB_BACKGROUND_IMAGE] = {"background-image", false},
    [GARB_BACKGROUND_REPEAT] = {"background-repeat", false},
    [GARB_BACKGROUND_POSITION_X] = {"background-position-x", false},
    [GARB_BACKGROUND_POSITION_Y] = {"background-position-y", false},
    [GARB_BACKGROUND_SIZE] = {"background-size", false},
    [GARB_BACKGROUND_ORIGIN] = {"background-origin", false},
    [GARB_BACKGROUND_CLIP] = {"background-clip", false},
    [GARB_MARGIN_TOP] = {"margin-top", false},
    [GARB_MARGIN_RIGHT] = {"margin-right", false},
    [GARB_MARGIN_BOTTOM] = {"margin-bottom", false},
    [GARB_MARGIN_LEFT] = {"margin-left", false},
    [GARB_PADDING_TOP] = {"padding-top", false},
    [GARB_PADDING_RIGHT] = {"padding-right", false},
    [GARB_PADDING_BOTTOM] = {"padding-bottom", false},
    [GARB_PADDING_LEFT] = {"padding-left", false},
    [GARB_BORDER_TOP_WIDTH] = {"border-top-width", false},
    [GARB_BORDER_RIGHT_WIDTH] = {"border-right-width", false},
    [GARB_BORDER_BOTTOM_WIDTH] = {"border-bottom-width", false},
    [GARB_BORDER_LEFT_WIDTH] = {"border-left-width", false},
    [GARB_BORDER_TOP_STYLE] = {"border-top-style", false},
    [GARB_BORDER_RIGHT_STYLE] = {"border-right-style", false},
    [GARB_BORDER_BOTTOM_STYLE] = {"border-bottom-style", false},
    [GARB_BORDER_LEFT_STYLE] = {"border-left-style", false},
    [GARB_BORDER_TOP_COLOR] = {"border-top-color", false},
    [GARB_BORDER_RIGHT_COLOR] = {"border-right-color", false},
    [GARB_BORDER_BOTTOM_COLOR] = {"border-bottom-color", false},
    [GARB_BORDER_LEFT_COLOR] = {"border-left-color", false},
    [GARB_WIDTH] = {"width", false},
    [GARB_HEIGHT] = {"height", false},
    [GARB_MIN_WIDTH] = {"min-width", false},
    [GARB_MAX_WIDTH] = {"max-width", false},
    [GARB_MIN_HEIGHT] = {"min-height", false},
    [GARB_MAX_HEIGHT] = {"max-height", false},
    [GARB_BOX_SIZING] = {"box-sizing", false},
    [GARB_FONT_FAMILY] = {"font-family", true},
    [GARB_FONT_SIZE] = {"font-size", true},
    [GARB_FONT_WEIGHT] = {"font-weight", true},
    [GARB_FONT_STYLE] = {"font-style", true},
    [GARB_FONT_VARIANT] = {"font-variant", true},
    [GARB_LINE_HEIGHT] = {"line-height", true},
    [GARB_TEXT_ALIGN] = {"text-align", true},
    [GARB_VERTICAL_ALIGN] = {"vertical-align", false},
    [GARB_WHITE_SPACE] = {"white-space", true},
    [GARB_TEXT_DECORATION_LINE] = {"text-decoration-line", false},
    [GARB_TEXT_INDENT] = {"text-indent", true},
    [GARB_TEXT_TRANSFORM] = {"text-transform", true},
    [GARB_VISIBILITY] = {"visibility", true},
    [GARB_LIST_STYLE_TYPE] = {"list-style-type", true},
    [GARB_LIST_STYLE_POSITION] = {"list-style-position", true},
    [GARB_LIST_STYLE_IMAGE] = {"list-style-image", true},
    [GARB_BORDER_SPACING] = {"border-spacing", true},
    [GARB_BORDER_COLLAPSE] = {"border-collapse", true},
    [GARB_CAPTION_SIDE] = {"caption-side", true},
    [GARB_FLOAT] = {"float", false},
    [GARB_CLEAR] = {"clear", false},
    [GARB_OVERFLOW_X] = {"overflow-x", false},
    [GARB_OVERFLOW_Y] = {"overflow-y", false},
    [GARB_POSITION] = {"position", false},
    [GARB_TOP] = {"top", false},
    [GARB_RIGHT] = {"right", false},
    [GARB_BOTTOM] = {"bottom", false},
    [GARB_LEFT] = {"left", false},
    [GARB_Z_INDEX] = {"z-index", false},
    [GARB_OPACITY] = {"opacity", false},
    [GARB_POINTER_EVENTS] = {"pointer-events", true},
    [GARB_FLEX_DIRECTION] = {"flex-direction", false},
    [GARB_FLEX_WRAP] = {"flex-wrap", false},
    [GARB_JUSTIFY_CONTENT] = {"justify-content", false},
    [GARB_ALIGN_ITEMS] = {"align-items", false},
    [GARB_ALIGN_SELF] = {"align-self", false},
    [GARB_ALIGN_CONTENT] = {"align-content", false},
    [GARB_FLEX_GROW] = {"flex-grow", false},
    [GARB_FLEX_SHRINK] = {"flex-shrink", false},
    [GARB_FLEX_BASIS] = {"flex-basis", false},
    [GARB_ORDER] = {"order", false},
    [GARB_ROW_GAP] = {"row-gap", false},
    [GARB_COLUMN_GAP] = {"column-gap", false},
    [GARB_GRID_TEMPLATE_COLUMNS] = {"grid-template-columns", false},
    [GARB_GRID_TEMPLATE_ROWS] = {"grid-template-rows", false},
    [GARB_GRID_TEMPLATE_AREAS] = {"grid-template-areas", false},
    [GARB_GRID_AUTO_COLUMNS] = {"grid-auto-columns", false},
    [GARB_GRID_AUTO_ROWS] = {"grid-auto-rows", false},
    [GARB_GRID_AUTO_FLOW] = {"grid-auto-flow", false},
    [GARB_GRID_COLUMN_START] = {"grid-column-start", false},
    [GARB_GRID_COLUMN_END] = {"grid-column-end", false},
    [GARB_GRID_ROW_START] = {"grid-row-start", false},
    [GARB_GRID_ROW_END] = {"grid-row-end", false},
    [GARB_JUSTIFY_ITEMS] = {"justify-items", false},
    [GARB_JUSTIFY_SELF] = {"justify-self", false},
    [GARB_BORDER_TOP_LEFT_RADIUS] = {"border-top-left-radius", false},
    [GARB_BORDER_TOP_RIGHT_RADIUS] = {"border-top-right-radius", false},
    [GARB_BORDER_BOTTOM_RIGHT_RADIUS] = {"border-bottom-right-radius", false},
    [GARB_BORDER_BOTTOM_LEFT_RADIUS] = {"border-bottom-left-radius", false},
    [GARB_BOX_SHADOW] = {"box-shadow", false},
    [GARB_TEXT_SHADOW] = {"text-shadow", true},
};

const char *garb_prop_name(garb_prop_t prop)
{
    return prop < GARB_NPROPS ? kProps[prop].name : "";
}

bool garb_prop_inherited(garb_prop_t prop)
{
    return prop < GARB_NPROPS && kProps[prop].inherited;
}

// ── Keywords ────────────────────────────────────────────────────────────

static const char *const kWide[] = {"inherit", "initial", "unset", "revert", "revert-layer", NULL};
static const char *const kDisplay[] = {
    "none", "block", "inline", "inline-block", "list-item", "table", "inline-table",
    "table-row-group", "table-header-group", "table-footer-group", "table-row",
    "table-column-group", "table-column", "table-cell", "table-caption", "contents",
    "flow-root", "flex", "inline-flex", "grid", "inline-grid", NULL};
// Values this grammar reads but no slice lays out as written yet (GARB.md's
// pile 2, POSITION.md's slices). Read, so the cascade keeps them; not
// SUPPORTED, so @supports tells a page to use the fallback it wrote for
// exactly this. flow-root is laid out as a block. `contents` is not here:
// libflow gives such an element no box and flows its children into its
// parent, which is what it says. The list is for the layouts a page writes
// a fallback for — asks for one and is handed another — and each joins it
// the day its mapping is written and leaves the day the slice that lays it
// out lands.
static const struct {
    garb_prop_t prop;
    const char *keyword;
} kApproximated[] = {
    {GARB_DISPLAY, "flow-root"},
};

bool garb_set_approximated(const garb_set_t *set)
{
    const garb_val_t *v = &set->value;
    if (v->kind == GARB_V_WIDE || v->kind != GARB_V_KEYWORD)
        return false;
    for (size_t k = 0; k < sizeof(kApproximated) / sizeof(kApproximated[0]); k++)
        if (set->prop == kApproximated[k].prop && os64_streq(v->keyword, kApproximated[k].keyword))
            return true;
    return false;
}

static const char *const kBorderStyle[] = {"none", "hidden", "dotted", "dashed", "solid", "double",
                                           "groove", "ridge", "inset", "outset", NULL};
static const char *const kBorderWidth[] = {"thin", "medium", "thick", NULL};
static const char *const kAuto[] = {"auto", NULL};
static const char *const kNone[] = {"none", NULL};
static const char *const kSizeWords[] = {"auto", "min-content", "max-content", "fit-content", NULL};
static const char *const kMaxWords[] = {"none", "min-content", "max-content", "fit-content", NULL};
static const char *const kBoxSizing[] = {"content-box", "border-box", NULL};
static const char *const kGeneric[] = {"serif", "sans-serif", "monospace", "cursive", "fantasy",
                                       "system-ui", "ui-serif", "ui-sans-serif", "ui-monospace",
                                       "ui-rounded", "math", "emoji", "fangsong", NULL};
static const char *const kFontSize[] = {"xx-small", "x-small", "small", "medium", "large",
                                        "x-large", "xx-large", "xxx-large", "larger", "smaller",
                                        NULL};
static const char *const kFontWeight[] = {"normal", "bold", "bolder", "lighter", NULL};
static const char *const kFontStyle[] = {"normal", "italic", "oblique", NULL};
static const char *const kFontVariant[] = {"normal", "small-caps", NULL};
static const char *const kFontStretch[] = {"normal", "ultra-condensed", "extra-condensed",
                                           "condensed", "semi-condensed", "semi-expanded",
                                           "expanded", "extra-expanded", "ultra-expanded", NULL};
static const char *const kSystemFont[] = {"caption", "icon", "menu", "message-box",
                                          "small-caption", "status-bar", NULL};
static const char *const kNormal[] = {"normal", NULL};
static const char *const kTextAlign[] = {"left", "right", "center", "justify", "start", "end",
                                         "match-parent", NULL};
static const char *const kVerticalAlign[] = {"baseline", "sub", "super", "text-top",
                                             "text-bottom", "middle", "top", "bottom", NULL};
static const char *const kWhiteSpace[] = {"normal", "pre", "nowrap", "pre-wrap", "pre-line",
                                          "break-spaces", NULL};
static const char *const kLines[] = {"underline", "overline", "line-through", "blink", NULL};
static const char *const kDecorationStyle[] = {"solid", "double", "dotted", "dashed", "wavy", NULL};
static const char *const kThickness[] = {"auto", "from-font", NULL};
static const char *const kTransform[] = {"none", "capitalize", "uppercase", "lowercase",
                                         "full-width", "full-size-kana", NULL};
static const char *const kIndentWords[] = {"hanging", "each-line", NULL};
static const char *const kVisibility[] = {"visible", "hidden", "collapse", NULL};
static const char *const kListPosition[] = {"inside", "outside", NULL};
static const char *const kCollapse[] = {"collapse", "separate", NULL};
static const char *const kCaption[] = {"top", "bottom", NULL};
static const char *const kFloat[] = {"left", "right", "none", "inline-start", "inline-end", NULL};
static const char *const kClear[] = {"none", "left", "right", "both", "inline-start",
                                     "inline-end", NULL};
static const char *const kOverflow[] = {"visible", "hidden", "clip", "scroll", "auto", NULL};
static const char *const kPosition[] = {"static", "relative", "absolute", "fixed", "sticky", NULL};
// CSS UI 4 § 5.1 for HTML, and SVG 2's values, which an HTML box treats as
// auto: read so a sheet written for both keeps its declaration.
static const char *const kPointerEvents[] = {"auto", "none", "visiblepainted", "visiblefill",
                                             "visiblestroke", "visible", "painted", "fill",
                                             "stroke", "all", "bounding-box", NULL};
// Flexbox 1 § 5 and the Box Alignment 3 values a flex container reads.
// `safe` and `last baseline` are not read: a value using one is invalid,
// and the page's fallback stands (FLEX.md § Booked); `unsafe` is what
// every value means without it, and is read as that.
static const char *const kFlexDirection[] = {"row", "row-reverse", "column", "column-reverse",
                                             NULL};
static const char *const kFlexWrap[] = {"nowrap", "wrap", "wrap-reverse", NULL};
static const char *const kJustify[] = {"normal", "flex-start", "flex-end", "center",
                                       "space-between", "space-around", "space-evenly",
                                       "start", "end", "left", "right", "stretch", NULL};
static const char *const kAlignItems[] = {"normal", "stretch", "flex-start", "flex-end",
                                          "center", "baseline", "start", "end", "self-start",
                                          "self-end", NULL};
static const char *const kAlignSelf[] = {"auto", "normal", "stretch", "flex-start", "flex-end",
                                         "center", "baseline", "start", "end", "self-start",
                                         "self-end", NULL};
static const char *const kAlignContent[] = {"normal", "stretch", "flex-start", "flex-end",
                                            "center", "space-between", "space-around",
                                            "space-evenly", "start", "end", "baseline", NULL};
static const char *const kBasisWords[] = {"auto", "content", "min-content", "max-content",
                                          "fit-content", NULL};
static const char *const kJustifyItems[] = {"normal", "stretch", "start", "end", "center",
                                            "left", "right", "self-start", "self-end",
                                            "flex-start", "flex-end", "baseline", "legacy", NULL};
static const char *const kJustifySelf[] = {"auto", "normal", "stretch", "start", "end", "center",
                                           "left", "right", "self-start", "self-end",
                                           "flex-start", "flex-end", "baseline", NULL};
static const char *const kUnsafe[] = {"unsafe", NULL};
// Grid 2 § 7.2: a track's sizing keywords, and repeat()'s automatic counts.
static const char *const kTrackWords[] = {"auto", "min-content", "max-content", NULL};
static const char *const kAutoRepeat[] = {"auto-fill", "auto-fit", NULL};
static const char *const kFlowAxis[] = {"row", "column", NULL};
static const char *const kDense[] = {"dense", NULL};
static const char *const kSpan[] = {"span", NULL};
static const char *const kFirst[] = {"first", NULL};
static const char *const kRepeat[] = {"repeat-x", "repeat-y", NULL};
static const char *const kRepeat2[] = {"repeat", "space", "round", "no-repeat", NULL};
static const char *const kAttachment[] = {"scroll", "fixed", "local", NULL};
static const char *const kBox[] = {"border-box", "padding-box", "content-box", NULL};
static const char *const kClipBox[] = {"border-box", "padding-box", "content-box", "text", NULL};
static const char *const kBgSize[] = {"cover", "contain", NULL};
static const char *const kPosX[] = {"left", "right", "center", NULL};
static const char *const kPosY[] = {"top", "bottom", "center", NULL};
static const char *const kPosAll[] = {"left", "right", "top", "bottom", "center", NULL};

// ── Building the answer ─────────────────────────────────────────────────

typedef struct {
    garb_set_t *out;
    int32_t n;
    Arena a;
    bool quirks;
} Sets;

static void set(Sets *s, garb_prop_t p, const garb_val_t *v)
{
    if (s->n < GARB_SETS_MAX) {
        s->out[s->n].prop = p;
        s->out[s->n].value = *v;
        s->n++;
    }
}

static garb_val_t kw(const char *word)
{
    garb_val_t v = {0};
    v.kind = GARB_V_KEYWORD;
    v.keyword = word;
    return v;
}

static garb_val_t percent(double n)
{
    garb_val_t v = {0};
    v.kind = GARB_V_PERCENTAGE;
    v.number = n;
    return v;
}

static garb_val_t initial_color(void)
{
    garb_val_t v = {0};
    v.kind = GARB_V_COLOR;
    v.color.current = true;
    v.color.a = 1;
    return v;
}

static garb_val_t transparent(void)
{
    garb_val_t v = {0};
    v.kind = GARB_V_COLOR;
    return v;
}

static const garb_val_t *keep(Sets *s, const garb_val_t *items, int32_t n)
{
    garb_val_t *copy = os64_arena_alloc(s->a.arena, (size_t)(n > 0 ? n : 1) * sizeof(*copy));
    if (copy == NULL) {
        s->a.short_of_memory = true;
        return NULL;
    }
    for (int32_t k = 0; k < n; k++)
        copy[k] = items[k];
    return copy;
}

// ── Pieces grammars share ───────────────────────────────────────────────

// A keyword from `allowed` or a length/percentage as `accept` allows.
static bool kw_or_dim(Sets *s, VCur *c, const char *const *allowed, int accept, bool negative,
                      bool quirky, garb_val_t *out)
{
    const char *k = allowed != NULL ? vc_keyword(c, allowed) : NULL;
    if (k != NULL) {
        *out = kw(k);
        return true;
    }
    return vc_dim(c, accept, negative, quirky && s->quirks, &s->a, out);
}

// One to four values for top, right, bottom, left (CSS 2.1 § 8.3's box).
static bool box4(Sets *s, VCur *c, garb_prop_t first,
                 bool (*one)(Sets *, VCur *, garb_val_t *))
{
    garb_val_t v[4];
    int n = 0;
    while (n < 4 && !vc_done(c) && one(s, c, &v[n]))
        n++;
    if (n == 0 || !vc_done(c))
        return false;
    garb_val_t top = v[0], right = n > 1 ? v[1] : top, bottom = n > 2 ? v[2] : top,
               left = n > 3 ? v[3] : right;
    set(s, first, &top);
    set(s, (garb_prop_t)(first + 1), &right);
    set(s, (garb_prop_t)(first + 2), &bottom);
    set(s, (garb_prop_t)(first + 3), &left);
    return true;
}

// A corner's radius as one value of two items, horizontal and vertical:
// `n` given, the second the first's when only one is.
static bool radius_pair(Sets *s, const garb_val_t *given, int n, garb_val_t *out)
{
    garb_val_t two[2] = {given[0], n > 1 ? given[1] : given[0]};
    os64_memset(out, 0, sizeof(*out));
    out->kind = GARB_V_LENGTH;
    out->items = keep(s, two, 2);
    out->nitems = 2;
    return out->items != NULL;
}

// `border-radius` (Backgrounds 3 § 5.1): one to four horizontal radii,
// then `/` and one to four vertical ones, each list filled out as margin's
// is — top-left, top-right, bottom-right, bottom-left — and the vertical
// list the horizontal one when there is no slash.
static bool radius_shorthand(Sets *s, VCur *c)
{
    garb_val_t h[4], v[4];
    int nh = 0, nv = 0;
    while (nh < 4 && vc_dim(c, ACCEPT_LENGTH | ACCEPT_PERCENT, false, false, &s->a, &h[nh]))
        nh++;
    if (nh == 0)
        return false;
    const garb_value_t *t = vc_peek(c);
    if (is_delim_v(t, '/')) {
        c->i++;
        while (nv < 4 && vc_dim(c, ACCEPT_LENGTH | ACCEPT_PERCENT, false, false, &s->a, &v[nv]))
            nv++;
        if (nv == 0)
            return false;
    }
    if (!vc_done(c))
        return false;
    if (nv == 0) {
        os64_memcpy(v, h, sizeof(h));
        nv = nh;
    }
    garb_val_t *lists[2] = {h, v};
    int counts[2] = {nh, nv};
    for (int k = 0; k < 2; k++) {
        garb_val_t *l = lists[k];
        if (counts[k] < 2)
            l[1] = l[0];
        if (counts[k] < 3)
            l[2] = l[0];
        if (counts[k] < 4)
            l[3] = l[1];
    }
    for (int corner = 0; corner < 4; corner++) {
        garb_val_t pair[2] = {h[corner], v[corner]}, one;
        if (!radius_pair(s, pair, 2, &one))
            return false;
        set(s, (garb_prop_t)(GARB_BORDER_TOP_LEFT_RADIUS + corner), &one);
    }
    return true;
}

static const char *const kInset[] = {"inset", NULL};

// One shadow (Backgrounds 3 § 7.1; Text Decoration 3 § 4): two to four
// lengths together — x, y, a blur that is not negative, and for a box a
// spread — with a colour and, for a box, `inset`, each at most once, on
// either side of them. Read as one value whose items are always the same:
// `inset` first when it is there, the four lengths (0 for what was left
// out), and the colour, currentColor for none.
static bool shadow_one(Sets *s, VCur *c, bool box, garb_val_t *out)
{
    garb_val_t items[6], len[4], colour = {0};
    int n = 0;
    bool have_colour = false, have_inset = false, lengths_over = false;
    while (!vc_done(c) && vc_peek(c)->kind != GARB_COMMA) {
        if (box && !have_inset && vc_keyword(c, kInset) != NULL) {
            have_inset = true;
            lengths_over = n > 0;
        } else if (!have_colour && vc_color(c, &colour)) {
            have_colour = true;
            lengths_over = n > 0;
        } else if (!lengths_over && n < (box ? 4 : 3) &&
                   vc_dim(c, ACCEPT_LENGTH, n != 2, false, &s->a, &len[n])) {
            n++;
        } else {
            return false;
        }
    }
    if (n < 2)
        return false;
    int k = 0;
    if (have_inset)
        items[k++] = kw("inset");
    for (int i = 0; i < 4; i++) {
        if (i < n) {
            items[k++] = len[i];
        } else {
            os64_memset(&items[k], 0, sizeof(items[k]));
            items[k].kind = GARB_V_LENGTH;
            items[k++].unit = GARB_U_PX;
        }
    }
    if (!have_colour) {
        os64_memset(&colour, 0, sizeof(colour));
        colour.kind = GARB_V_COLOR;
        colour.color.current = true;
    }
    items[k++] = colour;
    os64_memset(out, 0, sizeof(*out));
    out->kind = GARB_V_LENGTH;
    out->items = keep(s, items, k);
    out->nitems = k;
    return out->items != NULL;
}

// `none`, or shadows separated by commas, the first painted on top — as
// many as a page writes: pixel art is drawn with hundreds of them.
static bool shadow_list(Sets *s, VCur *c, bool box, garb_val_t *out)
{
    if (vc_keyword(c, kNone) != NULL) {
        *out = kw("none");
        return true;
    }
    garb_val_t *list = NULL;
    int32_t n = 0, cap = 0;
    bool ok = true;
    for (;;) {
        if (n == cap) {
            int32_t cap2 = cap != 0 ? cap * 2 : 8;
            garb_val_t *grown = os64_realloc(list, (size_t)cap2 * sizeof(*grown));
            if (grown == NULL) {
                // Not the arena's, so said here: a list cut short by memory
                // is an incomplete read, never an invalid declaration
                // (Quinn, #203).
                s->a.short_of_memory = true;
                ok = false;
                break;
            }
            list = grown;
            cap = cap2;
        }
        if (!shadow_one(s, c, box, &list[n])) {
            ok = false;
            break;
        }
        n++;
        if (vc_done(c))
            break;
        c->i++;                 // shadow_one stopped at a comma
    }
    if (ok) {
        os64_memset(out, 0, sizeof(*out));
        out->kind = GARB_V_LENGTH;
        out->items = keep(s, list, n);
        out->nitems = n;
        out->comma = true;
        ok = out->items != NULL;
    }
    os64_free(list);
    return ok;
}

static bool margin_one(Sets *s, VCur *c, garb_val_t *out)
{
    return kw_or_dim(s, c, kAuto, ACCEPT_LENGTH | ACCEPT_PERCENT, true, true, out);
}

static bool padding_one(Sets *s, VCur *c, garb_val_t *out)
{
    return kw_or_dim(s, c, NULL, ACCEPT_LENGTH | ACCEPT_PERCENT, false, true, out);
}

static bool bwidth_one(Sets *s, VCur *c, garb_val_t *out)
{
    return kw_or_dim(s, c, kBorderWidth, ACCEPT_LENGTH, false, true, out);
}

static bool bstyle_one(Sets *s, VCur *c, garb_val_t *out)
{
    (void)s;
    const char *k = vc_keyword(c, kBorderStyle);
    if (k == NULL)
        return false;
    *out = kw(k);
    return true;
}

static bool bcolor_one(Sets *s, VCur *c, garb_val_t *out)
{
    (void)s;
    return vc_color(c, out);
}

// `border` and `border-top` alike (Backgrounds 3 § 4.4): a width, a style
// and a colour, any order, each at most once; each left out is its
// initial value (medium, none, currentColor).
static bool border_side(Sets *s, VCur *c, const garb_prop_t *sides, int nsides)
{
    garb_val_t width = kw("medium"), style = kw("none"), color = initial_color();
    bool have_w = false, have_s = false, have_c = false;
    while (!vc_done(c)) {
        garb_val_t v;
        if (!have_w && bwidth_one(s, c, &v)) {
            width = v;
            have_w = true;
        } else if (!have_s && bstyle_one(s, c, &v)) {
            style = v;
            have_s = true;
        } else if (!have_c && vc_color(c, &v)) {
            color = v;
            have_c = true;
        } else {
            return false;
        }
    }
    if (!have_w && !have_s && !have_c)
        return false;
    for (int k = 0; k < nsides; k++) {
        set(s, (garb_prop_t)(GARB_BORDER_TOP_WIDTH + sides[k]), &width);
        set(s, (garb_prop_t)(GARB_BORDER_TOP_STYLE + sides[k]), &style);
        set(s, (garb_prop_t)(GARB_BORDER_TOP_COLOR + sides[k]), &color);
    }
    return true;
}

// ── Fonts (CSS Fonts 4 § 2-3) ───────────────────────────────────────────

// font-family: family names — a string, or identifiers joined by single
// spaces — and generic families, comma-separated.
static bool families(Sets *s, VCur *c, garb_val_t *out)
{
    garb_val_t items[64];
    int32_t n = 0;
    for (;;) {
        const garb_value_t *t = vc_peek(c);
        if (t == NULL || n == 64)
            return false;
        garb_val_t item = {0};
        if (t->kind == GARB_STRING) {
            item.kind = GARB_V_STRING;
            item.text = t->text;
            item.len = t->len;
            c->i++;
        } else if (t->kind == GARB_IDENT) {
            const char *generic = NULL;
            VCur look = *c;
            look.i++;
            const garb_value_t *after = vc_peek(&look);
            if (after == NULL || after->kind == GARB_COMMA)
                generic = vc_keyword(c, kGeneric);
            if (generic != NULL) {
                item = kw(generic);
            } else {
                // Identifiers joined with one space each; a CSS-wide or
                // `default` keyword may not be one of them.
                size_t total = 0;
                int32_t start = c->i, words = 0;
                while ((t = vc_peek(c)) != NULL && t->kind == GARB_IDENT) {
                    if (vc_keyword(&(VCur){c->v, c->n, c->i}, kWide) != NULL ||
                        ieq(t->text, t->len, "default"))
                        return false;
                    total += t->len + 1;
                    words++;
                    c->i++;
                }
                char *name = os64_arena_alloc(s->a.arena, total);
                if (name == NULL) {
                    s->a.short_of_memory = true;
                    return false;
                }
                size_t at = 0;
                for (int32_t k = start; k < c->i; k++) {
                    if (c->v[k].kind != GARB_IDENT)
                        continue;
                    if (at > 0)
                        name[at++] = ' ';
                    os64_memcpy(name + at, c->v[k].text, c->v[k].len);
                    at += c->v[k].len;
                }
                name[at] = '\0';
                (void)words;
                item.kind = GARB_V_STRING;
                item.text = name;
                item.len = at;
            }
        } else {
            return false;
        }
        items[n++] = item;
        t = vc_peek(c);
        if (t == NULL)
            break;
        if (t->kind != GARB_COMMA)
            return false;
        c->i++;
    }
    os64_memset(out, 0, sizeof(*out));
    out->items = keep(s, items, n);
    if (out->items == NULL)
        return false;
    out->nitems = n;
    out->comma = true;
    out->kind = GARB_V_STRING;
    return true;
}

static bool font_size(Sets *s, VCur *c, garb_val_t *out)
{
    return kw_or_dim(s, c, kFontSize, ACCEPT_LENGTH | ACCEPT_PERCENT, false, true, out);
}

static bool font_weight(Sets *s, VCur *c, garb_val_t *out)
{
    const char *k = vc_keyword(c, kFontWeight);
    if (k != NULL) {
        *out = kw(k);
        return true;
    }
    VCur look = *c;
    if (!vc_dim(&look, ACCEPT_NUMBER, false, false, &s->a, out))
        return false;
    if (out->kind == GARB_V_NUMBER && (out->number < 1 || out->number > 1000))
        return false;
    *c = look;
    return true;
}

static bool font_style(Sets *s, VCur *c, garb_val_t *out)
{
    (void)s;
    const char *k = vc_keyword(c, kFontStyle);
    if (k == NULL)
        return false;
    *out = kw(k);
    // `oblique <angle>`: the angle is read and not kept — an oblique is
    // drawn as the italic face (packet 04's faces).
    const garb_value_t *t = vc_peek(c);
    if (k == kFontStyle[2] && t != NULL && t->kind == GARB_DIMENSION &&
        (ieq(t->unit, t->unit_len, "deg") || ieq(t->unit, t->unit_len, "rad") ||
         ieq(t->unit, t->unit_len, "grad") || ieq(t->unit, t->unit_len, "turn")))
        c->i++;
    return true;
}

static bool line_height(Sets *s, VCur *c, garb_val_t *out)
{
    return kw_or_dim(s, c, kNormal, ACCEPT_LENGTH | ACCEPT_PERCENT | ACCEPT_NUMBER, false, false,
                     out);
}

// The `font` shorthand (Fonts 4 § 3.7): [style || variant || weight ||
// stretch]? size [/ line-height]? family — or a system font. Everything it
// leaves out is reset to its initial value.
static bool font_shorthand(Sets *s, VCur *c)
{
    const char *system = vc_keyword(c, kSystemFont);
    if (system != NULL) {
        if (!vc_done(c))
            return false;
        // The system's own font: this machine's is the UI's sans.
        garb_val_t fam = {0}, item = kw("system-ui");
        fam.kind = GARB_V_STRING;
        fam.items = keep(s, &item, 1);
        fam.nitems = 1;
        fam.comma = true;
        garb_val_t size = kw("medium"), normal = kw("normal");
        set(s, GARB_FONT_FAMILY, &fam);
        set(s, GARB_FONT_SIZE, &size);
        set(s, GARB_FONT_WEIGHT, &normal);
        set(s, GARB_FONT_STYLE, &normal);
        set(s, GARB_FONT_VARIANT, &normal);
        set(s, GARB_LINE_HEIGHT, &normal);
        return fam.items != NULL;
    }
    garb_val_t style = kw("normal"), variant = kw("normal"), weight = kw("normal");
    bool have_style = false, have_variant = false, have_weight = false, have_stretch = false;
    int normals = 0;
    for (int k = 0; k < 4; k++) {
        garb_val_t v;
        VCur look = *c;
        if (vc_keyword(&look, kNormal) != NULL) {
            // `normal` fills whichever of the four has not been named.
            *c = look;
            normals++;
            continue;
        }
        if (!have_style && font_style(s, c, &v)) {
            style = v;
            have_style = true;
        } else if (!have_variant && vc_keyword(&look, kFontVariant) != NULL) {
            *c = look;
            variant = kw("small-caps");
            have_variant = true;
        } else if (!have_weight && font_weight(s, c, &v)) {
            weight = v;
            have_weight = true;
        } else if (!have_stretch && vc_keyword(c, kFontStretch) != NULL) {
            have_stretch = true;
        } else {
            break;
        }
    }
    if (normals + have_style + have_variant + have_weight + have_stretch > 4)
        return false;
    garb_val_t size, lh = kw("normal"), fam;
    if (!font_size(s, c, &size))
        return false;
    if (is_delim_v(vc_peek(c), '/')) {
        c->i++;
        if (!line_height(s, c, &lh))
            return false;
    }
    if (!families(s, c, &fam) || !vc_done(c))
        return false;
    set(s, GARB_FONT_STYLE, &style);
    set(s, GARB_FONT_VARIANT, &variant);
    set(s, GARB_FONT_WEIGHT, &weight);
    set(s, GARB_FONT_SIZE, &size);
    set(s, GARB_LINE_HEIGHT, &lh);
    set(s, GARB_FONT_FAMILY, &fam);
    return true;
}

// ── Backgrounds (Backgrounds 3 § 3) ─────────────────────────────────────

// <bg-position> (Backgrounds 3 § 3.6), read into the two longhands: a
// keyword and an offset from its edge become an offset from the left or
// top — `right 10px` is calc(100% - 10px), as the property computes.
static bool edge_offset(Sets *s, const char *edge, const garb_val_t *offset, garb_val_t *out)
{
    bool far = edge != NULL && (ieq(edge, os64_strlen(edge), "right") ||
                                ieq(edge, os64_strlen(edge), "bottom"));
    bool middle = edge != NULL && ieq(edge, os64_strlen(edge), "center");
    if (middle) {
        *out = percent(50);
        return offset == NULL;
    }
    if (offset == NULL) {
        *out = percent(far ? 100 : 0);
        return true;
    }
    if (!far) {
        *out = *offset;
        return true;
    }
    // 100% - offset, as a sum.
    garb_calc_t *sum = os64_arena_calloc(s->a.arena, 1, sizeof(*sum));
    garb_calc_t *hundred = os64_arena_calloc(s->a.arena, 1, sizeof(*hundred));
    garb_calc_t *off = os64_arena_calloc(s->a.arena, 1, sizeof(*off));
    const garb_calc_t **args = os64_arena_alloc(s->a.arena, 2 * sizeof(*args));
    bool *inv = os64_arena_alloc(s->a.arena, 2 * sizeof(*inv));
    if (sum == NULL || hundred == NULL || off == NULL || args == NULL || inv == NULL) {
        s->a.short_of_memory = true;
        return false;
    }
    hundred->op = GARB_CALC_LEAF;
    hundred->leaf = percent(100);
    if (offset->kind == GARB_V_CALC) {
        *off = *offset->calc;
    } else {
        off->op = GARB_CALC_LEAF;
        off->leaf = *offset;
    }
    args[0] = hundred;
    args[1] = off;
    inv[0] = false;
    inv[1] = true;
    sum->op = GARB_CALC_SUM;
    sum->args = (const garb_calc_t *const *)args;
    sum->invert = inv;
    sum->nargs = 2;
    os64_memset(out, 0, sizeof(*out));
    out->kind = GARB_V_CALC;
    out->calc = sum;
    return true;
}

static bool position(Sets *s, VCur *c, garb_val_t *x, garb_val_t *y)
{
    // Up to four parts, each a keyword or a length-percentage.
    const char *word[4] = {NULL, NULL, NULL, NULL};
    garb_val_t off[4];
    bool has_off[4] = {false, false, false, false};
    int n = 0;
    while (n < 4) {
        const char *k = vc_keyword(c, kPosAll);
        if (k != NULL) {
            word[n++] = k;
            continue;
        }
        garb_val_t v;
        if (!vc_dim(c, ACCEPT_LENGTH | ACCEPT_PERCENT, true, s->quirks, &s->a, &v))
            break;
        off[n] = v;
        has_off[n] = true;
        n++;
    }
    if (n == 0)
        return false;
    #define IS(i, w) (word[i] != NULL && os64_streq(word[i], (w)))
    #define HORIZ(i) (IS(i, "left") || IS(i, "right"))
    #define VERT(i) (IS(i, "top") || IS(i, "bottom"))
    if (n == 1) {
        if (VERT(0))
            return edge_offset(s, "center", NULL, x) && edge_offset(s, word[0], NULL, y);
        if (has_off[0])
            *x = off[0];
        else if (!edge_offset(s, word[0], NULL, x))
            return false;
        return edge_offset(s, "center", NULL, y);
    }
    if (n == 2) {
        // A keyword pair may come in either order; with a length the first
        // is horizontal.
        if (word[0] != NULL && word[1] != NULL && (VERT(0) || HORIZ(1))) {
            if (VERT(1) || HORIZ(0))
                return false;
            return edge_offset(s, word[1], NULL, x) && edge_offset(s, word[0], NULL, y);
        }
        if (VERT(0) || HORIZ(1))
            return false;
        if (has_off[0])
            *x = off[0];
        else if (!edge_offset(s, word[0], NULL, x))
            return false;
        if (has_off[1])
            *y = off[1];
        else if (!edge_offset(s, word[1], NULL, y))
            return false;
        return true;
    }
    // Three or four: keyword-offset pairs, an offset after any keyword but
    // center, one pair horizontal and one vertical, in either order.
    const char *pw[2];
    const garb_val_t *po[2];
    int pairs = 0;
    for (int i = 0; i < n; i++) {
        if (word[i] == NULL || pairs == 2)
            return false;               // an offset must follow its keyword
        pw[pairs] = word[i];
        po[pairs] = NULL;
        if (i + 1 < n && has_off[i + 1]) {
            if (IS(i, "center"))
                return false;
            po[pairs] = &off[i + 1];
            i++;
        }
        pairs++;
    }
    if (pairs != 2)
        return false;
    bool swap = (pw[0] != NULL && (os64_streq(pw[0], "top") || os64_streq(pw[0], "bottom"))) ||
                (pw[1] != NULL && (os64_streq(pw[1], "left") || os64_streq(pw[1], "right")));
    const char *kx = pw[swap ? 1 : 0], *ky = pw[swap ? 0 : 1];
    const garb_val_t *ox = po[swap ? 1 : 0], *oy = po[swap ? 0 : 1];
    if (os64_streq(kx, "top") || os64_streq(kx, "bottom") || os64_streq(ky, "left") ||
        os64_streq(ky, "right"))
        return false;
    return edge_offset(s, kx, ox, x) && edge_offset(s, ky, oy, y);
    #undef IS
    #undef HORIZ
    #undef VERT
}

// One or two repeat keywords, as the single keyword they amount to.
// ── Gradients (Images 3 § 3) ────────────────────────────────────────────
//
// A gradient is drawn by nobody yet, but whether one is VALID decides what
// a `background` does: the pre-standard `linear-gradient(top, …)` (no
// `to`) is refused by every browser, so the `background: #333` before it
// stands, and a page that relied on that is dark. So a linear or radial
// gradient is read against its grammar; conic gradients and image-set are
// accepted by name (GARB.md's pile-3 row says so).

// One comma-separated argument group of a gradient: [from, to) of `v`.
typedef struct {
    const garb_value_t *v;
    int32_t n;
} Group;

static bool only_ws(const Group *g)
{
    for (int32_t i = 0; i < g->n; i++)
        if (g->v[i].kind != GARB_WHITESPACE)
            return false;
    return true;
}

static bool angle(VCur *c)
{
    const garb_value_t *t = vc_peek(c);
    if (t == NULL)
        return false;
    bool ok = (t->kind == GARB_DIMENSION &&
               (ieq(t->unit, t->unit_len, "deg") || ieq(t->unit, t->unit_len, "grad") ||
                ieq(t->unit, t->unit_len, "rad") || ieq(t->unit, t->unit_len, "turn"))) ||
              (t->kind == GARB_NUMBER && t->number == 0);   // Images 3: a unitless zero
    if (ok)
        c->i++;
    return ok;
}

static bool length_percentage(Sets *s, VCur *c)
{
    garb_val_t v;
    return vc_dim(c, ACCEPT_LENGTH | ACCEPT_PERCENT, true, false, &s->a, &v);
}

// `<color> <length-percentage>{0,2}` (a stop), or one length-percentage
// alone (a hint).
static bool stop(Sets *s, const Group *g, bool *hint)
{
    VCur c = {g->v, g->n, 0};
    garb_val_t v;
    *hint = false;
    if (vc_color(&c, &v)) {
        for (int k = 0; k < 2 && !vc_done(&c); k++)
            if (!length_percentage(s, &c))
                return false;
        return vc_done(&c);
    }
    *hint = true;
    return length_percentage(s, &c) && vc_done(&c);
}

// §3.4: at least two stops, a hint only between two of them.
static bool stops(Sets *s, const Group *g, int32_t n)
{
    int32_t colours = 0;
    bool last_hint = true;          // nothing before the first: it may not be a hint
    for (int32_t i = 0; i < n; i++) {
        bool hint;
        if (!stop(s, &g[i], &hint) || (hint && last_hint))
            return false;
        colours += !hint;
        last_hint = hint;
    }
    return colours >= 2 && !last_hint;
}

// `to` [left | right] || [top | bottom], or an angle.
static bool linear_prelude(const Group *g)
{
    VCur c = {g->v, g->n, 0};
    static const char *const kTo[] = {"to", NULL};
    static const char *const kX[] = {"left", "right", NULL};
    static const char *const kY[] = {"top", "bottom", NULL};
    if (vc_keyword(&c, kTo) != NULL) {
        bool x = vc_keyword(&c, kX) != NULL, y = vc_keyword(&c, kY) != NULL;
        if (!x && vc_keyword(&c, kX) != NULL)
            x = true;
        return (x || y) && vc_done(&c);
    }
    return angle(&c) && vc_done(&c);
}

// [<ending-shape> || <size>]? [at <position>]?, one of them at least. The
// size's finer rules (a circle takes one length and no percentage) are not
// checked: what they refuse is rare, and nothing draws it yet.
static bool radial_prelude(Sets *s, const Group *g)
{
    VCur c = {g->v, g->n, 0};
    static const char *const kShape[] = {"circle", "ellipse", NULL};
    static const char *const kExtent[] = {"closest-side", "farthest-side", "closest-corner",
                                          "farthest-corner", NULL};
    static const char *const kAt[] = {"at", NULL};
    bool shape = false, size = false, any = false;
    for (int k = 0; k < 2; k++) {
        if (!shape && vc_keyword(&c, kShape) != NULL) {
            shape = any = true;
        } else if (!size && vc_keyword(&c, kExtent) != NULL) {
            size = any = true;
        } else if (!size && length_percentage(s, &c)) {
            (void)length_percentage(s, &c);     // an ellipse's second radius
            size = any = true;
        }
    }
    if (vc_keyword(&c, kAt) != NULL) {
        Sets strict = *s;
        strict.quirks = false;
        garb_val_t x, y;
        bool ok = position(&strict, &c, &x, &y);
        s->a.short_of_memory |= strict.a.short_of_memory;
        if (!ok)
            return false;
        any = true;
    }
    return any && vc_done(&c);
}

// Whether `f`, a linear or radial gradient function, fits its grammar.
static bool gradient_ok(Sets *s, const garb_value_t *f, bool radial)
{
    Group g[64];
    int32_t n = 0, from = 0;
    for (int32_t i = 0; i <= f->nchildren; i++)
        if (i == f->nchildren || f->children[i].kind == GARB_COMMA) {
            if (n == (int32_t)(sizeof(g) / sizeof(g[0])))
                return false;               // past 63 stops: refused, not guessed
            g[n++] = (Group){f->children + from, i - from};
            from = i + 1;
        }
    for (int32_t i = 0; i < n; i++)
        if (only_ws(&g[i]))
            return false;
    if (n > 0 && (radial ? radial_prelude(s, &g[0]) : linear_prelude(&g[0])))
        return stops(s, g + 1, n - 1);
    return stops(s, g, n);
}

// ── A gradient kept (values.h's GARB_V_IMAGE) ──

// One length-percentage, read.
static bool lp_val(Sets *s, VCur *c, garb_val_t *out)
{
    return vc_dim(c, ACCEPT_LENGTH | ACCEPT_PERCENT, true, false, &s->a, out);
}

static garb_val_t number_val(double n)
{
    garb_val_t v;
    os64_memset(&v, 0, sizeof(v));
    v.kind = GARB_V_NUMBER;
    v.number = n;
    return v;
}

// The stops of groups [0, n), each a COLOR with its position or a hint,
// appended to `out` from `*k`; a stop written with two positions is two.
// The grammar is already checked.
static bool gradient_stops(Sets *s, const Group *g, int32_t n, garb_val_t *out, int32_t *k)
{
    for (int32_t i = 0; i < n; i++) {
        VCur c = {g[i].v, g[i].n, 0};
        garb_val_t colour, pos;
        if (!vc_color(&c, &colour)) {
            if (!lp_val(s, &c, &out[*k]))
                return false;
            (*k)++;
            continue;
        }
        int positions = 0;
        while (!vc_done(&c) && lp_val(s, &c, &pos)) {
            out[*k] = colour;
            out[*k].items = keep(s, &pos, 1);
            out[*k].nitems = 1;
            if (out[*k].items == NULL)
                return false;
            (*k)++;
            positions++;
        }
        if (positions == 0)
            out[(*k)++] = colour;
    }
    return true;
}

// The angle a linear gradient's prelude names, in degrees, or its `to`.
static garb_val_t linear_geometry(const Group *g, bool have)
{
    static const char *const kTo[] = {"to", NULL};
    static const char *const kX[] = {"left", "right", NULL};
    static const char *const kY[] = {"top", "bottom", NULL};
    static const char *const kNames[3][3] = {
        {"to top left", "to top", "to top right"},
        {"to left", NULL, "to right"},
        {"to bottom left", "to bottom", "to bottom right"},
    };
    if (!have)
        return number_val(180);             // the initial direction: to bottom
    VCur c = {g->v, g->n, 0};
    if (vc_keyword(&c, kTo) != NULL) {
        int x = 1, y = 1;
        for (int k = 0; k < 2 && !vc_done(&c); k++) {
            const char *w = vc_keyword(&c, kX);
            if (w != NULL)
                x = os64_streq(w, "left") ? 0 : 2;
            else if ((w = vc_keyword(&c, kY)) != NULL)
                y = os64_streq(w, "top") ? 0 : 2;
        }
        return kw(kNames[y][x]);
    }
    const garb_value_t *t = vc_peek(&c);
    double n = t->number;
    if (t->kind == GARB_DIMENSION) {
        n = ieq(t->unit, t->unit_len, "grad") ? n * 0.9
            : ieq(t->unit, t->unit_len, "rad") ? n * 57.29577951308232
            : ieq(t->unit, t->unit_len, "turn") ? n * 360 : n;
    }
    return number_val(n);
}

// A radial gradient's prelude, or its initial one: an ellipse to the
// farthest corner, at the centre. A shape left out is a circle when one
// length is its size, else an ellipse (Images 3 § 3.2).
static bool radial_geometry(Sets *s, const Group *g, bool have, garb_val_t *out)
{
    static const char *const kShape[] = {"circle", "ellipse", NULL};
    static const char *const kExtent[] = {"closest-side", "farthest-side", "closest-corner",
                                          "farthest-corner", NULL};
    static const char *const kAt[] = {"at", NULL};
    garb_val_t parts[4];
    parts[0] = kw("farthest-corner");
    parts[1] = kw("auto");
    garb_val_t half;
    os64_memset(&half, 0, sizeof(half));
    half.kind = GARB_V_PERCENTAGE;
    half.number = 50;
    parts[2] = parts[3] = half;
    const char *shape = NULL;
    int lengths = 0;
    if (have) {
        VCur c = {g->v, g->n, 0};
        for (int k = 0; k < 2 && !vc_done(&c); k++) {
            const char *w;
            garb_val_t a, b;
            if (shape == NULL && (w = vc_keyword(&c, kShape)) != NULL) {
                shape = w;
            } else if ((w = vc_keyword(&c, kExtent)) != NULL) {
                parts[0] = kw(w);
            } else if (lp_val(s, &c, &a)) {
                parts[0] = a;
                lengths = 1;
                if (lp_val(s, &c, &b)) {
                    parts[1] = b;
                    lengths = 2;
                }
            }
        }
        if (vc_keyword(&c, kAt) != NULL) {
            Sets strict = *s;
            strict.quirks = false;
            bool ok = position(&strict, &c, &parts[2], &parts[3]);
            s->a.short_of_memory |= strict.a.short_of_memory;
            if (!ok)
                return false;
        }
    }
    *out = kw(shape != NULL ? shape : lengths == 1 ? "circle" : "ellipse");
    out->items = keep(s, parts, 4);
    out->nitems = 4;
    return out->items != NULL;
}

// The gradient `f` (its grammar checked) as values.h describes it, into
// `out`, which vc_image has made its IMAGE.
static bool gradient_value(Sets *s, const garb_value_t *f, bool radial, garb_val_t *out)
{
    Group g[64];
    int32_t n = 0, from = 0;
    for (int32_t i = 0; i <= f->nchildren; i++)
        if (i == f->nchildren || f->children[i].kind == GARB_COMMA) {
            g[n++] = (Group){f->children + from, i - from};
            from = i + 1;
        }
    // The prelude by gradient_ok's own test, which asks it first: a unitless
    // 0 is an angle there, though it would read as a hint too.
    bool prelude = n > 0 && (radial ? radial_prelude(s, &g[0]) : linear_prelude(&g[0]));
    garb_val_t items[1 + 2 * 64];
    int32_t k = 1;
    if (radial) {
        if (!radial_geometry(s, &g[0], prelude, &items[0]))
            return false;
    } else {
        items[0] = linear_geometry(&g[0], prelude);
    }
    if (!gradient_stops(s, g + prelude, n - prelude, items, &k))
        return false;
    out->items = keep(s, items, k);
    out->nitems = k;
    out->comma = true;
    return out->items != NULL;
}

// An <image>: a url, or a gradient the grammar above admits, kept whole,
// or one this library takes by name (vc_image).
static bool bg_image(Sets *s, VCur *c, garb_val_t *out)
{
    const garb_value_t *t = vc_peek(c);
    bool linear = false, radial = false;
    if (t != NULL && t->kind == GARB_FUNCTION) {
        linear = ieq(t->text, t->len, "linear-gradient") ||
                 ieq(t->text, t->len, "repeating-linear-gradient");
        radial = ieq(t->text, t->len, "radial-gradient") ||
                 ieq(t->text, t->len, "repeating-radial-gradient");
        if ((linear || radial) && !gradient_ok(s, t, radial))
            return false;
    }
    if (!vc_image(c, out))
        return false;
    return !(linear || radial) || gradient_value(s, t, radial, out);
}

static bool repeat(VCur *c, garb_val_t *out)
{
    const char *one = vc_keyword(c, kRepeat);
    if (one != NULL) {
        *out = kw(one);
        return true;
    }
    const char *a = vc_keyword(c, kRepeat2);
    if (a == NULL)
        return false;
    const char *b = vc_keyword(c, kRepeat2);
    // What the tiler can draw: `space` and `round` tile as `repeat` does.
    bool ra = !os64_streq(a, "no-repeat"), rb = b == NULL ? ra : !os64_streq(b, "no-repeat");
    *out = kw(ra && rb ? "repeat" : ra ? "repeat-x" : rb ? "repeat-y" : "no-repeat");
    return true;
}

// A background value per layer, gathered as a list is read.
typedef struct {
    garb_val_t *v;
    int32_t n, cap;
} Layers;

static bool layers_add(Sets *s, Layers *l, const garb_val_t *v)
{
    if (l->n == l->cap) {
        int32_t cap = l->cap != 0 ? l->cap * 2 : 4;
        garb_val_t *grown = os64_realloc(l->v, (size_t)cap * sizeof(*grown));
        if (grown == NULL) {
            // Not the arena's, so said here, as shadow_list says it.
            s->a.short_of_memory = true;
            return false;
        }
        l->v = grown;
        l->cap = cap;
    }
    l->v[l->n++] = *v;
    return true;
}

// The list as one value — its one layer's, or a LAYERS of them all — and
// the list freed. False when the arena is out of room.
static bool layers_done(Sets *s, Layers *l, garb_val_t *out)
{
    bool ok = l->n > 0;
    if (ok && l->n == 1) {
        *out = l->v[0];
    } else if (ok) {
        os64_memset(out, 0, sizeof(*out));
        out->kind = GARB_V_LAYERS;
        out->items = keep(s, l->v, l->n);
        out->nitems = l->n;
        out->comma = true;
        ok = out->items != NULL;
    }
    os64_free(l->v);
    *l = (Layers){0};
    return ok;
}

// <bg-size> (Backgrounds 3 § 3.9): cover, contain, or a width and a height,
// each a length-percentage or auto, the height auto when not written.
static bool bg_size(Sets *s, VCur *c, garb_val_t *out)
{
    const char *k = vc_keyword(c, kBgSize);
    if (k != NULL) {
        *out = kw(k);
        return true;
    }
    garb_val_t two[2];
    int n = 0;
    while (n < 2 && kw_or_dim(s, c, kAuto, ACCEPT_LENGTH | ACCEPT_PERCENT, false, false, &two[n]))
        n++;
    if (n == 0)
        return false;
    if (n == 1)
        two[1] = kw("auto");
    os64_memset(out, 0, sizeof(*out));
    out->kind = GARB_V_LENGTH;
    out->items = keep(s, two, 2);
    out->nitems = 2;
    return out->items != NULL;
}

static garb_val_t auto_size(Sets *s)
{
    garb_val_t v;
    garb_val_t two[2] = {kw("auto"), kw("auto")};
    os64_memset(&v, 0, sizeof(v));
    v.kind = GARB_V_LENGTH;
    v.items = keep(s, two, 2);
    v.nitems = 2;
    return v;
}

// The longhands each layer of the `background` shorthand sets, in the
// order they are set.
enum { L_IMAGE, L_REPEAT, L_X, L_Y, L_SIZE, L_ORIGIN, L_CLIP, L_N };
static const garb_prop_t kLayerProps[L_N] = {
    GARB_BACKGROUND_IMAGE, GARB_BACKGROUND_REPEAT, GARB_BACKGROUND_POSITION_X,
    GARB_BACKGROUND_POSITION_Y, GARB_BACKGROUND_SIZE, GARB_BACKGROUND_ORIGIN,
    GARB_BACKGROUND_CLIP,
};

// The shorthand's layers, each one's values added to `lists`, and the
// colour the last may carry.
static bool background_layers(Sets *s, VCur *c, Layers lists[L_N], garb_val_t *color)
{
    for (;;) {
        bool have_img = false, have_pos = false, have_rep = false, have_att = false,
             have_color = false;
        int boxes = 0;
        const char *box[2] = {NULL, NULL};
        garb_val_t l[L_N] = {kw("none"), kw("repeat"), percent(0), percent(0), auto_size(s),
                             kw("padding-box"), kw("border-box")};
        if (l[L_SIZE].items == NULL)
            return false;
        for (;;) {
            const garb_value_t *t = vc_peek(c);
            if (t == NULL || t->kind == GARB_COMMA)
                break;
            garb_val_t v, x, y;
            if (!have_img && vc_keyword(c, kNone) != NULL) {
                have_img = true;
            } else if (!have_img && bg_image(s, c, &v)) {
                l[L_IMAGE] = v;
                have_img = true;
            } else if (!have_pos && position(s, c, &x, &y)) {
                l[L_X] = x;
                l[L_Y] = y;
                have_pos = true;
                if (is_delim_v(vc_peek(c), '/')) {
                    c->i++;
                    if (!bg_size(s, c, &l[L_SIZE]))
                        return false;
                }
            } else if (!have_rep && repeat(c, &v)) {
                l[L_REPEAT] = v;
                have_rep = true;
            } else if (!have_att && vc_keyword(c, kAttachment) != NULL) {
                have_att = true;
            } else if (boxes < 2 && (box[boxes] = vc_keyword(c, kClipBox)) != NULL) {
                boxes++;
            } else if (!have_color && vc_color(c, &v)) {
                *color = v;
                have_color = true;
            } else {
                return false;
            }
        }
        bool last = vc_peek(c) == NULL;
        if (have_color && !last)
            return false;               // only the final layer has a colour
        if (!have_img && !have_pos && !have_rep && !have_att && boxes == 0 && !have_color)
            return false;               // an empty layer
        // `text` clips and never places: the origin is the other box, or
        // stays as it was when there is none.
        if (boxes == 2 && os64_streq(box[0], "text"))
            return false;
        if (boxes > 0) {
            l[L_CLIP] = kw(box[boxes - 1]);
            if (!os64_streq(box[0], "text"))
                l[L_ORIGIN] = kw(box[0]);
        }
        for (int k = 0; k < L_N; k++)
            if (!layers_add(s, &lists[k], &l[k]))
                return false;
        if (last)
            return true;
        c->i++;                         // the comma
    }
}

// The `background` shorthand: comma-separated layers, the top one first,
// the last of which may carry the colour (Backgrounds 3 § 3.10). One box
// sets the origin and the clip; two, the origin then the clip.
static bool background(Sets *s, VCur *c)
{
    Layers lists[L_N] = {{0}};
    garb_val_t color = transparent(), v[L_N];
    bool ok = background_layers(s, c, lists, &color);
    for (int k = 0; k < L_N; k++)
        ok = layers_done(s, &lists[k], &v[k]) && ok;    // each list freed, whatever
    if (!ok)
        return false;
    set(s, GARB_BACKGROUND_COLOR, &color);
    for (int k = 0; k < L_N; k++)
        set(s, kLayerProps[k], &v[k]);
    return true;
}

// ── Text decoration and lists ───────────────────────────────────────────

// <text-decoration-line>: none, or any of the lines, each once.
static bool decoration_line(Sets *s, VCur *c, garb_val_t *out)
{
    if (vc_keyword(c, kNone) != NULL) {
        *out = kw("none");
        return true;
    }
    garb_val_t items[4];
    int32_t n = 0;
    const char *k;
    while ((k = vc_keyword(c, kLines)) != NULL) {
        for (int32_t j = 0; j < n; j++)
            if (items[j].keyword == k)
                return false;
        items[n++] = kw(k);
    }
    if (n == 0)
        return false;
    os64_memset(out, 0, sizeof(*out));
    out->kind = GARB_V_KEYWORD;
    out->keyword = items[0].keyword;
    out->items = keep(s, items, n);
    out->nitems = n;
    return out->items != NULL;
}

// `text-decoration` (Text Decoration 3 § 2.4): the line is kept; its style,
// colour and thickness are read for validity and not drawn yet.
static bool text_decoration(Sets *s, VCur *c)
{
    garb_val_t line = kw("none"), v;
    bool have_line = false, have_style = false, have_color = false, have_thick = false;
    while (!vc_done(c)) {
        if (!have_line && decoration_line(s, c, &v)) {
            line = v;
            have_line = true;
        } else if (!have_style && vc_keyword(c, kDecorationStyle) != NULL) {
            have_style = true;
        } else if (!have_color && vc_color(c, &v)) {
            have_color = true;
        } else if (!have_thick && kw_or_dim(s, c, kThickness, ACCEPT_LENGTH | ACCEPT_PERCENT,
                                            false, false, &v)) {
            have_thick = true;
        } else {
            return false;
        }
    }
    if (!have_line && !have_style && !have_color && !have_thick)
        return false;
    set(s, GARB_TEXT_DECORATION_LINE, &line);
    return true;
}

// <counter-style>: any identifier names one (Lists 3); `none` is none.
static bool list_type(Sets *s, VCur *c, garb_val_t *out)
{
    (void)s;
    const garb_value_t *t = vc_peek(c);
    if (t == NULL)
        return false;
    os64_memset(out, 0, sizeof(*out));
    if (t->kind == GARB_STRING) {
        out->kind = GARB_V_STRING;
        out->text = t->text;
        out->len = t->len;
    } else if (t->kind == GARB_IDENT && vc_keyword(&(VCur){c->v, c->n, c->i}, kWide) == NULL) {
        // Predefined names match without case; the table keeps them lowercase.
        char *low = os64_arena_alloc(s->a.arena, t->len + 1);
        if (low == NULL) {
            s->a.short_of_memory = true;
            return false;
        }
        for (size_t k = 0; k < t->len; k++)
            low[k] = t->text[k] >= 'A' && t->text[k] <= 'Z' ? (char)(t->text[k] + 32) : t->text[k];
        low[t->len] = '\0';
        out->kind = GARB_V_KEYWORD;
        out->keyword = low;
    } else {
        return false;
    }
    c->i++;
    return true;
}

// `list-style` (Lists 3 § 3.4): type, position and image in any order; a
// lone `none` sets whichever of type and image was not otherwise named.
static bool list_style(Sets *s, VCur *c)
{
    garb_val_t type = kw("disc"), pos = kw("outside"), image = kw("none"), v;
    bool have_type = false, have_pos = false, have_image = false;
    int nones = 0;
    while (!vc_done(c)) {
        VCur look = *c;
        if (vc_keyword(&look, kNone) != NULL) {
            *c = look;
            nones++;
            continue;
        }
        const char *k;
        if (!have_pos && (k = vc_keyword(c, kListPosition)) != NULL) {
            pos = kw(k);
            have_pos = true;
        } else if (!have_image && vc_url(c, &v)) {
            image = v;
            have_image = true;
        } else if (!have_type && list_type(s, c, &v)) {
            type = v;
            have_type = true;
        } else {
            return false;
        }
    }
    if (nones > 2 || (nones == 2 && (have_type || have_image)) ||
        (nones == 1 && have_type && have_image))
        return false;
    if (nones >= 1 && !have_type)
        type = kw("none");
    if (!have_type && !have_pos && !have_image && nones == 0)
        return false;
    set(s, GARB_LIST_STYLE_TYPE, &type);
    set(s, GARB_LIST_STYLE_POSITION, &pos);
    set(s, GARB_LIST_STYLE_IMAGE, &image);
    return true;
}

// ── One property ────────────────────────────────────────────────────────

typedef enum {
    G_KEYWORDS,             // one keyword from `words`
    G_COLOR,
    G_MARGIN, G_PADDING, G_BWIDTH, G_BSTYLE, G_BCOLOR,
    G_SIZE,                 // width, height, min-*: auto | lp >= 0 | the content words
    G_MAX,                  // max-*: none | lp >= 0 | the content words
    G_FAMILY, G_FONT_SIZE, G_FONT_WEIGHT, G_FONT_STYLE, G_LINE_HEIGHT,
    G_VALIGN, G_DECORATION_LINE, G_INDENT, G_LIST_TYPE, G_LIST_IMAGE, G_BG_IMAGE,
    G_BG_REPEAT, G_BG_POS_X, G_BG_POS_Y, G_BG_SIZE,
    G_BG_BOX,               // background-origin, -clip: a box from `words`
    G_SPACING,
    G_Z_INDEX,              // auto | <integer>
    G_ALPHA,                // <alpha-value>: a number or a percentage, clamped when computed
    G_ALIGN,                // an alignment keyword from `words`, `unsafe` or `first` before it
    G_FACTOR,               // <number [0,∞]>: flex-grow, flex-shrink
    G_BASIS,                // flex-basis: `words` | lp >= 0
    G_INTEGER,              // <integer>: order
    G_GAP,                  // normal | lp >= 0
    G_TEMPLATE,             // grid-template-*: none | a track list
    G_AUTO_TRACKS,          // grid-auto-*: one or more track sizes
    G_AREAS,                // none | strings, a rectangle per name
    G_AUTO_FLOW,            // [row | column] || dense
    G_GRID_LINE,            // auto | <integer> | span <integer> | <custom-ident>
    G_RADIUS,               // a corner: <lp [0,∞]>{1,2}, horizontal then vertical
    G_SHADOW,               // none | <shadow>#, a box's (`inset`, spread) or text's
} Grammar;

typedef struct {
    garb_prop_t prop;
    Grammar g;
    const char *const *words;
} Longhand;

static const Longhand kLonghands[] = {
    {GARB_DISPLAY, G_KEYWORDS, kDisplay}, {GARB_COLOR, G_COLOR, NULL},
    {GARB_BACKGROUND_COLOR, G_COLOR, NULL}, {GARB_BACKGROUND_IMAGE, G_BG_IMAGE, NULL},
    {GARB_BACKGROUND_REPEAT, G_BG_REPEAT, NULL}, {GARB_BACKGROUND_POSITION_X, G_BG_POS_X, NULL},
    {GARB_BACKGROUND_POSITION_Y, G_BG_POS_Y, NULL}, {GARB_BACKGROUND_SIZE, G_BG_SIZE, NULL},
    {GARB_BACKGROUND_ORIGIN, G_BG_BOX, kBox}, {GARB_BACKGROUND_CLIP, G_BG_BOX, kClipBox},
    {GARB_MARGIN_TOP, G_MARGIN, NULL}, {GARB_MARGIN_RIGHT, G_MARGIN, NULL},
    {GARB_MARGIN_BOTTOM, G_MARGIN, NULL}, {GARB_MARGIN_LEFT, G_MARGIN, NULL},
    {GARB_PADDING_TOP, G_PADDING, NULL}, {GARB_PADDING_RIGHT, G_PADDING, NULL},
    {GARB_PADDING_BOTTOM, G_PADDING, NULL}, {GARB_PADDING_LEFT, G_PADDING, NULL},
    {GARB_BORDER_TOP_WIDTH, G_BWIDTH, NULL}, {GARB_BORDER_RIGHT_WIDTH, G_BWIDTH, NULL},
    {GARB_BORDER_BOTTOM_WIDTH, G_BWIDTH, NULL}, {GARB_BORDER_LEFT_WIDTH, G_BWIDTH, NULL},
    {GARB_BORDER_TOP_STYLE, G_BSTYLE, NULL}, {GARB_BORDER_RIGHT_STYLE, G_BSTYLE, NULL},
    {GARB_BORDER_BOTTOM_STYLE, G_BSTYLE, NULL}, {GARB_BORDER_LEFT_STYLE, G_BSTYLE, NULL},
    {GARB_BORDER_TOP_COLOR, G_BCOLOR, NULL}, {GARB_BORDER_RIGHT_COLOR, G_BCOLOR, NULL},
    {GARB_BORDER_BOTTOM_COLOR, G_BCOLOR, NULL}, {GARB_BORDER_LEFT_COLOR, G_BCOLOR, NULL},
    {GARB_WIDTH, G_SIZE, NULL}, {GARB_HEIGHT, G_SIZE, NULL}, {GARB_MIN_WIDTH, G_SIZE, NULL},
    {GARB_MIN_HEIGHT, G_SIZE, NULL}, {GARB_MAX_WIDTH, G_MAX, NULL},
    {GARB_MAX_HEIGHT, G_MAX, NULL}, {GARB_BOX_SIZING, G_KEYWORDS, kBoxSizing},
    {GARB_FONT_FAMILY, G_FAMILY, NULL}, {GARB_FONT_SIZE, G_FONT_SIZE, NULL},
    {GARB_FONT_WEIGHT, G_FONT_WEIGHT, NULL}, {GARB_FONT_STYLE, G_FONT_STYLE, NULL},
    {GARB_FONT_VARIANT, G_KEYWORDS, kFontVariant}, {GARB_LINE_HEIGHT, G_LINE_HEIGHT, NULL},
    {GARB_TEXT_ALIGN, G_KEYWORDS, kTextAlign}, {GARB_VERTICAL_ALIGN, G_VALIGN, NULL},
    {GARB_WHITE_SPACE, G_KEYWORDS, kWhiteSpace},
    {GARB_TEXT_DECORATION_LINE, G_DECORATION_LINE, NULL}, {GARB_TEXT_INDENT, G_INDENT, NULL},
    {GARB_TEXT_TRANSFORM, G_KEYWORDS, kTransform}, {GARB_VISIBILITY, G_KEYWORDS, kVisibility},
    {GARB_LIST_STYLE_TYPE, G_LIST_TYPE, NULL},
    {GARB_LIST_STYLE_POSITION, G_KEYWORDS, kListPosition},
    {GARB_LIST_STYLE_IMAGE, G_LIST_IMAGE, NULL}, {GARB_BORDER_SPACING, G_SPACING, NULL},
    {GARB_BORDER_COLLAPSE, G_KEYWORDS, kCollapse}, {GARB_CAPTION_SIDE, G_KEYWORDS, kCaption},
    {GARB_FLOAT, G_KEYWORDS, kFloat}, {GARB_CLEAR, G_KEYWORDS, kClear},
    {GARB_OVERFLOW_X, G_KEYWORDS, kOverflow}, {GARB_OVERFLOW_Y, G_KEYWORDS, kOverflow},
    // Position 3 § 3.1. The insets are the margins' grammar exactly — auto
    // or a length-percentage, negative allowed — and are on the Quirks
    // standard's unitless-length list (§ 3.2) with them.
    {GARB_POSITION, G_KEYWORDS, kPosition},
    {GARB_TOP, G_MARGIN, NULL}, {GARB_RIGHT, G_MARGIN, NULL},
    {GARB_BOTTOM, G_MARGIN, NULL}, {GARB_LEFT, G_MARGIN, NULL},
    {GARB_Z_INDEX, G_Z_INDEX, NULL}, {GARB_OPACITY, G_ALPHA, NULL},
    {GARB_POINTER_EVENTS, G_KEYWORDS, kPointerEvents},
    {GARB_FLEX_DIRECTION, G_KEYWORDS, kFlexDirection}, {GARB_FLEX_WRAP, G_KEYWORDS, kFlexWrap},
    {GARB_JUSTIFY_CONTENT, G_ALIGN, kJustify}, {GARB_ALIGN_ITEMS, G_ALIGN, kAlignItems},
    {GARB_ALIGN_SELF, G_ALIGN, kAlignSelf}, {GARB_ALIGN_CONTENT, G_ALIGN, kAlignContent},
    {GARB_FLEX_GROW, G_FACTOR, NULL}, {GARB_FLEX_SHRINK, G_FACTOR, NULL},
    {GARB_FLEX_BASIS, G_BASIS, kBasisWords}, {GARB_ORDER, G_INTEGER, NULL},
    {GARB_ROW_GAP, G_GAP, NULL}, {GARB_COLUMN_GAP, G_GAP, NULL},
    {GARB_GRID_TEMPLATE_COLUMNS, G_TEMPLATE, NULL}, {GARB_GRID_TEMPLATE_ROWS, G_TEMPLATE, NULL},
    {GARB_GRID_TEMPLATE_AREAS, G_AREAS, NULL},
    {GARB_GRID_AUTO_COLUMNS, G_AUTO_TRACKS, NULL}, {GARB_GRID_AUTO_ROWS, G_AUTO_TRACKS, NULL},
    {GARB_GRID_AUTO_FLOW, G_AUTO_FLOW, NULL},
    {GARB_GRID_COLUMN_START, G_GRID_LINE, NULL}, {GARB_GRID_COLUMN_END, G_GRID_LINE, NULL},
    {GARB_GRID_ROW_START, G_GRID_LINE, NULL}, {GARB_GRID_ROW_END, G_GRID_LINE, NULL},
    {GARB_JUSTIFY_ITEMS, G_ALIGN, kJustifyItems}, {GARB_JUSTIFY_SELF, G_ALIGN, kJustifySelf},
    {GARB_BORDER_TOP_LEFT_RADIUS, G_RADIUS, NULL}, {GARB_BORDER_TOP_RIGHT_RADIUS, G_RADIUS, NULL},
    {GARB_BORDER_BOTTOM_RIGHT_RADIUS, G_RADIUS, NULL},
    {GARB_BORDER_BOTTOM_LEFT_RADIUS, G_RADIUS, NULL},
    {GARB_BOX_SHADOW, G_SHADOW, NULL}, {GARB_TEXT_SHADOW, G_SHADOW, NULL},
};

static bool longhand_one(Sets *s, VCur *c, const Longhand *l, garb_val_t *out);

// ── Grid (Grid 2 § 7, § 8) ──────────────────────────────────────────────

// A function's arguments, split at their commas: up to `max` cursors.
static int32_t fn_args(const garb_value_t *f, VCur *args, int32_t max)
{
    int32_t n = 0, from = 0;
    for (int32_t i = 0; i <= f->nchildren; i++) {
        if (i == f->nchildren || f->children[i].kind == GARB_COMMA) {
            if (n == max)
                return -1;
            args[n++] = (VCur){f->children + from, i - from, 0};
            from = i + 1;
        }
    }
    return n;
}

static bool is_fn(const garb_value_t *t, const char *name)
{
    return t != NULL && t->kind == GARB_FUNCTION && ieq(t->text, t->len, name);
}

static garb_val_t fn_val(Sets *s, const char *name, const garb_val_t *args, int32_t n)
{
    garb_val_t v = {0};
    v.kind = GARB_V_FUNCTION;
    v.keyword = name;
    v.items = keep(s, args, n);
    v.nitems = v.items != NULL ? n : 0;
    v.comma = true;
    return v;
}

// A track breadth: a length or percentage (never negative), a flexible
// `<n>fr` where `flex` allows, or a sizing keyword where `words` does.
static bool track_breadth(Sets *s, VCur *c, bool flex, bool words, garb_val_t *out)
{
    const garb_value_t *t = vc_peek(c);
    if (t == NULL)
        return false;
    if (t->kind == GARB_DIMENSION && ieq(t->unit, t->unit_len, "fr")) {
        if (!flex || t->number < 0)
            return false;
        os64_memset(out, 0, sizeof(*out));
        out->kind = GARB_V_LENGTH;
        out->unit = GARB_U_FR;
        out->number = t->number;
        c->i++;
        return true;
    }
    const char *k = words ? vc_keyword(c, kTrackWords) : NULL;
    if (k != NULL) {
        *out = kw(k);
        return true;
    }
    return vc_dim(c, ACCEPT_LENGTH | ACCEPT_PERCENT, false, false, &s->a, out);
}

static bool fixed_breadth(const garb_val_t *v)
{
    return v->kind == GARB_V_PERCENTAGE || v->kind == GARB_V_CALC ||
           (v->kind == GARB_V_LENGTH && v->unit != GARB_U_FR);
}

// A track size (§ 7.2.1): a breadth, minmax(min, max) — whose minimum is
// never flexible — or fit-content(length-percentage). `fixed` says it has
// a fixed size at one end, which an auto-repeated track must.
static bool track_size(Sets *s, VCur *c, garb_val_t *out, bool *fixed)
{
    const garb_value_t *t = vc_peek(c);
    VCur args[2];
    garb_val_t two[2];
    if (is_fn(t, "minmax")) {
        if (fn_args(t, args, 2) != 2 || !track_breadth(s, &args[0], false, true, &two[0]) ||
            !vc_done(&args[0]) || !track_breadth(s, &args[1], true, true, &two[1]) ||
            !vc_done(&args[1]))
            return false;
        c->i++;
        *out = fn_val(s, "minmax", two, 2);
        *fixed = fixed_breadth(&two[0]) || fixed_breadth(&two[1]);
        return out->nitems == 2;
    }
    if (is_fn(t, "fit-content")) {
        if (fn_args(t, args, 1) != 1 ||
            !vc_dim(&args[0], ACCEPT_LENGTH | ACCEPT_PERCENT, false, false, &s->a, &two[0]) ||
            !vc_done(&args[0]))
            return false;
        c->i++;
        *out = fn_val(s, "fit-content", two, 1);
        *fixed = false;
        return out->nitems == 1;
    }
    if (!track_breadth(s, c, true, true, out))
        return false;
    *fixed = fixed_breadth(out);
    return true;
}

// Line names in brackets, read and dropped (GRID.md, decision 3): each an
// identifier, and never `span` or `auto`.
static bool line_names(VCur *c)
{
    const garb_value_t *t = vc_peek(c);
    if (t == NULL || t->kind != GARB_BLOCK || t->open != '[')
        return true;
    for (int32_t i = 0; i < t->nchildren; i++) {
        const garb_value_t *n = &t->children[i];
        if (n->kind == GARB_WHITESPACE)
            continue;
        if (n->kind != GARB_IDENT || ieq(n->text, n->len, "span") || ieq(n->text, n->len, "auto"))
            return false;
    }
    c->i++;
    return true;
}

// A track list (§ 7.2): track sizes and repeat()s, line names between.
// `repeat` allows repeat(); `autos` counts the auto-repeats met, of which
// a list may hold one, alongside fixed tracks only.
static bool track_list(Sets *s, VCur *c, bool repeat, int *autos, bool *all_fixed, garb_val_t *out)
{
    garb_val_t tracks[GARB_TRACKS_MAX];
    int32_t n = 0;
    *all_fixed = true;
    while (!vc_done(c)) {
        if (!line_names(c))
            return false;
        if (vc_done(c))
            break;
        if (n == GARB_TRACKS_MAX)
            return false;
        const garb_value_t *t = vc_peek(c);
        if (repeat && is_fn(t, "repeat")) {
            VCur args[2];
            garb_val_t two[2];
            if (fn_args(t, args, 2) != 2)
                return false;
            const char *k = vc_keyword(&args[0], kAutoRepeat);
            if (k != NULL) {
                two[0] = kw(k);
                (*autos)++;
            } else {
                const garb_value_t *count = vc_peek(&args[0]);
                if (count == NULL || count->kind != GARB_NUMBER || !count->integer ||
                    count->number < 1)
                    return false;
                args[0].i++;
                os64_memset(&two[0], 0, sizeof(two[0]));
                two[0].kind = GARB_V_NUMBER;
                two[0].number = count->number;
            }
            int inner = 0;
            bool fixed;
            if (!vc_done(&args[0]) || !track_list(s, &args[1], false, &inner, &fixed, &two[1]))
                return false;
            if (k != NULL && !fixed)
                return false;           // an auto-repeat repeats fixed sizes only
            *all_fixed &= fixed;
            c->i++;
            tracks[n++] = fn_val(s, "repeat", two, 2);
            if (tracks[n - 1].nitems != 2)
                return false;
            continue;
        }
        bool fixed;
        if (!track_size(s, c, &tracks[n], &fixed))
            return false;
        *all_fixed &= fixed;
        n++;
    }
    if (n == 0 || *autos > 1 || (*autos == 1 && !*all_fixed))
        return false;
    os64_memset(out, 0, sizeof(*out));
    out->kind = GARB_V_TRACKS;
    out->items = keep(s, tracks, n);
    out->nitems = out->items != NULL ? n : 0;
    return out->nitems == n;
}

int32_t garb_area_cells(const char *s, uint32_t len, garb_area_cell_t *out, int32_t cap)
{
    int32_t n = 0;
    uint32_t i = 0;
    while (i < len) {
        char ch = s[i];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f') {
            i++;
            continue;
        }
        uint32_t from = i;
        if (ch == '.') {
            while (i < len && s[i] == '.')
                i++;
            if (n < cap)
                out[n] = (garb_area_cell_t){s + from, 0};
            n++;
            continue;
        }
        // A name is ident code points: letters, digits, `-`, `_`, and
        // anything past ASCII.
        while (i < len) {
            unsigned char u = (unsigned char)s[i];
            if (!(u >= 0x80 || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                  (u >= '0' && u <= '9') || u == '-' || u == '_'))
                break;
            i++;
        }
        if (i == from)
            return -1;
        if (n < cap)
            out[n] = (garb_area_cell_t){s + from, i - from};
        n++;
    }
    return n;
}

static bool cell_is(garb_area_cell_t c, garb_area_cell_t name)
{
    return c.len == name.len && c.len > 0 && os64_memcmp(c.text, name.text, c.len) == 0;
}

// Whether every name in the rows covers a rectangle and nothing else
// (§ 7.3): the bounding box of its cells holds nothing but it.
static bool areas_rectangular(const garb_val_t *rows, int32_t nrows, int32_t ncols)
{
    garb_area_cell_t cells[GARB_TRACKS_MAX * 4];
    if ((int64_t)nrows * ncols > (int64_t)(sizeof(cells) / sizeof(cells[0])))
        return false;
    for (int32_t r = 0; r < nrows; r++)
        (void)garb_area_cells(rows[r].text, rows[r].len, cells + r * ncols, ncols);
    for (int32_t k = 0; k < nrows * ncols; k++) {
        garb_area_cell_t name = cells[k];
        if (name.len == 0)
            continue;
        int32_t r0 = nrows, r1 = -1, c0 = ncols, c1 = -1, count = 0;
        for (int32_t j = 0; j < nrows * ncols; j++)
            if (cell_is(cells[j], name)) {
                int32_t r = j / ncols, cc = j % ncols;
                r0 = r < r0 ? r : r0;
                r1 = r > r1 ? r : r1;
                c0 = cc < c0 ? cc : c0;
                c1 = cc > c1 ? cc : c1;
                count++;
            }
        if (count != (r1 - r0 + 1) * (c1 - c0 + 1))
            return false;
        for (int32_t r = r0; r <= r1; r++)
            for (int32_t cc = c0; cc <= c1; cc++)
                if (!cell_is(cells[r * ncols + cc], name))
                    return false;
    }
    return true;
}

// grid-template-areas (§ 7.3): strings of the same number of cells, each
// name covering a rectangle, a run of dots for none.
static bool areas(Sets *s, VCur *c, garb_val_t *out)
{
    if (vc_keyword(c, kNone) != NULL) {
        *out = kw("none");
        return true;
    }
    garb_val_t rows[GARB_TRACKS_MAX];
    int32_t n = 0, cols = -1;
    const garb_value_t *t;
    while ((t = vc_peek(c)) != NULL && t->kind == GARB_STRING) {
        if (n == GARB_TRACKS_MAX)
            return false;
        int32_t cells = garb_area_cells(t->text, t->len, NULL, 0);
        if (cells <= 0 || (cols >= 0 && cells != cols))
            return false;
        cols = cells;
        os64_memset(&rows[n], 0, sizeof(rows[n]));
        rows[n].kind = GARB_V_STRING;
        rows[n].text = t->text;
        rows[n].len = t->len;
        n++;
        c->i++;
    }
    if (n == 0 || !vc_done(c) || !areas_rectangular(rows, n, cols))
        return false;
    os64_memset(out, 0, sizeof(*out));
    out->kind = GARB_V_STRING;
    out->items = keep(s, rows, n);
    out->nitems = out->items != NULL ? n : 0;
    return out->nitems == n;
}

// A grid line (§ 8.3): auto, an integer other than 0, `span` with a
// positive integer or a name, or a name. A name beside a number — the
// n-th line of that name — is booked with named lines, and is invalid
// here.
static bool grid_line(Sets *s, VCur *c, garb_val_t *out)
{
    if (vc_keyword(c, kAuto) != NULL) {
        *out = kw("auto");
        return true;
    }
    bool span = vc_keyword(c, kSpan) != NULL;
    const garb_value_t *t = vc_peek(c);
    if (t != NULL && t->kind == GARB_NUMBER && t->integer && t->number != 0 &&
        (!span || t->number > 0)) {
        garb_val_t num = {0};
        num.kind = GARB_V_NUMBER;
        num.number = t->number;
        c->i++;
        *out = span ? fn_val(s, "span", &num, 1) : num;
        return !span || out->nitems == 1;
    }
    if (t != NULL && t->kind == GARB_IDENT && !ieq(t->text, t->len, "span") &&
        !ieq(t->text, t->len, "auto") && vc_keyword(&(VCur){c->v, c->n, c->i}, kWide) == NULL) {
        garb_val_t name = {0};
        name.kind = GARB_V_STRING;
        name.text = t->text;
        name.len = t->len;
        c->i++;
        *out = span ? fn_val(s, "span", &name, 1) : name;
        return !span || out->nitems == 1;
    }
    return false;
}

// The background longhands take a comma-separated list, one per layer
// (Backgrounds 3 § 3.1), the top layer first.
static bool layered(const Longhand *l)
{
    return l->g == G_BG_IMAGE || l->g == G_BG_REPEAT || l->g == G_BG_POS_X ||
           l->g == G_BG_POS_Y || l->g == G_BG_SIZE || l->g == G_BG_BOX;
}

static bool longhand(Sets *s, VCur *c, const Longhand *l, garb_val_t *out)
{
    if (!layered(l))
        return longhand_one(s, c, l, out);
    Layers list = {0};
    for (;;) {
        garb_val_t one;
        if (!longhand_one(s, c, l, &one) || !layers_add(s, &list, &one)) {
            os64_free(list.v);
            return false;
        }
        if (vc_peek(c) == NULL || vc_peek(c)->kind != GARB_COMMA)
            break;
        c->i++;
    }
    return layers_done(s, &list, out);
}

static bool longhand_one(Sets *s, VCur *c, const Longhand *l, garb_val_t *out)
{
    const char *k;
    switch (l->g) {
    case G_KEYWORDS:
    case G_BG_BOX:
        if ((k = vc_keyword(c, l->words)) == NULL)
            return false;
        *out = kw(k);
        return true;
    case G_COLOR:
    case G_BCOLOR: return vc_color(c, out);
    case G_MARGIN: return margin_one(s, c, out);
    case G_PADDING: return padding_one(s, c, out);
    case G_BWIDTH: return bwidth_one(s, c, out);
    case G_BSTYLE: return bstyle_one(s, c, out);
    case G_SIZE:
        if ((k = vc_keyword(c, kSizeWords)) != NULL) {
            *out = kw(k);
            return true;
        }
        return vc_dim(c, ACCEPT_LENGTH | ACCEPT_PERCENT, false, s->quirks, &s->a, out);
    case G_MAX:
        if ((k = vc_keyword(c, kMaxWords)) != NULL) {
            *out = kw(k);
            return true;
        }
        return vc_dim(c, ACCEPT_LENGTH | ACCEPT_PERCENT, false, s->quirks, &s->a, out);
    case G_FAMILY: return families(s, c, out);
    case G_FONT_SIZE: return font_size(s, c, out);
    case G_FONT_WEIGHT: return font_weight(s, c, out);
    case G_FONT_STYLE: return font_style(s, c, out);
    case G_LINE_HEIGHT: return line_height(s, c, out);
    case G_VALIGN:
        return kw_or_dim(s, c, kVerticalAlign, ACCEPT_LENGTH | ACCEPT_PERCENT, true, true, out);
    case G_DECORATION_LINE: return decoration_line(s, c, out);
    case G_INDENT: {
        bool got = false;
        while (!vc_done(c)) {
            if (vc_keyword(c, kIndentWords) != NULL)
                continue;
            if (got || !vc_dim(c, ACCEPT_LENGTH | ACCEPT_PERCENT, true, s->quirks, &s->a, out))
                return false;
            got = true;
        }
        return got;
    }
    case G_LIST_TYPE: return list_type(s, c, out);
    case G_LIST_IMAGE:
    case G_BG_IMAGE:
        if (vc_keyword(c, kNone) != NULL) {
            *out = kw("none");
            return true;
        }
        return l->g == G_BG_IMAGE ? bg_image(s, c, out) : vc_url(c, out);
    case G_BG_REPEAT: return repeat(c, out);
    case G_BG_SIZE: return bg_size(s, c, out);
    case G_BG_POS_X:
        if ((k = vc_keyword(c, kPosX)) != NULL) {
            garb_val_t off, *op = NULL;
            if (!os64_streq(k, "center") && vc_dim(c, ACCEPT_LENGTH | ACCEPT_PERCENT, true, false, &s->a, &off))
                op = &off;
            return edge_offset(s, k, op, out);
        }
        return vc_dim(c, ACCEPT_LENGTH | ACCEPT_PERCENT, true, s->quirks, &s->a, out);
    case G_BG_POS_Y:
        if ((k = vc_keyword(c, kPosY)) != NULL) {
            garb_val_t off, *op = NULL;
            if (!os64_streq(k, "center") && vc_dim(c, ACCEPT_LENGTH | ACCEPT_PERCENT, true, false, &s->a, &off))
                op = &off;
            return edge_offset(s, k, op, out);
        }
        return vc_dim(c, ACCEPT_LENGTH | ACCEPT_PERCENT, true, s->quirks, &s->a, out);
    case G_SPACING: {
        garb_val_t two[2];
        int n = 0;
        while (n < 2 && vc_dim(c, ACCEPT_LENGTH, false, s->quirks, &s->a, &two[n]))
            n++;
        if (n == 0)
            return false;
        if (n == 1)
            two[1] = two[0];
        os64_memset(out, 0, sizeof(*out));
        out->kind = GARB_V_LENGTH;
        out->items = keep(s, two, 2);
        out->nitems = 2;
        return out->items != NULL;
    }
    case G_SHADOW: return shadow_list(s, c, l->prop == GARB_BOX_SHADOW, out);
    case G_RADIUS: {
        // Backgrounds 3 § 5.1: one radius is both; a second is the vertical.
        garb_val_t two[2];
        int n = 0;
        while (n < 2 && vc_dim(c, ACCEPT_LENGTH | ACCEPT_PERCENT, false, false, &s->a, &two[n]))
            n++;
        return n > 0 && radius_pair(s, two, n, out);
    }
    case G_Z_INDEX: {
        // CSS 2.1 § 9.9.1. An integer as written: `1.0` and `1e3` are
        // numbers, not integers. A calc() that works out to one is not read.
        if (vc_keyword(c, kAuto) != NULL) {
            *out = kw("auto");
            return true;
        }
        const garb_value_t *t = vc_peek(c);
        if (t == NULL || t->kind != GARB_NUMBER || !t->integer)
            return false;
        c->i++;
        os64_memset(out, 0, sizeof(*out));
        out->kind = GARB_V_NUMBER;
        out->number = t->number;
        return true;
    }
    case G_ALPHA:
        // Color 4 § 4.2: outside 0 to 1 (or 0% to 100%) is not invalid,
        // it is clamped when the value is computed.
        return vc_dim(c, ACCEPT_NUMBER | ACCEPT_PERCENT, true, false, &s->a, out);
    case G_ALIGN: {
        // Box Alignment 3 § 4: `first baseline` is `baseline`, and
        // `unsafe` is what a position means without it.
        bool unsafe = vc_keyword(c, kUnsafe) != NULL;
        bool first = !unsafe && vc_keyword(c, kFirst) != NULL;
        if ((k = vc_keyword(c, l->words)) == NULL)
            return false;
        if (first && !os64_streq(k, "baseline"))
            return false;
        if (unsafe && (os64_streq(k, "normal") || os64_streq(k, "stretch") ||
                       os64_streq(k, "baseline") || os64_streq(k, "auto") ||
                       os64_streq(k, "space-between") || os64_streq(k, "space-around") ||
                       os64_streq(k, "space-evenly")))
            return false;
        *out = kw(k);
        return true;
    }
    case G_FACTOR:
        return vc_dim(c, ACCEPT_NUMBER, false, false, &s->a, out);
    case G_BASIS:
        if ((k = vc_keyword(c, l->words)) != NULL) {
            *out = kw(k);
            return true;
        }
        return vc_dim(c, ACCEPT_LENGTH | ACCEPT_PERCENT, false, false, &s->a, out);
    case G_INTEGER: {
        const garb_value_t *t = vc_peek(c);
        if (t == NULL || t->kind != GARB_NUMBER || !t->integer)
            return false;
        c->i++;
        os64_memset(out, 0, sizeof(*out));
        out->kind = GARB_V_NUMBER;
        out->number = t->number;
        return true;
    }
    case G_GAP:
        if (vc_keyword(c, kNormal) != NULL) {
            *out = kw("normal");
            return true;
        }
        return vc_dim(c, ACCEPT_LENGTH | ACCEPT_PERCENT, false, false, &s->a, out);
    case G_TEMPLATE: {
        if (vc_keyword(c, kNone) != NULL) {
            *out = kw("none");
            return true;
        }
        int autos = 0;
        bool fixed;
        return track_list(s, c, true, &autos, &fixed, out);
    }
    case G_AUTO_TRACKS: {
        // One or more track sizes, no repeat() and no line names (§ 7.6).
        garb_val_t tracks[GARB_TRACKS_MAX];
        int32_t n = 0;
        while (!vc_done(c)) {
            bool fixed;
            if (n == GARB_TRACKS_MAX || !track_size(s, c, &tracks[n], &fixed))
                return false;
            n++;
        }
        if (n == 0)
            return false;
        os64_memset(out, 0, sizeof(*out));
        out->kind = GARB_V_TRACKS;
        out->items = keep(s, tracks, n);
        out->nitems = out->items != NULL ? n : 0;
        return out->nitems == n;
    }
    case G_AREAS: return areas(s, c, out);
    case G_AUTO_FLOW: {
        // [row | column] || dense, written as the four keywords it names.
        const char *axis = NULL;
        bool dense = false;
        while (!vc_done(c)) {
            if (axis == NULL && (k = vc_keyword(c, kFlowAxis)) != NULL)
                axis = k;
            else if (!dense && vc_keyword(c, kDense) != NULL)
                dense = true;
            else
                return false;
        }
        if (axis == NULL && !dense)
            return false;
        bool column = axis != NULL && os64_streq(axis, "column");
        *out = kw(column ? (dense ? "column dense" : "column") : (dense ? "row dense" : "row"));
        return true;
    }
    case G_GRID_LINE: return grid_line(s, c, out);
    }
    return false;
}

// ── Shorthands ──────────────────────────────────────────────────────────

typedef enum {
    SH_MARGIN, SH_PADDING, SH_BORDER, SH_BORDER_TOP, SH_BORDER_RIGHT, SH_BORDER_BOTTOM,
    SH_BORDER_LEFT, SH_BORDER_WIDTH, SH_BORDER_STYLE, SH_BORDER_COLOR, SH_BACKGROUND,
    SH_BACKGROUND_POSITION, SH_FONT, SH_LIST_STYLE, SH_TEXT_DECORATION, SH_OVERFLOW, SH_INSET,
    SH_FLEX, SH_FLEX_FLOW, SH_GAP,
    SH_GRID_COLUMN, SH_GRID_ROW, SH_GRID_AREA, SH_PLACE_ITEMS, SH_PLACE_SELF, SH_PLACE_CONTENT,
    SH_BORDER_RADIUS,
} Shorthand;

static const struct { const char *name; Shorthand sh; } kShorthands[] = {
    {"margin", SH_MARGIN}, {"padding", SH_PADDING}, {"border", SH_BORDER},
    {"border-top", SH_BORDER_TOP}, {"border-right", SH_BORDER_RIGHT},
    {"border-bottom", SH_BORDER_BOTTOM}, {"border-left", SH_BORDER_LEFT},
    {"border-width", SH_BORDER_WIDTH}, {"border-style", SH_BORDER_STYLE},
    {"border-color", SH_BORDER_COLOR}, {"background", SH_BACKGROUND},
    {"background-position", SH_BACKGROUND_POSITION}, {"font", SH_FONT},
    {"list-style", SH_LIST_STYLE}, {"text-decoration", SH_TEXT_DECORATION},
    {"overflow", SH_OVERFLOW}, {"inset", SH_INSET},
    {"flex", SH_FLEX}, {"flex-flow", SH_FLEX_FLOW}, {"gap", SH_GAP},
    // Grid 1's first spelling of `gap`, still written by nearly every sheet
    // that wants the gap in the browsers of 2018 (Box Alignment 3 § 8.4).
    {"grid-gap", SH_GAP},
    {"grid-column", SH_GRID_COLUMN}, {"grid-row", SH_GRID_ROW}, {"grid-area", SH_GRID_AREA},
    {"place-items", SH_PLACE_ITEMS}, {"place-self", SH_PLACE_SELF},
    {"place-content", SH_PLACE_CONTENT}, {"border-radius", SH_BORDER_RADIUS},
};

// The longhands a shorthand sets, for a CSS-wide keyword to reach them all.
static int shorthand_longhands(Shorthand sh, garb_prop_t *out)
{
    static const garb_prop_t kBg[] = {GARB_BACKGROUND_COLOR, GARB_BACKGROUND_IMAGE,
                                      GARB_BACKGROUND_REPEAT, GARB_BACKGROUND_POSITION_X,
                                      GARB_BACKGROUND_POSITION_Y, GARB_BACKGROUND_SIZE,
                                      GARB_BACKGROUND_ORIGIN, GARB_BACKGROUND_CLIP};
    static const garb_prop_t kFont[] = {GARB_FONT_STYLE, GARB_FONT_VARIANT, GARB_FONT_WEIGHT,
                                        GARB_FONT_SIZE, GARB_LINE_HEIGHT, GARB_FONT_FAMILY};
    static const garb_prop_t kList[] = {GARB_LIST_STYLE_TYPE, GARB_LIST_STYLE_POSITION,
                                        GARB_LIST_STYLE_IMAGE};
    int n = 0;
    switch (sh) {
    case SH_MARGIN: for (int k = 0; k < 4; k++) out[n++] = (garb_prop_t)(GARB_MARGIN_TOP + k); break;
    case SH_PADDING: for (int k = 0; k < 4; k++) out[n++] = (garb_prop_t)(GARB_PADDING_TOP + k); break;
    case SH_BORDER:
        for (int k = 0; k < 12; k++) out[n++] = (garb_prop_t)(GARB_BORDER_TOP_WIDTH + k);
        break;
    case SH_BORDER_TOP: case SH_BORDER_RIGHT: case SH_BORDER_BOTTOM: case SH_BORDER_LEFT: {
        int side = sh - SH_BORDER_TOP;
        out[n++] = (garb_prop_t)(GARB_BORDER_TOP_WIDTH + side);
        out[n++] = (garb_prop_t)(GARB_BORDER_TOP_STYLE + side);
        out[n++] = (garb_prop_t)(GARB_BORDER_TOP_COLOR + side);
        break;
    }
    case SH_BORDER_WIDTH: for (int k = 0; k < 4; k++) out[n++] = (garb_prop_t)(GARB_BORDER_TOP_WIDTH + k); break;
    case SH_BORDER_STYLE: for (int k = 0; k < 4; k++) out[n++] = (garb_prop_t)(GARB_BORDER_TOP_STYLE + k); break;
    case SH_BORDER_COLOR: for (int k = 0; k < 4; k++) out[n++] = (garb_prop_t)(GARB_BORDER_TOP_COLOR + k); break;
    case SH_BACKGROUND: for (size_t k = 0; k < sizeof(kBg) / sizeof(kBg[0]); k++) out[n++] = kBg[k]; break;
    case SH_BACKGROUND_POSITION: out[n++] = GARB_BACKGROUND_POSITION_X; out[n++] = GARB_BACKGROUND_POSITION_Y; break;
    case SH_FONT: for (int k = 0; k < 6; k++) out[n++] = kFont[k]; break;
    case SH_LIST_STYLE: for (int k = 0; k < 3; k++) out[n++] = kList[k]; break;
    case SH_TEXT_DECORATION: out[n++] = GARB_TEXT_DECORATION_LINE; break;
    case SH_OVERFLOW: out[n++] = GARB_OVERFLOW_X; out[n++] = GARB_OVERFLOW_Y; break;
    case SH_INSET: for (int k = 0; k < 4; k++) out[n++] = (garb_prop_t)(GARB_TOP + k); break;
    case SH_FLEX: out[n++] = GARB_FLEX_GROW; out[n++] = GARB_FLEX_SHRINK; out[n++] = GARB_FLEX_BASIS; break;
    case SH_FLEX_FLOW: out[n++] = GARB_FLEX_DIRECTION; out[n++] = GARB_FLEX_WRAP; break;
    case SH_GAP: out[n++] = GARB_ROW_GAP; out[n++] = GARB_COLUMN_GAP; break;
    case SH_GRID_COLUMN: out[n++] = GARB_GRID_COLUMN_START; out[n++] = GARB_GRID_COLUMN_END; break;
    case SH_GRID_ROW: out[n++] = GARB_GRID_ROW_START; out[n++] = GARB_GRID_ROW_END; break;
    case SH_GRID_AREA:
        out[n++] = GARB_GRID_ROW_START; out[n++] = GARB_GRID_COLUMN_START;
        out[n++] = GARB_GRID_ROW_END; out[n++] = GARB_GRID_COLUMN_END;
        break;
    case SH_PLACE_ITEMS: out[n++] = GARB_ALIGN_ITEMS; out[n++] = GARB_JUSTIFY_ITEMS; break;
    case SH_PLACE_SELF: out[n++] = GARB_ALIGN_SELF; out[n++] = GARB_JUSTIFY_SELF; break;
    case SH_PLACE_CONTENT: out[n++] = GARB_ALIGN_CONTENT; out[n++] = GARB_JUSTIFY_CONTENT; break;
    case SH_BORDER_RADIUS: for (int k = 0; k < 4; k++) out[n++] = (garb_prop_t)(GARB_BORDER_TOP_LEFT_RADIUS + k); break;
    }
    return n;
}

// Up to `max` grid lines split by slashes (Grid 2 § 8.4): what each one
// left out is its partner's name where that one is a name, else auto.
static bool grid_lines(Sets *s, VCur *c, garb_val_t *lines, int32_t max)
{
    int32_t n = 0;
    for (;;) {
        if (n == max || !grid_line(s, c, &lines[n]))
            return false;
        n++;
        const garb_value_t *t = vc_peek(c);
        if (t == NULL)
            break;
        if (!is_delim_v(t, '/'))
            return false;
        c->i++;
    }
    // grid-area's four: row-start, column-start, row-end, column-end; each
    // missing one copies the one across from it (index - 2) when that is
    // a name, and the second copies the first when there is only one.
    for (int32_t i = n; i < max; i++) {
        const garb_val_t *from = &lines[i >= 2 ? i - 2 : 0];
        lines[i] = from->kind == GARB_V_STRING ? *from : kw("auto");
    }
    return true;
}

// `flex` (Flexbox 1 § 7.1): `none`, or a grow factor with an optional
// shrink factor and a basis, in either order. What is left out is not the
// longhand's initial value: grow 1, shrink 1, basis 0% — `flex: 1` is
// `1 1 0%`. A unitless zero is a factor unless two factors came before it.
static bool flex_shorthand(Sets *s, VCur *c)
{
    garb_val_t grow = {0}, shrink = {0}, basis = percent(0);
    grow.kind = shrink.kind = GARB_V_NUMBER;
    grow.number = shrink.number = 1;
    if (vc_keyword(c, kNone) != NULL) {
        if (!vc_done(c))
            return false;
        grow.number = shrink.number = 0;
        basis = kw("auto");
    } else {
        bool factors = false, have_basis = false;
        while (!vc_done(c)) {
            const garb_value_t *t = vc_peek(c);
            if (!factors && t != NULL && t->kind == GARB_NUMBER) {
                if (!vc_dim(c, ACCEPT_NUMBER, false, false, &s->a, &grow))
                    return false;
                const garb_value_t *u = vc_peek(c);
                if (u != NULL && u->kind == GARB_NUMBER &&
                    !vc_dim(c, ACCEPT_NUMBER, false, false, &s->a, &shrink))
                    return false;
                factors = true;
            } else if (!have_basis) {
                const char *k = vc_keyword(c, kBasisWords);
                if (k != NULL)
                    basis = kw(k);
                else if (!vc_dim(c, ACCEPT_LENGTH | ACCEPT_PERCENT, false, false, &s->a, &basis))
                    return false;
                have_basis = true;
            } else {
                return false;
            }
        }
        if (!factors && !have_basis)
            return false;
    }
    set(s, GARB_FLEX_GROW, &grow);
    set(s, GARB_FLEX_SHRINK, &shrink);
    set(s, GARB_FLEX_BASIS, &basis);
    return true;
}

static bool shorthand(Sets *s, VCur *c, Shorthand sh)
{
    static const garb_prop_t kAll[4] = {0, 1, 2, 3};
    garb_val_t x, y;
    const char *k;
    switch (sh) {
    case SH_MARGIN: return box4(s, c, GARB_MARGIN_TOP, margin_one);
    case SH_PADDING: return box4(s, c, GARB_PADDING_TOP, padding_one);
    // Position 3 § 3.1.1: the margin's one-to-four, top right bottom left.
    case SH_INSET: return box4(s, c, GARB_TOP, margin_one);
    case SH_BORDER: return border_side(s, c, kAll, 4);
    case SH_BORDER_TOP: case SH_BORDER_RIGHT: case SH_BORDER_BOTTOM: case SH_BORDER_LEFT: {
        garb_prop_t side = (garb_prop_t)(sh - SH_BORDER_TOP);
        return border_side(s, c, &side, 1);
    }
    case SH_BORDER_WIDTH: return box4(s, c, GARB_BORDER_TOP_WIDTH, bwidth_one);
    case SH_BORDER_STYLE: return box4(s, c, GARB_BORDER_TOP_STYLE, bstyle_one);
    case SH_BORDER_COLOR: return box4(s, c, GARB_BORDER_TOP_COLOR, bcolor_one);
    case SH_BACKGROUND: return background(s, c);
    case SH_BACKGROUND_POSITION: {
        // A position per layer, each split into its two longhands' lists.
        Layers xs = {0}, ys = {0};
        bool ok = true;
        for (;;) {
            if (!position(s, c, &x, &y) || !layers_add(s, &xs, &x) || !layers_add(s, &ys, &y)) {
                ok = false;
                break;
            }
            if (vc_peek(c) == NULL || vc_peek(c)->kind != GARB_COMMA)
                break;
            c->i++;
        }
        ok = ok && vc_done(c);
        ok = layers_done(s, &xs, &x) && ok;     // each list freed, whatever
        ok = layers_done(s, &ys, &y) && ok;
        if (!ok)
            return false;
        set(s, GARB_BACKGROUND_POSITION_X, &x);
        set(s, GARB_BACKGROUND_POSITION_Y, &y);
        return true;
    }
    case SH_FONT: return font_shorthand(s, c);
    case SH_BORDER_RADIUS: return radius_shorthand(s, c);
    case SH_LIST_STYLE: return list_style(s, c);
    case SH_TEXT_DECORATION: return text_decoration(s, c);
    case SH_FLEX: return flex_shorthand(s, c);
    case SH_FLEX_FLOW: {
        // Flexbox 1 § 5.3: a direction and a wrap, either order, each at
        // most once; the one left out is its initial value.
        garb_val_t dir = kw("row"), wrap = kw("nowrap");
        bool have_dir = false, have_wrap = false;
        while (!vc_done(c)) {
            if (!have_dir && (k = vc_keyword(c, kFlexDirection)) != NULL) {
                dir = kw(k);
                have_dir = true;
            } else if (!have_wrap && (k = vc_keyword(c, kFlexWrap)) != NULL) {
                wrap = kw(k);
                have_wrap = true;
            } else {
                return false;
            }
        }
        if (!have_dir && !have_wrap)
            return false;
        set(s, GARB_FLEX_DIRECTION, &dir);
        set(s, GARB_FLEX_WRAP, &wrap);
        return true;
    }
    case SH_GAP: {
        // Box Alignment 3 § 8.3: a row gap and a column gap, the second
        // the first when it is left out.
        const Longhand gap = {GARB_ROW_GAP, G_GAP, NULL};
        if (!longhand_one(s, c, &gap, &x))
            return false;
        y = x;
        if (!vc_done(c) && !longhand_one(s, c, &gap, &y))
            return false;
        if (!vc_done(c))
            return false;
        set(s, GARB_ROW_GAP, &x);
        set(s, GARB_COLUMN_GAP, &y);
        return true;
    }
    case SH_GRID_COLUMN: case SH_GRID_ROW: {
        garb_val_t two[2];
        if (!grid_lines(s, c, two, 2))
            return false;
        garb_prop_t start = sh == SH_GRID_COLUMN ? GARB_GRID_COLUMN_START : GARB_GRID_ROW_START;
        set(s, start, &two[0]);
        set(s, (garb_prop_t)(start + 1), &two[1]);
        return true;
    }
    case SH_GRID_AREA: {
        garb_val_t four[4];
        if (!grid_lines(s, c, four, 4))
            return false;
        set(s, GARB_GRID_ROW_START, &four[0]);
        set(s, GARB_GRID_COLUMN_START, &four[1]);
        set(s, GARB_GRID_ROW_END, &four[2]);
        set(s, GARB_GRID_COLUMN_END, &four[3]);
        return true;
    }
    case SH_PLACE_ITEMS: case SH_PLACE_SELF: case SH_PLACE_CONTENT: {
        // Box Alignment 3 § 5: the align value, then the justify value,
        // which is the align value when it is left out.
        static const Longhand kPlace[3][2] = {
            {{GARB_ALIGN_ITEMS, G_ALIGN, kAlignItems}, {GARB_JUSTIFY_ITEMS, G_ALIGN, kJustifyItems}},
            {{GARB_ALIGN_SELF, G_ALIGN, kAlignSelf}, {GARB_JUSTIFY_SELF, G_ALIGN, kJustifySelf}},
            {{GARB_ALIGN_CONTENT, G_ALIGN, kAlignContent},
             {GARB_JUSTIFY_CONTENT, G_ALIGN, kJustify}},
        };
        const Longhand *pair = kPlace[sh - SH_PLACE_ITEMS];
        if (!longhand_one(s, c, &pair[0], &x))
            return false;
        if (vc_done(c)) {
            VCur again = {&(garb_value_t){.kind = GARB_IDENT, .text = x.keyword,
                                           .len = (uint32_t)os64_strlen(x.keyword)}, 1, 0};
            if (!longhand_one(s, &again, &pair[1], &y))
                return false;
        } else if (!longhand_one(s, c, &pair[1], &y) || !vc_done(c)) {
            return false;
        }
        set(s, pair[0].prop, &x);
        set(s, pair[1].prop, &y);
        return true;
    }
    case SH_OVERFLOW:
        if ((k = vc_keyword(c, kOverflow)) == NULL)
            return false;
        x = kw(k);
        y = x;
        if ((k = vc_keyword(c, kOverflow)) != NULL)
            y = kw(k);
        if (!vc_done(c))
            return false;
        set(s, GARB_OVERFLOW_X, &x);
        set(s, GARB_OVERFLOW_Y, &y);
        return true;
    }
    return false;
}

int32_t prop_longhands(const char *name, size_t len, garb_prop_t *out)
{
    for (size_t k = 0; k < sizeof(kLonghands) / sizeof(kLonghands[0]); k++)
        if (ieq(name, len, kProps[kLonghands[k].prop].name)) {
            out[0] = kLonghands[k].prop;
            return 1;
        }
    for (size_t k = 0; k < sizeof(kShorthands) / sizeof(kShorthands[0]); k++)
        if (ieq(name, len, kShorthands[k].name))
            return shorthand_longhands(kShorthands[k].sh, out);
    return 0;
}

// ── Declarations ────────────────────────────────────────────────────────

// var() and env() alike: each is replaced before the value can be read
// (Custom Properties 1 § 3, Environment Variables 1 § 3).
static bool has_var(const garb_value_t *v, int32_t n)
{
    for (int32_t i = 0; i < n; i++) {
        if (v[i].kind == GARB_FUNCTION &&
            (ieq(v[i].text, v[i].len, "var") || ieq(v[i].text, v[i].len, "env")))
            return true;
        if ((v[i].kind == GARB_FUNCTION || v[i].kind == GARB_BLOCK) &&
            has_var(v[i].children, v[i].nchildren))
            return true;
    }
    return false;
}

bool garb_decl_has_var(const garb_decl_t *decl)
{
    return has_var(decl->value, decl->nvalue);
}

bool garb_decl_is_custom(const garb_decl_t *decl)
{
    return decl->len >= 2 && decl->name[0] == '-' && decl->name[1] == '-';
}

int32_t garb_read_declaration(garb_parsed_t *owner, const garb_decl_t *decl, bool quirks,
                              garb_set_t *out)
{
    Sets s = {out, 0, {owner->arena, false}, quirks};
    VCur c = {decl->value, decl->nvalue, 0};
    const Longhand *l = NULL;
    int sh = -1;
    for (size_t k = 0; k < sizeof(kLonghands) / sizeof(kLonghands[0]) && l == NULL; k++)
        if (ieq(decl->name, decl->len, kProps[kLonghands[k].prop].name))
            l = &kLonghands[k];
    for (size_t k = 0; k < sizeof(kShorthands) / sizeof(kShorthands[0]) && l == NULL && sh < 0; k++)
        if (ieq(decl->name, decl->len, kShorthands[k].name))
            sh = (int)kShorthands[k].sh;
    if (l == NULL && sh < 0)
        return 0;
    if (garb_decl_has_var(decl))
        return GARB_DECL_HELD;
    // A CSS-wide keyword alone reaches every longhand the property sets.
    const char *wide = vc_keyword(&c, kWide);
    if (wide != NULL) {
        if (!vc_done(&c))
            return GARB_DECL_INVALID;
        garb_val_t v = {0};
        v.kind = GARB_V_WIDE;
        v.keyword = wide;
        if (l != NULL) {
            set(&s, l->prop, &v);
        } else {
            garb_prop_t props[16];
            int n = shorthand_longhands((Shorthand)sh, props);
            for (int k = 0; k < n; k++)
                set(&s, props[k], &v);
        }
        return s.n;
    }
    bool ok;
    if (l != NULL) {
        garb_val_t v;
        ok = longhand(&s, &c, l, &v) && vc_done(&c);
        if (ok)
            set(&s, l->prop, &v);
    } else {
        ok = shorthand(&s, &c, (Shorthand)sh);
    }
    if (s.a.short_of_memory)
        owner->incomplete = true;
    return ok && !s.a.short_of_memory ? s.n : GARB_DECL_INVALID;
}

// ── Writing a value back ────────────────────────────────────────────────

typedef struct {
    char *out;
    size_t cap, len;
} Out;

static void put(Out *o, const char *s)
{
    for (; *s != '\0'; s++, o->len++)
        if (o->len + 1 < o->cap)
            o->out[o->len] = *s;
    if (o->cap > 0)
        o->out[o->len < o->cap ? o->len : o->cap - 1] = '\0';
}

static void put_n(Out *o, const char *s, size_t n)
{
    for (size_t k = 0; k < n; k++, o->len++)
        if (o->len + 1 < o->cap)
            o->out[o->len] = s[k];
    if (o->cap > 0)
        o->out[o->len < o->cap ? o->len : o->cap - 1] = '\0';
}

// A number to six decimal places, the zeros after them trimmed — how
// Color 4 serializes a channel, and plenty for a length. One past what an
// int64_t holds (1e300px is a valid length), or not a number at all, is
// written by the parser's own writer, which tests the range before any
// cast.
static void num(Out *o, double v)
{
    char b[64];
    if (!(v < 1e15 && v > -1e15)) {
        garb_format_double(b, sizeof(b), v);
        put(o, b);
        return;
    }
    bool neg = v < 0;
    if (neg)
        v = -v;
    int64_t whole = (int64_t)v;
    int64_t frac = (int64_t)((v - (double)whole) * 1e6 + 0.5);
    if (frac >= 1000000) {
        whole++;
        frac -= 1000000;
    }
    if (frac == 0) {
        os64_snprintf(b, sizeof(b), "%s%ld", neg && whole != 0 ? "-" : "", (long)whole);
    } else {
        char f[8];
        os64_snprintf(f, sizeof(f), "%06ld", (long)frac);
        int end = 6;
        while (end > 0 && f[end - 1] == '0')
            end--;
        f[end] = '\0';
        os64_snprintf(b, sizeof(b), "%s%ld.%s", neg ? "-" : "", (long)whole, f);
    }
    put(o, b);
}

static const char *const kUnitNames[] = {"px", "em", "rem", "ex", "ch", "vw", "vh", "vmin",
                                         "vmax", "pt", "pc", "cm", "mm", "in", "q", "fr"};

static void val(Out *o, const garb_val_t *v);

// Where a node is written: at the top (it is the whole value, so `calc(`),
// as an argument of a comparison (bare), or inside a sum or product
// (parenthesized).
typedef enum { AT_TOP, AT_ARG, AT_NESTED } At;

static void calc(Out *o, const garb_calc_t *c, At at)
{
    switch (c->op) {
    case GARB_CALC_LEAF:
        val(o, &c->leaf);
        return;
    case GARB_CALC_SUM:
    case GARB_CALC_PRODUCT:
        put(o, at == AT_TOP ? "calc(" : at == AT_NESTED ? "(" : "");
        for (int32_t k = 0; k < c->nargs; k++) {
            if (k > 0)
                put(o, c->op == GARB_CALC_SUM ? (c->invert[k] ? " - " : " + ")
                                              : (c->invert[k] ? " / " : " * "));
            calc(o, c->args[k], AT_NESTED);
        }
        put(o, at == AT_ARG ? "" : ")");
        return;
    default:
        put(o, c->op == GARB_CALC_MIN ? "min(" : c->op == GARB_CALC_MAX ? "max(" : "clamp(");
        for (int32_t k = 0; k < c->nargs; k++) {
            if (k > 0)
                put(o, ", ");
            calc(o, c->args[k], AT_ARG);
        }
        put(o, ")");
        return;
    }
}

static void val(Out *o, const garb_val_t *v)
{
    // An image is written by its function's name alone, whatever a gradient
    // carries of itself.
    if (v->kind == GARB_V_IMAGE) {
        put_n(o, v->text, v->len);
        put(o, "(...)");
        return;
    }
    if (v->kind == GARB_V_FUNCTION && os64_streq(v->keyword, "span")) {
        put(o, "span ");            // a keyword and its argument, not a call
        val(o, &v->items[0]);
        return;
    }
    if (v->kind == GARB_V_FUNCTION) {
        put(o, v->keyword);
        put(o, "(");
        for (int32_t k = 0; k < v->nitems; k++) {
            if (k > 0)
                put(o, ", ");
            val(o, &v->items[k]);
        }
        put(o, ")");
        return;
    }
    if (v->nitems > 0) {
        for (int32_t k = 0; k < v->nitems; k++) {
            if (k > 0)
                put(o, v->comma ? ", " : " ");
            val(o, &v->items[k]);
        }
        return;
    }
    switch (v->kind) {
    case GARB_V_KEYWORD:
    case GARB_V_WIDE: put(o, v->keyword); break;
    case GARB_V_LENGTH: num(o, v->number); put(o, kUnitNames[v->unit]); break;
    case GARB_V_PERCENTAGE: num(o, v->number); put(o, "%"); break;
    case GARB_V_NUMBER: num(o, v->number); break;
    case GARB_V_COLOR:
        if (v->color.current) {
            put(o, "currentcolor");
            break;
        }
        put(o, v->color.a < 1 ? "rgba(" : "rgb(");
        num(o, v->color.r);
        put(o, ", ");
        num(o, v->color.g);
        put(o, ", ");
        num(o, v->color.b);
        if (v->color.a < 1) {
            put(o, ", ");
            num(o, v->color.a);
        }
        put(o, ")");
        break;
    case GARB_V_URL: put(o, "url("); put_n(o, v->text, v->len); put(o, ")"); break;
    case GARB_V_STRING: put(o, "\""); put_n(o, v->text, v->len); put(o, "\""); break;
    case GARB_V_CALC: calc(o, v->calc, AT_TOP); break;
    case GARB_V_IMAGE: put_n(o, v->text, v->len); put(o, "(...)"); break;
    case GARB_V_TRACKS: case GARB_V_FUNCTION: case GARB_V_LAYERS: break;  // always with items, above
    }
}

size_t garb_val_dump(const garb_val_t *v, char *out, size_t cap)
{
    Out o = {out, cap, 0};
    if (cap > 0)
        out[0] = '\0';
    val(&o, v);
    return o.len;
}
