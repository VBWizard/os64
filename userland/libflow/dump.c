// dump.c — a page's layout as text, for the host harness and for a probe
// in the guest: a change to the engine is a diff to a file.

#include "internal.h"
#include "os64/fmt.h"

typedef struct {
    char *out;
    size_t cap, len;    // len counts everything, written or not
} Buf;

static void put(Buf *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (b->len + 1 < b->cap)
            b->out[b->len] = s[i];
        b->len++;
    }
}

static void puts_(Buf *b, const char *s)
{
    put(b, s, os64_strlen(s));
}

static void putf(Buf *b, const char *fmt, ...) OS64_PRINTF(2, 3);
static void putf(Buf *b, const char *fmt, ...)
{
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    int32_t n = os64_vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0)
        put(b, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

// 26.6 as a decimal: whole pixels bare, a fraction to two places.
static void unit(Buf *b, int32_t v)
{
    if (v < 0) {
        puts_(b, "-");
        v = -v;
    }
    int32_t whole = v / 64, frac = v % 64;
    if (frac == 0) {
        putf(b, "%d", (int)whole);
        return;
    }
    int32_t hundredths = (frac * 100 + 32) / 64;
    if (hundredths == 100) {
        putf(b, "%d", (int)whole + 1);
        return;
    }
    putf(b, "%d.%02d", (int)whole, (int)hundredths);
}

static void length(Buf *b, flow_length_t l)
{
    if (l.kind == FLOW_LENGTH_AUTO) {
        puts_(b, "auto");
    } else {
        unit(b, l.value);
        if (l.kind == FLOW_LENGTH_PERCENT)
            puts_(b, "%");
    }
}

static bool length_eq(flow_length_t a, flow_length_t b)
{
    return a.kind == b.kind && a.value == b.value;
}

static bool is_zero(flow_length_t l)
{
    return l.kind != FLOW_LENGTH_AUTO && l.value == 0;
}

static void color(Buf *b, const flow_env_t *env, uint32_t rgb)
{
    if (rgb == env->ink)
        puts_(b, "ink");
    else if (rgb == env->link_ink)
        puts_(b, "link");
    else if (rgb == env->paper)
        puts_(b, "paper");
    else
        putf(b, "#%06x", (unsigned)(rgb & 0xFFFFFF));
}

static const char *const s_display[] = {
    "inline", "block", "list-item", "inline-block", "table", "table-caption",
    "table-row-group", "table-header-group", "table-footer-group", "table-row",
    "table-cell", "table-column-group", "table-column", "contents", "none",
};
static const char *const s_generic[] = {"serif", "sans", "mono"};
static const char *const s_border[] = {"none", "hidden", "solid", "inset", "outset", "groove"};
static const char *const s_align[] = {"left", "right", "center", "justify", "html-left",
                                      "html-right", "html-center", "html-justify"};
static const char *const s_valign[] = {"baseline", "sub", "super", "top", "text-top",
                                       "middle", "bottom", "html-middle"};
static const char *const s_ws[] = {"normal", "pre", "nowrap", "pre-wrap"};
static const char *const s_list[] = {"disc", "circle", "square", "decimal", "lower-alpha",
                                     "upper-alpha", "lower-roman", "upper-roman",
                                     "disclosure-closed", "disclosure-open", "none"};
static const char *const s_visibility[] = {"visible", "hidden", "collapse"};
static const char *const s_float[] = {"none", "left", "right"};
static const char *const s_clear[] = {"none", "left", "right", "both"};

static bool family_eq(const flow_family_list_t *a, const flow_family_list_t *b)
{
    if (a->generic != b->generic || a->count != b->count)
        return false;
    for (uint32_t i = 0; i < a->count; i++)
        if (a->names[i].len != b->names[i].len ||
            os64_memcmp(a->names[i].name, b->names[i].name, a->names[i].len) != 0)
            return false;
    return true;
}

static void four_lengths(Buf *b, const char *name, const flow_length_t v[4])
{
    putf(b, " %s=", name);
    for (int i = 0; i < 4; i++) {
        if (i > 0)
            puts_(b, "/");
        length(b, v[i]);
    }
}

static void element(Buf *b, const FStyles *styles, const os64_html_node_t *n,
                    const FStyled *self, const flow_style_t *parent, int32_t depth)
{
    const flow_env_t *env = styles->env;
    const flow_style_t *s = &self->style;
    flow_style_t base;
    // The comparison for inherited properties: the parent, or at the root
    // the initial style a root inherits from.
    if (parent == NULL) {
        os64_memset(&base, 0, sizeof(base));
        base.family = (flow_family_list_t){NULL, 0, env->default_generic};
        base.font_weight = 400;
        base.font_size = (flow_unit_t)env->viewport_font_px * FLOW_UNITS_PER_PX;
        base.color = env->ink;
        parent = &base;
    }

    for (int32_t i = 0; i < depth; i++)
        puts_(b, "  ");
    const char *name = n->ns == OS64_HTML_NS_HTML && n->tag != OS64_HTML_TAG_UNKNOWN
                           ? os64_html_tag_name(n->tag) : n->name;
    puts_(b, name != NULL ? name : "?");
    puts_(b, " ");
    puts_(b, s_display[s->display]);
    // An element that makes no box has nothing else worth reading.
    if (s->display == FLOW_DISPLAY_NONE) {
        puts_(b, "\n");
        return;
    }

    if (!family_eq(&s->family, &parent->family)) {
        puts_(b, " font=");
        for (uint32_t i = 0; i < s->family.count; i++) {
            puts_(b, "\"");
            put(b, s->family.names[i].name, s->family.names[i].len);
            puts_(b, "\",");
        }
        puts_(b, s_generic[s->family.generic]);
    }
    if (s->font_weight != parent->font_weight)
        putf(b, " weight=%u", (unsigned)s->font_weight);
    if (s->font_style != parent->font_style)
        puts_(b, s->font_style == FLOW_FONT_ITALIC ? " italic" : " upright");
    if (s->font_size != parent->font_size) {
        puts_(b, " size=");
        unit(b, s->font_size);
    }
    if (s->color != parent->color) {
        puts_(b, " color=");
        color(b, env, s->color);
    }
    if (s->has_background) {
        puts_(b, " bg=");
        color(b, env, s->background);
    }
    bool margins = false, paddings = false, borders = false;
    for (int i = 0; i < 4; i++) {
        margins |= !is_zero(s->margin[i]);
        paddings |= !is_zero(s->padding[i]);
        borders |= s->border_style[i] != FLOW_BORDER_NONE;
    }
    if (margins)
        four_lengths(b, "margin", s->margin);
    if (paddings)
        four_lengths(b, "padding", s->padding);
    if (borders) {
        puts_(b, " border=");
        bool same = true;
        for (int i = 1; i < 4; i++)
            same &= s->border_width[i] == s->border_width[0] &&
                    s->border_style[i] == s->border_style[0] &&
                    s->border_color[i] == s->border_color[0];
        for (int i = 0; i < (same ? 1 : 4); i++) {
            if (i > 0)
                puts_(b, "/");
            unit(b, s->border_width[i]);
            puts_(b, ":");
            puts_(b, s_border[s->border_style[i]]);
            puts_(b, ":");
            color(b, env, s->border_color[i]);
        }
    }
    flow_length_t automatic = {FLOW_LENGTH_AUTO, 0};
    if (!length_eq(s->width, automatic)) {
        puts_(b, " width=");
        length(b, s->width);
    }
    if (!length_eq(s->height, automatic)) {
        puts_(b, " height=");
        length(b, s->height);
    }
    if (s->text_align != parent->text_align)
        putf(b, " align=%s", s_align[s->text_align]);
    if (s->vertical_align != FLOW_VALIGN_BASELINE)
        putf(b, " valign=%s", s_valign[s->vertical_align]);
    if (s->white_space != parent->white_space)
        putf(b, " ws=%s", s_ws[s->white_space]);
    if (s->text_decoration & FLOW_DECORATION_UNDERLINE)
        puts_(b, " underline");
    if (s->text_decoration & FLOW_DECORATION_LINE_THROUGH)
        puts_(b, " line-through");
    if (s->visibility != parent->visibility)
        putf(b, " visibility=%s", s_visibility[s->visibility]);
    if (s->list_style_type != parent->list_style_type)
        putf(b, " list=%s", s_list[s->list_style_type]);
    if (s->list_style_position != parent->list_style_position)
        puts_(b, s->list_style_position == FLOW_LIST_INSIDE ? " list-inside" : " list-outside");
    if (s->border_spacing[0] != parent->border_spacing[0] ||
        s->border_spacing[1] != parent->border_spacing[1]) {
        puts_(b, " spacing=");
        unit(b, s->border_spacing[0]);
        puts_(b, "/");
        unit(b, s->border_spacing[1]);
    }
    if (s->border_collapse != parent->border_collapse)
        puts_(b, s->border_collapse == FLOW_COLLAPSE_BORDERS ? " collapse" : " separate");
    if (s->caption_side != parent->caption_side)
        puts_(b, s->caption_side == FLOW_CAPTION_BOTTOM ? " caption-bottom" : " caption-top");
    if (s->float_side != FLOW_FLOAT_NONE)
        putf(b, " float=%s", s_float[s->float_side]);
    if (s->clear != FLOW_CLEAR_NONE)
        putf(b, " clear=%s", s_clear[s->clear]);
    if (self->holds_block)
        puts_(b, " holds-block");
    puts_(b, "\n");
}

int64_t f_style_dump(const FStyles *styles, char *out, size_t cap)
{
    Buf b = {out, cap, 0};
    if (styles == NULL) {
        if (cap > 0)
            out[0] = '\0';
        return 0;
    }
    const os64_html_document_t *doc = styles->doc;
    const os64_html_node_t *top = doc->document;
    const os64_html_node_t *cur = top != NULL ? top->first_child : doc->html;
    int32_t depth = 0;
    while (cur != NULL) {
        const FStyled *self = cur->kind == OS64_HTML_ELEMENT ? f_style_of(styles, cur) : NULL;
        const os64_html_node_t *down = NULL;
        if (self != NULL) {
            const FStyled *up = f_style_of(styles, cur->parent);
            element(&b, styles, cur, self, up != NULL ? &up->style : NULL, depth);
            if (self->style.display != FLOW_DISPLAY_NONE)
                down = cur->first_child;
        }
        if (down != NULL) {
            cur = down;
            depth++;
            continue;
        }
        while (cur != NULL) {
            if (top == NULL && cur == doc->html) {
                cur = NULL;
                break;
            }
            if (cur->next != NULL) {
                cur = cur->next;
                break;
            }
            cur = cur->parent;
            depth--;
            if (cur == top)
                cur = NULL;
        }
    }
    if (cap > 0)
        out[b.len < cap ? b.len : cap - 1] = '\0';
    return (int64_t)b.len;
}

// ── The box tree ────────────────────────────────────────────────────────

static void indent(Buf *b, int32_t depth)
{
    for (int32_t i = 0; i < depth; i++)
        puts_(b, "  ");
}

static const char *node_name(const os64_html_node_t *n)
{
    if (n == NULL)
        return "anon";
    const char *name = n->ns == OS64_HTML_NS_HTML && n->tag != OS64_HTML_TAG_UNKNOWN
                           ? os64_html_tag_name(n->tag) : n->name;
    return name != NULL ? name : "?";
}

// Text as a quoted string, with the bytes that would break a line or a
// quote escaped. UTF-8 passes through as itself.
static void quoted(Buf *b, const char *s, uint32_t len)
{
    puts_(b, "\"");
    for (uint32_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            char e[2] = {'\\', (char)c};
            put(b, e, 2);
        } else if (c == '\n') {
            puts_(b, "\\n");
        } else if (c == '\t') {
            puts_(b, "\\t");
        } else if (c < 0x20 || c == 0x7F) {
            putf(b, "\\x%02x", c);
        } else {
            put(b, (const char *)&s[i], 1);
        }
    }
    puts_(b, "\"");
}

static const char *const s_box[] = {
    "block", "replaced", "table", "caption", "column-group", "column", "row-group", "row", "cell",
};

static void box_lines(Buf *b, const FBox *box, int32_t depth);

static void item_line(Buf *b, const FItem *it, int32_t depth)
{
    indent(b, depth);
    switch (it->kind) {
    case FI_TEXT:
        puts_(b, "text ");
        quoted(b, it->text, it->len);
        if (it->offset != 0)
            putf(b, " @%u", (unsigned)it->offset);
        if (it->generated)
            putf(b, " generated-by %s", node_name(it->node));
        break;
    case FI_OPEN:
    case FI_CLOSE:
        putf(b, "%s %s", it->kind == FI_OPEN ? "open" : "close", node_name(it->node));
        if (it->kind == FI_OPEN && it->inl != NULL && it->inl->link >= 0)
            putf(b, " link %d", (int)it->inl->link);
        if (it->continuation)
            puts_(b, it->kind == FI_OPEN ? " continued" : " split");
        break;
    case FI_ATOMIC:
        putf(b, "atomic %s", node_name(it->node));
        if (it->link >= 0)
            putf(b, " link %d", (int)it->link);
        if (it->control >= 0)
            putf(b, " control %d", (int)it->control);
        break;
    case FI_BREAK:
        puts_(b, it->node != NULL ? "br" : "newline");
        break;
    case FI_WBR:
        puts_(b, "wbr");
        break;
    case FI_MARKER:
        puts_(b, "marker ");
        quoted(b, it->text, it->len);
        break;
    }
    puts_(b, "\n");
    if (it->kind == FI_ATOMIC && it->content != NULL)
        box_lines(b, it->content, depth + 1);
}

static void box_lines(Buf *b, const FBox *box, int32_t depth)
{
    indent(b, depth);
    putf(b, "%s %s", s_box[box->kind], node_name(box->node));
    if (box->marker != NULL) {
        puts_(b, " marker ");
        quoted(b, box->marker, box->marker_len);
    }
    if (box->link >= 0)
        putf(b, " link %d", (int)box->link);
    puts_(b, "\n");
    for (const FItem *it = box->items; it != NULL; it = it->next)
        item_line(b, it, depth + 1);
    for (const FBox *c = box->first; c != NULL; c = c->next)
        box_lines(b, c, depth + 1);
}

int64_t f_boxes_dump(const FBoxes *boxes, char *out, size_t cap)
{
    Buf b = {out, cap, 0};
    if (boxes != NULL && boxes->root != NULL)
        box_lines(&b, boxes->root, 0);
    if (boxes != NULL && boxes->incomplete)
        puts_(&b, "incomplete\n");
    if (cap > 0)
        out[b.len < cap ? b.len : cap - 1] = '\0';
    return (int64_t)b.len;
}

// ── The laid-out tree ───────────────────────────────────────────────────
//
// Document pixels, rounded ONCE by the painter's own rule — to nearest,
// ties up — and a size as round(end) - round(start), so neighbours abut
// in the dump exactly as they will on the glass.

static int64_t px_round(int64_t v)
{
    int64_t q = (v + 32) / 64;
    return (v + 32) % 64 < 0 ? q - 1 : q;
}

static void rect(Buf *b, int64_t x, int64_t y, int64_t w, int64_t h)
{
    int64_t x0 = px_round(x), y0 = px_round(y);
    putf(b, " %lld %lld %lld %lld", (long long)x0, (long long)y0,
         (long long)(px_round(x + w) - x0), (long long)(px_round(y + h) - y0));
}

static void frag_line(Buf *b, const FFrag *fr, int32_t depth);
static void layout_box(Buf *b, const FBox *box, int32_t depth);

static void frag_line(Buf *b, const FFrag *fr, int32_t depth)
{
    indent(b, depth);
    switch (fr->kind) {
    case FF_TEXT:
        puts_(b, "text ");
        quoted(b, fr->text + fr->begin, fr->end - fr->begin);
        rect(b, fr->x, fr->y, fr->w, fr->h);
        putf(b, " %s %d", s_generic[fr->style->family.generic],
             (int)((fr->style->font_size + 32) / 64));
        if (fr->style->font_weight >= 600)
            puts_(b, " bold");
        if (fr->style->font_style == FLOW_FONT_ITALIC)
            puts_(b, " italic");
        if (fr->decoration & FLOW_DECORATION_UNDERLINE)
            puts_(b, " underline");
        if (fr->decoration & FLOW_DECORATION_LINE_THROUGH)
            puts_(b, " line-through");
        break;
    case FF_ATOMIC:
        putf(b, "atomic %s", node_name(fr->node));
        rect(b, fr->x, fr->y, fr->w, fr->h);
        if (fr->item->control >= 0)
            putf(b, " control %d", (int)fr->item->control);
        break;
    case FF_MARKER:
        puts_(b, "marker ");
        quoted(b, fr->text, fr->end);
        rect(b, fr->x, fr->y, fr->w, fr->h);
        break;
    }
    if (fr->link >= 0)
        putf(b, " link %d", (int)fr->link);
    puts_(b, "\n");
    if (fr->kind == FF_ATOMIC && fr->item->content != NULL)
        layout_box(b, fr->item->content, depth + 1);
}

static void layout_box(Buf *b, const FBox *box, int32_t depth)
{
    if (!box->placed)
        return;
    indent(b, depth);
    putf(b, "%s %s", s_box[box->kind], node_name(box->node));
    rect(b, box->x, box->y, box->w, box->h);
    if (box->link >= 0)
        putf(b, " link %d", (int)box->link);
    puts_(b, "\n");
    if (box->marker_frag != NULL)
        frag_line(b, box->marker_frag, depth + 1);
    for (const FLine *ln = box->lines; ln != NULL; ln = ln->next) {
        indent(b, depth + 1);
        puts_(b, "line");
        rect(b, ln->x, ln->y, ln->w, ln->h);
        putf(b, " base %lld\n", (long long)(px_round(ln->baseline) - px_round(ln->y)));
        for (const FSpan *sp = ln->spans; sp != NULL; sp = sp->next) {
            indent(b, depth + 2);
            putf(b, "span %s", node_name(sp->inl->node));
            rect(b, sp->x0, sp->top, sp->x1 - sp->x0, sp->bottom - sp->top);
            if (sp->inl->link >= 0)
                putf(b, " link %d", (int)sp->inl->link);
            puts_(b, "\n");
        }
        for (const FFrag *fr = ln->frags; fr != NULL; fr = fr->next)
            frag_line(b, fr, depth + 2);
    }
    for (const FBox *c = box->first; c != NULL; c = c->next)
        layout_box(b, c, depth + 1);
}

int64_t f_layout_dump(const FLayout *layout, char *out, size_t cap)
{
    Buf b = {out, cap, 0};
    if (layout != NULL && layout->boxes != NULL && layout->boxes->root != NULL) {
        putf(&b, "page %lld %lld\n", (long long)px_round(layout->width),
             (long long)px_round(layout->height));
        layout_box(&b, layout->boxes->root, 0);
    }
    if (layout != NULL && layout->incomplete)
        puts_(&b, "incomplete\n");
    if (cap > 0)
        out[b.len < cap ? b.len : cap - 1] = '\0';
    return (int64_t)b.len;
}
