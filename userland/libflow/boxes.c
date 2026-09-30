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
// AN OUT-OF-FLOW BOX (POSITION.md) is built where it was written, hung under
// the formatting context it was written in, and held by none: no line and
// no anonymous block is made round it, and its container's mix is decided
// without it. Among inline content it leaves a PLACEHOLDER item, which is
// where its static position will be read. Every element that is a
// containing block — positioned, and making a box of its own — gets an
// entry (FPos) in tree order, and each out-of-flow box is filed under its
// containing block's entry.
//
// Recursion follows the element tree. Its depth is bounded HERE, by
// F_DEPTH_MAX, not by libhtml's max_depth, which is the caller's option:
// every later pass recurses along the boxes this builds, so this is the
// bound that keeps the whole layout inside a thread's stack.

#include "internal.h"
#include "os64/fmt.h"

typedef struct {
    const os64_html_document_t *doc;
    const os64_page_t *model;
    const FStyles *styles;
    const flow_env_t *env;
    FBoxes *out;
    int32_t depth;      // descents open, each costed as a block level (F_DEPTH_MAX)
    bool stopped;       // memory or the arena's budget ran out, or the page nests past F_DEPTH_MAX
    FMap pos_of;        // element -> its positioning entry, for an absolute box's containing block
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


uint32_t f_collapse_white(const char *in, uint32_t n, char *out, bool *space)
{
    uint32_t len = 0;
    bool sp = *space;
    for (uint32_t i = 0; i < n; i++) {
        if (css_space(in[i])) {
            if (!sp)
                out[len++] = ' ';
            sp = true;
        } else {
            out[len++] = in[i];
            sp = false;
        }
    }
    *space = sp;
    return len;
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
    if (!f_ws_collapses(s->white_space))
        return true;
    // Under pre-line a line break is kept, so it is content.
    for (size_t i = 0; i < n->text_len; i++)
        if (!css_space(n->text[i]) || (n->text[i] == '\n' && s->white_space == FLOW_WS_PRE_LINE))
            return true;
    return false;
}

// ── Depth ───────────────────────────────────────────────────────────────

// Every recursive descent of the build over the TREE opens with descend()
// and closes with ascend(), and a descent that costs more stack than a
// block level opens more than one. reopen() recurses too, over the inlines
// open at a point, which the descents that opened them already count. Past F_DEPTH_MAX the build stops, as it does when memory
// runs out, so what it made is still a prefix of the whole build.
static bool descend(B *b)
{
    if (b->stopped)
        return false;
    if (b->depth >= F_DEPTH_MAX) {
        b->stopped = true;
        return false;
    }
    b->depth++;
    return true;
}

static void ascend(B *b)
{
    b->depth--;
}

// ── Allocation ──────────────────────────────────────────────────────────

// A box whose content the build could not finish says so, and so does
// every box above it: they are the path to where the build stopped, and
// layout must not present them as whole (LAYOUT.md § Proof, rule (c)).
static void mark_cut(const B *b, FBox *box)
{
    if (b->stopped && box != NULL)
        box->unfinished = true;
}

static void *alloc(B *b, size_t size)
{
    if (b->stopped)
        return NULL;
    void *p = f_arena_alloc(&b->out->arena, size);
    if (p == NULL)
        b->stopped = true;
    return p;
}

// A box is in the link its node is or sits in (FStyled.link), read off the
// element tree and not the inline edges open where the box is built,
// because a link has no edge a box could fall outside: `<a href><div>` is a
// clickable block once the `a` is split round it, and an inline-block's
// content is inside the `a` its atom is. A box with no node of its own —
// an anonymous block, an anonymous table part — is in its parent's.
static FBox *new_box(B *b, FBox *parent, f_box_kind_t kind, const os64_html_node_t *node,
                     const flow_style_t *style)
{
    FBox *box = alloc(b, sizeof(*box));
    if (box == NULL)
        return NULL;
    box->kind = kind;
    box->node = node;
    box->style = style;
    const FStyled *st = node != NULL ? styled(b, node) : NULL;
    box->link = st != NULL ? st->link : parent != NULL ? parent->link : -1;
    box->control = -1;
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

// ── Positioning ─────────────────────────────────────────────────────────

// An entry for an element that is a containing block (f_contains_absolute),
// appended in tree order and found again by its node.
static FPos *pos_add(B *b, f_pos_kind_t kind, const os64_html_node_t *node,
                     const flow_style_t *style)
{
    FPos *p = alloc(b, sizeof(*p));
    if (p == NULL)
        return NULL;
    if (!f_map_put(&b->pos_of, node, p)) {
        b->stopped = true;
        return NULL;
    }
    p->kind = kind;
    p->node = node;
    p->style = style;
    if (b->out->last_positioned != NULL)
        b->out->last_positioned->next = p;
    else
        b->out->positioned = p;
    b->out->last_positioned = p;
    return p;
}

// A box made for element `el`: its entry when it is a containing block,
// and, when it is out of flow, its place in its containing block's list —
// the entry of its parent's anchor (FStyled.anchor), or the initial
// containing block's. `block_level` is false for an inline-block's
// content, which is a containing block but is reached through its atom,
// and for the root, which is where every walk starts.
static void position_box(B *b, FBox *box, const os64_html_node_t *el, bool block_level)
{
    if (box == NULL || !f_contains_absolute(box->style))
        return;
    // What the box is comes from its style, entry or none: a build that
    // stops here leaves an out-of-flow box filed nowhere, which is never
    // laid out, rather than one laid out in a flow the whole build takes it
    // out of (LAYOUT.md § Proof, the prefix rule).
    box->positioned = block_level;
    box->out_of_flow = block_level && f_out_of_flow(box->style);
    FPos *p = pos_add(b, FP_BOX, el, box->style);
    if (p == NULL)
        return;
    p->box = box;
    box->pos = p;
    if (!box->out_of_flow)
        return;
    // A fixed box's containing block is the viewport, whatever is round it
    // (a transform would change that, and is booked): it is filed with the
    // initial containing block's, whose rect is the viewport's too.
    const FStyled *up = styled(b, el->parent);
    bool fixed = box->style->position == FLOW_POSITION_FIXED;
    p->cb = !fixed && up != NULL && up->anchor != NULL ? f_map_get(&b->pos_of, up->anchor) : NULL;
    FPos **first = p->cb != NULL ? &p->cb->abs_first : &b->out->icb_first;
    FPos **last = p->cb != NULL ? &p->cb->abs_last : &b->out->icb_last;
    if (*last != NULL)
        (*last)->abs_next = p;
    else
        *first = p;
    *last = p;
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
    if (fr == NULL || f->b->stopped)
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
    if (f->target != NULL || f->b->stopped)
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
    for (Frame *fr = f->open; fr != NULL && !f->b->stopped; fr = fr->up) {
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

// text-transform (CSS Text 3 § 2.1), on the letters whose other case is
// the same length in UTF-8 — ASCII's and Latin-1's — so an item keeps its
// offsets into its node. A letter whose other case is longer (ß, ÿ) and
// every letter past Latin-1 is left as it is (GARB.md § Booked).
// `capitalize` raises the first letter after a space, or at `word_start`.
static void transform(char *t, uint32_t n, flow_text_transform_t how, bool word_start)
{
    for (uint32_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)t[i];
        bool up = how == FLOW_TRANSFORM_UPPERCASE || (how == FLOW_TRANSFORM_CAPITALIZE && word_start);
        bool down = how == FLOW_TRANSFORM_LOWERCASE;
        if (c < 0x80) {
            if (up && c >= 'a' && c <= 'z')
                t[i] = (char)(c - 32);
            else if (down && c >= 'A' && c <= 'Z')
                t[i] = (char)(c + 32);
            word_start = css_space((char)c);
        } else if (c == 0xC3 && i + 1 < n) {
            // U+00C0..U+00FF: the second byte's 0x20 bit is the case, but
            // for × and ÷ (0x97, 0xB7), ß (0x9F) and ÿ (0xBF).
            unsigned char d = (unsigned char)t[i + 1];
            if (up && d >= 0xA0 && d <= 0xBE && d != 0xB7)
                t[i + 1] = (char)(d - 0x20);
            else if (down && d >= 0x80 && d <= 0x9E && d != 0x97)
                t[i + 1] = (char)(d + 0x20);
            i++;
            word_start = false;
        } else if (c >= 0xC0) {
            word_start = false;
        }
    }
}

static void text_item(Flow *f, FBox *ifc, const os64_html_node_t *node,
                      const flow_style_t *style, const char *text, uint32_t len,
                      uint32_t offset, bool generated)
{
    if (!generated && len > 0 && style->text_transform != FLOW_TRANSFORM_NONE) {
        char *copy = alloc(f->b, len);
        if (copy == NULL)
            return;
        os64_memcpy(copy, text, len);
        const FItem *last = ifc->last_item;
        bool word_start = last == NULL || last->kind == FI_BREAK ||
                          (last->kind == FI_TEXT && last->len > 0 &&
                           css_space(last->text[last->len - 1]));
        transform(copy, len, style->text_transform, word_start);
        text = copy;
    }
    FItem *item = new_item(f->b, ifc, FI_TEXT, node, style);
    if (item == NULL)
        return;
    item->text = text;
    item->len = len;
    item->offset = offset;
    item->generated = generated;
}

// White-space processing (CSS 2.1 §16.6.1), once, here. Under `normal`,
// `nowrap` and `pre-line` a tab, a newline or a run of either becomes one
// space — but `pre-line` keeps each newline as a forced break — and a
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
    if (!f_ws_collapses(s->white_space)) {
        FBox *ifc = target(f);
        if (ifc == NULL)
            return;
        uint32_t start = 0;
        for (uint32_t i = 0; i <= (uint32_t)n->text_len && !b->stopped; i++) {
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
    // Pre-line keeps its line breaks: the text is collapsed a line at a
    // time, and each kept break ends an item, less a space just before it
    // (CSS Text 3 § 4.1.1), with the next line starting as if after one.
    if (s->white_space == FLOW_WS_PRE_LINE) {
        char *copy = alloc(b, n->text_len);
        if (copy == NULL)
            return;
        uint32_t len = 0, seg = 0;
        bool space = ifc->collapse_space;
        size_t from = 0;
        for (size_t i = 0; i <= n->text_len; i++) {
            bool kept = i < n->text_len && n->text[i] == '\n';
            if (i < n->text_len && !kept)
                continue;
            len += f_collapse_white(n->text + from, (uint32_t)(i - from), copy + len, &space);
            from = i + 1;
            if (!kept)
                break;
            if (len > seg && copy[len - 1] == ' ')
                len--;
            if (len > seg)
                text_item(f, ifc, n, s, copy + seg, len - seg, seg, false);
            if (new_item(b, ifc, FI_BREAK, NULL, s) == NULL)
                return;
            seg = len;
            space = true;
        }
        ifc->collapse_space = space;
        if (len > seg)
            text_item(f, ifc, n, s, copy + seg, len - seg, seg, false);
        return;
    }
    // Point into the tree when processing changes nothing — no tab or
    // newline, no space after a space, no leading space after one — and
    // copy only when it does: the box tree copies no text it can point at.
    bool space = ifc->collapse_space;
    bool changes = false;
    for (size_t i = 0; i < n->text_len && !changes; i++) {
        char ch = n->text[i];
        changes = css_space(ch) && (ch != ' ' || (i == 0 ? space : n->text[i - 1] == ' '));
    }
    const char *text = n->text;
    uint32_t len = (uint32_t)n->text_len;
    if (changes) {
        char *copy = alloc(b, n->text_len);
        if (copy == NULL)
            return;
        len = f_collapse_white(n->text, (uint32_t)n->text_len, copy, &space);
        text = copy;
    } else if (len > 0) {
        space = n->text[len - 1] == ' ';
    }
    ifc->collapse_space = space;
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

static void scan(B *b, const os64_html_node_t *first, const os64_html_node_t *stop,
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
        // An out-of-flow box is neither: no formatting context holds it.
        if (s == NULL || s->style.display == FLOW_DISPLAY_NONE || f_out_of_flow(&s->style))
            continue;
        if (s->style.display == FLOW_DISPLAY_CONTENTS) {
            // A box-less element's children are its parent's, so the scan
            // goes into them — a descent like any other, and charged as one.
            if (!descend(b))
                return;
            scan(b, c->first_child, NULL, m);
            ascend(b);
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
    // The anonymous block being filled when the build stopped is the cut too.
    mark_cut(b, f.target);
    end_run(&f);
    mark_cut(b, box);
}

// The box of a block-level element and everything in it, under `parent`.
static FBox *element_box(B *b, FBox *parent, const os64_html_node_t *el, const FStyled *s,
                         Scope *scope)
{
    if (replaced(el)) {
        FBox *box = new_box(b, parent, FB_REPLACED, el, &s->style);
        // A control made a block is still a control: its widget goes where
        // this box is, as an inline one's goes where its atom is.
        if (box != NULL && b->model != NULL)
            box->control = os64_page_control_for(b->model, el);
        position_box(b, box, el, true);
        return box;
    }
    if (s->style.display == FLOW_DISPLAY_TABLE) {
        FBox *table = new_box(b, parent, FB_TABLE, el, &s->style);
        position_box(b, table, el, true);
        if (table != NULL) {
            table_range(b, table, el->first_child, NULL, scope);
            mark_cut(b, table);
        }
        return table;
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
    // The marker is made BEFORE the box, so a build that stops making it
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
        if (b->stopped)
            return NULL;
    }
    FBox *box = new_box(b, parent, FB_BLOCK, el, &s->style);
    position_box(b, box, el, true);
    if (box == NULL)
        return NULL;
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
    return box;
}

static void block_box(Flow *f, const os64_html_node_t *el, const FStyled *s, Scope *scope)
{
    end_run(f);
    f->anon_table = NULL;
    element_box(f->b, f->container, el, s, scope);
}

// An out-of-flow box: under the context it was written in, which it does
// not interrupt — among inline content with a placeholder where it stood,
// among blocks where the flow had reached. Its level costs two descents: it
// is laid out from inside its containing block's frame, the positioned
// layout's frames on top of a block's (LAYOUT.md § Bounds).
static void absolute_box(Flow *f, const os64_html_node_t *el, const FStyled *s, Scope *scope)
{
    if (!descend(f->b))
        return;
    FItem *place = NULL;
    if (f->target != NULL)
        place = new_item(f->b, f->target, FI_PLACEHOLDER, el, &s->style);
    FBox *box = place != NULL || f->target == NULL
                    ? element_box(f->b, f->target != NULL ? f->target : f->container, el, s, scope)
                    : NULL;
    if (place != NULL)
        place->absolute = box;
    ascend(f->b);
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
        // own, laid out inside the atom. Its content costs two descents: a
        // level of it holds a line's frame as well as a block's, twice a
        // block level's stack (LAYOUT.md § Bounds).
        if (!replaced(el)) {
            FBox *content = new_box(b, NULL, FB_BLOCK, el, &s->style);
            position_box(b, content, el, false);
            if (content != NULL) {
                content->parent = ifc;
                item->content = content;
                if (descend(b)) {
                    build_container(b, content, el->first_child, NULL, scope, NULL, 0);
                    ascend(b);
                }
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
    // A positioned inline: its pieces may land in several anonymous blocks,
    // so it is finished when the container they are all in is.
    if (f_contains_absolute(&s->style)) {
        inl->pos = pos_add(b, FP_INLINE, el, &s->style);
        if (inl->pos == NULL)
            return;
        inl->pos->inl = inl;
        inl->pos->home = f->container;
        inl->pos->home_next = f->container->homed;
        f->container->homed = inl->pos;
    }
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
    // Both answers are pass 1's (FStyled.quotes, .has_rp): asked here per
    // element, a walk up the ancestors or across the siblings would be
    // quadratic on a page of nested quotes or a ruby of many annotations.
    const char *before = NULL, *after = NULL;
    const FStyled *holder = styled(b, el->parent);
    if (is(el, OS64_HTML_TAG_Q)) {
        int32_t level = holder != NULL ? holder->quotes : 0;
        before = level % 2 == 0 ? "\xe2\x80\x9c" : "\xe2\x80\x98";   // U+201C, U+2018
        after = level % 2 == 0 ? "\xe2\x80\x9d" : "\xe2\x80\x99";    // U+201D, U+2019
    } else if (is(el, OS64_HTML_TAG_RT) && el->parent != NULL) {
        if (holder == NULL || !holder->has_rp) {
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
    if (!descend(b))
        return;
    for (const os64_html_node_t *c = first; c != stop && !b->stopped; c = c->next) {
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
        } else if (f_out_of_flow(&s->style)) {
            absolute_box(f, c, s, scope);
        } else if (table_internal(d)) {
            // An internal table box with no table round it gets an
            // anonymous one, shared with the internal boxes beside it.
            if (f->anon_table == NULL) {
                end_run(f);
                f->anon_table = new_box(b, f->container, FB_TABLE, NULL,
                                        anon_style(b, f->container->style, FLOW_DISPLAY_TABLE));
            }
            if (f->anon_table != NULL) {
                table_range(b, f->anon_table, c, c->next, scope);
                mark_cut(b, f->anon_table);
            }
        } else if (block_level(d)) {
            block_box(f, c, s, scope);
        } else {
            f->anon_table = NULL;
            inline_element(f, c, s, scope);
        }
    }
    ascend(b);
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
    if (!descend(b))
        return;
    for (const os64_html_node_t *c = first; c != stop && !b->stopped;) {
        if (structural(AT_ROW, c, display_of(b, c))) {
            FBox *cell = new_box(b, row, FB_CELL, c, &styled(b, c)->style);
            position_box(b, cell, c, true);
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
    ascend(b);
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
        position_box(b, cap, c, true);
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
        mark_cut(b, row);
        break;
    }
    default: {
        FBox *group = new_box(b, table, FB_ROW_GROUP, c, s);
        if (group != NULL)
            rows_range(b, group, AT_GROUP, c->first_child, NULL, scope);
        mark_cut(b, group);
        break;
    }
    }
}

// A table's or a row group's children: its own parts, and runs of cells or
// loose content, consecutive ones sharing one anonymous row.
static void rows_range(B *b, FBox *parent, Level level, const os64_html_node_t *first,
                       const os64_html_node_t *stop, Scope *scope)
{
    if (!descend(b))
        return;
    FBox *anon_row = NULL;
    for (const os64_html_node_t *c = first; c != stop && !b->stopped;) {
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
                break;
            const os64_html_node_t *end = d == FLOW_DISPLAY_TABLE_CELL
                                              ? c->next : loose_end(b, level, c, stop);
            row_range(b, anon_row, c, end, scope);
            mark_cut(b, anon_row);
            c = end;
            continue;
        }
        c = c->next;
    }
    ascend(b);
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
    out->styles = styles;
    out->arena.cap = f_arena_budget(env);
    B b = {.doc = doc, .model = model, .styles = styles, .env = env, .out = out};
    const os64_html_node_t *html = doc->html;
    const FStyled *root = html != NULL ? styled(&b, html) : NULL;
    if (root != NULL && root->style.display != FLOW_DISPLAY_NONE) {
        out->root = new_box(&b, NULL, FB_BLOCK, html, &root->style);
        position_box(&b, out->root, html, false);
        Scope scope = {1, false};
        if (out->root != NULL)
            build_container(&b, out->root, html->first_child, NULL, &scope, NULL, 0);
    }
    f_map_free(&b.pos_of);
    out->incomplete = b.stopped;
    return out;
}

void f_boxes_free(FBoxes *boxes)
{
    if (boxes == NULL)
        return;
    f_arena_free(&boxes->arena);
    os64_free(boxes);
}
