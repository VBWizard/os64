// boxes.c — pass 2: the tree and its styles in, the box tree out.
//
// WHAT MAKES A BOX. A block-level element makes a block-level box; an
// inline element makes none of its own here, only the two edges of an
// inline box in its container's item sequence; `display: contents` makes
// nothing and its children are its parent's; `display: none` makes nothing
// and nothing inside it does either. Replaced elements — pictures, form
// controls, frames — are atomic: their insides are the face's widget or
// picture, never walked.
//
// THE TWO KINDS OF ANONYMOUS BOX CSS 2.1 REQUIRES are made here. A block container whose content mixes inline content and block
// boxes puts each inline run in an anonymous block (§9.2.1.1), and an
// inline that CONTAINS a block is split around it — the old web's
// `<font>` round every paragraph. Missing table structure gets anonymous
// table objects (§17.2.1). Whether a container mixes is decided BEFORE its
// children are built, from pass 1's holds-block bits, so a build that stops
// early is a prefix of the whole build (LAYOUT.md § Proof).
//
// Recursion follows the element tree, so it is bounded by libhtml's
// max_depth; a frame of it is small.

#include "internal.h"
#include "os64/fmt.h"

typedef struct {
    const os64_html_document_t *doc;
    const os64_page_t *model;
    const FStyles *styles;
    const flow_env_t *env;
    FBoxes *out;
    bool oom;
} B;

// A list's counter: the number the next item wears.
typedef struct {
    int32_t next;
    bool down;
} Scope;

// The inline elements open around the point the walk has reached,
// innermost first, so a block that splits them can close them and the
// content after it can open them again.
typedef struct Frame {
    FInline *inl;
    struct Frame *up;
} Frame;

// One block container's content being built.
typedef struct {
    B *b;
    FBox *container;
    bool mixed;             // inline runs go into anonymous blocks
    FBox *target;           // the context inline content lands in; NULL between runs
    Frame *open;
    FBox *anon_table;       // an anonymous table gathering internal table boxes
} Flow;

static void flow_range(Flow *f, const os64_html_node_t *first, const os64_html_node_t *stop,
                       Scope *scope);

// ── Reading the tree ────────────────────────────────────────────────────

static bool is(const os64_html_node_t *n, os64_html_tag_t tag)
{
    return n != NULL && n->kind == OS64_HTML_ELEMENT && n->ns == OS64_HTML_NS_HTML &&
           n->tag == tag;
}

static const FStyled *styled(const B *b, const os64_html_node_t *n)
{
    return f_style_of(b->styles, n);
}

static bool css_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool collapsible(flow_white_space_t ws)
{
    return ws == FLOW_WS_NORMAL || ws == FLOW_WS_NOWRAP;
}

// Replaced elements: laid out at a size the face supplies, their insides
// its widget or its picture.
static bool replaced(const os64_html_node_t *n)
{
    if (n->ns != OS64_HTML_NS_HTML)
        return false;
    switch (n->tag) {
    case OS64_HTML_TAG_IMG: case OS64_HTML_TAG_INPUT: case OS64_HTML_TAG_BUTTON:
    case OS64_HTML_TAG_SELECT: case OS64_HTML_TAG_TEXTAREA: case OS64_HTML_TAG_IFRAME:
    case OS64_HTML_TAG_FRAME: case OS64_HTML_TAG_METER: case OS64_HTML_TAG_PROGRESS:
        return true;
    default:
        return false;
    }
}

// Elements that make no box in this browser. An `embed` and an `audio`
// play nothing here and have no fallback content; `source` and `track`
// are data for a player; `keygen` is a control nothing implements.
static bool boxless(const os64_html_node_t *n)
{
    return is(n, OS64_HTML_TAG_EMBED) || is(n, OS64_HTML_TAG_AUDIO) ||
           is(n, OS64_HTML_TAG_SOURCE) || is(n, OS64_HTML_TAG_TRACK) ||
           is(n, OS64_HTML_TAG_KEYGEN);
}

static bool block_level(flow_display_t d)
{
    return d != FLOW_DISPLAY_INLINE && d != FLOW_DISPLAY_INLINE_BLOCK &&
           d != FLOW_DISPLAY_CONTENTS && d != FLOW_DISPLAY_NONE;
}

static bool table_internal(flow_display_t d)
{
    return d == FLOW_DISPLAY_TABLE_CAPTION || d == FLOW_DISPLAY_TABLE_ROW_GROUP ||
           d == FLOW_DISPLAY_TABLE_HEADER_GROUP || d == FLOW_DISPLAY_TABLE_FOOTER_GROUP ||
           d == FLOW_DISPLAY_TABLE_ROW || d == FLOW_DISPLAY_TABLE_CELL ||
           d == FLOW_DISPLAY_TABLE_COLUMN_GROUP || d == FLOW_DISPLAY_TABLE_COLUMN;
}

// The text of an SVG or MathML element is not prose: `wend`'s rule, kept.
static bool foreign_text(const os64_html_node_t *n)
{
    return n->kind == OS64_HTML_TEXT && n->parent != NULL &&
           n->parent->kind == OS64_HTML_ELEMENT && n->parent->ns != OS64_HTML_NS_HTML;
}

// A closed `details` shows its first `summary` and nothing else, text
// included; pass 1 already hid its other elements.
static bool hidden_by_details(const os64_html_node_t *n)
{
    return n->kind == OS64_HTML_TEXT && is(n->parent, OS64_HTML_TAG_DETAILS) &&
           os64_html_attr(n->parent, "open") == NULL;
}

// The style text is drawn in: its parent element's. NULL when the parent
// has none, and then the text makes nothing.
static const flow_style_t *text_style(const B *b, const os64_html_node_t *text)
{
    const FStyled *s = styled(b, text->parent);
    return s != NULL ? &s->style : NULL;
}

static bool text_significant(const B *b, const os64_html_node_t *n)
{
    const flow_style_t *s = text_style(b, n);
    if (s == NULL || n->text_len == 0)
        return false;
    if (!collapsible(s->white_space))
        return true;
    for (size_t i = 0; i < n->text_len; i++)
        if (!css_space(n->text[i]))
            return true;
    return false;
}

// ── Allocation ──────────────────────────────────────────────────────────

static void *alloc(B *b, size_t size)
{
    if (b->oom)
        return NULL;
    void *p = f_arena_alloc(&b->out->arena, size);
    if (p == NULL)
        b->oom = true;
    return p;
}

static FBox *new_box(B *b, FBox *parent, f_box_kind_t kind, const os64_html_node_t *node,
                     const flow_style_t *style)
{
    FBox *box = alloc(b, sizeof(*box));
    if (box == NULL)
        return NULL;
    box->kind = kind;
    box->node = node;
    box->style = style;
    box->link = -1;
    box->parent = parent;
    // Attached at once, so a build that stops here leaves a tree that is
    // whole up to this box.
    if (parent != NULL) {
        if (parent->last != NULL)
            parent->last->next = box;
        else
            parent->first = box;
        parent->last = box;
    }
    return box;
}

static const flow_style_t *anon_style(B *b, const flow_style_t *parent, flow_display_t display)
{
    flow_style_t *s = alloc(b, sizeof(*s));
    if (s != NULL)
        f_style_anonymous(b->styles, parent, display, s);
    return s;
}

static FItem *new_item(B *b, FBox *ifc, f_item_kind_t kind, const os64_html_node_t *node,
                       const flow_style_t *style)
{
    FItem *item = alloc(b, sizeof(*item));
    if (item == NULL)
        return NULL;
    item->kind = kind;
    item->node = node;
    item->style = style;
    item->link = item->control = -1;
    if (ifc->last_item != NULL)
        ifc->last_item->next = item;
    else
        ifc->items = item;
    ifc->last_item = item;
    ifc->nitems++;
    return item;
}

// ── The inline formatting context ───────────────────────────────────────

static void reopen(Flow *f, Frame *fr)
{
    if (fr == NULL || f->b->oom)
        return;
    reopen(f, fr->up);
    FItem *item = new_item(f->b, f->target, FI_OPEN, fr->inl->node, fr->inl->style);
    if (item == NULL)
        return;
    item->inl = fr->inl;
    item->continuation = fr->inl->pieces > 0;
    fr->inl->pieces++;
    fr->inl->open_in = f->target;
}

// Where inline content goes: the container itself when it holds nothing
// but inline content, else an anonymous block made on the first content
// of a run — never earlier, so a run of nothing but collapsible space
// makes no box — with the inline elements still open around this point
// opened again inside it.
static FBox *target(Flow *f)
{
    if (f->target != NULL || f->b->oom)
        return f->target;
    f->anon_table = NULL;
    FBox *anon = new_box(f->b, f->container, FB_BLOCK, NULL,
                         anon_style(f->b, f->container->style, FLOW_DISPLAY_BLOCK));
    if (anon == NULL)
        return NULL;
    anon->ifc = true;
    anon->collapse_space = true;
    f->target = anon;
    reopen(f, f->open);
    return f->target;
}

// A block interrupts the run: every inline open in it is closed there, to
// be opened again after the block.
static void end_run(Flow *f)
{
    if (!f->mixed || f->target == NULL)
        return;
    for (Frame *fr = f->open; fr != NULL && !f->b->oom; fr = fr->up) {
        if (fr->inl->open_in != f->target)
            continue;
        FItem *item = new_item(f->b, f->target, FI_CLOSE, fr->inl->node, fr->inl->style);
        if (item != NULL) {
            item->inl = fr->inl;
            item->continuation = true;
        }
        fr->inl->open_in = NULL;
    }
    f->target = NULL;
}

static void text_item(Flow *f, FBox *ifc, const os64_html_node_t *node,
                      const flow_style_t *style, const char *text, uint32_t len,
                      uint32_t offset, bool generated)
{
    FItem *item = new_item(f->b, ifc, FI_TEXT, node, style);
    if (item == NULL)
        return;
    item->text = text;
    item->len = len;
    item->offset = offset;
    item->generated = generated;
}

// White-space processing (CSS 2.1 §16.6.1), once, here. Under `normal` and
// `nowrap` a tab, a newline or a run of either becomes one space, and a
// space that follows a collapsible space ANYWHERE in the same inline
// formatting context — across element boundaries — is removed; a context
// starts as if after a space, which is the rule removing a line's leading
// space applied to its first line. Under `pre` and `pre-wrap` every byte
// is kept and a newline is a forced break.
static void text_node(Flow *f, const os64_html_node_t *n)
{
    B *b = f->b;
    const flow_style_t *s = text_style(b, n);
    if (s == NULL || n->text_len == 0)
        return;
    if (!collapsible(s->white_space)) {
        FBox *ifc = target(f);
        if (ifc == NULL)
            return;
        uint32_t start = 0;
        for (uint32_t i = 0; i <= (uint32_t)n->text_len && !b->oom; i++) {
            if (i < n->text_len && n->text[i] != '\n')
                continue;
            if (i > start)
                text_item(f, ifc, n, s, n->text + start, i - start, start, false);
            if (i < n->text_len && new_item(b, ifc, FI_BREAK, NULL, s) == NULL)
                return;
            start = i + 1;
        }
        ifc->collapse_space = false;
        return;
    }
    // Nothing but collapsible space: it opens no run, and inside one it is
    // one space, or none after another.
    if (!text_significant(b, n)) {
        if (f->target != NULL && !f->target->collapse_space) {
            text_item(f, f->target, n, s, " ", 1, 0, false);
            f->target->collapse_space = true;
        }
        return;
    }
    FBox *ifc = target(f);
    if (ifc == NULL)
        return;
    char *copy = alloc(b, n->text_len);
    if (copy == NULL)
        return;
    uint32_t len = 0;
    bool space = ifc->collapse_space;
    for (size_t i = 0; i < n->text_len; i++) {
        if (css_space(n->text[i])) {
            if (!space)
                copy[len++] = ' ';
            space = true;
        } else {
            copy[len++] = n->text[i];
            space = false;
        }
    }
    ifc->collapse_space = space;
    // Point into the tree when processing changed nothing: the box tree
    // copies no text it can point at.
    const char *text = len == n->text_len && os64_memcmp(copy, n->text, len) == 0 ? n->text
                                                                               : copy;
    if (len > 0)
        text_item(f, ifc, n, s, text, len, 0, false);
}

static void generated(Flow *f, const os64_html_node_t *el, const flow_style_t *s,
                      const char *text)
{
    FBox *ifc = target(f);
    if (ifc == NULL)
        return;
    text_item(f, ifc, el, s, text, (uint32_t)os64_strlen(text), 0, true);
    ifc->collapse_space = false;
}

// ── List markers ────────────────────────────────────────────────────────

static uint32_t roman(int32_t v, bool upper, char *out)
{
    static const struct { int32_t v; const char *lo, *up; } k[] = {
        {1000, "m", "M"}, {900, "cm", "CM"}, {500, "d", "D"}, {400, "cd", "CD"},
        {100, "c", "C"}, {90, "xc", "XC"}, {50, "l", "L"}, {40, "xl", "XL"},
        {10, "x", "X"}, {9, "ix", "IX"}, {5, "v", "V"}, {4, "iv", "IV"}, {1, "i", "I"},
    };
    uint32_t n = 0;
    for (int32_t i = 0; i < F_ARRAY(k); i++)
        while (v >= k[i].v) {
            const char *s = upper ? k[i].up : k[i].lo;
            while (*s != '\0')
                out[n++] = *s++;
            v -= k[i].v;
        }
    return n;
}

static uint32_t alpha(int32_t v, bool upper, char *out)
{
    // Bijective base 26: z is 26, aa is 27.
    char tmp[16];
    uint32_t n = 0;
    while (v > 0 && n < sizeof(tmp)) {
        v--;
        tmp[n++] = (char)((upper ? 'A' : 'a') + v % 26);
        v /= 26;
    }
    for (uint32_t i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

// The marker's text, suffix included: CSS Counter Styles' symbols and
// suffixes. A number a style cannot spell — zero or less in letters, past
// 3999 in Roman numerals — falls back to decimal, as the styles define.
static const char *marker_text(B *b, flow_list_style_type_t type, int32_t value, uint32_t *len)
{
    const char *fixed = NULL;
    switch (type) {
    case FLOW_LIST_NONE: return NULL;
    case FLOW_LIST_DISC: fixed = "\xe2\x80\xa2 "; break;               // U+2022
    case FLOW_LIST_CIRCLE: fixed = "\xe2\x97\xa6 "; break;             // U+25E6
    case FLOW_LIST_SQUARE: fixed = "\xe2\x96\xaa "; break;             // U+25AA
    case FLOW_LIST_DISCLOSURE_CLOSED: fixed = "\xe2\x96\xb8 "; break;  // U+25B8
    case FLOW_LIST_DISCLOSURE_OPEN: fixed = "\xe2\x96\xbe "; break;    // U+25BE
    default: break;
    }
    if (fixed != NULL) {
        *len = (uint32_t)os64_strlen(fixed);
        return fixed;
    }
    char *out = alloc(b, 24);
    if (out == NULL)
        return NULL;
    uint32_t n = 0;
    bool upper = type == FLOW_LIST_UPPER_ALPHA || type == FLOW_LIST_UPPER_ROMAN;
    if ((type == FLOW_LIST_LOWER_ALPHA || type == FLOW_LIST_UPPER_ALPHA) && value > 0)
        n = alpha(value, upper, out);
    else if ((type == FLOW_LIST_LOWER_ROMAN || type == FLOW_LIST_UPPER_ROMAN) && value > 0 &&
             value < 4000)
        n = roman(value, upper, out);
    else
        n = (uint32_t)os64_snprintf(out, 16, "%d", (int)value);
    out[n++] = '.';
    out[n++] = ' ';
    *len = n;
    return out;
}

// How many list items a reversed list counts down from: the items in its
// scope, which ends at a nested list.
static int32_t count_items(const B *b, const os64_html_node_t *list)
{
    int32_t count = 0;
    const os64_html_node_t *cur = list->first_child;
    while (cur != NULL) {
        const os64_html_node_t *down = NULL;
        if (cur->kind == OS64_HTML_ELEMENT) {
            const FStyled *s = styled(b, cur);
            if (s != NULL && s->style.display == FLOW_DISPLAY_LIST_ITEM && is(cur, OS64_HTML_TAG_LI))
                count++;
            bool nested = is(cur, OS64_HTML_TAG_OL) || is(cur, OS64_HTML_TAG_UL) ||
                          is(cur, OS64_HTML_TAG_MENU);
            if (s != NULL && s->style.display != FLOW_DISPLAY_NONE && !nested)
                down = cur->first_child;
        }
        if (down != NULL) {
            cur = down;
            continue;
        }
        while (cur != NULL && cur != list && cur->next == NULL)
            cur = cur->parent;
        cur = cur != NULL && cur != list ? cur->next : NULL;
    }
    return count;
}

// ── Mixing ──────────────────────────────────────────────────────────────

typedef struct {
    bool block, inline_content;
} Mix;

static void scan(const B *b, const os64_html_node_t *first, const os64_html_node_t *stop,
                 Mix *m)
{
    for (const os64_html_node_t *c = first; c != stop && !(m->block && m->inline_content);
         c = c->next) {
        if (c->kind == OS64_HTML_TEXT) {
            if (!foreign_text(c) && !hidden_by_details(c) && text_significant(b, c))
                m->inline_content = true;
            continue;
        }
        if (c->kind != OS64_HTML_ELEMENT || boxless(c))
            continue;
        const FStyled *s = styled(b, c);
        if (s == NULL || s->style.display == FLOW_DISPLAY_NONE)
            continue;
        if (s->style.display == FLOW_DISPLAY_CONTENTS) {
            scan(b, c->first_child, NULL, m);
        } else if (block_level(s->style.display)) {
            m->block = true;
        } else {
            m->inline_content = true;
            if (s->holds_block && !replaced(c))
                m->block = true;
        }
    }
}

// ── Building ────────────────────────────────────────────────────────────

static void table_range(B *b, FBox *table, const os64_html_node_t *first,
                        const os64_html_node_t *stop, Scope *scope);

// A block container's content from a range of sibling nodes.
static void build_container(B *b, FBox *box, const os64_html_node_t *first,
                            const os64_html_node_t *stop, Scope *scope,
                            const char *inside_marker, uint32_t inside_len)
{
    Mix m = {false, inside_marker != NULL};
    scan(b, first, stop, &m);
    Flow f = {b, box, m.block, NULL, NULL, NULL};
    if (!f.mixed) {
        box->ifc = true;
        box->collapse_space = true;
        f.target = box;
    }
    if (inside_marker != NULL) {
        FBox *ifc = target(&f);
        FItem *item = ifc != NULL ? new_item(b, ifc, FI_MARKER, box->node, box->style) : NULL;
        if (item != NULL) {
            item->text = inside_marker;
            item->len = inside_len;
        }
    }
    flow_range(&f, first, stop, scope);
    end_run(&f);
}

// The link a block sits inside when it splits an `a`: `<a href><div>` is
// a clickable block on every page that writes it, and once the `a` is
// split round the block no inline edge is left to say so.
static int32_t enclosing_link(const Flow *f)
{
    for (const Frame *fr = f->open; fr != NULL; fr = fr->up)
        if (fr->inl->link >= 0)
            return fr->inl->link;
    return -1;
}

static void block_box(Flow *f, const os64_html_node_t *el, const FStyled *s, Scope *scope)
{
    B *b = f->b;
    end_run(f);
    f->anon_table = NULL;
    if (replaced(el)) {
        FBox *box = new_box(b, f->container, FB_REPLACED, el, &s->style);
        if (box != NULL && b->model != NULL)
            box->link = os64_page_link_for(b->model, el);
        return;
    }
    if (s->style.display == FLOW_DISPLAY_TABLE) {
        FBox *table = new_box(b, f->container, FB_TABLE, el, &s->style);
        if (table != NULL) {
            table->link = enclosing_link(f);
            table_range(b, table, el->first_child, NULL, scope);
        }
        return;
    }
    // A list resets the list-item counter (`ol, ul, menu`, not `dir`: the
    // chapter's sheet), and an item takes the next number.
    Scope inner = *scope;
    Scope *use = scope;
    if (is(el, OS64_HTML_TAG_OL) || is(el, OS64_HTML_TAG_UL) || is(el, OS64_HTML_TAG_MENU)) {
        // `start` is the first item's number, reversed or not; a reversed
        // list without one counts down from how many items it has.
        const os64_html_attr_t *start = is(el, OS64_HTML_TAG_OL)
                                            ? os64_html_attr(el, "start") : NULL;
        inner.down = is(el, OS64_HTML_TAG_OL) && os64_html_attr(el, "reversed") != NULL;
        if (start == NULL || !f_parse_integer(start->value, &inner.next))
            inner.next = inner.down ? count_items(b, el) : 1;
        use = &inner;
    }
    // The marker is made BEFORE the box, so running out of memory for it
    // leaves no box behind that a whole build would have drawn with one.
    const char *marker = NULL;
    uint32_t marker_len = 0;
    if (s->style.display == FLOW_DISPLAY_LIST_ITEM) {
        // An `li` takes the next number, or the one its `value` names, and
        // the items after it count on from there. A summary is a list item
        // that counts nothing: its marker is the disclosure triangle.
        int32_t value = 0;
        if (is(el, OS64_HTML_TAG_LI)) {
            const os64_html_attr_t *v = os64_html_attr(el, "value");
            if (v != NULL)
                (void)f_parse_integer(v->value, &scope->next);
            value = scope->next;
            scope->next += scope->down ? -1 : 1;
        }
        marker = marker_text(b, s->style.list_style_type, value, &marker_len);
        if (b->oom)
            return;
    }
    FBox *box = new_box(b, f->container, FB_BLOCK, el, &s->style);
    if (box == NULL)
        return;
    box->link = enclosing_link(f);
    const char *inside = NULL;
    uint32_t inside_len = 0;
    if (marker != NULL && s->style.list_style_position == FLOW_LIST_INSIDE) {
        inside = marker;
        inside_len = marker_len;
    } else if (marker != NULL) {
        box->marker = marker;
        box->marker_len = marker_len;
    }
    build_container(b, box, el->first_child, NULL, use, inside, inside_len);
}

static void inline_element(Flow *f, const os64_html_node_t *el, const FStyled *s, Scope *scope)
{
    B *b = f->b;
    if (replaced(el) || s->style.display == FLOW_DISPLAY_INLINE_BLOCK) {
        FBox *ifc = target(f);
        FItem *item = ifc != NULL ? new_item(b, ifc, FI_ATOMIC, el, &s->style) : NULL;
        if (item == NULL)
            return;
        if (b->model != NULL) {
            item->link = os64_page_link_for(b->model, el);
            item->control = os64_page_control_for(b->model, el);
        }
        ifc->collapse_space = false;
        // An inline-block that is not replaced (a marquee) is a block of its
        // own, laid out inside the atom.
        if (!replaced(el)) {
            FBox *content = new_box(b, NULL, FB_BLOCK, el, &s->style);
            if (content != NULL) {
                content->parent = ifc;
                item->content = content;
                build_container(b, content, el->first_child, NULL, scope, NULL, 0);
            }
        }
        return;
    }
    if (is(el, OS64_HTML_TAG_BR)) {
        FBox *ifc = target(f);
        if (ifc != NULL && new_item(b, ifc, FI_BREAK, el, &s->style) != NULL)
            ifc->collapse_space = true;
        return;
    }
    if (is(el, OS64_HTML_TAG_WBR)) {
        // A break opportunity where there is already a line to break.
        if (f->target != NULL)
            new_item(b, f->target, FI_WBR, el, &s->style);
        return;
    }

    FInline *inl = alloc(b, sizeof(*inl));
    if (inl == NULL)
        return;
    inl->node = el;
    inl->style = &s->style;
    inl->link = b->model != NULL && (is(el, OS64_HTML_TAG_A) || is(el, OS64_HTML_TAG_AREA))
                    ? os64_page_link_for(b->model, el) : -1;
    Frame frame = {inl, f->open};
    f->open = &frame;
    if (f->target != NULL) {
        FItem *item = new_item(b, f->target, FI_OPEN, el, &s->style);
        if (item != NULL) {
            item->inl = inl;
            inl->pieces++;
            inl->open_in = f->target;
        }
    }

    // Generated content the sheet asks for: `q`'s quotation marks, nested
    // levels alternating double and single; and the parentheses a browser
    // that lays out no ruby puts round an `rt` whose ruby has no `rp`.
    const char *before = NULL, *after = NULL;
    if (is(el, OS64_HTML_TAG_Q)) {
        int32_t level = 0;
        for (const os64_html_node_t *p = el->parent; p != NULL; p = p->parent)
            level += is(p, OS64_HTML_TAG_Q);
        before = level % 2 == 0 ? "\xe2\x80\x9c" : "\xe2\x80\x98";   // U+201C, U+2018
        after = level % 2 == 0 ? "\xe2\x80\x9d" : "\xe2\x80\x99";    // U+201D, U+2019
    } else if (is(el, OS64_HTML_TAG_RT) && el->parent != NULL) {
        bool rp = false;
        for (const os64_html_node_t *c = el->parent->first_child; c != NULL; c = c->next)
            rp |= is(c, OS64_HTML_TAG_RP);
        if (!rp) {
            before = "(";
            after = ")";
        }
    }
    if (before != NULL)
        generated(f, el, &s->style, before);
    flow_range(f, el->first_child, NULL, scope);
    if (after != NULL)
        generated(f, el, &s->style, after);

    if (inl->open_in != NULL && inl->open_in == f->target) {
        FItem *item = new_item(b, f->target, FI_CLOSE, el, &s->style);
        if (item != NULL)
            item->inl = inl;
    }
    inl->open_in = NULL;
    f->open = frame.up;
}

static void flow_range(Flow *f, const os64_html_node_t *first, const os64_html_node_t *stop,
                       Scope *scope)
{
    B *b = f->b;
    for (const os64_html_node_t *c = first; c != stop && !b->oom; c = c->next) {
        if (c->kind == OS64_HTML_TEXT) {
            if (foreign_text(c) || hidden_by_details(c))
                continue;
            // Collapsible space between internal table boxes belongs to
            // neither (§17.2.1's first stage); anything else ends the run.
            if (f->anon_table != NULL && !text_significant(b, c))
                continue;
            f->anon_table = NULL;
            text_node(f, c);
            continue;
        }
        if (c->kind != OS64_HTML_ELEMENT || boxless(c))
            continue;
        const FStyled *s = styled(b, c);
        if (s == NULL || s->style.display == FLOW_DISPLAY_NONE)
            continue;
        flow_display_t d = s->style.display;
        if (d == FLOW_DISPLAY_CONTENTS) {
            flow_range(f, c->first_child, NULL, scope);
        } else if (table_internal(d)) {
            // An internal table box with no table round it gets an
            // anonymous one, shared with the internal boxes beside it.
            if (f->anon_table == NULL) {
                end_run(f);
                f->anon_table = new_box(b, f->container, FB_TABLE, NULL,
                                        anon_style(b, f->container->style, FLOW_DISPLAY_TABLE));
            }
            if (f->anon_table != NULL)
                table_range(b, f->anon_table, c, c->next, scope);
        } else if (block_level(d)) {
            block_box(f, c, s, scope);
        } else {
            f->anon_table = NULL;
            inline_element(f, c, s, scope);
        }
    }
}

// ── Tables (CSS 2.1 §17.2.1) ────────────────────────────────────────────

static bool proper_table_child(flow_display_t d)
{
    return d == FLOW_DISPLAY_TABLE_CAPTION || d == FLOW_DISPLAY_TABLE_ROW_GROUP ||
           d == FLOW_DISPLAY_TABLE_HEADER_GROUP || d == FLOW_DISPLAY_TABLE_FOOTER_GROUP ||
           d == FLOW_DISPLAY_TABLE_ROW || d == FLOW_DISPLAY_TABLE_COLUMN_GROUP ||
           d == FLOW_DISPLAY_TABLE_COLUMN;
}

static flow_display_t display_of(const B *b, const os64_html_node_t *n)
{
    if (n->kind != OS64_HTML_ELEMENT || boxless(n))
        return FLOW_DISPLAY_NONE;
    const FStyled *s = styled(b, n);
    return s != NULL ? s->style.display : FLOW_DISPLAY_NONE;
}

// What each level of a table takes as its own child; everything else there
// is LOOSE and gets the anonymous parents it lacks — a row round it in a
// table or a row group, a cell round it in a row.
typedef enum { AT_TABLE, AT_GROUP, AT_ROW } Level;

static bool structural(Level level, const os64_html_node_t *n, flow_display_t d)
{
    if (n->kind != OS64_HTML_ELEMENT)
        return false;
    switch (level) {
    case AT_TABLE: return proper_table_child(d) || d == FLOW_DISPLAY_TABLE_CELL;
    case AT_GROUP: return d == FLOW_DISPLAY_TABLE_ROW || d == FLOW_DISPLAY_TABLE_CELL;
    case AT_ROW: return d == FLOW_DISPLAY_TABLE_CELL;
    }
    return false;
}

// Loose content that needs a box: collapsible space between table parts
// belongs to none of them (§17.2.1's first stage), and a node that makes no
// box needs none.
static bool loose(const B *b, Level level, const os64_html_node_t *n)
{
    if (n->kind == OS64_HTML_TEXT)
        return !foreign_text(n) && text_significant(b, n);
    flow_display_t d = display_of(b, n);
    return d != FLOW_DISPLAY_NONE && !structural(level, n, d);
}

static const os64_html_node_t *loose_end(const B *b, Level level, const os64_html_node_t *n,
                                         const os64_html_node_t *stop)
{
    while (n != stop && !structural(level, n, display_of(b, n)))
        n = n->next;
    return n;
}

static FBox *anon_part(B *b, FBox *parent, f_box_kind_t kind, flow_display_t display)
{
    return new_box(b, parent, kind, NULL, anon_style(b, parent->style, display));
}

// A row's children: cells, and runs of loose content each in an anonymous
// cell, which lays the run out as any block container would — a table part
// inside it gets an anonymous table of its own.
static void row_range(B *b, FBox *row, const os64_html_node_t *first,
                      const os64_html_node_t *stop, Scope *scope)
{
    for (const os64_html_node_t *c = first; c != stop && !b->oom;) {
        if (structural(AT_ROW, c, display_of(b, c))) {
            FBox *cell = new_box(b, row, FB_CELL, c, &styled(b, c)->style);
            if (cell != NULL)
                build_container(b, cell, c->first_child, NULL, scope, NULL, 0);
            c = c->next;
        } else if (loose(b, AT_ROW, c)) {
            const os64_html_node_t *end = loose_end(b, AT_ROW, c, stop);
            FBox *cell = anon_part(b, row, FB_CELL, FLOW_DISPLAY_TABLE_CELL);
            if (cell != NULL)
                build_container(b, cell, c, end, scope, NULL, 0);
            c = end;
        } else {
            c = c->next;
        }
    }
}

static void rows_range(B *b, FBox *parent, Level level, const os64_html_node_t *first,
                       const os64_html_node_t *stop, Scope *scope);

// A table's own part: a caption, a column group with its columns, a lone
// column, a row group, a row.
static void table_part(B *b, FBox *table, const os64_html_node_t *c, flow_display_t d,
                       Scope *scope)
{
    const flow_style_t *s = &styled(b, c)->style;
    switch (d) {
    case FLOW_DISPLAY_TABLE_CAPTION: {
        FBox *cap = new_box(b, table, FB_CAPTION, c, s);
        if (cap != NULL)
            build_container(b, cap, c->first_child, NULL, scope, NULL, 0);
        break;
    }
    case FLOW_DISPLAY_TABLE_COLUMN_GROUP: {
        // A column group holds columns, and nothing else in it counts.
        FBox *group = new_box(b, table, FB_COLUMN_GROUP, c, s);
        for (const os64_html_node_t *k = c->first_child; k != NULL && group != NULL; k = k->next)
            if (display_of(b, k) == FLOW_DISPLAY_TABLE_COLUMN)
                new_box(b, group, FB_COLUMN, k, &styled(b, k)->style);
        break;
    }
    case FLOW_DISPLAY_TABLE_COLUMN:
        new_box(b, table, FB_COLUMN, c, s);
        break;
    case FLOW_DISPLAY_TABLE_ROW: {
        FBox *row = new_box(b, table, FB_ROW, c, s);
        if (row != NULL)
            row_range(b, row, c->first_child, NULL, scope);
        break;
    }
    default: {
        FBox *group = new_box(b, table, FB_ROW_GROUP, c, s);
        if (group != NULL)
            rows_range(b, group, AT_GROUP, c->first_child, NULL, scope);
        break;
    }
    }
}

// A table's or a row group's children: its own parts, and runs of cells or
// loose content, consecutive ones sharing one anonymous row.
static void rows_range(B *b, FBox *parent, Level level, const os64_html_node_t *first,
                       const os64_html_node_t *stop, Scope *scope)
{
    FBox *anon_row = NULL;
    for (const os64_html_node_t *c = first; c != stop && !b->oom;) {
        flow_display_t d = display_of(b, c);
        if (structural(level, c, d) && d != FLOW_DISPLAY_TABLE_CELL) {
            anon_row = NULL;
            table_part(b, parent, c, d, scope);
            c = c->next;
            continue;
        }
        if (d == FLOW_DISPLAY_TABLE_CELL || loose(b, level, c)) {
            if (anon_row == NULL)
                anon_row = anon_part(b, parent, FB_ROW, FLOW_DISPLAY_TABLE_ROW);
            if (anon_row == NULL)
                return;
            const os64_html_node_t *end = d == FLOW_DISPLAY_TABLE_CELL
                                              ? c->next : loose_end(b, level, c, stop);
            row_range(b, anon_row, c, end, scope);
            c = end;
            continue;
        }
        c = c->next;
    }
}

static void table_range(B *b, FBox *table, const os64_html_node_t *first,
                        const os64_html_node_t *stop, Scope *scope)
{
    rows_range(b, table, AT_TABLE, first, stop, scope);
}

// ── The build ───────────────────────────────────────────────────────────

FBoxes *f_boxes_build(const os64_html_document_t *doc, const os64_page_t *model,
                      const FStyles *styles, const flow_env_t *env)
{
    if (doc == NULL || styles == NULL || env == NULL)
        return NULL;
    FBoxes *out = os64_calloc(1, sizeof(*out));
    if (out == NULL)
        return NULL;
    B b = {doc, model, styles, env, out, false};
    const os64_html_node_t *html = doc->html;
    const FStyled *root = html != NULL ? styled(&b, html) : NULL;
    if (root != NULL && root->style.display != FLOW_DISPLAY_NONE) {
        out->root = new_box(&b, NULL, FB_BLOCK, html, &root->style);
        Scope scope = {1, false};
        if (out->root != NULL)
            build_container(&b, out->root, html->first_child, NULL, &scope, NULL, 0);
    }
    out->incomplete = b.oom;
    return out;
}

void f_boxes_free(FBoxes *boxes)
{
    if (boxes == NULL)
        return;
    f_arena_free(&boxes->arena);
    os64_free(boxes);
}
