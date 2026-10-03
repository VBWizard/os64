// style.c — pass 1: the tree in, a computed style per element out.
//
// THE FIRST PRODUCER OF flow_style_t. What it compiles is the WHATWG HTML
// standard's Rendering chapter — the browsers' shared user-agent style
// sheet written down — plus the presentational attributes the chapter maps
// onto properties, and the quirks-mode rules it states. Within each stage
// below the rules follow the chapter's sections and name them, so a reader
// can hold this file against the standard line by line; where a rule is
// deliberately not honoured the reason is at the rule.
//
// The order inside one element is the cascade's: inheritance first, then
// the user-agent sheet, then the presentational hints (which an author's
// sheet outranks and the user-agent sheet does not), then the quirks-mode
// user-agent rules that the chapter states in prose, then the page's own
// sheets — libgarb's author winners, computed into the same fields.
//
// The walk is iterative, over the tree's own parent pointers, so a page as
// deep as libhtml allows is styled without a stack as deep as the page.

#include "internal.h"
#include "garb/cascade.h"

// ── Lengths while an element is being styled ────────────────────────────
//
// The sheet writes `1em` before the element's font size is known, so a
// length is held as written and resolved once, after the font.
// UNSET is what nothing wrote: a margin or padding of 0, a width of auto.
typedef enum { L_UNSET = 0, L_AUTO, L_PX, L_PCT, L_EM, L_FIT, L_CONTENT } LKind;
typedef struct {
    LKind kind;
    int32_t v;      // PX: 26.6 px; PCT: 1/64 percent; EM: 1/1000 em
    int32_t off;    // PCT: 26.6 px added to it, a calc()'s fixed part
} Len;

static Len em(int32_t thousandths) { return (Len){L_EM, thousandths, 0}; }
static const Len kAuto = {L_AUTO, 0, 0};
static const Len kFitContent = {L_FIT, 0, 0};
static const Len kContent = {L_CONTENT, 0, 0};

static flow_length_t resolve(Len l, flow_unit_t font, flow_length_t unset);

// How the font size was written, resolved against the parent's.
typedef enum {
    FS_INHERIT = 0,
    FS_PX,          // v: 26.6
    FS_EM,          // v: 1/1000 of the parent's
    FS_KEYWORD,     // v: 0 xx-small … 7 xxx-large
    FS_SMALLER,
    FS_LARGER,
} FsKind;

typedef struct {
    flow_style_t s;             // everything that needs no font to resolve
    Len margin[4], padding[4], inset[4], width, height;
    Len radius[4][2];
    Len flex_basis, row_gap, column_gap;     // AUTO: auto, and `normal`
    Len min_width, max_width, min_height, max_height;
    Len text_indent;                // UNSET: as inherited
    FsKind fs_kind;
    int32_t fs_v;
    bool border_color_set[4];
    int32_t border_px[4];       // declared widths in 26.6; style decides if drawn
    bool bolder;
} Spec;

typedef struct {
    const os64_html_document_t *doc;
    const os64_page_t *model;
    const flow_env_t *env;
    FStyles *out;
    bool quirks;                // full quirks mode
    // `body link=` recolours every link, and the body is styled before any
    // link it contains is.
    bool has_body_link;
    uint32_t body_link;
    flow_unit_t root_font;      // the html element's font size: what `rem` is
    double zoom;                // device pixels per CSS pixel (flow_env_t.zoom)
} Ctx;

// `n` CSS pixels in the tree's units, which are device pixels' (zoomed).
static flow_unit_t css(const Ctx *c, double n)
{
    double u = n * FLOW_UNITS_PER_PX * c->zoom;
    return (flow_unit_t)(u >= 0 ? u + 0.5 : u - 0.5);
}

static Len px(const Ctx *c, int32_t n) { return (Len){L_PX, css(c, n), 0}; }

// ── Reading the tree ────────────────────────────────────────────────────

static bool is(const os64_html_node_t *n, os64_html_tag_t tag)
{
    return n != NULL && n->kind == OS64_HTML_ELEMENT && n->ns == OS64_HTML_NS_HTML &&
           n->tag == tag;
}

static const char *attr(const os64_html_node_t *n, const char *name)
{
    const os64_html_attr_t *a = os64_html_attr(n, name);
    return a != NULL ? a->value : NULL;
}

static bool has(const os64_html_node_t *n, const char *name)
{
    return os64_html_attr(n, name) != NULL;
}

// `[name=value i]`
static bool attr_is(const os64_html_node_t *n, const char *name, const char *value)
{
    return f_eq_nocase(attr(n, name), value);
}

static bool is_heading(const os64_html_node_t *n)
{
    return is(n, OS64_HTML_TAG_H1) || is(n, OS64_HTML_TAG_H2) || is(n, OS64_HTML_TAG_H3) ||
           is(n, OS64_HTML_TAG_H4) || is(n, OS64_HTML_TAG_H5) || is(n, OS64_HTML_TAG_H6);
}

static bool is_list(const os64_html_node_t *n)
{
    return is(n, OS64_HTML_TAG_DIR) || is(n, OS64_HTML_TAG_MENU) || is(n, OS64_HTML_TAG_OL) ||
           is(n, OS64_HTML_TAG_UL);
}

static bool is_table_part(const os64_html_node_t *n)
{
    return is(n, OS64_HTML_TAG_THEAD) || is(n, OS64_HTML_TAG_TBODY) ||
           is(n, OS64_HTML_TAG_TFOOT) || is(n, OS64_HTML_TAG_TR) || is(n, OS64_HTML_TAG_TD) ||
           is(n, OS64_HTML_TAG_TH);
}

static bool is_row_group(const os64_html_node_t *n)
{
    return is(n, OS64_HTML_TAG_THEAD) || is(n, OS64_HTML_TAG_TBODY) || is(n, OS64_HTML_TAG_TFOOT);
}

static bool is_li(const os64_html_node_t *n)
{
    return is(n, OS64_HTML_TAG_LI);
}

static bool is_list_or_dl(const os64_html_node_t *n)
{
    return is_list(n) || is(n, OS64_HTML_TAG_DL);
}

// The chapter's descendant selectors (`ul ul`, `li ul`), answered from the
// parent's record — which counts itself and everything above it
// (FStyled.lists) — rather than by walking up from every element.
static const FStyled *above(const Ctx *c, const os64_html_node_t *n)
{
    return f_map_get(&c->out->map, n->parent);
}

static int32_t lists_above(const Ctx *c, const os64_html_node_t *n)
{
    const FStyled *up = above(c, n);
    return up != NULL ? up->lists : 0;
}

// The table a cell belongs to by the chapter's selectors: `table > tr > td`
// or `table > thead|tbody|tfoot > tr > td`, and nothing looser — a cell of
// a nested table is that table's.
static const os64_html_node_t *cell_table(const os64_html_node_t *cell)
{
    const os64_html_node_t *row = cell->parent;
    if (!is(row, OS64_HTML_TAG_TR))
        return NULL;
    const os64_html_node_t *up = row->parent;
    if (is_row_group(up))
        up = up->parent;
    return is(up, OS64_HTML_TAG_TABLE) ? up : NULL;
}

// `table[border]` counts when the attribute is present and is not
// "equivalent to zero": a parse error counts, a zero does not.
static bool border_not_zero(const os64_html_node_t *table)
{
    const char *b = attr(table, "border");
    if (b == NULL)
        return false;
    int32_t v;
    return !f_parse_nonnegative(b, &v) || v != 0;
}

// Margin collapsing quirks (§15.3.9) speak of SUBSTANTIAL nodes: an element,
// or text that is not inter-element white space.
static bool substantial(const os64_html_node_t *n)
{
    if (n->kind == OS64_HTML_ELEMENT)
        return true;
    if (n->kind != OS64_HTML_TEXT)
        return false;
    for (size_t i = 0; i < n->text_len; i++) {
        char c = n->text[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\f' && c != '\r')
            return true;
    }
    return false;
}

static bool blank(const os64_html_node_t *n)
{
    for (const os64_html_node_t *c = n->first_child; c != NULL; c = c->next)
        if (substantial(c))
            return false;
    return true;
}

static bool substantial_before(const os64_html_node_t *n)
{
    for (const os64_html_node_t *p = n->prev; p != NULL; p = p->prev)
        if (substantial(p))
            return true;
    return false;
}

static bool substantial_after(const os64_html_node_t *n)
{
    for (const os64_html_node_t *p = n->next; p != NULL; p = p->next)
        if (substantial(p))
            return true;
    return false;
}

static bool default_margins(const os64_html_node_t *n)
{
    return is(n, OS64_HTML_TAG_BLOCKQUOTE) || is(n, OS64_HTML_TAG_DIR) ||
           is(n, OS64_HTML_TAG_DL) || is_heading(n) || is(n, OS64_HTML_TAG_LISTING) ||
           is(n, OS64_HTML_TAG_MENU) || is(n, OS64_HTML_TAG_OL) || is(n, OS64_HTML_TAG_P) ||
           is(n, OS64_HTML_TAG_PLAINTEXT) || is(n, OS64_HTML_TAG_PRE) ||
           is(n, OS64_HTML_TAG_UL) || is(n, OS64_HTML_TAG_XMP);
}

// ── Setting properties ──────────────────────────────────────────────────

static void margin_block(Spec *sp, Len v)
{
    sp->margin[FLOW_TOP] = sp->margin[FLOW_BOTTOM] = v;
}

static void margin_inline(Spec *sp, Len v)
{
    sp->margin[FLOW_LEFT] = sp->margin[FLOW_RIGHT] = v;
}

static void padding_all(Spec *sp, Len v)
{
    for (int i = 0; i < 4; i++)
        sp->padding[i] = v;
}

static void border(const Ctx *c, Spec *sp, flow_border_style_t style, int32_t width_px)
{
    for (int i = 0; i < 4; i++) {
        sp->s.border_style[i] = style;
        sp->border_px[i] = css(c, width_px);
    }
}

static void border_color(Spec *sp, uint32_t rgb)
{
    for (int i = 0; i < 4; i++) {
        sp->s.border_color[i] = rgb;
        sp->border_color_set[i] = true;
    }
}

static void monospace(Spec *sp)
{
    sp->s.family = (flow_family_list_t){NULL, 0, FLOW_GENERIC_MONO};
}

// "Maps to the dimension property": the attribute, read as a dimension,
// sets the property — a length in CSS pixels, zoomed — and one that does
// not parse sets nothing.
static void map_dimension(const Ctx *c, const os64_html_node_t *n, const char *name, bool nonzero,
                          Len *out)
{
    int32_t v;
    f_dim_kind_t kind = f_parse_dimension(attr(n, name), nonzero, &v);
    if (kind == F_DIM_PERCENT)
        *out = (Len){L_PCT, v, 0};
    else if (kind == F_DIM_PX)
        *out = (Len){L_PX, css(c, (double)v / FLOW_UNITS_PER_PX), 0};
}

// "Maps to the pixel length property": a non-negative integer, in px.
static bool pixel_length(const Ctx *c, const os64_html_node_t *n, const char *name, Len *out)
{
    int32_t v;
    if (!f_parse_nonnegative(attr(n, name), &v))
        return false;
    *out = px(c, v);
    return true;
}

static bool legacy_color(const os64_html_node_t *n, const char *name, uint32_t *out)
{
    const char *v = attr(n, name);
    return v != NULL && f_parse_legacy_color(v, out);
}

static void background(Spec *sp, uint32_t rgb)
{
    sp->s.has_background = true;
    sp->s.background = rgb;
    sp->s.current_colours &= (uint8_t)~FLOW_CURRENT_BACKGROUND;
}

// ── The initial style, and inheritance ──────────────────────────────────

// One layer of nothing: no picture, repeated from the padding box's
// corner at its own size, cut to the border box.
static const flow_bg_image_t kNoImage = {NULL, 0, -1, NULL};
static const flow_repeat_t kRepeat = FLOW_REPEAT;
static const flow_length_t kAtStart = {FLOW_LENGTH_PERCENT, 0, 0};
static const flow_bg_size_t kOwnSize = {FLOW_FIT_LENGTHS, {{FLOW_LENGTH_AUTO, 0, 0},
                                                           {FLOW_LENGTH_AUTO, 0, 0}}};
static const flow_edge_t kPaddingBox = FLOW_EDGE_PADDING, kBorderBox = FLOW_EDGE_BORDER;
static const flow_backgrounds_t kBackgrounds = {&kNoImage, &kRepeat, &kAtStart, &kAtStart,
                                                &kOwnSize, &kPaddingBox, &kBorderBox,
                                                1, 1, 1, 1, 1, 1, 1};

static flow_style_t initial(const Ctx *c)
{
    flow_style_t s;
    os64_memset(&s, 0, sizeof(s));
    s.display = FLOW_DISPLAY_INLINE;
    s.family = (flow_family_list_t){NULL, 0, c->env->default_generic};
    s.font_weight = 400;
    s.font_style = FLOW_FONT_NORMAL;
    s.font_size = css(c, c->env->viewport_font_px);
    s.color = c->env->ink;
    s.backgrounds = kBackgrounds;
    s.opacity = 1000;
    s.flex_shrink = 1000;
    s.align_self = FLOW_PLACE_AUTO;
    s.justify_self = FLOW_PLACE_AUTO;
    // Margins and padding start at 0px — NOT at zero bytes, which is `auto`.
    for (int i = 0; i < 4; i++) {
        s.margin[i] = (flow_length_t){FLOW_LENGTH_PX, 0, 0};
        s.padding[i] = (flow_length_t){FLOW_LENGTH_PX, 0, 0};
    }
    // Everything else starts at zero bytes, which is each property's
    // initial value: auto width and height, no borders, left, baseline,
    // normal, disc outside, separate, top, no float, static with auto
    // insets, z-index auto, and a row that does not wrap, its alignments
    // normal, its items growing by nothing from an auto basis, order 0,
    // and gaps of normal; no grid tracks or areas, auto-placement by rows,
    // and auto lines.
    return s;
}

// What a child takes from its parent before any rule of its own: the
// inherited properties, and the initial value of every other.
static flow_style_t inherit(const Ctx *c, const flow_style_t *parent)
{
    flow_style_t s = initial(c);
    if (parent == NULL)
        return s;
    s.family = parent->family;
    s.font_weight = parent->font_weight;
    s.font_style = parent->font_style;
    s.font_size = parent->font_size;
    s.color = parent->color;
    s.text_align = parent->text_align;
    s.white_space = parent->white_space;
    s.line_height = parent->line_height;
    s.text_indent = parent->text_indent;
    s.text_transform = parent->text_transform;
    s.text_shadows = parent->text_shadows;
    s.ntext_shadows = parent->ntext_shadows;
    s.pixelated = parent->pixelated;
    s.visibility = parent->visibility;
    s.pointer_events_none = parent->pointer_events_none;
    s.list_style_type = parent->list_style_type;
    s.list_style_position = parent->list_style_position;
    s.border_spacing[0] = parent->border_spacing[0];
    s.border_spacing[1] = parent->border_spacing[1];
    s.border_collapse = parent->border_collapse;
    s.caption_side = parent->caption_side;
    return s;
}

// ── Fonts ───────────────────────────────────────────────────────────────

// The absolute-size keywords as CSS Fonts' scaling factors on `medium`:
// xx-small … xxx-large.
static const struct { int32_t num, den; } s_keyword[8] = {
    {3, 5}, {3, 4}, {8, 9}, {1, 1}, {6, 5}, {3, 2}, {2, 1}, {3, 1},
};

// A size times a ratio, held to F_INT_MAX pixels either way: sizes compound
// down the tree (each nested <big> is 6/5 of its parent's), so a page can
// ask for more than a 26.6 int32_t holds, and a wrapped one is negative.
static int32_t scale(int32_t v, int32_t num, int32_t den)
{
    int64_t r = ((int64_t)v * num + den / 2) / den;
    int64_t most = (int64_t)F_INT_MAX * FLOW_UNITS_PER_PX;
    return (int32_t)(r > most ? most : r < -most ? -most : r);
}

static flow_unit_t font_size(const Ctx *c, const Spec *sp, const flow_style_t *parent)
{
    flow_unit_t medium = css(c, c->env->viewport_font_px);
    flow_unit_t base = parent != NULL ? parent->font_size : medium;
    switch (sp->fs_kind) {
    case FS_PX: return sp->fs_v;
    case FS_EM: return scale(base, sp->fs_v, 1000);
    case FS_KEYWORD: return scale(medium, s_keyword[sp->fs_v].num, s_keyword[sp->fs_v].den);
    // CSS Fonts' suggested ratio between neighbouring sizes.
    case FS_SMALLER: return scale(base, 5, 6);
    case FS_LARGER: return scale(base, 6, 5);
    case FS_INHERIT: break;
    }
    return base;
}

// CSS Fonts' `bolder`, from the parent's weight.
// CSS Fonts 4 § 2.2's table: a weight at or above 900 stays as it is.
static uint16_t bolder(uint16_t w)
{
    return w < 350 ? 400 : w < 550 ? 700 : w < 900 ? 900 : w;
}

// Which generic a named face stands for, when the page spelled no generic
// of its own: a monospace face by its name, a serif face by its name, and
// sans for the rest, which is what most faces a page names are.
static bool contains_nocase(const char *s, uint32_t len, const char *word)
{
    size_t wl = os64_strlen(word);
    for (uint32_t i = 0; i + wl <= len; i++) {
        size_t k = 0;
        while (k < wl) {
            char ch = s[i + k];
            if (ch >= 'A' && ch <= 'Z')
                ch = (char)(ch + 32);
            if (ch != word[k])
                break;
            k++;
        }
        if (k == wl)
            return true;
    }
    return false;
}

static bool generic_named(const char *s, uint32_t len, flow_generic_t *out)
{
    static const struct { const char *name; flow_generic_t g; } k[] = {
        {"serif", FLOW_GENERIC_SERIF}, {"sans-serif", FLOW_GENERIC_SANS},
        {"monospace", FLOW_GENERIC_MONO},
    };
    for (int32_t i = 0; i < F_ARRAY(k); i++) {
        size_t wl = os64_strlen(k[i].name);
        if (wl == len && contains_nocase(s, len, k[i].name)) {
            *out = k[i].g;
            return true;
        }
    }
    return false;
}

static flow_generic_t guess_generic(const char *s, uint32_t len)
{
    if (contains_nocase(s, len, "mono") || contains_nocase(s, len, "courier"))
        return FLOW_GENERIC_MONO;
    if (contains_nocase(s, len, "sans"))
        return FLOW_GENERIC_SANS;
    if (contains_nocase(s, len, "serif") || contains_nocase(s, len, "times") ||
        contains_nocase(s, len, "georgia"))
        return FLOW_GENERIC_SERIF;
    return FLOW_GENERIC_SANS;
}

// `<font face="Verdana, Arial, sans-serif">`: the names as written, each a
// slice of the attribute, and the generic the list ends in — the first
// generic keyword the page wrote, else a guess from the first name. A
// generic keyword is a family, not a name, so it is not passed as one.
static bool font_face(Ctx *c, const char *v, flow_family_list_t *out)
{
    uint32_t count = 0;
    for (const char *p = v; *p != '\0'; p++)
        if (*p == ',')
            count++;
    count++;
    flow_family_name_t *names = f_arena_alloc(&c->out->arena, count * sizeof(*names));
    if (names == NULL)
        return false;
    uint32_t n = 0;
    bool have_generic = false;
    flow_generic_t generic = FLOW_GENERIC_SANS;
    const char *p = v;
    for (;;) {
        const char *start = p;
        while (*p != '\0' && *p != ',')
            p++;
        const char *end = p;
        while (start < end && (*start == ' ' || *start == '\t' || *start == '\n' ||
                               *start == '\f' || *start == '\r'))
            start++;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' ||
                               end[-1] == '\f' || end[-1] == '\r'))
            end--;
        uint32_t len = (uint32_t)(end - start);
        flow_generic_t g;
        if (len > 0 && generic_named(start, len, &g)) {
            if (!have_generic) {
                generic = g;
                have_generic = true;
            }
        } else if (len > 0) {
            names[n++] = (flow_family_name_t){start, len};
        }
        if (*p == '\0')
            break;
        p++;
    }
    // A face of nothing but commas and space declares no family at all.
    if (!have_generic && n == 0)
        return true;
    if (!have_generic)
        generic = guess_generic(names[0].name, names[0].len);
    *out = (flow_family_list_t){n > 0 ? names : NULL, n, generic};
    return true;
}

// "The rules for parsing a legacy font size" (§15.3.4).
static bool legacy_font_size(const char *v, int32_t *keyword)
{
    while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\f' || *v == '\r')
        v++;
    if (*v == '\0')
        return false;
    int mode = 0;
    if (*v == '+') {
        mode = 1;
        v++;
    } else if (*v == '-') {
        mode = -1;
        v++;
    }
    if (*v < '0' || *v > '9')
        return false;
    int32_t value = 0;
    for (; *v >= '0' && *v <= '9'; v++)
        if (value < 1000)
            value = value * 10 + (*v - '0');
    if (mode > 0)
        value = 3 + value;
    else if (mode < 0)
        value = 3 - value;
    if (value > 7)
        value = 7;
    if (value < 1)
        value = 1;
    // 1 → x-small … 7 → xxx-large; the keyword table starts at xx-small.
    *keyword = value;
    return true;
}

// ── The user-agent sheet ────────────────────────────────────────────────

static void sheet(Ctx *c, const os64_html_node_t *n, Spec *sp)
{
    flow_style_t *s = &sp->s;

    // §15.3.1 Hidden elements.
    switch (n->tag) {
    case OS64_HTML_TAG_AREA: case OS64_HTML_TAG_BASE: case OS64_HTML_TAG_BASEFONT:
    case OS64_HTML_TAG_DATALIST: case OS64_HTML_TAG_HEAD: case OS64_HTML_TAG_LINK:
    case OS64_HTML_TAG_META: case OS64_HTML_TAG_NOEMBED: case OS64_HTML_TAG_NOFRAMES:
    case OS64_HTML_TAG_PARAM: case OS64_HTML_TAG_RP: case OS64_HTML_TAG_SCRIPT:
    case OS64_HTML_TAG_STYLE: case OS64_HTML_TAG_TEMPLATE: case OS64_HTML_TAG_TITLE:
        s->display = FLOW_DISPLAY_NONE;
        return;
    default:
        break;
    }
    // `[hidden=until-found]` is content-visibility: hidden — found by a
    // search, drawn by nothing — and with no search to find it, it is laid
    // out as the hidden thing it is.
    if (has(n, "hidden") && !is(n, OS64_HTML_TAG_EMBED)) {
        s->display = FLOW_DISPLAY_NONE;
        return;
    }
    if (is(n, OS64_HTML_TAG_INPUT) && attr_is(n, "type", "hidden")) {
        s->display = FLOW_DISPLAY_NONE;
        return;
    }
    // §15.3.3 `[popover]:not(:popover-open):not(dialog[open])`: opening a
    // popover takes script, which this browser does not run, so every one
    // but an open dialog is hidden, as a browser shows it until something
    // opens it.
    if (has(n, "popover") && !(is(n, OS64_HTML_TAG_DIALOG) && has(n, "open"))) {
        s->display = FLOW_DISPLAY_NONE;
        return;
    }
    // `noscript` is shown: this browser runs no script, so the
    // `@media (scripting)` rule that hides it does not apply.

    switch (n->tag) {
    // §15.3.2 The page, §15.3.3 Flow content.
    case OS64_HTML_TAG_HTML: case OS64_HTML_TAG_BODY:
    case OS64_HTML_TAG_CENTER: case OS64_HTML_TAG_DIV:
    case OS64_HTML_TAG_FIGCAPTION: case OS64_HTML_TAG_FOOTER: case OS64_HTML_TAG_FORM:
    case OS64_HTML_TAG_HEADER: case OS64_HTML_TAG_MAIN:
    case OS64_HTML_TAG_SEARCH:
    // §15.3.6 Sections and headings.
    case OS64_HTML_TAG_ARTICLE: case OS64_HTML_TAG_ASIDE: case OS64_HTML_TAG_HGROUP:
    case OS64_HTML_TAG_NAV: case OS64_HTML_TAG_SECTION:
    // §15.3.7 Lists.
    case OS64_HTML_TAG_DD: case OS64_HTML_TAG_DT:
    // §15.5.5 details; §15.5.16 option and optgroup, laid out only by a
    // select, which is the face's widget.
    case OS64_HTML_TAG_DETAILS: case OS64_HTML_TAG_SUMMARY:
    case OS64_HTML_TAG_OPTION: case OS64_HTML_TAG_OPTGROUP:
    // §15.6: a frameset is a surface its frames divide. The first cut draws
    // each frame as the link to what it shows (LAYOUT.md § Pass 2), so the
    // frameset is a block they stack in.
    case OS64_HTML_TAG_FRAMESET: case OS64_HTML_TAG_FRAME:
        s->display = FLOW_DISPLAY_BLOCK;
        break;
    case OS64_HTML_TAG_ADDRESS:
        s->display = FLOW_DISPLAY_BLOCK;
        s->font_style = FLOW_FONT_ITALIC;
        break;
    case OS64_HTML_TAG_BLOCKQUOTE: case OS64_HTML_TAG_FIGURE:
        s->display = FLOW_DISPLAY_BLOCK;
        margin_block(sp, em(1000));
        margin_inline(sp, px(c, 40));
        break;
    case OS64_HTML_TAG_P:
        s->display = FLOW_DISPLAY_BLOCK;
        margin_block(sp, em(1000));
        break;
    case OS64_HTML_TAG_LISTING: case OS64_HTML_TAG_PLAINTEXT: case OS64_HTML_TAG_PRE:
    case OS64_HTML_TAG_XMP:
        s->display = FLOW_DISPLAY_BLOCK;
        margin_block(sp, em(1000));
        monospace(sp);
        s->white_space = FLOW_WS_PRE;
        break;
    case OS64_HTML_TAG_DIALOG:
        // Open, it is positioned over the page where it was written,
        // centred between its containing block's sides and as wide as its
        // content wants. The block-axis insets are the modal dialog's,
        // which only script opens.
        if (!has(n, "open")) {
            s->display = FLOW_DISPLAY_NONE;
            return;
        }
        s->display = FLOW_DISPLAY_BLOCK;
        s->position = FLOW_POSITION_ABSOLUTE;
        sp->inset[FLOW_LEFT] = sp->inset[FLOW_RIGHT] = px(c, 0);
        for (int i = 0; i < 4; i++)
            sp->margin[i] = kAuto;
        sp->width = kFitContent;
        border(c, sp, FLOW_BORDER_SOLID, 3);   // `border: solid` is medium: 3px
        padding_all(sp, em(1000));
        background(sp, c->env->paper);
        s->color = c->env->ink;
        break;
    case OS64_HTML_TAG_SLOT:
        s->display = FLOW_DISPLAY_CONTENTS;
        break;

    // §15.3.4 Phrasing content.
    case OS64_HTML_TAG_CITE: case OS64_HTML_TAG_DFN: case OS64_HTML_TAG_EM:
    case OS64_HTML_TAG_I: case OS64_HTML_TAG_VAR:
        s->font_style = FLOW_FONT_ITALIC;
        break;
    case OS64_HTML_TAG_B: case OS64_HTML_TAG_STRONG:
        sp->bolder = true;
        break;
    case OS64_HTML_TAG_CODE: case OS64_HTML_TAG_KBD: case OS64_HTML_TAG_SAMP:
    case OS64_HTML_TAG_TT:
        monospace(sp);
        break;
    case OS64_HTML_TAG_BIG:
        sp->fs_kind = FS_LARGER;
        break;
    case OS64_HTML_TAG_SMALL:
        sp->fs_kind = FS_SMALLER;
        break;
    case OS64_HTML_TAG_SUB: case OS64_HTML_TAG_SUP:
        s->vertical_align = n->tag == OS64_HTML_TAG_SUB ? FLOW_VALIGN_SUB : FLOW_VALIGN_SUPER;
        sp->fs_kind = FS_SMALLER;
        break;
    case OS64_HTML_TAG_MARK:
        background(sp, 0xFFFF00);
        s->color = 0x000000;
        break;
    case OS64_HTML_TAG_ABBR: case OS64_HTML_TAG_ACRONYM:
        // `dotted underline`; the dotted line is drawn as a line.
        if (has(n, "title"))
            s->text_decoration |= FLOW_DECORATION_UNDERLINE;
        break;
    case OS64_HTML_TAG_INS: case OS64_HTML_TAG_U:
        s->text_decoration |= FLOW_DECORATION_UNDERLINE;
        break;
    case OS64_HTML_TAG_DEL: case OS64_HTML_TAG_S: case OS64_HTML_TAG_STRIKE:
        s->text_decoration |= FLOW_DECORATION_LINE_THROUGH;
        break;
    case OS64_HTML_TAG_NOBR:
        s->white_space = FLOW_WS_NOWRAP;
        break;
    // `nobr wbr { white-space: normal }`: the one break a nowrap run
    // allows, which pass 3 honours only if the wbr's own style says so.
    case OS64_HTML_TAG_WBR: {
        const FStyled *up = f_map_get(&c->out->map, n->parent);
        if (up != NULL && up->in_nobr)
            s->white_space = FLOW_WS_NORMAL;
        break;
    }
    // `ruby` and `rt` are the chapter's `display: ruby` and `ruby-text`,
    // which this struct does not have: both are laid out inline, the
    // annotation reading after its base (LAYOUT.md § Booked).

    // §15.3.6 headings.
    case OS64_HTML_TAG_H1: case OS64_HTML_TAG_H2: case OS64_HTML_TAG_H3:
    case OS64_HTML_TAG_H4: case OS64_HTML_TAG_H5: case OS64_HTML_TAG_H6: {
        static const int32_t size[6] = {2000, 1500, 1170, 1000, 830, 670};
        static const int32_t margin[6] = {670, 830, 1000, 1330, 1670, 2330};
        int32_t level = n->tag - OS64_HTML_TAG_H1;
        s->display = FLOW_DISPLAY_BLOCK;
        s->font_weight = 700;
        sp->fs_kind = FS_EM;
        sp->fs_v = size[level];
        margin_block(sp, em(margin[level]));
        break;
    }

    // §15.3.7 Lists.
    case OS64_HTML_TAG_DL:
        s->display = FLOW_DISPLAY_BLOCK;
        margin_block(sp, em(1000));
        break;
    case OS64_HTML_TAG_DIR: case OS64_HTML_TAG_MENU: case OS64_HTML_TAG_OL:
    case OS64_HTML_TAG_UL: {
        s->display = FLOW_DISPLAY_BLOCK;
        margin_block(sp, em(1000));
        sp->padding[FLOW_LEFT] = px(c, 40);
        if (n->tag == OS64_HTML_TAG_OL) {
            s->list_style_type = FLOW_LIST_DECIMAL;
        } else {
            int32_t depth = lists_above(c, n);
            s->list_style_type = depth >= 2 ? FLOW_LIST_SQUARE
                               : depth == 1 ? FLOW_LIST_CIRCLE : FLOW_LIST_DISC;
        }
        break;
    }
    case OS64_HTML_TAG_LI:
        s->display = FLOW_DISPLAY_LIST_ITEM;
        break;

    // §15.3.8 Tables.
    case OS64_HTML_TAG_TABLE:
        s->display = FLOW_DISPLAY_TABLE;
        s->border_spacing[0] = s->border_spacing[1] = css(c, 2);
        s->border_collapse = FLOW_SEPARATE;
        break;
    case OS64_HTML_TAG_CAPTION:
        s->display = FLOW_DISPLAY_TABLE_CAPTION;
        s->text_align = FLOW_ALIGN_CENTER;
        break;
    case OS64_HTML_TAG_COLGROUP:
        s->display = FLOW_DISPLAY_TABLE_COLUMN_GROUP;
        break;
    case OS64_HTML_TAG_COL:
        s->display = FLOW_DISPLAY_TABLE_COLUMN;
        break;
    case OS64_HTML_TAG_THEAD: case OS64_HTML_TAG_TBODY: case OS64_HTML_TAG_TFOOT:
        s->display = n->tag == OS64_HTML_TAG_THEAD ? FLOW_DISPLAY_TABLE_HEADER_GROUP
                   : n->tag == OS64_HTML_TAG_TFOOT ? FLOW_DISPLAY_TABLE_FOOTER_GROUP
                   : FLOW_DISPLAY_TABLE_ROW_GROUP;
        s->vertical_align = FLOW_VALIGN_MIDDLE;
        break;
    case OS64_HTML_TAG_TR:
        s->display = FLOW_DISPLAY_TABLE_ROW;
        break;
    case OS64_HTML_TAG_TD: case OS64_HTML_TAG_TH:
        s->display = FLOW_DISPLAY_TABLE_CELL;
        padding_all(sp, px(c, 1));
        if (n->tag == OS64_HTML_TAG_TH)
            s->font_weight = 700;
        break;

    // §15.3.10 Form controls: laid out as boxes the face's widgets fill.
    // The chapter's `text-align` for controls and `white-space` for a
    // textarea place the text INSIDE the widget, which the face draws.
    case OS64_HTML_TAG_INPUT: case OS64_HTML_TAG_BUTTON: case OS64_HTML_TAG_SELECT:
    case OS64_HTML_TAG_TEXTAREA: case OS64_HTML_TAG_METER: case OS64_HTML_TAG_PROGRESS:
        s->display = FLOW_DISPLAY_INLINE_BLOCK;
        break;
    // §15.5.13: a marquee that does not move (booked) is the box it moves in.
    case OS64_HTML_TAG_MARQUEE:
        s->display = FLOW_DISPLAY_INLINE_BLOCK;
        s->text_align = FLOW_ALIGN_LEFT;
        break;

    // §15.3.11 The hr element.
    case OS64_HTML_TAG_HR:
        s->display = FLOW_DISPLAY_BLOCK;
        s->color = 0x808080;
        border(c, sp, FLOW_BORDER_INSET, 1);
        margin_block(sp, em(500));
        margin_inline(sp, kAuto);
        break;

    // §15.3.12 fieldset and legend. A legend drawn ON the fieldset's border
    // is not CSS 2.1's; it is laid out inside, first, which is where it
    // reads (LAYOUT.md § Booked).
    case OS64_HTML_TAG_FIELDSET:
        s->display = FLOW_DISPLAY_BLOCK;
        margin_inline(sp, px(c, 2));
        border(c, sp, FLOW_BORDER_GROOVE, 2);
        border_color(sp, 0xC0C0C0);     // ThreeDFace, the classic 3D grey
        sp->padding[FLOW_TOP] = em(350);
        sp->padding[FLOW_BOTTOM] = em(625);
        sp->padding[FLOW_LEFT] = sp->padding[FLOW_RIGHT] = em(750);
        break;
    case OS64_HTML_TAG_LEGEND:
        s->display = FLOW_DISPLAY_BLOCK;
        sp->padding[FLOW_LEFT] = sp->padding[FLOW_RIGHT] = px(c, 2);
        break;

    // §15.4.1 Embedded content.
    case OS64_HTML_TAG_IFRAME:
        border(c, sp, FLOW_BORDER_INSET, 2);
        break;
    default:
        break;
    }

    // A form inside table structure is an empty marker the parser left
    // there: its controls belong to it by owner, not by nesting.
    if (is(n, OS64_HTML_TAG_FORM) &&
        (is(n->parent, OS64_HTML_TAG_TABLE) || is_row_group(n->parent) ||
         is(n->parent, OS64_HTML_TAG_TR)))
        s->display = FLOW_DISPLAY_NONE;

    // §15.3.7: a list inside a list keeps no block margins.
    if (is_list_or_dl(n) && above(c, n) != NULL && above(c, n)->lists_or_dls > 0)
        margin_block(sp, px(c, 0));
    // `dd { margin-inline-start: 40px }`
    if (is(n, OS64_HTML_TAG_DD))
        sp->margin[FLOW_LEFT] = px(c, 40);

    // §15.3.8: cells take their vertical alignment from the row, and the
    // row from the group; `table > tr` without a group is middle itself.
    const FStyled *parent = f_map_get(&c->out->map, n->parent);
    if (is(n, OS64_HTML_TAG_TR) || is(n, OS64_HTML_TAG_TD) || is(n, OS64_HTML_TAG_TH))
        s->vertical_align = parent != NULL ? parent->style.vertical_align : FLOW_VALIGN_BASELINE;
    if (is(n, OS64_HTML_TAG_TR) && is(n->parent, OS64_HTML_TAG_TABLE))
        s->vertical_align = FLOW_VALIGN_MIDDLE;
    // …and their border colour from the table, through the groups and rows.
    if ((is_row_group(n) || is(n, OS64_HTML_TAG_TR)) && parent != NULL)
        for (int i = 0; i < 4; i++) {
            s->border_color[i] = parent->style.border_color[i];
            sp->border_color_set[i] = true;
        }
    // A th centres its text when its parent's alignment was never set: the
    // chapter states this rule as a condition, not a selector.
    if (is(n, OS64_HTML_TAG_TH) && parent != NULL &&
        parent->style.text_align == FLOW_ALIGN_LEFT)
        s->text_align = FLOW_ALIGN_CENTER;

    // §15.3.8 `rules` and `frame`: the borders a table's attributes ask for.
    if (is(n, OS64_HTML_TAG_TABLE)) {
        const char *rules = attr(n, "rules");
        const char *frame = attr(n, "frame");
        static const char *const rule_values[] = {"none", "groups", "rows", "cols", "all"};
        static const char *const frame_values[] = {"void", "above", "below", "hsides", "lhs",
                                                    "rhs", "vsides", "box", "border"};
        bool ruled = false, framed = false;
        for (int32_t i = 0; i < F_ARRAY(rule_values); i++)
            ruled |= f_eq_nocase(rules, rule_values[i]);
        for (int32_t i = 0; i < F_ARRAY(frame_values); i++)
            framed |= f_eq_nocase(frame, frame_values[i]);
        if (ruled || framed)
            border_color(sp, 0x000000);
    }
    // The cells' half of the same rule names `rules` alone: a table's
    // `frame` blackens the table's own border, not its cells'.
    if (is(n, OS64_HTML_TAG_TD) || is(n, OS64_HTML_TAG_TH)) {
        const os64_html_node_t *table = cell_table(n);
        const char *rules = table != NULL ? attr(table, "rules") : NULL;
        static const char *const any[] = {"none", "groups", "rows", "cols", "all"};
        for (int32_t i = 0; i < F_ARRAY(any); i++)
            if (f_eq_nocase(rules, any[i]))
                border_color(sp, 0x000000);
    }

    // §15.3.9 is prose and applies after the hints; §15.3.7's and §15.3.8's
    // quirks-mode sheets apply here, at user-agent level.
    if (c->quirks) {
        if (is(n, OS64_HTML_TAG_FORM) && s->display != FLOW_DISPLAY_NONE)
            sp->margin[FLOW_BOTTOM] = em(1000);
        if (is(n, OS64_HTML_TAG_TABLE)) {
            s->font_weight = 400;
            s->font_style = FLOW_FONT_NORMAL;
            sp->fs_kind = FS_KEYWORD;
            sp->fs_v = 3;
            s->white_space = FLOW_WS_NORMAL;
            s->text_align = FLOW_ALIGN_LEFT;
            // The Quirks Mode standard's 3.12: a table takes the BODY's
            // colour, not its parent's — Netscape's tables inherited no
            // font, which is why old pages repeat <font> in every cell.
            const FStyled *body = f_map_get(&c->out->map, c->doc->body);
            s->color = body != NULL ? body->style.color : c->env->ink;
        }
        // `li { inside }`, `li :is(lists) { outside }`, and
        // `:is(lists) :is(lists, li) { unset }` — the last wins on order.
        bool in_list = lists_above(c, n) > 0;
        if (is_li(n) && !in_list)
            s->list_style_position = FLOW_LIST_INSIDE;
        if (is_list(n) && !in_list && above(c, n) != NULL && above(c, n)->items > 0)
            s->list_style_position = FLOW_LIST_OUTSIDE;
    }
}

// ── The presentational hints ────────────────────────────────────────────

static void align_block(const os64_html_node_t *n, flow_style_t *s)
{
    // `align` on div (§15.3.3) and the table parts (§15.3.8): text aligned
    // AND block descendants aligned.
    if (attr_is(n, "align", "center") || attr_is(n, "align", "middle"))
        s->text_align = FLOW_ALIGN_HTML_CENTER;
    else if (attr_is(n, "align", "left"))
        s->text_align = FLOW_ALIGN_HTML_LEFT;
    else if (attr_is(n, "align", "right"))
        s->text_align = FLOW_ALIGN_HTML_RIGHT;
    else if (attr_is(n, "align", "justify"))
        s->text_align = FLOW_ALIGN_HTML_JUSTIFY;
}

static bool is_embedded_aligned(const os64_html_node_t *n)
{
    return is(n, OS64_HTML_TAG_EMBED) || is(n, OS64_HTML_TAG_IFRAME) ||
           is(n, OS64_HTML_TAG_IMG) || is(n, OS64_HTML_TAG_OBJECT) ||
           (is(n, OS64_HTML_TAG_INPUT) && attr_is(n, "type", "image"));
}

// §15.3.4: a link's colour, where libpage says the node IS a link. The
// user-agent rule is `:link`, and `body link=` is a hint on the same
// selector, so it wins over the sheet and loses to anything the link's own
// descendants say. It is applied with the hints for that reason, and
// without the hint to the user-agent origin `revert` goes back to.
static void link_rule(const Ctx *c, const os64_html_node_t *n, Spec *sp, bool hint)
{
    if ((is(n, OS64_HTML_TAG_A) || is(n, OS64_HTML_TAG_AREA)) && c->model != NULL &&
        os64_page_link_for(c->model, n) >= 0) {
        sp->s.color = hint && c->has_body_link ? c->body_link : c->env->link_ink;
        sp->s.text_decoration |= FLOW_DECORATION_UNDERLINE;
    }
}

static bool hints(Ctx *c, const os64_html_node_t *n, Spec *sp)
{
    flow_style_t *s = &sp->s;
    uint32_t rgb;

    switch (n->tag) {
    // §15.3.2 The page.
    case OS64_HTML_TAG_BODY: {
        // The FIRST of the attributes that exists decides, and one that
        // will not parse means the 8px default — not the next attribute.
        Len v = px(c, 8);
        if (has(n, "marginheight") ? !pixel_length(c, n, "marginheight", &v)
                                   : has(n, "topmargin") && !pixel_length(c, n, "topmargin", &v))
            v = px(c, 8);
        margin_block(sp, v);
        v = px(c, 8);
        if (has(n, "marginwidth") ? !pixel_length(c, n, "marginwidth", &v)
                                  : has(n, "leftmargin") && !pixel_length(c, n, "leftmargin", &v))
            v = px(c, 8);
        margin_inline(sp, v);
        if (legacy_color(n, "bgcolor", &rgb))
            background(sp, rgb);
        if (legacy_color(n, "text", &rgb))
            s->color = rgb;
        if (legacy_color(n, "link", &rgb)) {
            c->has_body_link = true;
            c->body_link = rgb;
        }
        break;
    }
    // §15.3.3
    case OS64_HTML_TAG_PRE:
        if (has(n, "wrap"))
            s->white_space = FLOW_WS_PRE_WRAP;
        break;
    case OS64_HTML_TAG_CENTER:
        s->text_align = FLOW_ALIGN_HTML_CENTER;
        break;
    case OS64_HTML_TAG_DIV:
        align_block(n, s);
        break;
    // §15.3.4
    case OS64_HTML_TAG_BR:
        if (attr_is(n, "clear", "left"))
            s->clear = FLOW_CLEAR_LEFT;
        else if (attr_is(n, "clear", "right"))
            s->clear = FLOW_CLEAR_RIGHT;
        else if (attr_is(n, "clear", "all") || attr_is(n, "clear", "both"))
            s->clear = FLOW_CLEAR_BOTH;
        break;
    case OS64_HTML_TAG_FONT: {
        if (legacy_color(n, "color", &rgb))
            s->color = rgb;
        const char *face = attr(n, "face");
        if (face != NULL && !font_face(c, face, &s->family))
            return false;
        int32_t keyword;
        const char *size = attr(n, "size");
        if (size != NULL && legacy_font_size(size, &keyword)) {
            sp->fs_kind = FS_KEYWORD;
            sp->fs_v = keyword;
        }
        break;
    }
    // §15.3.6
    case OS64_HTML_TAG_P: case OS64_HTML_TAG_H1: case OS64_HTML_TAG_H2:
    case OS64_HTML_TAG_H3: case OS64_HTML_TAG_H4: case OS64_HTML_TAG_H5:
    case OS64_HTML_TAG_H6:
        if (attr_is(n, "align", "left"))
            s->text_align = FLOW_ALIGN_LEFT;
        else if (attr_is(n, "align", "right"))
            s->text_align = FLOW_ALIGN_RIGHT;
        else if (attr_is(n, "align", "center"))
            s->text_align = FLOW_ALIGN_CENTER;
        else if (attr_is(n, "align", "justify"))
            s->text_align = FLOW_ALIGN_JUSTIFY;
        break;
    // §15.3.7 Lists. The letter types are case-SENSITIVE: `a` and `A` are
    // two different lists.
    case OS64_HTML_TAG_OL: case OS64_HTML_TAG_UL: case OS64_HTML_TAG_LI: {
        const char *type = attr(n, "type");
        if (type == NULL)
            break;
        if (n->tag != OS64_HTML_TAG_UL) {
            if (os64_streq(type, "1"))
                s->list_style_type = FLOW_LIST_DECIMAL;
            else if (os64_streq(type, "a"))
                s->list_style_type = FLOW_LIST_LOWER_ALPHA;
            else if (os64_streq(type, "A"))
                s->list_style_type = FLOW_LIST_UPPER_ALPHA;
            else if (os64_streq(type, "i"))
                s->list_style_type = FLOW_LIST_LOWER_ROMAN;
            else if (os64_streq(type, "I"))
                s->list_style_type = FLOW_LIST_UPPER_ROMAN;
        }
        if (n->tag != OS64_HTML_TAG_OL) {
            if (f_eq_nocase(type, "none"))
                s->list_style_type = FLOW_LIST_NONE;
            else if (f_eq_nocase(type, "disc"))
                s->list_style_type = FLOW_LIST_DISC;
            else if (f_eq_nocase(type, "circle"))
                s->list_style_type = FLOW_LIST_CIRCLE;
            else if (f_eq_nocase(type, "square"))
                s->list_style_type = FLOW_LIST_SQUARE;
        }
        break;
    }
    // §15.3.8 Tables.
    case OS64_HTML_TAG_TABLE: {
        if (attr_is(n, "align", "left"))
            s->float_side = FLOW_FLOAT_LEFT;
        else if (attr_is(n, "align", "right"))
            s->float_side = FLOW_FLOAT_RIGHT;
        else if (attr_is(n, "align", "center"))
            margin_inline(sp, kAuto);
        map_dimension(c, n, "width", true, &sp->width);
        map_dimension(c, n, "height", false, &sp->height);
        if (legacy_color(n, "bgcolor", &rgb))
            background(sp, rgb);
        if (legacy_color(n, "bordercolor", &rgb))
            border_color(sp, rgb);
        Len spacing;
        if (pixel_length(c, n, "cellspacing", &spacing))
            s->border_spacing[0] = s->border_spacing[1] = spacing.v;
        const char *rules = attr(n, "rules");
        static const char *const rule_values[] = {"none", "groups", "rows", "cols", "all"};
        for (int32_t i = 0; i < F_ARRAY(rule_values); i++)
            if (f_eq_nocase(rules, rule_values[i])) {
                for (int k = 0; k < 4; k++)
                    s->border_style[k] = FLOW_BORDER_HIDDEN;
                s->border_collapse = FLOW_COLLAPSE_BORDERS;
            }
        if (has(n, "border")) {
            int32_t width;
            if (!f_parse_nonnegative(attr(n, "border"), &width))
                width = 1;
            for (int k = 0; k < 4; k++)
                sp->border_px[k] = css(c, width);
            if (border_not_zero(n))
                for (int k = 0; k < 4; k++)
                    s->border_style[k] = FLOW_BORDER_OUTSET;
        }
        const char *frame = attr(n, "frame");
        if (frame != NULL) {
            // top right bottom left
            static const struct { const char *v; uint8_t on; } f[] = {
                {"void", 0x0}, {"above", 0x8}, {"below", 0x2}, {"hsides", 0xA},
                {"lhs", 0x1}, {"rhs", 0x4}, {"vsides", 0x5}, {"box", 0xF}, {"border", 0xF},
            };
            for (int32_t i = 0; i < F_ARRAY(f); i++)
                if (f_eq_nocase(frame, f[i].v)) {
                    static const uint8_t bit[4] = {0x8, 0x4, 0x2, 0x1};
                    for (int k = 0; k < 4; k++)
                        s->border_style[k] = (f[i].on & bit[k]) ? FLOW_BORDER_OUTSET
                                                                : FLOW_BORDER_HIDDEN;
                }
        }
        break;
    }
    case OS64_HTML_TAG_CAPTION:
        if (attr_is(n, "align", "bottom"))
            s->caption_side = FLOW_CAPTION_BOTTOM;
        break;
    // A column group's width is its columns' when they give none (and a
    // group with no `col` in it is its `span` of columns).
    case OS64_HTML_TAG_COL: case OS64_HTML_TAG_COLGROUP:
        map_dimension(c, n, "width", false, &sp->width);
        break;
    default:
        break;
    }

    if (is_table_part(n)) {
        if (attr_is(n, "align", "absmiddle"))
            s->text_align = FLOW_ALIGN_CENTER;
        else
            align_block(n, s);
        if (attr_is(n, "valign", "top"))
            s->vertical_align = FLOW_VALIGN_TOP;
        else if (attr_is(n, "valign", "middle"))
            s->vertical_align = FLOW_VALIGN_MIDDLE;
        else if (attr_is(n, "valign", "bottom"))
            s->vertical_align = FLOW_VALIGN_BOTTOM;
        else if (attr_is(n, "valign", "baseline"))
            s->vertical_align = FLOW_VALIGN_BASELINE;
        if (legacy_color(n, "bgcolor", &rgb))
            background(sp, rgb);
        if (is_row_group(n) || is(n, OS64_HTML_TAG_TR))
            map_dimension(c, n, "height", false, &sp->height);
    }
    if (is(n, OS64_HTML_TAG_TD) || is(n, OS64_HTML_TAG_TH)) {
        if (has(n, "nowrap"))
            s->white_space = FLOW_WS_NOWRAP;
        map_dimension(c, n, "width", true, &sp->width);
        map_dimension(c, n, "height", true, &sp->height);
        const os64_html_node_t *table = cell_table(n);
        if (table != NULL) {
            Len pad;
            if (pixel_length(c, table, "cellpadding", &pad))
                padding_all(sp, pad);
            if (has(table, "border") && border_not_zero(table))
                border(c, sp, FLOW_BORDER_INSET, 1);
            const char *rules = attr(table, "rules");
            if (f_eq_nocase(rules, "none") || f_eq_nocase(rules, "groups") ||
                f_eq_nocase(rules, "rows")) {
                border(c, sp, FLOW_BORDER_NONE, 1);
            } else if (f_eq_nocase(rules, "cols")) {
                border(c, sp, FLOW_BORDER_SOLID, 1);
                s->border_style[FLOW_TOP] = s->border_style[FLOW_BOTTOM] = FLOW_BORDER_NONE;
            } else if (f_eq_nocase(rules, "all")) {
                border(c, sp, FLOW_BORDER_SOLID, 1);
            }
        }
        // In quirks mode a nowrap cell with a width in PIXELS wraps after
        // all: the width was the page's way of saying how wide it is.
        int32_t w;
        if (c->quirks && has(n, "nowrap") &&
            f_parse_dimension(attr(n, "width"), true, &w) == F_DIM_PX)
            s->white_space = FLOW_WS_NORMAL;
    }
    if (is(n, OS64_HTML_TAG_TR) || is_row_group(n)) {
        const os64_html_node_t *table = is(n, OS64_HTML_TAG_TR) && is_row_group(n->parent)
                                            ? n->parent->parent : n->parent;
        if (is(table, OS64_HTML_TAG_TABLE)) {
            const char *rules = attr(table, "rules");
            bool rows_rule = f_eq_nocase(rules, "rows") && is(n, OS64_HTML_TAG_TR);
            bool groups_rule = f_eq_nocase(rules, "groups") && is_row_group(n);
            if (rows_rule || groups_rule) {
                sp->border_px[FLOW_TOP] = sp->border_px[FLOW_BOTTOM] = css(c, 1);
                s->border_style[FLOW_TOP] = s->border_style[FLOW_BOTTOM] = FLOW_BORDER_SOLID;
            }
        }
    }
    if (is(n, OS64_HTML_TAG_COLGROUP) && is(n->parent, OS64_HTML_TAG_TABLE) &&
        attr_is(n->parent, "rules", "groups")) {
        sp->border_px[FLOW_LEFT] = sp->border_px[FLOW_RIGHT] = css(c, 1);
        s->border_style[FLOW_LEFT] = s->border_style[FLOW_RIGHT] = FLOW_BORDER_SOLID;
    }

    // §15.3.11 The hr element.
    if (is(n, OS64_HTML_TAG_HR)) {
        if (attr_is(n, "align", "left")) {
            sp->margin[FLOW_LEFT] = px(c, 0);
            sp->margin[FLOW_RIGHT] = kAuto;
        } else if (attr_is(n, "align", "right")) {
            sp->margin[FLOW_LEFT] = kAuto;
            sp->margin[FLOW_RIGHT] = px(c, 0);
        } else if (attr_is(n, "align", "center")) {
            margin_inline(sp, kAuto);
        }
        bool solid = has(n, "color") || has(n, "noshade");
        if (solid)
            for (int k = 0; k < 4; k++)
                s->border_style[k] = FLOW_BORDER_SOLID;
        int32_t size;
        if (f_parse_nonnegative(attr(n, "size"), &size)) {
            if (solid) {
                for (int k = 0; k < 4; k++)
                    sp->border_px[k] = css(c, size / 2.0);
            } else if (size == 1) {
                sp->border_px[FLOW_BOTTOM] = 0;
            } else if (size > 1) {
                sp->height = px(c, size - 2);
            }
        }
        map_dimension(c, n, "width", false, &sp->width);
        if (legacy_color(n, "color", &rgb))
            s->color = rgb;
    }

    // §15.4.3 Attributes for embedded content and images.
    if (is_embedded_aligned(n)) {
        if (attr_is(n, "align", "left"))
            s->float_side = FLOW_FLOAT_LEFT;
        else if (attr_is(n, "align", "right"))
            s->float_side = FLOW_FLOAT_RIGHT;
        else if (attr_is(n, "align", "top"))
            s->vertical_align = FLOW_VALIGN_TOP;
        else if (attr_is(n, "align", "baseline"))
            s->vertical_align = FLOW_VALIGN_BASELINE;
        else if (attr_is(n, "align", "texttop"))
            s->vertical_align = FLOW_VALIGN_TEXT_TOP;
        else if (attr_is(n, "align", "absmiddle") || attr_is(n, "align", "abscenter"))
            s->vertical_align = FLOW_VALIGN_MIDDLE;
        else if (attr_is(n, "align", "bottom"))
            s->vertical_align = FLOW_VALIGN_BOTTOM;
        else if (attr_is(n, "align", "center") || attr_is(n, "align", "middle"))
            s->vertical_align = FLOW_VALIGN_HTML_MIDDLE;
        map_dimension(c, n, "width", false, &sp->width);
        map_dimension(c, n, "height", false, &sp->height);
    }
    if (is(n, OS64_HTML_TAG_EMBED) || is(n, OS64_HTML_TAG_IMG) || is(n, OS64_HTML_TAG_OBJECT) ||
        (is(n, OS64_HTML_TAG_INPUT) && attr_is(n, "type", "image"))) {
        Len v = {L_UNSET, 0, 0};
        map_dimension(c, n, "hspace", false, &v);
        if (v.kind != L_UNSET)
            margin_inline(sp, v);
        v = (Len){L_UNSET, 0, 0};
        map_dimension(c, n, "vspace", false, &v);
        if (v.kind != L_UNSET)
            margin_block(sp, v);
        if (!is(n, OS64_HTML_TAG_EMBED)) {
            int32_t b;
            if (f_parse_nonnegative(attr(n, "border"), &b) && b > 0)
                border(c, sp, FLOW_BORDER_SOLID, b);
        }
    }
    if (is(n, OS64_HTML_TAG_IFRAME)) {
        int32_t fb;
        if (has(n, "frameborder") && (!f_parse_integer(attr(n, "frameborder"), &fb) || fb == 0))
            for (int k = 0; k < 4; k++)
                sp->border_px[k] = 0;
    }
    if (is(n, OS64_HTML_TAG_EMBED) && has(n, "hidden")) {
        sp->width = px(c, 0);
        sp->height = px(c, 0);
    }

    // §15.3.4: a link's colour, where libpage says the node IS a link
    // (link_rule): `body link=` is a hint on the same selector as the
    // user-agent rule, so it wins over the sheet and loses to anything the
    // link's own descendants say.
    link_rule(c, n, sp, true);
    return true;
}

// §15.4.2: in quirks mode an image floated by its align attribute keeps
// 3px of margin from the text beside it. User-agent level: an `hspace`
// the page wrote outranks it.
static void image_quirk(const Ctx *c, const os64_html_node_t *n, Spec *sp)
{
    if (!c->quirks || !is(n, OS64_HTML_TAG_IMG) || has(n, "hspace"))
        return;
    if (attr_is(n, "align", "left"))
        sp->margin[FLOW_RIGHT] = px(c, 3);
    else if (attr_is(n, "align", "right"))
        sp->margin[FLOW_LEFT] = px(c, 3);
}

// §15.3.9 Margin collapsing quirks.
static void margin_quirks(const Ctx *c, const os64_html_node_t *n, Spec *sp)
{
    if (!c->quirks || !default_margins(n))
        return;
    const os64_html_node_t *p = n->parent;
    bool in_cell = is(p, OS64_HTML_TAG_TD) || is(p, OS64_HTML_TAG_TH);
    bool first = !substantial_before(n);
    if ((in_cell || is(p, OS64_HTML_TAG_BODY)) && first) {
        sp->margin[FLOW_TOP] = px(c, 0);
        if (blank(n))
            sp->margin[FLOW_BOTTOM] = px(c, 0);
    }
    if (in_cell && !substantial_after(n) && blank(n))
        sp->margin[FLOW_TOP] = px(c, 0);
    if (in_cell && is(n, OS64_HTML_TAG_P) && !substantial_after(n))
        sp->margin[FLOW_BOTTOM] = px(c, 0);
}

// ── The page's own sheets ───────────────────────────────────────────────
//
// libgarb's cascade hands each element its AUTHOR-origin winners: declared
// values, checked against their grammars, var() already replaced. Here
// they are computed into the Spec the chapter and the hints wrote, which
// they outrank. A property flow_style_t has no field for waits for the
// layout that needs it (GARB.md § Booked); a value its grammar allows and
// libflow cannot draw takes the nearest value libflow has, named where the
// value is read.

typedef struct {
    Ctx *c;
    const flow_style_t *parent;
    flow_unit_t font;           // the element's own font size, once it is known
    garb_env_t view;
    int32_t sheet;              // the winner being read: the sheet it came from
} Author;

static bool word(const garb_val_t *v, const char *w)
{
    return v->kind == GARB_V_KEYWORD && os64_streq(v->keyword, w);
}

// A length in 26.6 px, `font` being the em it is relative to — the
// element's own, or its parent's for font-size (Values 4 § 6). A font is
// already in device pixels; every other unit is CSS pixels, zoomed here.
static double length_px(const Author *a, const garb_val_t *v, double font)
{
    double n = v->number, css;
    switch (v->unit) {
    case GARB_U_EM: return n * font;
    case GARB_U_REM: return n * a->c->root_font;
    // No face metrics reach the style pass: an ex and a ch are taken as
    // half an em, the fallback Values 4 gives for both.
    case GARB_U_EX: case GARB_U_CH: return n * font / 2;
    case GARB_U_PX: css = n; break;
    case GARB_U_VW: css = n * a->view.width / 100; break;
    case GARB_U_VH: css = n * a->view.height / 100; break;
    case GARB_U_VMIN:
        css = n * (a->view.width < a->view.height ? a->view.width : a->view.height) / 100;
        break;
    case GARB_U_VMAX:
        css = n * (a->view.width > a->view.height ? a->view.width : a->view.height) / 100;
        break;
    case GARB_U_PT: css = n * 96 / 72; break;
    case GARB_U_PC: css = n * 16; break;
    case GARB_U_IN: css = n * 96; break;
    case GARB_U_CM: css = n * 96 / 2.54; break;
    case GARB_U_MM: css = n * 96 / 25.4; break;
    case GARB_U_Q: css = n * 96 / 101.6; break;
    default: return 0;              // a track's share (fr), never a length
    }
    return css * FLOW_UNITS_PER_PX * a->c->zoom;
}

// A calc() as a fixed part and a percentage part — what libflow's lengths
// carry — or a plain number.
typedef struct {
    double px, pct;             // px in 26.6
    bool number;
} Lp;

static bool calc(const Author *a, const garb_calc_t *k, double font, Lp *out)
{
    *out = (Lp){0, 0, false};
    switch (k->op) {
    case GARB_CALC_LEAF:
        if (k->leaf.kind == GARB_V_NUMBER)
            *out = (Lp){k->leaf.number, 0, true};
        else if (k->leaf.kind == GARB_V_PERCENTAGE)
            out->pct = k->leaf.number;
        else if (k->leaf.kind == GARB_V_LENGTH)
            out->px = length_px(a, &k->leaf, font);
        else
            return false;
        return true;
    case GARB_CALC_SUM:
        for (int32_t i = 0; i < k->nargs; i++) {
            Lp t;
            if (!calc(a, k->args[i], font, &t))
                return false;
            double sign = k->invert != NULL && k->invert[i] ? -1 : 1;
            out->px += sign * t.px;
            out->pct += sign * t.pct;
            out->number = t.number;
        }
        return true;
    case GARB_CALC_PRODUCT:
        *out = (Lp){1, 0, true};
        for (int32_t i = 0; i < k->nargs; i++) {
            Lp t;
            if (!calc(a, k->args[i], font, &t))
                return false;
            bool divide = k->invert != NULL && k->invert[i];
            // The grammar let at most one factor be a length, and no divisor.
            double f = t.number ? t.px : 0;
            if (divide && (!t.number || f == 0))
                return false;
            if (t.number) {
                double m = divide ? 1 / f : f;
                out->px *= m;
                out->pct *= m;
            } else if (out->number) {
                *out = (Lp){t.px * out->px, t.pct * out->px, false};
            } else {
                return false;
            }
        }
        return true;
    case GARB_CALC_MIN: case GARB_CALC_MAX: case GARB_CALC_CLAMP: {
        // Comparing needs one unit: a percentage against a length can only
        // be settled at layout, which flow_length_t cannot carry.
        // clamp(MIN, VAL, MAX) is max(MIN, min(VAL, MAX)), so its arguments
        // are taken VAL, MAX, MIN: a min of the first two, then a max.
        static const int32_t clamp_order[3] = {1, 2, 0};
        if (k->op == GARB_CALC_CLAMP && k->nargs != 3)
            return false;
        Lp best = {0, 0, false};
        for (int32_t i = 0; i < k->nargs; i++) {
            Lp t;
            int32_t at = k->op == GARB_CALC_CLAMP ? clamp_order[i] : i;
            if (!calc(a, k->args[at], font, &t))
                return false;
            if (t.pct != 0 && t.px != 0)
                return false;
            if (i == 0) {
                best = t;
                continue;
            }
            if ((best.pct != 0) != (t.pct != 0) && (best.px != 0 || t.px != 0))
                return false;
            double bv = best.px + best.pct, tv = t.px + t.pct;
            bool smaller = k->op == GARB_CALC_MIN || (k->op == GARB_CALC_CLAMP && i == 1);
            if (smaller ? tv < bv : tv > bv)
                best = t;
        }
        *out = best;
        return true;
    }
    }
    return false;
}

static int32_t round_i32(double v)
{
    if (v > 2e9)
        return 2000000000;
    if (v < -2e9)
        return -2000000000;
    return (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
}

// A length-percentage (or `auto`, where `allow_auto`) into a Len; false
// for anything else, which leaves the property as it was.
static bool author_len(const Author *a, const garb_val_t *v, bool allow_auto, Len *out)
{
    switch (v->kind) {
    case GARB_V_KEYWORD:
        if (allow_auto && word(v, "auto")) {
            *out = kAuto;
            return true;
        }
        return false;
    case GARB_V_LENGTH:
        *out = (Len){L_PX, round_i32(length_px(a, v, a->font)), 0};
        return true;
    case GARB_V_PERCENTAGE:
        *out = (Len){L_PCT, round_i32(v->number * 64), 0};
        return true;
    case GARB_V_CALC: {
        Lp r;
        if (v->calc == NULL || !calc(a, v->calc, a->font, &r) || r.number)
            return false;
        if (r.pct == 0)
            *out = (Len){L_PX, round_i32(r.px), 0};
        else
            *out = (Len){L_PCT, round_i32(r.pct * 64), round_i32(r.px)};
        return true;
    }
    default:
        return false;
    }
}

static int32_t channel(double v)
{
    return v <= 0 ? 0 : v >= 255 ? 255 : (int32_t)(v + 0.5);
}

// sRGB with alpha, as a flow colour (flow.h): its alpha kept, as how
// transparent it is, for the face to blend over whatever is under it.
static uint32_t colour_of(garb_color_t k)
{
    double al = k.a < 0 ? 0 : k.a > 1 ? 1 : k.a;
    return (uint32_t)(255 - channel(al * 255)) << 24 | (uint32_t)channel(k.r) << 16 |
           (uint32_t)channel(k.g) << 8 | (uint32_t)channel(k.b);
}

// One keyword of a table, by index; -1 when `v` is none of them.
static int32_t pick(const garb_val_t *v, const char *const *words, int32_t n)
{
    if (v->kind != GARB_V_KEYWORD)
        return -1;
    for (int32_t i = 0; i < n; i++)
        if (os64_streq(v->keyword, words[i]))
            return i;
    return -1;
}

static bool author_display(const garb_val_t *v, flow_display_t *out)
{
    static const char *const words[] = {
        "none", "block", "inline", "inline-block", "list-item", "table", "inline-table",
        "table-row-group", "table-header-group", "table-footer-group", "table-row",
        "table-column-group", "table-column", "table-cell", "table-caption", "contents",
        "flow-root", "flex", "inline-flex", "grid", "inline-grid",
    };
    // An inline table is laid out as a table (GARB.md § Booked).
    static const flow_display_t as[] = {
        FLOW_DISPLAY_NONE, FLOW_DISPLAY_BLOCK, FLOW_DISPLAY_INLINE, FLOW_DISPLAY_INLINE_BLOCK,
        FLOW_DISPLAY_LIST_ITEM, FLOW_DISPLAY_TABLE, FLOW_DISPLAY_TABLE,
        FLOW_DISPLAY_TABLE_ROW_GROUP, FLOW_DISPLAY_TABLE_HEADER_GROUP,
        FLOW_DISPLAY_TABLE_FOOTER_GROUP, FLOW_DISPLAY_TABLE_ROW,
        FLOW_DISPLAY_TABLE_COLUMN_GROUP, FLOW_DISPLAY_TABLE_COLUMN, FLOW_DISPLAY_TABLE_CELL,
        FLOW_DISPLAY_TABLE_CAPTION, FLOW_DISPLAY_CONTENTS, FLOW_DISPLAY_BLOCK,
        FLOW_DISPLAY_FLEX, FLOW_DISPLAY_INLINE_FLEX, FLOW_DISPLAY_GRID,
        FLOW_DISPLAY_INLINE_GRID,
    };
    int32_t i = pick(v, words, F_ARRAY(words));
    if (i < 0)
        return false;
    *out = as[i];
    return true;
}

static bool author_border_style(const garb_val_t *v, flow_border_style_t *out)
{
    static const char *const words[] = {"none", "hidden", "dotted", "dashed", "solid",
                                        "double", "groove", "ridge", "inset", "outset"};
    static const flow_border_style_t as[] = {
        FLOW_BORDER_NONE,   FLOW_BORDER_HIDDEN, FLOW_BORDER_DOTTED, FLOW_BORDER_DASHED,
        FLOW_BORDER_SOLID,  FLOW_BORDER_DOUBLE, FLOW_BORDER_GROOVE, FLOW_BORDER_RIDGE,
        FLOW_BORDER_INSET,  FLOW_BORDER_OUTSET,
    };
    int32_t i = pick(v, words, F_ARRAY(words));
    if (i < 0)
        return false;
    *out = as[i];
    return true;
}

// `thin`, `medium`, `thick`: 1, 3 and 5px (Backgrounds 3 § 4.3).
static bool author_border_width(const Author *a, const garb_val_t *v, int32_t *out)
{
    static const char *const words[] = {"thin", "medium", "thick"};
    int32_t i = pick(v, words, F_ARRAY(words));
    if (i >= 0) {
        *out = css(a->c, 1 + 2 * i);
        return true;
    }
    Len l;
    if (!author_len(a, v, false, &l) || l.kind != L_PX)
        return false;
    *out = l.v < 0 ? 0 : l.v;
    return true;
}

// A generic family by its keyword. The families this machine has no face
// for fall to the nearest of the three it has.
static bool author_generic(const char *k, flow_generic_t *out)
{
    static const struct { const char *name; flow_generic_t g; } map[] = {
        {"serif", FLOW_GENERIC_SERIF}, {"sans-serif", FLOW_GENERIC_SANS},
        {"monospace", FLOW_GENERIC_MONO}, {"cursive", FLOW_GENERIC_SERIF},
        {"fantasy", FLOW_GENERIC_SERIF}, {"system-ui", FLOW_GENERIC_SANS},
        {"ui-serif", FLOW_GENERIC_SERIF}, {"ui-sans-serif", FLOW_GENERIC_SANS},
        {"ui-monospace", FLOW_GENERIC_MONO}, {"ui-rounded", FLOW_GENERIC_SANS},
        {"math", FLOW_GENERIC_SERIF}, {"emoji", FLOW_GENERIC_SANS},
        {"fangsong", FLOW_GENERIC_SERIF},
    };
    for (int32_t i = 0; i < F_ARRAY(map); i++)
        if (os64_streq(k, map[i].name)) {
            *out = map[i].g;
            return true;
        }
    return false;
}

// `font-family`: the names in order and the first generic, as `<font
// face>` builds them. The names point into the sheet.
// The names are copied into the styles arena for every element the
// declaration reaches: a `* { font-family: … }` reset costs one list an
// element (48 bytes for three names). Bounded by the document, as the
// styles arena is; one list per winning set would cost it once.
static bool author_family(Author *a, const garb_val_t *v, flow_family_list_t *out)
{
    if (v->items == NULL || v->nitems <= 0)
        return true;
    flow_family_name_t *names = f_arena_alloc(&a->c->out->arena,
                                              (size_t)v->nitems * sizeof(*names));
    if (names == NULL)
        return false;
    uint32_t n = 0;
    bool have_generic = false;
    flow_generic_t generic = FLOW_GENERIC_SANS;
    for (int32_t i = 0; i < v->nitems; i++) {
        const garb_val_t *it = &v->items[i];
        flow_generic_t g;
        if (it->kind == GARB_V_KEYWORD && author_generic(it->keyword, &g)) {
            if (!have_generic) {
                generic = g;
                have_generic = true;
            }
        } else if (it->kind == GARB_V_STRING && it->len > 0) {
            names[n++] = (flow_family_name_t){it->text, (uint32_t)it->len};
        }
    }
    if (!have_generic && n > 0)
        generic = guess_generic(names[0].name, names[0].len);
    *out = (flow_family_list_t){n > 0 ? names : NULL, n, generic};
    return true;
}

// `font-size`: its em and its percentage are the PARENT's font (Fonts 4 §
// 2.5), and so are a calc()'s.
static void author_font_size(Author *a, Spec *sp, const garb_val_t *v)
{
    static const char *const words[] = {"xx-small", "x-small", "small", "medium",
                                        "large", "x-large", "xx-large", "xxx-large"};
    double base = a->parent != NULL ? a->parent->font_size
                                    : (double)css(a->c, a->c->env->viewport_font_px);
    int32_t i = pick(v, words, F_ARRAY(words));
    if (i >= 0) {
        sp->fs_kind = FS_KEYWORD;
        sp->fs_v = i;
    } else if (word(v, "larger")) {
        sp->fs_kind = FS_LARGER;
    } else if (word(v, "smaller")) {
        sp->fs_kind = FS_SMALLER;
    } else if (v->kind == GARB_V_PERCENTAGE) {
        sp->fs_kind = FS_EM;
        sp->fs_v = round_i32(v->number * 10);
    } else if (v->kind == GARB_V_LENGTH) {
        double px = length_px(a, v, base);
        sp->fs_kind = FS_PX;
        sp->fs_v = px < 0 ? 0 : round_i32(px);
    } else if (v->kind == GARB_V_CALC) {
        Lp r;
        if (v->calc == NULL || !calc(a, v->calc, base, &r) || r.number)
            return;
        double px = r.px + base * r.pct / 100;
        sp->fs_kind = FS_PX;
        sp->fs_v = px < 0 ? 0 : round_i32(px);
    }
}

static void author_font_weight(Author *a, Spec *sp, const garb_val_t *v)
{
    uint16_t up = a->parent != NULL ? a->parent->font_weight : 400;
    sp->bolder = false;
    if (word(v, "normal"))
        sp->s.font_weight = 400;
    else if (word(v, "bold"))
        sp->s.font_weight = 700;
    else if (word(v, "bolder"))
        sp->s.font_weight = bolder(up);
    else if (word(v, "lighter"))    // Fonts 4 § 2.2's table
        sp->s.font_weight = up < 100 ? up : up < 550 ? 100 : up < 750 ? 400 : 700;
    else if (v->kind == GARB_V_NUMBER)
        sp->s.font_weight = (uint16_t)(v->number < 1 ? 1 : v->number > 1000 ? 1000
                                                             : v->number + 0.5);
}

static void author_list_type(Spec *sp, const garb_val_t *v)
{
    static const char *const words[] = {
        "disc", "circle", "square", "decimal", "lower-alpha", "upper-alpha", "lower-roman",
        "upper-roman", "disclosure-closed", "disclosure-open", "none", "lower-latin",
        "upper-latin", "decimal-leading-zero",
    };
    static const flow_list_style_type_t as[] = {
        FLOW_LIST_DISC, FLOW_LIST_CIRCLE, FLOW_LIST_SQUARE, FLOW_LIST_DECIMAL,
        FLOW_LIST_LOWER_ALPHA, FLOW_LIST_UPPER_ALPHA, FLOW_LIST_LOWER_ROMAN,
        FLOW_LIST_UPPER_ROMAN, FLOW_LIST_DISCLOSURE_CLOSED, FLOW_LIST_DISCLOSURE_OPEN,
        FLOW_LIST_NONE, FLOW_LIST_LOWER_ALPHA, FLOW_LIST_UPPER_ALPHA, FLOW_LIST_DECIMAL,
    };
    int32_t i = pick(v, words, F_ARRAY(words));
    if (i >= 0)
        sp->s.list_style_type = as[i];
    else if (v->kind == GARB_V_KEYWORD)
        // A counter style this browser does not define is decimal (Counter
        // Styles 3 § 2); a string marker is booked with the markers.
        sp->s.list_style_type = FLOW_LIST_DECIMAL;
}

// The side a longhand names, in libgarb's top-right-bottom-left order.
static int side(garb_prop_t p, garb_prop_t top)
{
    return (int)(p - top);
}

// Copies the fields `prop` sets from one Spec to another: how a CSS-wide
// keyword takes a value from the parent, the initial values, or the
// user-agent origin.
static void take(Spec *dst, const Spec *src, garb_prop_t prop)
{
    flow_style_t *d = &dst->s;
    const flow_style_t *s = &src->s;
    switch (prop) {
    case GARB_DISPLAY: d->display = s->display; break;
    case GARB_COLOR: d->color = s->color; break;
    case GARB_BACKGROUND_COLOR:
        d->has_background = s->has_background;
        d->background = s->background;
        d->current_colours = (uint8_t)((d->current_colours & ~FLOW_CURRENT_BACKGROUND) |
                                       (s->current_colours & FLOW_CURRENT_BACKGROUND));
        break;
    // A list is the style's, kept as long as any style is, so it is shared
    // and not copied.
    case GARB_BACKGROUND_IMAGE:
        d->backgrounds.image = s->backgrounds.image;
        d->backgrounds.nimage = s->backgrounds.nimage;
        break;
    case GARB_BACKGROUND_REPEAT:
        d->backgrounds.repeat = s->backgrounds.repeat;
        d->backgrounds.nrepeat = s->backgrounds.nrepeat;
        break;
    case GARB_BACKGROUND_POSITION_X:
        d->backgrounds.x = s->backgrounds.x;
        d->backgrounds.nx = s->backgrounds.nx;
        break;
    case GARB_BACKGROUND_POSITION_Y:
        d->backgrounds.y = s->backgrounds.y;
        d->backgrounds.ny = s->backgrounds.ny;
        break;
    case GARB_BACKGROUND_SIZE:
        d->backgrounds.size = s->backgrounds.size;
        d->backgrounds.nsize = s->backgrounds.nsize;
        break;
    case GARB_BACKGROUND_ORIGIN:
        d->backgrounds.origin = s->backgrounds.origin;
        d->backgrounds.norigin = s->backgrounds.norigin;
        break;
    case GARB_BACKGROUND_CLIP:
        d->backgrounds.clip = s->backgrounds.clip;
        d->backgrounds.nclip = s->backgrounds.nclip;
        break;
    case GARB_MARGIN_TOP: case GARB_MARGIN_RIGHT: case GARB_MARGIN_BOTTOM: case GARB_MARGIN_LEFT:
        dst->margin[side(prop, GARB_MARGIN_TOP)] = src->margin[side(prop, GARB_MARGIN_TOP)];
        break;
    case GARB_PADDING_TOP: case GARB_PADDING_RIGHT: case GARB_PADDING_BOTTOM:
    case GARB_PADDING_LEFT:
        dst->padding[side(prop, GARB_PADDING_TOP)] = src->padding[side(prop, GARB_PADDING_TOP)];
        break;
    case GARB_BORDER_TOP_WIDTH: case GARB_BORDER_RIGHT_WIDTH: case GARB_BORDER_BOTTOM_WIDTH:
    case GARB_BORDER_LEFT_WIDTH: {
        int i = side(prop, GARB_BORDER_TOP_WIDTH);
        dst->border_px[i] = src->border_px[i];
        break;
    }
    case GARB_BORDER_TOP_STYLE: case GARB_BORDER_RIGHT_STYLE: case GARB_BORDER_BOTTOM_STYLE:
    case GARB_BORDER_LEFT_STYLE: {
        int i = side(prop, GARB_BORDER_TOP_STYLE);
        d->border_style[i] = s->border_style[i];
        break;
    }
    case GARB_BORDER_TOP_COLOR: case GARB_BORDER_RIGHT_COLOR: case GARB_BORDER_BOTTOM_COLOR:
    case GARB_BORDER_LEFT_COLOR: {
        int i = side(prop, GARB_BORDER_TOP_COLOR);
        dst->border_color_set[i] = src->border_color_set[i];
        d->border_color[i] = s->border_color[i];
        break;
    }
    case GARB_WIDTH: dst->width = src->width; break;
    case GARB_HEIGHT: dst->height = src->height; break;
    case GARB_MIN_WIDTH: dst->min_width = src->min_width; break;
    case GARB_MAX_WIDTH: dst->max_width = src->max_width; break;
    case GARB_MIN_HEIGHT: dst->min_height = src->min_height; break;
    case GARB_MAX_HEIGHT: dst->max_height = src->max_height; break;
    case GARB_BOX_SIZING: d->box_sizing = s->box_sizing; break;
    case GARB_OVERFLOW_X: d->overflow_x = s->overflow_x; break;
    case GARB_OVERFLOW_Y: d->overflow_y = s->overflow_y; break;
    case GARB_FONT_FAMILY: d->family = s->family; break;
    case GARB_FONT_SIZE:
        dst->fs_kind = src->fs_kind;
        dst->fs_v = src->fs_v;
        d->font_size = s->font_size;
        break;
    case GARB_FONT_WEIGHT:
        d->font_weight = s->font_weight;
        dst->bolder = src->bolder;
        break;
    case GARB_FONT_STYLE: d->font_style = s->font_style; break;
    case GARB_TEXT_ALIGN: d->text_align = s->text_align; break;
    case GARB_VERTICAL_ALIGN: d->vertical_align = s->vertical_align; break;
    case GARB_WHITE_SPACE: d->white_space = s->white_space; break;
    case GARB_LINE_HEIGHT: d->line_height = s->line_height; break;
    case GARB_TEXT_INDENT:
        dst->text_indent = src->text_indent;
        d->text_indent = s->text_indent;
        break;
    case GARB_TEXT_TRANSFORM: d->text_transform = s->text_transform; break;
    case GARB_TEXT_DECORATION_LINE: d->text_decoration = s->text_decoration; break;
    case GARB_VISIBILITY: d->visibility = s->visibility; break;
    case GARB_LIST_STYLE_TYPE: d->list_style_type = s->list_style_type; break;
    case GARB_LIST_STYLE_POSITION: d->list_style_position = s->list_style_position; break;
    case GARB_BORDER_SPACING:
        d->border_spacing[0] = s->border_spacing[0];
        d->border_spacing[1] = s->border_spacing[1];
        break;
    case GARB_BORDER_COLLAPSE: d->border_collapse = s->border_collapse; break;
    case GARB_CAPTION_SIDE: d->caption_side = s->caption_side; break;
    case GARB_FLOAT: d->float_side = s->float_side; break;
    case GARB_CLEAR: d->clear = s->clear; break;
    case GARB_POSITION: d->position = s->position; break;
    case GARB_TOP: case GARB_RIGHT: case GARB_BOTTOM: case GARB_LEFT:
        dst->inset[side(prop, GARB_TOP)] = src->inset[side(prop, GARB_TOP)];
        break;
    case GARB_Z_INDEX:
        d->has_z_index = s->has_z_index;
        d->z_index = s->z_index;
        break;
    case GARB_OPACITY: d->opacity = s->opacity; break;
    case GARB_POINTER_EVENTS: d->pointer_events_none = s->pointer_events_none; break;
    case GARB_FLEX_DIRECTION: d->flex_direction = s->flex_direction; break;
    case GARB_FLEX_WRAP: d->flex_wrap = s->flex_wrap; break;
    case GARB_JUSTIFY_CONTENT: d->justify_content = s->justify_content; break;
    case GARB_ALIGN_ITEMS: d->align_items = s->align_items; break;
    case GARB_ALIGN_SELF: d->align_self = s->align_self; break;
    case GARB_ALIGN_CONTENT: d->align_content = s->align_content; break;
    case GARB_FLEX_GROW: d->flex_grow = s->flex_grow; break;
    case GARB_FLEX_SHRINK: d->flex_shrink = s->flex_shrink; break;
    case GARB_FLEX_BASIS: dst->flex_basis = src->flex_basis; break;
    case GARB_ORDER: d->order = s->order; break;
    case GARB_ROW_GAP: dst->row_gap = src->row_gap; break;
    case GARB_COLUMN_GAP: dst->column_gap = src->column_gap; break;
    case GARB_GRID_TEMPLATE_COLUMNS: d->grid_template_columns = s->grid_template_columns; break;
    case GARB_GRID_TEMPLATE_ROWS: d->grid_template_rows = s->grid_template_rows; break;
    case GARB_GRID_AUTO_COLUMNS: d->grid_auto_columns = s->grid_auto_columns; break;
    case GARB_GRID_AUTO_ROWS: d->grid_auto_rows = s->grid_auto_rows; break;
    case GARB_GRID_TEMPLATE_AREAS:
        d->grid_areas = s->grid_areas;
        d->ngrid_areas = s->ngrid_areas;
        d->grid_area_rows = s->grid_area_rows;
        d->grid_area_cols = s->grid_area_cols;
        break;
    case GARB_GRID_AUTO_FLOW:
        d->grid_auto_flow_column = s->grid_auto_flow_column;
        d->grid_dense = s->grid_dense;
        break;
    case GARB_GRID_ROW_START: d->grid_row_start = s->grid_row_start; break;
    case GARB_GRID_ROW_END: d->grid_row_end = s->grid_row_end; break;
    case GARB_GRID_COLUMN_START: d->grid_column_start = s->grid_column_start; break;
    case GARB_GRID_COLUMN_END: d->grid_column_end = s->grid_column_end; break;
    case GARB_JUSTIFY_ITEMS: d->justify_items = s->justify_items; break;
    case GARB_JUSTIFY_SELF: d->justify_self = s->justify_self; break;
    case GARB_BORDER_TOP_LEFT_RADIUS: case GARB_BORDER_TOP_RIGHT_RADIUS:
    case GARB_BORDER_BOTTOM_RIGHT_RADIUS: case GARB_BORDER_BOTTOM_LEFT_RADIUS: {
        int i = prop - GARB_BORDER_TOP_LEFT_RADIUS;
        dst->radius[i][0] = src->radius[i][0];
        dst->radius[i][1] = src->radius[i][1];
        break;
    }
    case GARB_BOX_SHADOW:
        d->box_shadows = s->box_shadows;
        d->nbox_shadows = s->nbox_shadows;
        break;
    case GARB_TEXT_SHADOW:
        d->text_shadows = s->text_shadows;
        d->ntext_shadows = s->ntext_shadows;
        break;
    case GARB_IMAGE_RENDERING: d->pixelated = s->pixelated; break;
    default: break;
    }
}

static Len len_of(flow_length_t l)
{
    switch (l.kind) {
    case FLOW_LENGTH_PX: return (Len){L_PX, l.value, 0};
    case FLOW_LENGTH_PERCENT: return (Len){L_PCT, l.value, l.offset};
    case FLOW_LENGTH_FIT_CONTENT: return kFitContent;
    case FLOW_LENGTH_CONTENT: return kContent;
    case FLOW_LENGTH_AUTO: break;
    }
    return kAuto;
}

// What `initial` gives every property, as a Spec.
static void initial_spec(const Ctx *c, Spec *out)
{
    os64_memset(out, 0, sizeof(*out));
    out->s = initial(c);
    for (int i = 0; i < 4; i++) {
        out->margin[i] = px(c, 0);
        out->padding[i] = px(c, 0);
        out->inset[i] = kAuto;
        out->border_px[i] = css(c, 3);     // medium
    }
    out->width = out->height = kAuto;
    out->min_width = out->max_width = out->min_height = out->max_height = kAuto;
    out->flex_basis = out->row_gap = out->column_gap = kAuto;
    out->text_indent = px(c, 0);
    out->fs_kind = FS_KEYWORD;
    out->fs_v = 3;                                      // medium
}

// What `inherit` gives every property: the parent's computed values.
static void inherited_spec(const Ctx *c, const flow_style_t *parent, Spec *out)
{
    if (parent == NULL) {
        initial_spec(c, out);
        return;
    }
    os64_memset(out, 0, sizeof(*out));
    out->s = *parent;
    for (int i = 0; i < 4; i++) {
        out->margin[i] = len_of(parent->margin[i]);
        out->padding[i] = len_of(parent->padding[i]);
        out->inset[i] = len_of(parent->inset[i]);
        out->border_px[i] = parent->border_width[i];
        // currentColor stays currentColor: finish resolves it against the
        // child's own colour.
        out->border_color_set[i] = !(parent->current_colours & FLOW_CURRENT_BORDER(i));
        out->radius[i][0] = len_of(parent->radius[i][0]);
        out->radius[i][1] = len_of(parent->radius[i][1]);
    }
    out->width = len_of(parent->width);
    out->height = len_of(parent->height);
    out->min_width = len_of(parent->min_width);
    out->max_width = len_of(parent->max_width);
    out->min_height = len_of(parent->min_height);
    out->max_height = len_of(parent->max_height);
    out->flex_basis = len_of(parent->flex_basis);
    out->row_gap = len_of(parent->row_gap);
    out->column_gap = len_of(parent->column_gap);
    out->text_indent = len_of(parent->text_indent);
    out->fs_kind = FS_PX;
    out->fs_v = parent->font_size;
}

// ── Grid values (GRID.md § Pass 1) ──────────────────────────────────────

// A track's sizing function at one end: a flexible share, a keyword, or a
// length-percentage, resolved here as any other length is.
static bool author_breadth(const Author *a, const garb_val_t *v, flow_breadth_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    if (v->kind == GARB_V_LENGTH && v->unit == GARB_U_FR) {
        out->kind = FLOW_TRACK_FR;
        out->fr = round_i32((v->number > 1e6 ? 1e6 : v->number) * 1000);
        return true;
    }
    if (v->kind == GARB_V_KEYWORD) {
        out->kind = word(v, "min-content")   ? FLOW_TRACK_MIN_CONTENT
                    : word(v, "max-content") ? FLOW_TRACK_MAX_CONTENT
                                             : FLOW_TRACK_AUTO;
        return true;
    }
    Len l;
    if (!author_len(a, v, false, &l))
        return false;
    out->kind = FLOW_TRACK_FIXED;
    out->len = resolve(l, a->font, (flow_length_t){FLOW_LENGTH_PX, 0, 0});
    return true;
}

// A track size (§ 7.2.1): minmax(min, max), fit-content(len), or one
// breadth that is both ends — except `<n>fr`, whose minimum is `auto`.
static bool author_track(const Author *a, const garb_val_t *v, flow_track_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    if (v->kind == GARB_V_FUNCTION && os64_streq(v->keyword, "minmax") && v->nitems == 2)
        return author_breadth(a, &v->items[0], &out->min) &&
               author_breadth(a, &v->items[1], &out->max);
    if (v->kind == GARB_V_FUNCTION && os64_streq(v->keyword, "fit-content") && v->nitems == 1) {
        out->min.kind = FLOW_TRACK_AUTO;
        out->fit = true;
        return author_breadth(a, &v->items[0], &out->max);
    }
    if (!author_breadth(a, v, &out->max))
        return false;
    out->min = out->max;
    if (out->min.kind == FLOW_TRACK_FR)
        out->min = (flow_breadth_t){FLOW_TRACK_AUTO, {FLOW_LENGTH_AUTO, 0, 0}, 0};
    return true;
}

// How many times a repeat() repeats: its count, held to the bound before
// it is made an integer (a count may be any size the page writes); an
// auto-repeat's tracks once.
static int64_t repeat_times(const garb_val_t *count)
{
    if (count->kind != GARB_V_NUMBER)
        return 1;
    return count->number > F_GRID_MAX ? F_GRID_MAX : (int64_t)count->number;
}

// What reading a grid value came to: a value to keep, one to leave the
// property as it was for, or no memory — which fails the element, as any
// allocation of the style pass does.
typedef enum { GRID_READ_OK, GRID_READ_UNUSABLE, GRID_READ_NO_MEMORY } GridRead;

// A track list with each repeat(n) written out, into the styles arena, and
// held to F_GRID_MAX tracks; an auto-repeat's tracks are kept once and
// marked (flow_tracks_t).
static GridRead author_tracks(Author *a, const garb_val_t *v, flow_tracks_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    if (v->kind == GARB_V_KEYWORD)
        return word(v, "none") || word(v, "auto") ? GRID_READ_OK : GRID_READ_UNUSABLE;
    // A single size (grid-auto-*) is a list of one.
    const garb_val_t *items = v->kind == GARB_V_TRACKS ? v->items : v;
    int32_t n = v->kind == GARB_V_TRACKS ? v->nitems : 1;
    int64_t total = 0;
    for (int32_t i = 0; i < n; i++) {
        const garb_val_t *t = &items[i];
        if (t->kind == GARB_V_FUNCTION && os64_streq(t->keyword, "repeat") && t->nitems == 2)
            total += repeat_times(&t->items[0]) * t->items[1].nitems;
        else
            total++;
    }
    if (total > F_GRID_MAX)
        total = F_GRID_MAX;
    flow_track_t *tracks = f_arena_alloc(&a->c->out->arena, (size_t)total * sizeof(*tracks));
    if (tracks == NULL)
        return GRID_READ_NO_MEMORY;
    int32_t k = 0;
    for (int32_t i = 0; i < n && k < total; i++) {
        const garb_val_t *t = &items[i];
        if (!(t->kind == GARB_V_FUNCTION && os64_streq(t->keyword, "repeat") && t->nitems == 2)) {
            if (!author_track(a, t, &tracks[k++]))
                return GRID_READ_UNUSABLE;
            continue;
        }
        const garb_val_t *inner = &t->items[1];
        bool automatic = t->items[0].kind == GARB_V_KEYWORD;
        int64_t times = repeat_times(&t->items[0]);
        if (automatic) {
            out->repeat_at = k;
            out->repeat_n = inner->nitems;
            out->repeat_fit = word(&t->items[0], "auto-fit");
        }
        for (int64_t r = 0; r < times && k < total; r++)
            for (int32_t j = 0; j < inner->nitems && k < total; j++)
                if (!author_track(a, &inner->items[j], &tracks[k++]))
                    return GRID_READ_UNUSABLE;
    }
    if (out->repeat_n > 0 && out->repeat_at + out->repeat_n > k)
        out->repeat_n = 0;      // held to the bound before it was met
    out->tracks = tracks;
    out->n = k;
    return GRID_READ_OK;
}

// grid-template-areas' strings into rectangles, one per name (the grammar
// checked that each name is one).
static GridRead author_areas(Author *a, const garb_val_t *v, flow_style_t *s)
{
    s->grid_areas = NULL;
    s->ngrid_areas = s->grid_area_rows = s->grid_area_cols = 0;
    if (v->kind != GARB_V_STRING || v->nitems <= 0)
        return word(v, "none") ? GRID_READ_OK : GRID_READ_UNUSABLE;
    int32_t rows = v->nitems;
    int32_t cols = garb_area_cells(v->items[0].text, (uint32_t)v->items[0].len, NULL, 0);
    if (cols <= 0 || (int64_t)rows * cols > F_GRID_AREA_CELLS)
        return GRID_READ_UNUSABLE;
    garb_area_cell_t cells[F_GRID_AREA_CELLS];
    for (int32_t r = 0; r < rows; r++)
        if (garb_area_cells(v->items[r].text, (uint32_t)v->items[r].len, cells + r * cols,
                            cols) != cols)
            return GRID_READ_UNUSABLE;
    flow_grid_area_t *areas = f_arena_alloc(&a->c->out->arena,
                                            (size_t)rows * cols * sizeof(*areas));
    if (areas == NULL)
        return GRID_READ_NO_MEMORY;
    int32_t n = 0;
    for (int32_t k = 0; k < rows * cols; k++) {
        garb_area_cell_t name = cells[k];
        if (name.len == 0)
            continue;
        int32_t found = -1;
        for (int32_t j = 0; j < n && found < 0; j++)
            if (areas[j].name_len == name.len &&
                os64_memcmp(areas[j].name, name.text, name.len) == 0)
                found = j;
        int32_t r = k / cols, c = k % cols;
        if (found < 0) {
            char *copy = f_arena_alloc(&a->c->out->arena, name.len);
            if (copy == NULL)
                return GRID_READ_NO_MEMORY;
            os64_memcpy(copy, name.text, name.len);
            areas[n++] = (flow_grid_area_t){copy, name.len, r, r + 1, c, c + 1};
            continue;
        }
        flow_grid_area_t *ar = &areas[found];
        ar->row1 = r + 1 > ar->row1 ? r + 1 : ar->row1;
        ar->col1 = c + 1 > ar->col1 ? c + 1 : ar->col1;
    }
    s->grid_areas = areas;
    s->ngrid_areas = n;
    s->grid_area_rows = rows;
    s->grid_area_cols = cols;
    return GRID_READ_OK;
}

// A grid line (§ 8.3). A span of a NAME is a span of one: it counts lines
// of that name, and names in brackets are dropped (GRID.md, decision 3).
static GridRead author_line(Author *a, const garb_val_t *v, flow_grid_line_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    if (v->kind == GARB_V_KEYWORD)
        return GRID_READ_OK;            // auto
    if (v->kind == GARB_V_NUMBER) {
        double n = v->number;
        out->kind = FLOW_GRID_LINE_NUMBER;
        out->n = n > 10000 ? 10000 : n < -10000 ? -10000 : (int32_t)n;
        return GRID_READ_OK;
    }
    if (v->kind == GARB_V_FUNCTION && os64_streq(v->keyword, "span") && v->nitems == 1) {
        out->kind = FLOW_GRID_LINE_SPAN;
        out->n = 1;
        if (v->items[0].kind == GARB_V_NUMBER)
            out->n = v->items[0].number > F_GRID_MAX ? F_GRID_MAX : (int32_t)v->items[0].number;
        return GRID_READ_OK;
    }
    if (v->kind == GARB_V_STRING && v->len > 0) {
        char *copy = f_arena_alloc(&a->c->out->arena, v->len);
        if (copy == NULL)
            return GRID_READ_NO_MEMORY;
        os64_memcpy(copy, v->text, v->len);
        out->kind = FLOW_GRID_LINE_NAME;
        out->name = copy;
        out->name_len = (uint32_t)v->len;
        return GRID_READ_OK;
    }
    return GRID_READ_UNUSABLE;
}

// One winner that is not a CSS-wide keyword.
// A length-percentage as flow's, at the element's font size; `unset` for
// what will not read.
static flow_length_t author_lp(const Author *a, const garb_val_t *v, flow_length_t unset)
{
    Len l;
    return author_len(a, v, false, &l) ? resolve(l, a->font, unset) : unset;
}

// An angle in degrees as thousandths in [0, 360000). It is taken into
// [0, 360) BEFORE it is scaled and narrowed, or `1e20deg` overflows the
// integer and points anywhere. Exactly: each step subtracts the largest
// 360 * 2^k not above it, and a subtraction of two doubles within a factor
// of two of each other is exact (Sterbenz), so this is fmod's answer
// without libmath.
static int32_t angle_thousandths(double deg)
{
    if (!(deg == deg) || deg > 1e300 || deg < -1e300)
        return 0;                           // NaN or near-infinite: the initial
    double m = deg < 0 ? -deg : deg;
    while (m >= 360) {
        double s = 360;
        while (s <= m / 2)
            s *= 2;
        m -= s;
    }
    if (deg < 0 && m > 0)
        m = 360 - m;
    int64_t t = (int64_t)(m * 1000 + 0.5);
    return (int32_t)(t >= 360000 ? t - 360000 : t);
}

// libgarb's gradient (values.h's GARB_V_IMAGE: its geometry, then its
// stops) as flow's, in the style's arena; a currentColor stop is marked
// `current`, for the box it is drawn on to resolve. False only when memory
// runs out.
static bool author_gradient(const Author *a, const flow_style_t *s, const garb_val_t *v,
                            const flow_gradient_t **out)
{
    static const flow_length_t kUnwritten = {FLOW_LENGTH_AUTO, 0, 0};
    static const flow_length_t kHalf = {FLOW_LENGTH_PERCENT, 50 * 64, 0};
    flow_gradient_t *g = f_arena_alloc(&a->c->out->arena, sizeof(*g));
    flow_gradient_stop_t *stops =
        f_arena_alloc(&a->c->out->arena, (size_t)v->nitems * sizeof(*stops));
    if (g == NULL || stops == NULL)
        return false;
    os64_memset(g, 0, sizeof(*g));
    const char *name = v->text;             // one of libgarb's own names
    g->repeating = name != NULL && (os64_streq(name, "repeating-linear-gradient") ||
                                    os64_streq(name, "repeating-radial-gradient"));
    const garb_val_t *geo = &v->items[0];
    if (geo->kind == GARB_V_NUMBER) {
        g->kind = FLOW_GRADIENT_LINEAR;
        g->angle = angle_thousandths(geo->number);
    } else if (geo->kind == GARB_V_KEYWORD && geo->nitems == 0) {
        // "to top right" and the rest, libgarb's eight names: the side or
        // corner each points toward.
        static const struct { const char *name; int8_t x, y; } kTo[] = {
            {"to top", 0, -1},          {"to right", 1, 0},         {"to bottom", 0, 1},
            {"to left", -1, 0},         {"to top left", -1, -1},    {"to top right", 1, -1},
            {"to bottom left", -1, 1},  {"to bottom right", 1, 1},
        };
        g->kind = FLOW_GRADIENT_LINEAR;
        for (size_t k = 0; k < F_ARRAY(kTo); k++)
            if (word(geo, kTo[k].name)) {
                g->to_x = kTo[k].x;
                g->to_y = kTo[k].y;
            }
    } else {
        g->kind = FLOW_GRADIENT_RADIAL;
        g->circle = word(geo, "circle");
        g->extent = FLOW_EXTENT_FARTHEST_CORNER;
        g->radii[0] = g->radii[1] = kUnwritten;
        g->centre[0] = g->centre[1] = kHalf;
        if (geo->nitems == 4) {
            static const char *const kExtents[] = {"farthest-corner", "closest-side",
                                                   "farthest-side", "closest-corner"};
            int32_t e = pick(&geo->items[0], kExtents, F_ARRAY(kExtents));
            if (e >= 0) {
                g->extent = (flow_extent_t)e;
            } else {
                g->extent = FLOW_EXTENT_SIZE;
                g->radii[0] = author_lp(a, &geo->items[0], kUnwritten);
                g->radii[1] = word(&geo->items[1], "auto") ? g->radii[0]
                                                            : author_lp(a, &geo->items[1], kUnwritten);
            }
            g->centre[0] = author_lp(a, &geo->items[2], kHalf);
            g->centre[1] = author_lp(a, &geo->items[3], kHalf);
        }
    }
    int32_t n = 0;
    for (int32_t i = 1; i < v->nitems; i++) {
        const garb_val_t *st = &v->items[i];
        flow_gradient_stop_t *o = &stops[n++];
        o->at = kUnwritten;
        if (st->kind != GARB_V_COLOR) {
            o->hint = true;
            o->at = author_lp(a, st, kUnwritten);
            continue;
        }
        o->current = st->color.current;
        o->colour = st->color.current ? s->color : colour_of(st->color);
        if (st->nitems > 0)
            o->at = author_lp(a, &st->items[0], kUnwritten);
    }
    g->stops = stops;
    g->nstops = n;
    *out = g;
    return true;
}

// `none`, or libgarb's shadows (G_SHADOW): each `inset` first when it is
// there, four lengths and a colour. Lengths in pixels, at the element's
// own font size, which is known by now; a currentColor shadow is marked
// `current`, for the box it is drawn for to resolve. False only when
// memory runs out.
static bool author_shadows(const Author *a, flow_style_t *s, const garb_val_t *v, bool box)
{
    const flow_shadow_t **list = box ? &s->box_shadows : &s->text_shadows;
    int32_t *count = box ? &s->nbox_shadows : &s->ntext_shadows;
    if (word(v, "none") || v->nitems == 0) {
        *list = NULL;
        *count = 0;
        return true;
    }
    flow_shadow_t *out = f_arena_alloc(&a->c->out->arena, (size_t)v->nitems * sizeof(*out));
    if (out == NULL)
        return false;
    for (int32_t i = 0; i < v->nitems; i++) {
        const garb_val_t *one = &v->items[i];
        int32_t k = 0;
        flow_shadow_t *sh = &out[i];
        os64_memset(sh, 0, sizeof(*sh));
        if (k < one->nitems && word(&one->items[k], "inset")) {
            sh->inset = true;
            k++;
        }
        int32_t *len[4] = {&sh->x, &sh->y, &sh->blur, &sh->spread};
        for (int j = 0; j < 4 && k < one->nitems; j++, k++) {
            Len l;
            // To the nearest pixel, a negative offset as a positive one.
            *len[j] = !author_len(a, &one->items[k], false, &l) || l.kind != L_PX ? 0
                      : l.v >= 0 ? (l.v + 32) / 64 : -((-l.v + 32) / 64);
        }
        if (sh->blur < 0)
            sh->blur = 0;
        const garb_val_t *c = k < one->nitems ? &one->items[k] : NULL;
        sh->current = c == NULL || c->kind != GARB_V_COLOR || c->color.current;
        sh->colour = sh->current ? s->color : colour_of(c->color);
    }
    *list = out;
    *count = v->nitems;
    return true;
}

// A background property's list, one entry per layer libgarb read (a
// LAYERS value, or one value for one layer); an entry flow cannot read is
// the property's initial value. A url() is fetched by the face, a
// gradient libgarb kept is drawn (values.h); a conic gradient or an
// image-set is no picture yet (PILE3.md § Booked).
static bool author_layers(const Author *a, flow_style_t *s, garb_prop_t p, const garb_val_t *v)
{
    const garb_val_t *items = v->kind == GARB_V_LAYERS ? v->items : v;
    int32_t n = v->kind == GARB_V_LAYERS ? v->nitems : 1;
    if (n <= 0)
        return true;
    FArena *arena = &a->c->out->arena;
    flow_backgrounds_t *b = &s->backgrounds;
    Len l, two[2];
    int32_t w;
    switch (p) {
    case GARB_BACKGROUND_IMAGE: {
        flow_bg_image_t *list = f_arena_alloc(arena, (size_t)n * sizeof(*list));
        if (list == NULL)
            return false;
        for (int32_t k = 0; k < n; k++) {
            const garb_val_t *one = &items[k];
            list[k] = kNoImage;
            if (one->kind == GARB_V_URL && one->len > 0) {
                list[k].url = one->text;
                list[k].len = (uint32_t)one->len;
                list[k].sheet = a->sheet;
            } else if (one->kind == GARB_V_IMAGE && one->nitems > 0 &&
                       !author_gradient(a, s, one, &list[k].gradient)) {
                return false;
            }
        }
        b->image = list;
        b->nimage = n;
        return true;
    }
    case GARB_BACKGROUND_REPEAT: {
        static const char *const words[] = {"repeat", "repeat-x", "repeat-y", "no-repeat"};
        flow_repeat_t *list = f_arena_alloc(arena, (size_t)n * sizeof(*list));
        if (list == NULL)
            return false;
        for (int32_t k = 0; k < n; k++)
            list[k] = (w = pick(&items[k], words, F_ARRAY(words))) >= 0 ? (flow_repeat_t)w : kRepeat;
        b->repeat = list;
        b->nrepeat = n;
        return true;
    }
    case GARB_BACKGROUND_POSITION_X: case GARB_BACKGROUND_POSITION_Y: {
        flow_length_t *list = f_arena_alloc(arena, (size_t)n * sizeof(*list));
        if (list == NULL)
            return false;
        for (int32_t k = 0; k < n; k++)
            list[k] = author_len(a, &items[k], false, &l)
                          ? resolve(l, a->font, (flow_length_t){FLOW_LENGTH_PX, 0, 0})
                          : kAtStart;
        *(p == GARB_BACKGROUND_POSITION_X ? &b->x : &b->y) = list;
        *(p == GARB_BACKGROUND_POSITION_X ? &b->nx : &b->ny) = n;
        return true;
    }
    case GARB_BACKGROUND_SIZE: {
        // A keyword, or libgarb's pair of width and height.
        static const char *const fits[] = {"cover", "contain"};
        flow_bg_size_t *list = f_arena_alloc(arena, (size_t)n * sizeof(*list));
        if (list == NULL)
            return false;
        for (int32_t k = 0; k < n; k++) {
            const garb_val_t *one = &items[k];
            list[k] = kOwnSize;
            if ((w = pick(one, fits, F_ARRAY(fits))) >= 0) {
                list[k].fit = (flow_bg_fit_t)(FLOW_FIT_COVER + w);
            } else if (one->kind == GARB_V_LENGTH && one->nitems == 2 &&
                       author_len(a, &one->items[0], true, &two[0]) &&
                       author_len(a, &one->items[1], true, &two[1])) {
                for (int j = 0; j < 2; j++)
                    list[k].size[j] =
                        resolve(two[j], a->font, (flow_length_t){FLOW_LENGTH_AUTO, 0, 0});
            }
        }
        b->size = list;
        b->nsize = n;
        return true;
    }
    case GARB_BACKGROUND_ORIGIN: case GARB_BACKGROUND_CLIP: {
        static const char *const edges[] = {"border-box", "padding-box", "content-box", "text"};
        bool origin = p == GARB_BACKGROUND_ORIGIN;
        flow_edge_t *list = f_arena_alloc(arena, (size_t)n * sizeof(*list));
        if (list == NULL)
            return false;
        for (int32_t k = 0; k < n; k++)
            list[k] = (w = pick(&items[k], edges, F_ARRAY(edges))) >= 0 ? (flow_edge_t)w
                      : origin ? kPaddingBox : kBorderBox;
        *(origin ? &b->origin : &b->clip) = list;
        *(origin ? &b->norigin : &b->nclip) = n;
        return true;
    }
    default:
        return true;
    }
}

static bool author_value(Author *a, Spec *sp, const garb_set_t *set)
{
    const garb_val_t *v = &set->value;
    flow_style_t *s = &sp->s;
    garb_prop_t p = set->prop;
    int32_t i;
    switch (p) {
    case GARB_DISPLAY: author_display(v, &s->display); break;
    case GARB_COLOR:
        if (v->kind == GARB_V_COLOR && v->color.current)
            s->color = a->parent != NULL ? a->parent->color : a->c->env->ink;
        else if (v->kind == GARB_V_COLOR)
            s->color = colour_of(v->color);
        break;
    case GARB_BACKGROUND_COLOR:
        if (v->kind != GARB_V_COLOR)
            break;
        // currentColor is resolved in finish, which knows the final colour.
        if (v->color.current) {
            s->current_colours |= FLOW_CURRENT_BACKGROUND;
        } else {
            s->current_colours &= (uint8_t)~FLOW_CURRENT_BACKGROUND;
            s->has_background = v->color.a > 0;
            s->background = colour_of(v->color);
        }
        break;
    case GARB_BACKGROUND_IMAGE: case GARB_BACKGROUND_REPEAT: case GARB_BACKGROUND_POSITION_X:
    case GARB_BACKGROUND_POSITION_Y: case GARB_BACKGROUND_SIZE: case GARB_BACKGROUND_ORIGIN:
    case GARB_BACKGROUND_CLIP:
        if (!author_layers(a, s, p, v))
            return false;
        break;
    case GARB_MARGIN_TOP: case GARB_MARGIN_RIGHT: case GARB_MARGIN_BOTTOM: case GARB_MARGIN_LEFT:
        author_len(a, v, true, &sp->margin[side(p, GARB_MARGIN_TOP)]);
        break;
    case GARB_PADDING_TOP: case GARB_PADDING_RIGHT: case GARB_PADDING_BOTTOM:
    case GARB_PADDING_LEFT:
        author_len(a, v, false, &sp->padding[side(p, GARB_PADDING_TOP)]);
        break;
    case GARB_BORDER_TOP_WIDTH: case GARB_BORDER_RIGHT_WIDTH: case GARB_BORDER_BOTTOM_WIDTH:
    case GARB_BORDER_LEFT_WIDTH:
        author_border_width(a, v, &sp->border_px[side(p, GARB_BORDER_TOP_WIDTH)]);
        break;
    case GARB_BORDER_TOP_STYLE: case GARB_BORDER_RIGHT_STYLE: case GARB_BORDER_BOTTOM_STYLE:
    case GARB_BORDER_LEFT_STYLE:
        author_border_style(v, &s->border_style[side(p, GARB_BORDER_TOP_STYLE)]);
        break;
    case GARB_BORDER_TOP_COLOR: case GARB_BORDER_RIGHT_COLOR: case GARB_BORDER_BOTTOM_COLOR:
    case GARB_BORDER_LEFT_COLOR:
        i = side(p, GARB_BORDER_TOP_COLOR);
        if (v->kind != GARB_V_COLOR)
            break;
        // currentColor is left for finish, which knows the final colour.
        sp->border_color_set[i] = !v->color.current;
        s->border_color[i] = colour_of(v->color);
        break;
    case GARB_WIDTH: case GARB_HEIGHT: case GARB_MIN_WIDTH: case GARB_MAX_WIDTH:
    case GARB_MIN_HEIGHT: case GARB_MAX_HEIGHT: {
        Len *l = p == GARB_WIDTH ? &sp->width : p == GARB_HEIGHT ? &sp->height
               : p == GARB_MIN_WIDTH ? &sp->min_width : p == GARB_MAX_WIDTH ? &sp->max_width
               : p == GARB_MIN_HEIGHT ? &sp->min_height : &sp->max_height;
        // `auto`, a limit's `none`, and the content-sized keywords — which
        // are auto until libflow sizes by content on request (GARB.md §
        // Booked) — are all no size of the page's own.
        if (v->kind == GARB_V_KEYWORD)
            *l = kAuto;
        else
            author_len(a, v, true, l);
        break;
    }
    case GARB_LINE_HEIGHT:
        if (word(v, "normal")) {
            s->line_height = (flow_line_height_t){FLOW_LINE_NORMAL, 0};
        } else if (v->kind == GARB_V_NUMBER && v->number >= 0) {
            s->line_height = (flow_line_height_t){FLOW_LINE_NUMBER, round_i32(v->number * 1000)};
        } else if (v->kind == GARB_V_PERCENTAGE && v->number >= 0) {
            s->line_height = (flow_line_height_t){FLOW_LINE_PX,
                                                  round_i32(a->font * v->number / 100)};
        } else {
            Len l;
            if (author_len(a, v, false, &l) && l.kind == L_PX && l.v >= 0)
                s->line_height = (flow_line_height_t){FLOW_LINE_PX, l.v};
        }
        break;
    case GARB_TEXT_INDENT:
        // `hanging` and `each-line` are read and not drawn.
        if (v->kind == GARB_V_LENGTH || v->kind == GARB_V_PERCENTAGE || v->kind == GARB_V_CALC)
            author_len(a, v, false, &sp->text_indent);
        break;
    case GARB_TEXT_TRANSFORM: {
        static const char *const words[] = {"none", "uppercase", "lowercase", "capitalize"};
        i = pick(v, words, F_ARRAY(words));
        s->text_transform = i >= 0 ? (flow_text_transform_t)i : FLOW_TRANSFORM_NONE;
        break;
    }
    case GARB_OVERFLOW_X: case GARB_OVERFLOW_Y: {
        static const char *const words[] = {"visible", "hidden", "clip", "scroll", "auto"};
        if ((i = pick(v, words, F_ARRAY(words))) >= 0)
            *(p == GARB_OVERFLOW_X ? &s->overflow_x : &s->overflow_y) = (flow_overflow_t)i;
        break;
    }
    case GARB_BOX_SIZING:
        s->box_sizing = word(v, "border-box") ? FLOW_BORDER_BOX : FLOW_CONTENT_BOX;
        break;
    case GARB_FONT_FAMILY: return author_family(a, v, &s->family);
    case GARB_FONT_SIZE: break;         // read first, by author()
    case GARB_FONT_WEIGHT: author_font_weight(a, sp, v); break;
    case GARB_FONT_STYLE:
        s->font_style = word(v, "normal") ? FLOW_FONT_NORMAL : FLOW_FONT_ITALIC;
        break;
    case GARB_TEXT_ALIGN: {
        static const char *const words[] = {"left", "right", "center", "justify", "start", "end"};
        static const flow_text_align_t as[] = {FLOW_ALIGN_LEFT, FLOW_ALIGN_RIGHT,
                                               FLOW_ALIGN_CENTER, FLOW_ALIGN_JUSTIFY,
                                               FLOW_ALIGN_LEFT, FLOW_ALIGN_RIGHT};
        if ((i = pick(v, words, F_ARRAY(words))) >= 0)
            s->text_align = as[i];
        else if (word(v, "match-parent") && a->parent != NULL)
            s->text_align = a->parent->text_align;
        break;
    }
    case GARB_VERTICAL_ALIGN: {
        // text-bottom is drawn as bottom, and a length or a percentage
        // raises nothing yet (GARB.md § Booked).
        static const char *const words[] = {"baseline", "sub", "super", "text-top",
                                            "text-bottom", "middle", "top", "bottom"};
        static const flow_vertical_align_t as[] = {
            FLOW_VALIGN_BASELINE, FLOW_VALIGN_SUB, FLOW_VALIGN_SUPER, FLOW_VALIGN_TEXT_TOP,
            FLOW_VALIGN_BOTTOM, FLOW_VALIGN_MIDDLE, FLOW_VALIGN_TOP, FLOW_VALIGN_BOTTOM};
        if ((i = pick(v, words, F_ARRAY(words))) >= 0)
            s->vertical_align = as[i];
        else
            s->vertical_align = FLOW_VALIGN_BASELINE;
        break;
    }
    case GARB_WHITE_SPACE: {
        // break-spaces is drawn as pre-wrap: a space it keeps at a line's
        // end hangs rather than taking room (GARB.md § Booked).
        static const char *const words[] = {"normal", "pre", "nowrap", "pre-wrap",
                                            "pre-line", "break-spaces"};
        static const flow_white_space_t as[] = {FLOW_WS_NORMAL, FLOW_WS_PRE, FLOW_WS_NOWRAP,
                                                FLOW_WS_PRE_WRAP, FLOW_WS_PRE_LINE,
                                                FLOW_WS_PRE_WRAP};
        if ((i = pick(v, words, F_ARRAY(words))) >= 0)
            s->white_space = as[i];
        break;
    }
    case GARB_TEXT_DECORATION_LINE:
        // An overline and blink are not drawn.
        s->text_decoration = 0;
        for (int32_t k = 0; k < v->nitems; k++) {
            if (word(&v->items[k], "underline"))
                s->text_decoration |= FLOW_DECORATION_UNDERLINE;
            else if (word(&v->items[k], "line-through"))
                s->text_decoration |= FLOW_DECORATION_LINE_THROUGH;
        }
        break;
    case GARB_IMAGE_RENDERING: {
        // The crisp spellings; every other keyword libgarb takes smooths.
        static const char *const crisp[] = {"pixelated", "crisp-edges", "-moz-crisp-edges",
                                            "-webkit-optimize-contrast", "optimizespeed"};
        if (v->kind == GARB_V_KEYWORD)
            s->pixelated = pick(v, crisp, F_ARRAY(crisp)) >= 0;
        break;
    }
    case GARB_VISIBILITY: {
        static const char *const words[] = {"visible", "hidden", "collapse"};
        if ((i = pick(v, words, F_ARRAY(words))) >= 0)
            s->visibility = (flow_visibility_t)i;
        break;
    }
    case GARB_LIST_STYLE_TYPE: author_list_type(sp, v); break;
    case GARB_LIST_STYLE_POSITION:
        s->list_style_position = word(v, "inside") ? FLOW_LIST_INSIDE : FLOW_LIST_OUTSIDE;
        break;
    case GARB_BORDER_SPACING:
        for (int32_t k = 0; k < 2 && k < v->nitems; k++) {
            Len l;
            if (author_len(a, &v->items[k], false, &l) && l.kind == L_PX)
                s->border_spacing[k] = l.v < 0 ? 0 : l.v;
        }
        break;
    case GARB_BORDER_COLLAPSE:
        s->border_collapse = word(v, "collapse") ? FLOW_COLLAPSE_BORDERS : FLOW_SEPARATE;
        break;
    case GARB_CAPTION_SIDE:
        s->caption_side = word(v, "bottom") ? FLOW_CAPTION_BOTTOM : FLOW_CAPTION_TOP;
        break;
    case GARB_FLOAT: {
        static const char *const words[] = {"none", "left", "right", "inline-start",
                                            "inline-end"};
        static const flow_float_t as[] = {FLOW_FLOAT_NONE, FLOW_FLOAT_LEFT, FLOW_FLOAT_RIGHT,
                                          FLOW_FLOAT_LEFT, FLOW_FLOAT_RIGHT};
        if ((i = pick(v, words, F_ARRAY(words))) >= 0)
            s->float_side = as[i];
        break;
    }
    case GARB_CLEAR: {
        static const char *const words[] = {"none", "left", "right", "both", "inline-start",
                                            "inline-end"};
        static const flow_clear_t as[] = {FLOW_CLEAR_NONE, FLOW_CLEAR_LEFT, FLOW_CLEAR_RIGHT,
                                          FLOW_CLEAR_BOTH, FLOW_CLEAR_LEFT, FLOW_CLEAR_RIGHT};
        if ((i = pick(v, words, F_ARRAY(words))) >= 0)
            s->clear = as[i];
        break;
    }
    case GARB_POSITION: {
        static const char *const words[] = {"static", "relative", "absolute", "fixed", "sticky"};
        if ((i = pick(v, words, F_ARRAY(words))) >= 0)
            s->position = (flow_position_t)i;
        break;
    }
    case GARB_TOP: case GARB_RIGHT: case GARB_BOTTOM: case GARB_LEFT:
        author_len(a, v, true, &sp->inset[side(p, GARB_TOP)]);
        break;
    case GARB_Z_INDEX:
        s->has_z_index = v->kind == GARB_V_NUMBER;
        s->z_index = s->has_z_index ? round_i32(v->number) : 0;
        break;
    case GARB_POINTER_EVENTS:
        // SVG's values are auto to an HTML box.
        s->pointer_events_none = word(v, "none");
        break;
    case GARB_FLEX_DIRECTION: {
        static const char *const words[] = {"row", "row-reverse", "column", "column-reverse"};
        if ((i = pick(v, words, F_ARRAY(words))) >= 0)
            s->flex_direction = (flow_flex_direction_t)i;
        break;
    }
    case GARB_FLEX_WRAP: {
        static const char *const words[] = {"nowrap", "wrap", "wrap-reverse"};
        if ((i = pick(v, words, F_ARRAY(words))) >= 0)
            s->flex_wrap = (flow_flex_wrap_t)i;
        break;
    }
    case GARB_JUSTIFY_CONTENT: case GARB_ALIGN_ITEMS: case GARB_ALIGN_SELF:
    case GARB_ALIGN_CONTENT: {
        // One table for all four: each grammar admits only its own words.
        static const char *const words[] = {
            "normal", "auto", "stretch", "flex-start", "flex-end", "center", "baseline",
            "start", "end", "self-start", "self-end", "left", "right", "space-between",
            "space-around", "space-evenly"};
        _Static_assert(F_ARRAY(words) == FLOW_PLACE_SPACE_EVENLY + 1, "flow_place_t");
        if ((i = pick(v, words, F_ARRAY(words))) < 0)
            break;
        flow_place_t *field = p == GARB_JUSTIFY_CONTENT ? &s->justify_content
                            : p == GARB_ALIGN_ITEMS ? &s->align_items
                            : p == GARB_ALIGN_SELF ? &s->align_self : &s->align_content;
        *field = (flow_place_t)i;
        break;
    }
    case GARB_FLEX_GROW: case GARB_FLEX_SHRINK:
        if (v->kind == GARB_V_NUMBER) {
            // In thousandths, held where a sum of many cannot overflow.
            double f = v->number < 0 ? 0 : v->number > 1e6 ? 1e6 : v->number;
            *(p == GARB_FLEX_GROW ? &s->flex_grow : &s->flex_shrink) = round_i32(f * 1000);
        }
        break;
    case GARB_FLEX_BASIS:
        // `content` is the content's size, and so, here, are the three
        // sizing keywords: max-content exactly, min-content and fit-content
        // as the nearest this reads (FLEX.md § Booked).
        if (v->kind == GARB_V_KEYWORD && !word(v, "auto"))
            sp->flex_basis = kContent;
        else
            author_len(a, v, true, &sp->flex_basis);
        break;
    case GARB_ORDER:
        if (v->kind == GARB_V_NUMBER)
            s->order = round_i32(v->number);
        break;
    case GARB_GRID_TEMPLATE_COLUMNS: case GARB_GRID_TEMPLATE_ROWS:
    case GARB_GRID_AUTO_COLUMNS: case GARB_GRID_AUTO_ROWS: {
        flow_tracks_t t;
        GridRead r = author_tracks(a, v, &t);
        if (r == GRID_READ_NO_MEMORY)
            return false;
        if (r != GRID_READ_OK)
            break;
        *(p == GARB_GRID_TEMPLATE_COLUMNS ? &s->grid_template_columns
          : p == GARB_GRID_TEMPLATE_ROWS  ? &s->grid_template_rows
          : p == GARB_GRID_AUTO_COLUMNS   ? &s->grid_auto_columns
                                          : &s->grid_auto_rows) = t;
        break;
    }
    case GARB_GRID_TEMPLATE_AREAS:
        if (author_areas(a, v, s) == GRID_READ_NO_MEMORY)
            return false;
        break;
    case GARB_GRID_AUTO_FLOW:
        if (v->kind == GARB_V_KEYWORD) {
            s->grid_auto_flow_column = word(v, "column") || word(v, "column dense");
            s->grid_dense = word(v, "row dense") || word(v, "column dense");
        }
        break;
    case GARB_GRID_ROW_START: case GARB_GRID_ROW_END:
    case GARB_GRID_COLUMN_START: case GARB_GRID_COLUMN_END: {
        flow_grid_line_t l;
        GridRead r = author_line(a, v, &l);
        if (r == GRID_READ_NO_MEMORY)
            return false;
        if (r == GRID_READ_OK)
            *(p == GARB_GRID_ROW_START      ? &s->grid_row_start
              : p == GARB_GRID_ROW_END      ? &s->grid_row_end
              : p == GARB_GRID_COLUMN_START ? &s->grid_column_start
                                            : &s->grid_column_end) = l;
        break;
    }
    case GARB_JUSTIFY_ITEMS: case GARB_JUSTIFY_SELF: {
        // The alignment table, and `legacy` read as `normal`: all it adds
        // is taking an ancestor's `legacy center`, which is not read (Box
        // Alignment 3 § 6.1).
        static const char *const words[] = {
            "normal", "auto", "stretch", "flex-start", "flex-end", "center", "baseline",
            "start", "end", "self-start", "self-end", "left", "right"};
        i = pick(v, words, F_ARRAY(words));
        if (i < 0 && word(v, "legacy"))
            i = FLOW_PLACE_NORMAL;
        if (i >= 0)
            *(p == GARB_JUSTIFY_ITEMS ? &s->justify_items : &s->justify_self) = (flow_place_t)i;
        break;
    }
    case GARB_BOX_SHADOW: case GARB_TEXT_SHADOW:
        if (!author_shadows(a, s, v, p == GARB_BOX_SHADOW))
            return false;
        break;
    case GARB_BORDER_TOP_LEFT_RADIUS: case GARB_BORDER_TOP_RIGHT_RADIUS:
    case GARB_BORDER_BOTTOM_RIGHT_RADIUS: case GARB_BORDER_BOTTOM_LEFT_RADIUS:
        // A corner is two items, horizontal and vertical (libgarb's G_RADIUS).
        for (int32_t k = 0; k < 2 && k < v->nitems; k++)
            author_len(a, &v->items[k], false, &sp->radius[p - GARB_BORDER_TOP_LEFT_RADIUS][k]);
        break;
    case GARB_ROW_GAP: case GARB_COLUMN_GAP: {
        Len *gap = p == GARB_ROW_GAP ? &sp->row_gap : &sp->column_gap;
        if (word(v, "normal"))
            *gap = kAuto;
        else
            author_len(a, v, false, gap);
        break;
    }
    case GARB_OPACITY: {
        // Clamped to 0..1 now that it is computed (Color 4 § 4.2).
        double o = -1;
        Lp r;
        if (v->kind == GARB_V_NUMBER)
            o = v->number;
        else if (v->kind == GARB_V_PERCENTAGE)
            o = v->number / 100;
        else if (v->kind == GARB_V_CALC && v->calc != NULL && calc(a, v->calc, a->font, &r))
            o = r.number ? r.px : r.pct / 100;
        else
            break;
        s->opacity = (uint16_t)(o <= 0 ? 0 : o >= 1 ? 1000 : (int32_t)(o * 1000 + 0.5));
        break;
    }
    default:
        break;
    }
    return true;
}

static bool author_sets(garb_style_t st, int32_t prop)
{
    for (int32_t i = 0; i < st.n; i++)
        if ((int32_t)st.sets[i].prop == prop)
            return true;
    return false;
}

// Whether an author set gives `prop` a value of its own: anything but
// `revert`, which hands it back to the user-agent origin.
static bool author_declares(garb_style_t st, int32_t prop)
{
    for (int32_t i = 0; i < st.n; i++)
        if ((int32_t)st.sets[i].prop == prop)
            return !(st.sets[i].value.kind == GARB_V_WIDE &&
                     (os64_streq(st.sets[i].value.keyword, "revert") ||
                      os64_streq(st.sets[i].value.keyword, "revert-layer")));
    return false;
}

// The element's author winners over what the chapter and the hints wrote.
// `ua` is the Spec as the user-agent origin left it, which `revert` goes
// back to. False on no memory only.
static bool author(Ctx *c, const os64_html_node_t *n, Spec *sp, const Spec *ua,
                   const flow_style_t *parent)
{
    if (c->env->cascade == NULL)
        return true;
    garb_style_t st = garb_style_for(c->env->cascade, n);
    if (st.n == 0)
        return true;
    Author a = {c, parent, 0, garb_cascade_env(c->env->cascade), -1};
    Spec init, inh;
    bool have_init = false, have_inh = false;
    // font-size and color before the rest: every other em on the element
    // is its font size, and every currentColor its colour.
    for (int pass = 0; pass < 2; pass++) {
        for (int32_t i = 0; i < st.n; i++) {
            const garb_set_t *set = &st.sets[i];
            a.sheet = st.sheet != NULL ? st.sheet[i] : -1;
            bool font = set->prop == GARB_FONT_SIZE;
            bool first = font || set->prop == GARB_COLOR;
            if (first != (pass == 0))
                continue;
            if (set->value.kind != GARB_V_WIDE) {
                if (font)
                    author_font_size(&a, sp, &set->value);
                else if (!author_value(&a, sp, set))
                    return false;
                continue;
            }
            const char *k = set->value.keyword;
            bool inherits = os64_streq(k, "inherit") ||
                            (os64_streq(k, "unset") && garb_prop_inherited(set->prop));
            if (os64_streq(k, "revert") || os64_streq(k, "revert-layer")) {
                // No user origin, and no layers: both go back to the
                // user-agent origin (Cascade 5 § 7.3, § 7.4).
                take(sp, ua, set->prop);
            } else if (inherits) {
                if (!have_inh)
                    inherited_spec(c, parent, &inh);
                have_inh = true;
                take(sp, &inh, set->prop);
            } else {
                if (!have_init)
                    initial_spec(c, &init);
                have_init = true;
                take(sp, &init, set->prop);
            }
        }
        if (pass == 0)
            a.font = sp->fs_kind != FS_INHERIT ? font_size(c, sp, parent) : sp->s.font_size;
    }
    // A border style the author drew with no width from any origin has the
    // initial width, medium. The Spec's declared width starts at 0, not
    // medium, because the chapter always declares the width it draws — so a
    // 0 there means NO origin declared one, which is also what `revert`
    // leaves where the chapter declared none: a width reverted is not a
    // width declared.
    for (int i = 0; i < 4; i++)
        if (sp->border_px[i] == 0 && author_sets(st, GARB_BORDER_TOP_STYLE + i) &&
            !author_declares(st, GARB_BORDER_TOP_WIDTH + i))
            sp->border_px[i] = css(c, 3);
    return true;
}

// ── Resolving ───────────────────────────────────────────────────────────

static flow_length_t resolve(Len l, flow_unit_t font, flow_length_t unset)
{
    switch (l.kind) {
    case L_PX: return (flow_length_t){FLOW_LENGTH_PX, l.v, 0};
    case L_PCT: return (flow_length_t){FLOW_LENGTH_PERCENT, l.v, l.off};
    case L_EM: return (flow_length_t){FLOW_LENGTH_PX, scale(font, l.v, 1000), 0};
    case L_AUTO: return (flow_length_t){FLOW_LENGTH_AUTO, 0, 0};
    case L_FIT: return (flow_length_t){FLOW_LENGTH_FIT_CONTENT, 0, 0};
    case L_CONTENT: return (flow_length_t){FLOW_LENGTH_CONTENT, 0, 0};
    case L_UNSET: break;
    }
    return unset;
}

// What positioning does to the rest of a style (CSS 2.1 § 9.7), once every
// origin has spoken. A face asking for the page as it reads in document
// order (flow_env_t.static_only) gets every box static, and nothing here
// fires. A box that leaves the flow (f_out_of_flow) is BLOCKIFIED — laid
// out as the block it computes to, its float none — and keeps one bit of
// the display it was given (flow_style_t.specified_inline); a fixed one is
// in the flow until its slice, and keeps the display the page gave it, or
// a fixed span in a link would split its paragraph.
// CSS 2.1 § 9.7's table, and Display 3's blockification: the block-level
// display an inline-level one becomes, keeping the one bit of the old
// (flow_style_t.specified_inline). A box that makes none keeps its display.
// False when there was nothing to do.
static bool blockify(flow_style_t *s)
{
    switch (s->display) {
    case FLOW_DISPLAY_NONE: case FLOW_DISPLAY_CONTENTS:
        return false;
    case FLOW_DISPLAY_INLINE: case FLOW_DISPLAY_INLINE_BLOCK:
        s->specified_inline = true;
        s->display = FLOW_DISPLAY_BLOCK;
        break;
    case FLOW_DISPLAY_INLINE_FLEX:
        s->specified_inline = true;
        s->display = FLOW_DISPLAY_FLEX;
        break;
    case FLOW_DISPLAY_INLINE_GRID:
        s->specified_inline = true;
        s->display = FLOW_DISPLAY_GRID;
        break;
    case FLOW_DISPLAY_TABLE: case FLOW_DISPLAY_LIST_ITEM: case FLOW_DISPLAY_BLOCK:
    case FLOW_DISPLAY_FLEX: case FLOW_DISPLAY_GRID:
        break;
    default:
        // A table's parts: § 9.7's table makes each a block.
        s->display = FLOW_DISPLAY_BLOCK;
        break;
    }
    s->float_side = FLOW_FLOAT_NONE;
    return true;
}

static void positioning(const Ctx *c, Spec *sp)
{
    flow_style_t *s = &sp->s;
    if (c->env->static_only)
        s->position = FLOW_POSITION_STATIC;
    if (f_out_of_flow(s))
        (void)blockify(s);
}

// `item`: the element's box is its parent's flex or grid item, and is
// blockified (Flexbox 1 § 4, Grid 2 § 6) — as an out-of-flow one already
// was.
static void finish(const Ctx *c, Spec *sp, const flow_style_t *parent, bool item)
{
    flow_style_t *s = &sp->s;
    if (sp->fs_kind != FS_INHERIT)
        s->font_size = font_size(c, sp, parent);
    if (sp->bolder)
        s->font_weight = bolder(parent != NULL ? parent->font_weight : 400);
    const flow_length_t zero = {FLOW_LENGTH_PX, 0, 0};
    const flow_length_t automatic = {FLOW_LENGTH_AUTO, 0, 0};
    for (int i = 0; i < 4; i++) {
        s->margin[i] = resolve(sp->margin[i], s->font_size, zero);
        s->padding[i] = resolve(sp->padding[i], s->font_size, zero);
        s->radius[i][0] = resolve(sp->radius[i][0], s->font_size, zero);
        s->radius[i][1] = resolve(sp->radius[i][1], s->font_size, zero);
        bool drawn = s->border_style[i] != FLOW_BORDER_NONE &&
                     s->border_style[i] != FLOW_BORDER_HIDDEN;
        s->border_width[i] = drawn ? sp->border_px[i] : 0;
        if (!sp->border_color_set[i])
            s->border_color[i] = s->color;
        s->current_colours = (uint8_t)(sp->border_color_set[i]
                                           ? s->current_colours & ~FLOW_CURRENT_BORDER(i)
                                           : s->current_colours | FLOW_CURRENT_BORDER(i));
    }
    if (s->current_colours & FLOW_CURRENT_BACKGROUND) {
        s->has_background = flow_alpha(s->color) > 0;
        s->background = s->color;
    }
    s->width = resolve(sp->width, s->font_size, automatic);
    s->height = resolve(sp->height, s->font_size, automatic);
    for (int i = 0; i < 4; i++)
        s->inset[i] = resolve(sp->inset[i], s->font_size, automatic);
    positioning(c, sp);
    if (item && !f_out_of_flow(s))
        (void)blockify(s);
    // CSS Overflow 3 § 3: visible and clip do not stand beside an axis
    // that scrolls or hides.
    bool x_scrolls = s->overflow_x != FLOW_OVERFLOW_VISIBLE && s->overflow_x != FLOW_OVERFLOW_CLIP;
    bool y_scrolls = s->overflow_y != FLOW_OVERFLOW_VISIBLE && s->overflow_y != FLOW_OVERFLOW_CLIP;
    if (x_scrolls != y_scrolls) {
        flow_overflow_t *other = x_scrolls ? &s->overflow_y : &s->overflow_x;
        *other = *other == FLOW_OVERFLOW_VISIBLE ? FLOW_OVERFLOW_AUTO : FLOW_OVERFLOW_HIDDEN;
    }
    if (sp->text_indent.kind != L_UNSET)
        s->text_indent = resolve(sp->text_indent, s->font_size, zero);
    s->min_width = resolve(sp->min_width, s->font_size, automatic);
    s->max_width = resolve(sp->max_width, s->font_size, automatic);
    s->min_height = resolve(sp->min_height, s->font_size, automatic);
    s->max_height = resolve(sp->max_height, s->font_size, automatic);
    s->flex_basis = resolve(sp->flex_basis, s->font_size, automatic);
    s->row_gap = resolve(sp->row_gap, s->font_size, automatic);
    s->column_gap = resolve(sp->column_gap, s->font_size, automatic);
}

// ── The walk ────────────────────────────────────────────────────────────

// `details > summary:first-of-type`.
static bool first_summary(const os64_html_node_t *n)
{
    if (!is(n, OS64_HTML_TAG_SUMMARY))
        return false;
    for (const os64_html_node_t *p = n->prev; p != NULL; p = p->prev)
        if (is(p, OS64_HTML_TAG_SUMMARY))
            return false;
    return true;
}

// Whether an element keeps its ancestors' decorations off its content (an
// atomic inline; a table, in full quirks) — its own still reach it.
static bool decoration_edge(const os64_html_node_t *n, const flow_style_t *s, bool quirks)
{
    return f_display_atomic(s->display) ||
           (quirks && n->ns == OS64_HTML_NS_HTML && n->tag == OS64_HTML_TAG_TABLE);
}

static FStyled *style_element(Ctx *c, const os64_html_node_t *n)
{
    const FStyled *up = f_map_get(&c->out->map, n->parent);
    const flow_style_t *parent = up != NULL ? &up->style : NULL;
    FStyled *out = f_arena_alloc(&c->out->arena, sizeof(*out));
    if (out == NULL)
        return NULL;
    Spec sp;
    os64_memset(&sp, 0, sizeof(sp));
    sp.s = inherit(c, parent);
    if (n->ns != OS64_HTML_NS_HTML) {
        // An SVG or MathML element draws nothing here, and the HTML inside
        // it is laid out as its parent's (LAYOUT.md § Pass 2).
        sp.s.display = FLOW_DISPLAY_CONTENTS;
    } else {
        sheet(c, n, &sp);
        if (is(n->parent, OS64_HTML_TAG_DETAILS) && sp.s.display != FLOW_DISPLAY_NONE) {
            // §15.5.5: the first summary is the disclosure the person
            // presses; the rest of a closed details is not drawn.
            if (first_summary(n)) {
                sp.s.display = FLOW_DISPLAY_LIST_ITEM;
                sp.s.list_style_type = has(n->parent, "open") ? FLOW_LIST_DISCLOSURE_OPEN
                                                              : FLOW_LIST_DISCLOSURE_CLOSED;
                sp.s.list_style_position = FLOW_LIST_INSIDE;
            } else if (!has(n->parent, "open")) {
                sp.s.display = FLOW_DISPLAY_NONE;
            }
        }
        // What `revert` goes back to: the user-agent origin, which is the
        // chapter's sheet, its link rule and its quirks, without the hints.
        Spec ua = sp;
        if (sp.s.display != FLOW_DISPLAY_NONE) {
            if (!hints(c, n, &sp))
                return NULL;
            image_quirk(c, n, &sp);
            margin_quirks(c, n, &sp);
            link_rule(c, n, &ua, false);
            image_quirk(c, n, &ua);
            margin_quirks(c, n, &ua);
        }
        if (!author(c, n, &sp, &ua, parent))
            return NULL;
    }
    finish(c, &sp, parent, up != NULL && up->lays_out_items);
    out->style = sp.s;
    out->lays_out_items = f_display_flex(sp.s.display) || f_display_grid(sp.s.display) ||
                          (sp.s.display == FLOW_DISPLAY_CONTENTS && up != NULL &&
                           up->lays_out_items);
    if (n == c->doc->html)
        c->root_font = sp.s.font_size;
    out->link = c->model != NULL ? os64_page_link_for(c->model, n) : -1;
    if (out->link < 0 && up != NULL)
        out->link = up->link;
    out->lists = (up != NULL ? up->lists : 0) + is_list(n);
    out->lists_or_dls = (up != NULL ? up->lists_or_dls : 0) + is_list_or_dl(n);
    out->items = (up != NULL ? up->items : 0) + is_li(n);
    out->in_nobr = (up != NULL && up->in_nobr) || is(n, OS64_HTML_TAG_NOBR);
    out->quotes = (up != NULL ? up->quotes : 0) + is(n, OS64_HTML_TAG_Q);
    out->anchor = f_contains_absolute(&sp.s) ? n : up != NULL ? up->anchor : NULL;
    out->transparent = sp.s.opacity == 0 || (up != NULL && up->transparent);
    FStyled *holder = f_map_get(&c->out->map, n->parent);
    if (holder != NULL && is(n, OS64_HTML_TAG_RP))
        holder->has_rp = true;
    out->decoration = sp.s.text_decoration;
    out->decoration_colors = (FDecorationColors){sp.s.color, sp.s.color};
    if (up != NULL && !decoration_edge(n, &sp.s, c->quirks))
        f_decoration_inherit(&out->decoration, &out->decoration_colors, up->decoration,
                             up->decoration_colors);
    if (!f_map_put(&c->out->map, n, out))
        return NULL;
    return out;
}

// Whether a child makes its parent hold an IN-FLOW block: it is block-level
// itself, or it is inline (or makes no box) and holds one inside. An
// out-of-flow box is neither — no formatting context holds it — so an
// absolute badge inside an `<a>` splits nothing round it.
static bool contributes_block(const FStyled *child)
{
    if (f_out_of_flow(&child->style))
        return false;
    switch (child->style.display) {
    case FLOW_DISPLAY_NONE: case FLOW_DISPLAY_INLINE_BLOCK: case FLOW_DISPLAY_INLINE_FLEX:
    case FLOW_DISPLAY_INLINE_GRID:
        return false;
    case FLOW_DISPLAY_INLINE: case FLOW_DISPLAY_CONTENTS:
        return child->holds_block;
    default:
        return true;
    }
}

static const os64_html_node_t *next_element(const os64_html_node_t *n)
{
    while (n != NULL && n->kind != OS64_HTML_ELEMENT)
        n = n->next;
    return n;
}

FStyles *f_style_build(const os64_html_document_t *doc, const os64_page_t *model,
                       const flow_env_t *env)
{
    if (doc == NULL || env == NULL)
        return NULL;
    FStyles *out = os64_calloc(1, sizeof(*out));
    if (out == NULL)
        return NULL;
    out->doc = doc;
    out->env = env;
    // One record per element and the family names a `face` wrote: bounded
    // by the document libhtml admitted, never multiplied, so no budget —
    // pass 1 is all or nothing, and a cap here would blank a large page
    // that the boxes' budget would merely cut short.
    out->arena.cap = SIZE_MAX;
    Ctx c = {.doc = doc, .model = model, .env = env, .out = out,
             .quirks = doc->quirks == OS64_HTML_QUIRKS,
             .zoom = env->zoom != 0 ? env->zoom / 1000.0 : 1.0};
    c.root_font = css(&c, env->viewport_font_px);

    // Pre-order down, and each element FINISHED on the way back up, when
    // everything under it has been styled — which is when its holds_block
    // can be told to its parent.
    const os64_html_node_t *top = doc->document;
    const os64_html_node_t *cur = top != NULL ? next_element(top->first_child) : doc->html;
    while (cur != NULL) {
        FStyled *styled = style_element(&c, cur);
        if (styled == NULL) {
            f_style_free(out);
            return NULL;
        }
        const os64_html_node_t *child = styled->style.display == FLOW_DISPLAY_NONE
                                            ? NULL : next_element(cur->first_child);
        if (child != NULL) {
            cur = child;
            continue;
        }
        while (cur != NULL) {
            FStyled *done = f_map_get(&out->map, cur);
            FStyled *parent = f_map_get(&out->map, cur->parent);
            if (parent != NULL && done != NULL && contributes_block(done))
                parent->holds_block = true;
            // A document with no document node was walked from its html
            // element, and that element's siblings are not the page's.
            if (top == NULL && cur == doc->html) {
                cur = NULL;
                break;
            }
            const os64_html_node_t *sibling = next_element(cur->next);
            if (sibling != NULL) {
                cur = sibling;
                break;
            }
            cur = cur->parent;
            if (cur == top)
                cur = NULL;
        }
    }
    return out;
}

void f_style_anonymous(const FStyles *styles, const flow_style_t *parent,
                       flow_display_t display, flow_style_t *out)
{
    Ctx c = {.doc = styles->doc, .env = styles->env};
    *out = inherit(&c, parent);
    out->display = display;
    for (int i = 0; i < 4; i++)
        out->border_color[i] = out->color;
}

const FStyled *f_style_of(const FStyles *styles, const os64_html_node_t *node)
{
    return styles != NULL ? f_map_get(&styles->map, node) : NULL;
}

void f_style_free(FStyles *styles)
{
    if (styles == NULL)
        return;
    f_map_free(&styles->map);
    f_arena_free(&styles->arena);
    os64_free(styles);
}
