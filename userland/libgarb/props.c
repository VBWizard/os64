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
static const char *const kRepeat[] = {"repeat-x", "repeat-y", NULL};
static const char *const kRepeat2[] = {"repeat", "space", "round", "no-repeat", NULL};
static const char *const kAttachment[] = {"scroll", "fixed", "local", NULL};
static const char *const kBox[] = {"border-box", "padding-box", "content-box", "text", NULL};
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

static bool bg_size(Sets *s, VCur *c)
{
    if (vc_keyword(c, kBgSize) != NULL)
        return true;
    garb_val_t v;
    int n = 0;
    while (n < 2 && kw_or_dim(s, c, kAuto, ACCEPT_LENGTH | ACCEPT_PERCENT, false, false, &v))
        n++;
    return n > 0;
}

// The `background` shorthand: comma-separated layers, the last of which may
// carry the colour. What yonder paints is one picture, so the FIRST layer's
// image, repeat and position are the ones set — the one on top — and the
// rest are read for their validity (Backgrounds 3 § 3.10).
static bool background(Sets *s, VCur *c)
{
    garb_val_t color = transparent(), image = kw("none"), rep = kw("repeat"), px = percent(0),
               py = percent(0);
    for (int layer = 0;; layer++) {
        bool have_img = false, have_pos = false, have_rep = false, have_att = false,
             have_color = false;
        int boxes = 0;
        garb_val_t li = kw("none"), lr = kw("repeat"), lx = percent(0), ly = percent(0), lc;
        for (;;) {
            const garb_value_t *t = vc_peek(c);
            if (t == NULL || t->kind == GARB_COMMA)
                break;
            garb_val_t v, x, y;
            if (!have_img && vc_keyword(c, kNone) != NULL) {
                li = kw("none");
                have_img = true;
            } else if (!have_img && vc_image(c, &v)) {
                li = v;
                have_img = true;
            } else if (!have_pos && position(s, c, &x, &y)) {
                lx = x;
                ly = y;
                have_pos = true;
                if (is_delim_v(vc_peek(c), '/')) {
                    c->i++;
                    if (!bg_size(s, c))
                        return false;
                }
            } else if (!have_rep && repeat(c, &v)) {
                lr = v;
                have_rep = true;
            } else if (!have_att && vc_keyword(c, kAttachment) != NULL) {
                have_att = true;
            } else if (boxes < 2 && vc_keyword(c, kBox) != NULL) {
                boxes++;
            } else if (!have_color && vc_color(c, &v)) {
                lc = v;
                have_color = true;
            } else {
                return false;
            }
        }
        const garb_value_t *t = vc_peek(c);
        bool last = t == NULL;
        if (have_color && !last)
            return false;               // only the final layer has a colour
        if (!have_img && !have_pos && !have_rep && !have_att && boxes == 0 && !have_color)
            return false;               // an empty layer
        if (layer == 0) {
            image = li;
            rep = lr;
            px = lx;
            py = ly;
        }
        if (have_color)
            color = lc;
        if (last)
            break;
        c->i++;                         // the comma
    }
    set(s, GARB_BACKGROUND_COLOR, &color);
    set(s, GARB_BACKGROUND_IMAGE, &image);
    set(s, GARB_BACKGROUND_REPEAT, &rep);
    set(s, GARB_BACKGROUND_POSITION_X, &px);
    set(s, GARB_BACKGROUND_POSITION_Y, &py);
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
    G_BG_REPEAT, G_BG_POS_X, G_BG_POS_Y, G_SPACING,
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
    {GARB_BACKGROUND_POSITION_Y, G_BG_POS_Y, NULL},
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
};

static bool longhand_one(Sets *s, VCur *c, const Longhand *l, garb_val_t *out);

// The background longhands take a comma-separated list, one per layer
// (Backgrounds 3 § 3.1): the first — the layer on top, the one yonder
// draws — is kept, and the rest are read for their validity.
static bool layered(const Longhand *l)
{
    return l->g == G_BG_IMAGE || l->g == G_BG_REPEAT || l->g == G_BG_POS_X || l->g == G_BG_POS_Y;
}

static bool longhand(Sets *s, VCur *c, const Longhand *l, garb_val_t *out)
{
    if (!longhand_one(s, c, l, out))
        return false;
    while (layered(l) && vc_peek(c) != NULL && vc_peek(c)->kind == GARB_COMMA) {
        c->i++;
        garb_val_t other;
        if (!longhand_one(s, c, l, &other))
            return false;
    }
    return true;
}

static bool longhand_one(Sets *s, VCur *c, const Longhand *l, garb_val_t *out)
{
    const char *k;
    switch (l->g) {
    case G_KEYWORDS:
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
        return l->g == G_BG_IMAGE ? vc_image(c, out) : vc_url(c, out);
    case G_BG_REPEAT: return repeat(c, out);
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
    }
    return false;
}

// ── Shorthands ──────────────────────────────────────────────────────────

typedef enum {
    SH_MARGIN, SH_PADDING, SH_BORDER, SH_BORDER_TOP, SH_BORDER_RIGHT, SH_BORDER_BOTTOM,
    SH_BORDER_LEFT, SH_BORDER_WIDTH, SH_BORDER_STYLE, SH_BORDER_COLOR, SH_BACKGROUND,
    SH_BACKGROUND_POSITION, SH_FONT, SH_LIST_STYLE, SH_TEXT_DECORATION, SH_OVERFLOW,
} Shorthand;

static const struct { const char *name; Shorthand sh; } kShorthands[] = {
    {"margin", SH_MARGIN}, {"padding", SH_PADDING}, {"border", SH_BORDER},
    {"border-top", SH_BORDER_TOP}, {"border-right", SH_BORDER_RIGHT},
    {"border-bottom", SH_BORDER_BOTTOM}, {"border-left", SH_BORDER_LEFT},
    {"border-width", SH_BORDER_WIDTH}, {"border-style", SH_BORDER_STYLE},
    {"border-color", SH_BORDER_COLOR}, {"background", SH_BACKGROUND},
    {"background-position", SH_BACKGROUND_POSITION}, {"font", SH_FONT},
    {"list-style", SH_LIST_STYLE}, {"text-decoration", SH_TEXT_DECORATION},
    {"overflow", SH_OVERFLOW},
};

// The longhands a shorthand sets, for a CSS-wide keyword to reach them all.
static int shorthand_longhands(Shorthand sh, garb_prop_t *out)
{
    static const garb_prop_t kBg[] = {GARB_BACKGROUND_COLOR, GARB_BACKGROUND_IMAGE,
                                      GARB_BACKGROUND_REPEAT, GARB_BACKGROUND_POSITION_X,
                                      GARB_BACKGROUND_POSITION_Y};
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
    case SH_BACKGROUND: for (int k = 0; k < 5; k++) out[n++] = kBg[k]; break;
    case SH_BACKGROUND_POSITION: out[n++] = GARB_BACKGROUND_POSITION_X; out[n++] = GARB_BACKGROUND_POSITION_Y; break;
    case SH_FONT: for (int k = 0; k < 6; k++) out[n++] = kFont[k]; break;
    case SH_LIST_STYLE: for (int k = 0; k < 3; k++) out[n++] = kList[k]; break;
    case SH_TEXT_DECORATION: out[n++] = GARB_TEXT_DECORATION_LINE; break;
    case SH_OVERFLOW: out[n++] = GARB_OVERFLOW_X; out[n++] = GARB_OVERFLOW_Y; break;
    }
    return n;
}

static bool shorthand(Sets *s, VCur *c, Shorthand sh)
{
    static const garb_prop_t kAll[4] = {0, 1, 2, 3};
    garb_val_t x, y;
    const char *k;
    switch (sh) {
    case SH_MARGIN: return box4(s, c, GARB_MARGIN_TOP, margin_one);
    case SH_PADDING: return box4(s, c, GARB_PADDING_TOP, padding_one);
    case SH_BORDER: return border_side(s, c, kAll, 4);
    case SH_BORDER_TOP: case SH_BORDER_RIGHT: case SH_BORDER_BOTTOM: case SH_BORDER_LEFT: {
        garb_prop_t side = (garb_prop_t)(sh - SH_BORDER_TOP);
        return border_side(s, c, &side, 1);
    }
    case SH_BORDER_WIDTH: return box4(s, c, GARB_BORDER_TOP_WIDTH, bwidth_one);
    case SH_BORDER_STYLE: return box4(s, c, GARB_BORDER_TOP_STYLE, bstyle_one);
    case SH_BORDER_COLOR: return box4(s, c, GARB_BORDER_TOP_COLOR, bcolor_one);
    case SH_BACKGROUND: return background(s, c);
    case SH_BACKGROUND_POSITION:
        // A position per layer; the first is the one drawn.
        if (!position(s, c, &x, &y))
            return false;
        while (vc_peek(c) != NULL && vc_peek(c)->kind == GARB_COMMA) {
            garb_val_t ox, oy;
            c->i++;
            if (!position(s, c, &ox, &oy))
                return false;
        }
        if (!vc_done(c))
            return false;
        set(s, GARB_BACKGROUND_POSITION_X, &x);
        set(s, GARB_BACKGROUND_POSITION_Y, &y);
        return true;
    case SH_FONT: return font_shorthand(s, c);
    case SH_LIST_STYLE: return list_style(s, c);
    case SH_TEXT_DECORATION: return text_decoration(s, c);
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
        return -1;                      // the cascade reads it once the variable is known
    // A CSS-wide keyword alone reaches every longhand the property sets.
    const char *wide = vc_keyword(&c, kWide);
    if (wide != NULL) {
        if (!vc_done(&c))
            return -1;
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
    return ok && !s.a.short_of_memory ? s.n : -1;
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
// Color 4 serializes a channel, and plenty for a length.
static void num(Out *o, double v)
{
    char b[64];
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
                                         "vmax", "pt", "pc", "cm", "mm", "in", "q"};

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
