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
// user-agent rules that the chapter states in prose. A later cascade slots
// in after the hints with no field renamed.
//
// The walk is iterative, over the tree's own parent pointers, so a page as
// deep as libhtml allows is styled without a stack as deep as the page.

#include "internal.h"

// ── Lengths while an element is being styled ────────────────────────────
//
// The sheet writes `1em` before the element's font size is known, so a
// length is held as written and resolved once, after the font.
// UNSET is what nothing wrote: a margin or padding of 0, a width of auto.
typedef enum { L_UNSET = 0, L_AUTO, L_PX, L_PCT, L_EM } LKind;
typedef struct {
    LKind kind;
    int32_t v;      // PX: 26.6 px; PCT: 1/64 percent; EM: 1/1000 em
} Len;

static Len px(int32_t n) { return (Len){L_PX, n * FLOW_UNITS_PER_PX}; }
static Len em(int32_t thousandths) { return (Len){L_EM, thousandths}; }
static const Len kAuto = {L_AUTO, 0};

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
    Len margin[4], padding[4], width, height;
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
    bool quirks, any_quirks;    // full quirks; quirks or limited quirks
    // `body link=` recolours every link, and the body is styled before any
    // link it contains is.
    bool has_body_link;
    uint32_t body_link;
} Ctx;

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

static int32_t count_ancestors(const os64_html_node_t *n, bool (*match)(const os64_html_node_t *))
{
    int32_t count = 0;
    for (const os64_html_node_t *p = n->parent; p != NULL; p = p->parent)
        if (match(p))
            count++;
    return count;
}

static bool is_li(const os64_html_node_t *n)
{
    return is(n, OS64_HTML_TAG_LI);
}

static bool is_list_or_dl(const os64_html_node_t *n)
{
    return is_list(n) || is(n, OS64_HTML_TAG_DL);
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

static void border(Spec *sp, flow_border_style_t style, int32_t width_px)
{
    for (int i = 0; i < 4; i++) {
        sp->s.border_style[i] = style;
        sp->border_px[i] = width_px * FLOW_UNITS_PER_PX;
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

static Len dim(f_dim_kind_t kind, int32_t v)
{
    return kind == F_DIM_PERCENT ? (Len){L_PCT, v} : (Len){L_PX, v};
}

// "Maps to the dimension property": the attribute, read as a dimension,
// sets the property; one that does not parse sets nothing.
static void map_dimension(const os64_html_node_t *n, const char *name, bool nonzero, Len *out)
{
    int32_t v;
    f_dim_kind_t kind = f_parse_dimension(attr(n, name), nonzero, &v);
    if (kind != F_DIM_NONE)
        *out = dim(kind, v);
}

// "Maps to the pixel length property": a non-negative integer, in px.
static bool pixel_length(const os64_html_node_t *n, const char *name, Len *out)
{
    int32_t v;
    if (!f_parse_nonnegative(attr(n, name), &v))
        return false;
    *out = px(v);
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
}

// ── The initial style, and inheritance ──────────────────────────────────

static flow_style_t initial(const Ctx *c)
{
    flow_style_t s;
    os64_memset(&s, 0, sizeof(s));
    s.display = FLOW_DISPLAY_INLINE;
    s.family = (flow_family_list_t){NULL, 0, c->env->default_generic};
    s.font_weight = 400;
    s.font_style = FLOW_FONT_NORMAL;
    s.font_size = (flow_unit_t)c->env->viewport_font_px * FLOW_UNITS_PER_PX;
    s.color = c->env->ink;
    // Everything else starts at zero, which is each property's initial
    // value: auto lengths, no borders, left, baseline, normal, disc
    // outside, separate, top, no float.
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
    s.visibility = parent->visibility;
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

static int32_t scale(int32_t v, int32_t num, int32_t den)
{
    return (int32_t)(((int64_t)v * num + den / 2) / den);
}

static flow_unit_t font_size(const Ctx *c, const Spec *sp, const flow_style_t *parent)
{
    flow_unit_t medium = (flow_unit_t)c->env->viewport_font_px * FLOW_UNITS_PER_PX;
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
static uint16_t bolder(uint16_t w)
{
    return w < 350 ? 400 : w < 550 ? 700 : 900;
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
    // `noscript` is shown: this browser runs no script, so the
    // `@media (scripting)` rule that hides it does not apply.

    switch (n->tag) {
    // §15.3.2 The page, §15.3.3 Flow content.
    case OS64_HTML_TAG_HTML: case OS64_HTML_TAG_BODY:
    case OS64_HTML_TAG_ADDRESS: case OS64_HTML_TAG_CENTER: case OS64_HTML_TAG_DIV:
    case OS64_HTML_TAG_FIGCAPTION: case OS64_HTML_TAG_FOOTER: case OS64_HTML_TAG_FORM:
    case OS64_HTML_TAG_HEADER: case OS64_HTML_TAG_LEGEND: case OS64_HTML_TAG_MAIN:
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
    case OS64_HTML_TAG_BLOCKQUOTE: case OS64_HTML_TAG_FIGURE:
        s->display = FLOW_DISPLAY_BLOCK;
        margin_block(sp, em(1000));
        margin_inline(sp, px(40));
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
        // Open, it is positioned over the page; positioning is booked, so
        // it is laid out where it stands, with its border and padding.
        if (!has(n, "open")) {
            s->display = FLOW_DISPLAY_NONE;
            return;
        }
        s->display = FLOW_DISPLAY_BLOCK;
        border(sp, FLOW_BORDER_SOLID, 3);   // `border: solid` is medium: 3px
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
        sp->padding[FLOW_LEFT] = px(40);
        if (n->tag == OS64_HTML_TAG_OL) {
            s->list_style_type = FLOW_LIST_DECIMAL;
        } else {
            int32_t depth = count_ancestors(n, is_list);
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
        s->border_spacing[0] = s->border_spacing[1] = 2 * FLOW_UNITS_PER_PX;
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
        padding_all(sp, px(1));
        if (n->tag == OS64_HTML_TAG_TH)
            s->font_weight = 700;
        break;

    // §15.3.10 Form controls: laid out as boxes the face's widgets fill.
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
        border(sp, FLOW_BORDER_INSET, 1);
        margin_block(sp, em(500));
        margin_inline(sp, kAuto);
        break;

    // §15.3.12 fieldset and legend. A legend drawn ON the fieldset's border
    // is not CSS 2.1's; it is laid out inside, first, which is where it
    // reads (LAYOUT.md § Booked).
    case OS64_HTML_TAG_FIELDSET:
        s->display = FLOW_DISPLAY_BLOCK;
        margin_inline(sp, px(2));
        border(sp, FLOW_BORDER_GROOVE, 2);
        border_color(sp, 0xC0C0C0);     // ThreeDFace, the classic 3D grey
        sp->padding[FLOW_TOP] = em(350);
        sp->padding[FLOW_BOTTOM] = em(625);
        sp->padding[FLOW_LEFT] = sp->padding[FLOW_RIGHT] = em(750);
        break;

    // §15.4.1 Embedded content.
    case OS64_HTML_TAG_IFRAME:
        border(sp, FLOW_BORDER_INSET, 2);
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
    if ((is_list_or_dl(n)) && count_ancestors(n, is_list_or_dl) > 0)
        margin_block(sp, px(0));
    // `dd { margin-inline-start: 40px }`
    if (is(n, OS64_HTML_TAG_DD))
        sp->margin[FLOW_LEFT] = px(40);

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
    if (is(n, OS64_HTML_TAG_TD) || is(n, OS64_HTML_TAG_TH)) {
        const os64_html_node_t *table = cell_table(n);
        const char *rules = table != NULL ? attr(table, "rules") : NULL;
        const char *frame = table != NULL ? attr(table, "frame") : NULL;
        if (rules != NULL || frame != NULL) {
            static const char *const any[] = {"none", "groups", "rows", "cols", "all"};
            for (int32_t i = 0; i < F_ARRAY(any); i++)
                if (f_eq_nocase(rules, any[i]))
                    border_color(sp, 0x000000);
        }
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
        bool in_list = count_ancestors(n, is_list) > 0;
        if (is_li(n) && !in_list)
            s->list_style_position = FLOW_LIST_INSIDE;
        if (is_list(n) && !in_list && count_ancestors(n, is_li) > 0)
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

static bool hints(Ctx *c, const os64_html_node_t *n, Spec *sp)
{
    flow_style_t *s = &sp->s;
    uint32_t rgb;

    switch (n->tag) {
    // §15.3.2 The page.
    case OS64_HTML_TAG_BODY: {
        // The FIRST of the attributes that exists decides, and one that
        // will not parse means the 8px default — not the next attribute.
        Len v = px(8);
        if (has(n, "marginheight") ? !pixel_length(n, "marginheight", &v)
                                   : has(n, "topmargin") && !pixel_length(n, "topmargin", &v))
            v = px(8);
        margin_block(sp, v);
        v = px(8);
        if (has(n, "marginwidth") ? !pixel_length(n, "marginwidth", &v)
                                  : has(n, "leftmargin") && !pixel_length(n, "leftmargin", &v))
            v = px(8);
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
        map_dimension(n, "width", true, &sp->width);
        map_dimension(n, "height", false, &sp->height);
        if (legacy_color(n, "bgcolor", &rgb))
            background(sp, rgb);
        if (legacy_color(n, "bordercolor", &rgb))
            border_color(sp, rgb);
        Len spacing;
        if (pixel_length(n, "cellspacing", &spacing))
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
                sp->border_px[k] = width * FLOW_UNITS_PER_PX;
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
    case OS64_HTML_TAG_COL:
        map_dimension(n, "width", false, &sp->width);
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
            map_dimension(n, "height", false, &sp->height);
    }
    if (is(n, OS64_HTML_TAG_TD) || is(n, OS64_HTML_TAG_TH)) {
        if (has(n, "nowrap"))
            s->white_space = FLOW_WS_NOWRAP;
        map_dimension(n, "width", true, &sp->width);
        map_dimension(n, "height", true, &sp->height);
        const os64_html_node_t *table = cell_table(n);
        if (table != NULL) {
            Len pad;
            if (pixel_length(table, "cellpadding", &pad))
                padding_all(sp, pad);
            if (has(table, "border") && border_not_zero(table))
                border(sp, FLOW_BORDER_INSET, 1);
            const char *rules = attr(table, "rules");
            if (f_eq_nocase(rules, "none") || f_eq_nocase(rules, "groups") ||
                f_eq_nocase(rules, "rows")) {
                border(sp, FLOW_BORDER_NONE, 1);
            } else if (f_eq_nocase(rules, "cols")) {
                border(sp, FLOW_BORDER_SOLID, 1);
                s->border_style[FLOW_TOP] = s->border_style[FLOW_BOTTOM] = FLOW_BORDER_NONE;
            } else if (f_eq_nocase(rules, "all")) {
                border(sp, FLOW_BORDER_SOLID, 1);
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
                sp->border_px[FLOW_TOP] = sp->border_px[FLOW_BOTTOM] = FLOW_UNITS_PER_PX;
                s->border_style[FLOW_TOP] = s->border_style[FLOW_BOTTOM] = FLOW_BORDER_SOLID;
            }
        }
    }
    if (is(n, OS64_HTML_TAG_COLGROUP) && is(n->parent, OS64_HTML_TAG_TABLE) &&
        attr_is(n->parent, "rules", "groups")) {
        sp->border_px[FLOW_LEFT] = sp->border_px[FLOW_RIGHT] = FLOW_UNITS_PER_PX;
        s->border_style[FLOW_LEFT] = s->border_style[FLOW_RIGHT] = FLOW_BORDER_SOLID;
    }

    // §15.3.11 The hr element.
    if (is(n, OS64_HTML_TAG_HR)) {
        if (attr_is(n, "align", "left")) {
            sp->margin[FLOW_LEFT] = px(0);
            sp->margin[FLOW_RIGHT] = kAuto;
        } else if (attr_is(n, "align", "right")) {
            sp->margin[FLOW_LEFT] = kAuto;
            sp->margin[FLOW_RIGHT] = px(0);
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
                    sp->border_px[k] = size * FLOW_UNITS_PER_PX / 2;
            } else if (size == 1) {
                sp->border_px[FLOW_BOTTOM] = 0;
            } else if (size > 1) {
                sp->height = px(size - 2);
            }
        }
        map_dimension(n, "width", false, &sp->width);
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
        map_dimension(n, "width", false, &sp->width);
        map_dimension(n, "height", false, &sp->height);
    }
    if (is(n, OS64_HTML_TAG_EMBED) || is(n, OS64_HTML_TAG_IMG) || is(n, OS64_HTML_TAG_OBJECT) ||
        (is(n, OS64_HTML_TAG_INPUT) && attr_is(n, "type", "image"))) {
        Len v = {L_UNSET, 0};
        map_dimension(n, "hspace", false, &v);
        if (v.kind != L_UNSET)
            margin_inline(sp, v);
        v = (Len){L_UNSET, 0};
        map_dimension(n, "vspace", false, &v);
        if (v.kind != L_UNSET)
            margin_block(sp, v);
        if (!is(n, OS64_HTML_TAG_EMBED)) {
            int32_t b;
            if (f_parse_nonnegative(attr(n, "border"), &b) && b > 0)
                border(sp, FLOW_BORDER_SOLID, b);
        }
    }
    if (is(n, OS64_HTML_TAG_IFRAME)) {
        int32_t fb;
        if (has(n, "frameborder") && (!f_parse_integer(attr(n, "frameborder"), &fb) || fb == 0))
            for (int k = 0; k < 4; k++)
                sp->border_px[k] = 0;
    }
    if (is(n, OS64_HTML_TAG_EMBED) && has(n, "hidden")) {
        sp->width = px(0);
        sp->height = px(0);
    }

    // §15.3.4: a link's colour, where libpage says the node IS a link. The
    // user-agent rule is `:link`, and `body link=` is a hint on the same
    // selector, so it wins over the sheet and loses to anything the link's
    // own descendants say.
    if ((is(n, OS64_HTML_TAG_A) || is(n, OS64_HTML_TAG_AREA)) && c->model != NULL &&
        os64_page_link_for(c->model, n) >= 0) {
        s->color = c->has_body_link ? c->body_link : c->env->link_ink;
        s->text_decoration |= FLOW_DECORATION_UNDERLINE;
    }
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
        sp->margin[FLOW_RIGHT] = px(3);
    else if (attr_is(n, "align", "right"))
        sp->margin[FLOW_LEFT] = px(3);
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
        sp->margin[FLOW_TOP] = px(0);
        if (blank(n))
            sp->margin[FLOW_BOTTOM] = px(0);
    }
    if (in_cell && !substantial_after(n) && blank(n))
        sp->margin[FLOW_TOP] = px(0);
    if (in_cell && is(n, OS64_HTML_TAG_P) && !substantial_after(n))
        sp->margin[FLOW_BOTTOM] = px(0);
}

// ── Resolving ───────────────────────────────────────────────────────────

static flow_length_t resolve(Len l, flow_unit_t font, flow_length_t unset)
{
    switch (l.kind) {
    case L_PX: return (flow_length_t){FLOW_LENGTH_PX, l.v};
    case L_PCT: return (flow_length_t){FLOW_LENGTH_PERCENT, l.v};
    case L_EM: return (flow_length_t){FLOW_LENGTH_PX, scale(font, l.v, 1000)};
    case L_AUTO: return (flow_length_t){FLOW_LENGTH_AUTO, 0};
    case L_UNSET: break;
    }
    return unset;
}

static void finish(const Ctx *c, Spec *sp, const flow_style_t *parent)
{
    flow_style_t *s = &sp->s;
    if (sp->fs_kind != FS_INHERIT)
        s->font_size = font_size(c, sp, parent);
    if (sp->bolder)
        s->font_weight = bolder(parent != NULL ? parent->font_weight : 400);
    const flow_length_t zero = {FLOW_LENGTH_PX, 0};
    const flow_length_t automatic = {FLOW_LENGTH_AUTO, 0};
    for (int i = 0; i < 4; i++) {
        s->margin[i] = resolve(sp->margin[i], s->font_size, zero);
        s->padding[i] = resolve(sp->padding[i], s->font_size, zero);
        bool drawn = s->border_style[i] != FLOW_BORDER_NONE &&
                     s->border_style[i] != FLOW_BORDER_HIDDEN;
        s->border_width[i] = drawn ? sp->border_px[i] : 0;
        if (!sp->border_color_set[i])
            s->border_color[i] = s->color;
    }
    s->width = resolve(sp->width, s->font_size, automatic);
    s->height = resolve(sp->height, s->font_size, automatic);
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
        if (sp.s.display != FLOW_DISPLAY_NONE) {
            if (!hints(c, n, &sp))
                return NULL;
            image_quirk(c, n, &sp);
            margin_quirks(c, n, &sp);
        }
    }
    finish(c, &sp, parent);
    out->style = sp.s;
    if (!f_map_put(&c->out->map, n, out))
        return NULL;
    return out;
}

// Whether a child makes its parent hold a block: it is block-level itself,
// or it is inline (or makes no box) and holds one inside.
static bool contributes_block(const FStyled *child)
{
    switch (child->style.display) {
    case FLOW_DISPLAY_NONE: case FLOW_DISPLAY_INLINE_BLOCK:
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
    Ctx c = {doc, model, env, out, doc->quirks == OS64_HTML_QUIRKS,
             doc->quirks != OS64_HTML_NO_QUIRKS, false, 0};

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
