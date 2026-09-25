// layout.c — pass 3: the box tree and a width in, every box placed.
//
// Two formatting contexts, CSS 2.1's own: BLOCK formatting (§9.4.1,
// §10.3.3, §10.6.3) stacks block-level boxes and collapses their vertical
// margins (§8.3.1); INLINE formatting (§9.4.2, §10.8) breaks a container's
// item sequence into line boxes and aligns what is on each by its
// baseline. Everything is measured by the text engine — never a width
// table — and kept in 26.6 until the dump rounds it (LAYOUT.md § Rounding).
//
// Positions are 64-bit: a long page is taller than a 32-bit 26.6 count.
// Recursion follows the box tree, whose depth is a constant multiple of
// libhtml's max_depth (LAYOUT.md § Bounds).

#include "internal.h"

typedef struct {
    FLayout *out;
    const flow_env_t *env;
    const os64_page_t *model;
    bool quirks;        // full quirks
    bool any_quirks;    // quirks or limited quirks
    bool failed;
} L;

// The flow of one block formatting context: where the next box goes, the
// vertical margins adjoining that point that no box has resolved yet (they
// collapse — non-negative, so to their largest), and where the first
// content inside the current container was placed, if any has been.
typedef struct {
    int64_t y;
    int64_t pm;
    int64_t first;      // -1 until content resolves the pending margins
} Cursor;

static int64_t max64(int64_t a, int64_t b)
{
    return a > b ? a : b;
}

// A length against a containing block's width; auto reads as 0, and the
// caller that cares asks first.
static int64_t len(flow_length_t l, int64_t base)
{
    switch (l.kind) {
    case FLOW_LENGTH_PX: return l.value;
    case FLOW_LENGTH_PERCENT: return base * l.value / (100 * 64);
    case FLOW_LENGTH_AUTO: break;
    }
    return 0;
}

static void fail(L *l)
{
    l->failed = true;
    l->out->incomplete = true;
}

static void *alloc(L *l, size_t size)
{
    if (l->failed)
        return NULL;
    void *p = f_arena_alloc(&l->out->arena, size);
    if (p == NULL)
        fail(l);
    return p;
}

// ── Fonts and runs ──────────────────────────────────────────────────────

typedef struct {
    os64_text_font_t *const *list;
    size_t count;
    os64_font_face_info_t info;
} Fonts;

static bool fonts_for(L *l, const flow_style_t *s, Fonts *f)
{
    if (l->failed || l->env->fonts == NULL)
        return false;
    int64_t px = (s->font_size + 32) / 64;
    if (px < 1)
        px = 1;
    if (px > OS64_FONT_PIXEL_MAX)
        px = OS64_FONT_PIXEL_MAX;
    os64_font_status_t st = l->env->fonts(l->env->ctx, &s->family, s->font_weight >= 600,
                                          s->font_style == FLOW_FONT_ITALIC, (uint32_t)px,
                                          &f->list, &f->count, &f->info);
    if (st != OS64_FONT_OK || f->list == NULL || f->count == 0) {
        fail(l);
        return false;
    }
    return true;
}

static bool keep_run(L *l, os64_text_run_t *run)
{
    FLayout *o = l->out;
    if (o->nruns == o->cap_runs) {
        size_t cap = o->cap_runs != 0 ? o->cap_runs * 2 : 64;
        os64_text_run_t **grown = os64_realloc(o->runs, cap * sizeof(*grown));
        if (grown == NULL) {
            os64_text_run_release(run);
            fail(l);
            return false;
        }
        o->runs = grown;
        o->cap_runs = cap;
    }
    o->runs[o->nruns++] = run;
    return true;
}

// One run of the text engine. `tab_origin` is where the line's tab stops
// start, relative to the run's own origin.
static os64_text_run_t *run_of(L *l, const Fonts *f, const char *text, uint32_t n,
                               int64_t tab_origin, int64_t tab_interval)
{
    if (l->failed)
        return NULL;
    os64_text_layout_t opt = {
        .encoding = OS64_TEXT_UTF8_WESTERN_V1,
        .fonts = f->list,
        .font_count = f->count,
        .tab_origin = (os64_font_pos_t)tab_origin,
        .tab_interval = (os64_font_pos_t)(tab_interval > 0 ? tab_interval : 64 * 8),
    };
    os64_text_run_t *run = NULL;
    if (os64_text_layout(l->env->text, (const uint8_t *)text, n, &opt, &run) != OS64_FONT_OK) {
        fail(l);
        return NULL;
    }
    return run;
}

static int64_t caret_x(const os64_text_run_t *run, uint32_t byte)
{
    os64_text_caret_t c;
    if (os64_text_caret(run, byte, OS64_TEXT_BEFORE, &c) != OS64_FONT_OK)
        return 0;
    return c.x;
}

// ── Replaced sizes ──────────────────────────────────────────────────────

typedef struct {
    int64_t w, h;           // the content box
    bool alt;               // not a box at all: laid out as its alt text
} Replaced;

// CSS 2.1 §10.3.2 and §10.6.2: the size the page gave, else the one the
// face measured (scaled to keep its shape when the page gave one side),
// else the chapter's rule for a picture that has not arrived — its alt
// text inline when it has one, nothing when its alt is empty, a small
// icon when it has none — and a sensible box for anything else: a frame's
// 300x150, a control ten ems wide and one line of its own font high.
static Replaced replaced_size(L *l, const os64_html_node_t *node, const flow_style_t *s,
                              int64_t cbw)
{
    Replaced r = {0, 0, false};
    bool w_set = s->width.kind != FLOW_LENGTH_AUTO;
    bool h_set = s->height.kind == FLOW_LENGTH_PX;
    int64_t w = len(s->width, cbw), h = h_set ? s->height.value : 0;
    int32_t iw = 0, ih = 0;
    bool known = l->env->replaced_size != NULL &&
                 l->env->replaced_size(l->env->ctx, node, &iw, &ih) && iw >= 0 && ih >= 0;
    int64_t kw = (int64_t)iw * 64, kh = (int64_t)ih * 64;
    if (known) {
        if (w_set && h_set) {
            r.w = w;
            r.h = h;
        } else if (w_set) {
            r.w = w;
            r.h = kw > 0 ? w * kh / kw : kh;
        } else if (h_set) {
            r.h = h;
            r.w = kh > 0 ? h * kw / kh : kw;
        } else {
            r.w = kw;
            r.h = kh;
        }
        return r;
    }
    if (w_set || h_set) {
        r.w = w_set ? w : h;
        r.h = h_set ? h : w;
        return r;
    }
    bool img = node->ns == OS64_HTML_NS_HTML && node->tag == OS64_HTML_TAG_IMG;
    if (img) {
        const os64_html_attr_t *alt = os64_html_attr(node, "alt");
        if (alt != NULL && alt->value != NULL && alt->value[0] != '\0') {
            r.alt = true;
        } else if (alt == NULL) {
            r.w = r.h = 16 * 64;
        }
        return r;
    }
    bool frame = node->ns == OS64_HTML_NS_HTML &&
                 (node->tag == OS64_HTML_TAG_IFRAME || node->tag == OS64_HTML_TAG_FRAME);
    if (frame) {
        r.w = 300 * 64;
        r.h = 150 * 64;
        return r;
    }
    Fonts f;
    r.w = (int64_t)s->font_size * 10;
    r.h = fonts_for(l, s, &f) ? f.info.line_height : s->font_size;
    return r;
}

// ── Decorations and links ───────────────────────────────────────────────

typedef struct {
    uint8_t decoration;
    uint32_t color;
    int32_t link;
} Paint;

// What a fragment is drawn with that its own style does not say: the
// decorations of every ancestor, in the innermost decorating element's
// colour — they propagate through inline AND block descendants, but not
// into an atomic inline's content, nor (quirks mode, the Quirks standard's
// 3.11) into a table — and the link it sits in.
static Paint paint_for(const L *l, const FStyles *styles, const os64_html_node_t *n,
                       const os64_html_node_t *atom)
{
    Paint p = {0, 0, -1};
    bool colored = false;
    for (const os64_html_node_t *a = n; a != NULL; a = a->parent) {
        if (a->kind != OS64_HTML_ELEMENT)
            continue;
        if (p.link < 0 && l->model != NULL && a->ns == OS64_HTML_NS_HTML &&
            (a->tag == OS64_HTML_TAG_A || a->tag == OS64_HTML_TAG_AREA))
            p.link = os64_page_link_for(l->model, a);
        const FStyled *st = f_style_of(styles, a);
        if (st == NULL)
            continue;
        if (st->style.text_decoration != 0) {
            p.decoration |= st->style.text_decoration;
            if (!colored) {
                p.color = st->style.color;
                colored = true;
            }
        }
        // An atom's own decorations reach its content; nothing above it does.
        if (a != atom && st->style.display == FLOW_DISPLAY_INLINE_BLOCK)
            break;
        if (l->quirks && a->ns == OS64_HTML_NS_HTML && a->tag == OS64_HTML_TAG_TABLE)
            break;
    }
    return p;
}

// ── Inline formatting: segments ─────────────────────────────────────────
//
// An item sequence is cut into SEGMENTS for line breaking: a word (the
// bytes between two soft wrap opportunities, with the spaces after it), an
// atom, an inline edge, a break. A word never crosses an item, but a
// WORD — the unbreakable thing — may be several segments: `foo<b>bar</b>`
// is a segment, two edges and a segment with no opportunity between them.

typedef enum { SG_WORD, SG_ATOM, SG_OPEN, SG_CLOSE, SG_BREAK, SG_WBR, SG_MARKER } SegKind;

typedef struct {
    SegKind kind;
    const FItem *item;
    const char *text;       // WORD, MARKER: the bytes the word's run is cut from
    const flow_style_t *style;
    uint32_t b0, b1, s1;    // WORD: the word is [b0,b1), its spaces [b1,s1)
    int64_t w, sw;          // the word's (or box's) width, and its spaces'
    bool wrap_after;        // a soft wrap opportunity follows
    bool collapsible;       // WORD: its spaces vanish at a line's end
    bool hang;              // WORD: its spaces stay but do not count (pre-wrap)
    Replaced atom;          // ATOM: its content box
    int64_t frame_w, frame_h, ml, mr, mt, mb;   // ATOM: border+padding, margins
} Seg;

typedef struct {
    Seg *v;
    size_t n, cap;
} Segs;

static Seg *seg_add(L *l, Segs *s, SegKind kind)
{
    if (l->failed)
        return NULL;
    if (s->n == s->cap) {
        size_t cap = s->cap != 0 ? s->cap * 2 : 32;
        Seg *grown = os64_realloc(s->v, cap * sizeof(*grown));
        if (grown == NULL) {
            fail(l);
            return NULL;
        }
        s->v = grown;
        s->cap = cap;
    }
    Seg *g = &s->v[s->n++];
    os64_memset(g, 0, sizeof(*g));
    g->kind = kind;
    return g;
}

static bool wraps(flow_white_space_t ws)
{
    return ws == FLOW_WS_NORMAL || ws == FLOW_WS_PRE_WRAP;
}

// A text's words: measured ONCE, then cut at its spaces. A text longer than
// the engine takes in one run (OS64_TEXT_BYTES_MAX) is measured in WINDOWS
// cut after a space, so a word never straddles two; a window with no space
// in it is cut at a character boundary, and the word it splits stays one
// word — two segments with no opportunity between them. Never a refusal.
static void text_segments(L *l, Segs *s, const FItem *it, const char *text, uint32_t n,
                          const flow_style_t *style, int64_t tab_interval)
{
    Fonts f;
    if (n == 0 || !fonts_for(l, style, &f))
        return;
    flow_white_space_t ws = style->white_space;
    bool collapsible = ws == FLOW_WS_NORMAL || ws == FLOW_WS_NOWRAP;
    bool whole = ws == FLOW_WS_PRE || ws == FLOW_WS_NOWRAP;
    for (uint32_t w0 = 0; w0 < n && !l->failed;) {
        uint32_t w1 = n;
        if (n - w0 > OS64_TEXT_BYTES_MAX) {
            w1 = w0 + OS64_TEXT_BYTES_MAX;
            uint32_t cut = w1;
            while (cut > w0 && text[cut - 1] != ' ' && text[cut - 1] != '\t')
                cut--;
            if (cut > w0) {
                w1 = cut;
            } else {
                while (w1 > w0 && ((unsigned char)text[w1] & 0xC0) == 0x80)
                    w1--;
            }
        }
        // A measuring run lives only as long as it takes to read its carets:
        // a million-byte window costs tens of megabytes of the budget.
        os64_text_run_t *run = run_of(l, &f, text + w0, w1 - w0, 0, tab_interval);
        if (run == NULL)
            return;
        if (whole) {
            // No opportunity inside: one word per window, its trailing
            // collapsible space still removable at a line's end.
            uint32_t end = w1;
            if (collapsible && w1 == n)
                while (end > w0 && text[end - 1] == ' ')
                    end--;
            Seg *g = seg_add(l, s, SG_WORD);
            if (g != NULL) {
                *g = (Seg){.kind = SG_WORD, .item = it, .text = text, .style = style,
                           .b0 = w0, .b1 = end, .s1 = w1, .collapsible = collapsible};
                g->w = caret_x(run, end - w0);
                g->sw = caret_x(run, w1 - w0) - g->w;
            }
        } else {
            for (uint32_t at = w0; at < w1;) {
                uint32_t b0 = at;
                while (at < w1 && text[at] != ' ' && text[at] != '\t')
                    at++;
                uint32_t b1 = at;
                while (at < w1 && (text[at] == ' ' || text[at] == '\t'))
                    at++;
                Seg *g = seg_add(l, s, SG_WORD);
                if (g == NULL)
                    break;
                *g = (Seg){.kind = SG_WORD, .item = it, .text = text, .style = style,
                           .b0 = b0, .b1 = b1, .s1 = at, .collapsible = collapsible,
                           .hang = ws == FLOW_WS_PRE_WRAP, .wrap_after = at > b1};
                int64_t x0 = caret_x(run, b0 - w0), x1 = caret_x(run, b1 - w0);
                g->w = x1 - x0;
                g->sw = caret_x(run, at - w0) - x1;
            }
        }
        os64_text_run_release(run);
        w0 = w1;
    }
}

static int64_t tab_interval_for(L *l, const flow_style_t *block)
{
    // Eight times the block's own space: one font per paragraph decides
    // the stops, or a <b> mid-line would move them.
    Fonts f;
    if (!fonts_for(l, block, &f))
        return 0;
    os64_text_run_t *run = run_of(l, &f, " ", 1, 0, 64);
    if (run == NULL)
        return 0;
    int64_t w;
    os64_text_run_view_t v;
    w = os64_text_run_view(run, &v) == OS64_FONT_OK ? v.advance_x : 0;
    os64_text_run_release(run);
    return w * 8;
}

// In quirks mode a picture in a cell whose width is auto has no soft wrap
// opportunity either side of it (the Quirks standard's 3.8), so a row of
// sliced images stays a row.
static bool quirk_no_wrap_round(const L *l, const FBox *ifc, const FItem *it)
{
    if (!l->quirks || it->node->ns != OS64_HTML_NS_HTML || it->node->tag != OS64_HTML_TAG_IMG)
        return false;
    for (const FBox *b = ifc; b != NULL; b = b->parent)
        if (b->kind == FB_CELL)
            return b->style->width.kind == FLOW_LENGTH_AUTO;
    return false;
}

static void segments(L *l, FBox *ifc, int64_t cw, Segs *s)
{
    int64_t tabs = 0;
    for (const FItem *it = ifc->items; it != NULL && !l->failed; it = it->next) {
        switch (it->kind) {
        case FI_TEXT: {
            if (tabs == 0 && !(it->style->white_space == FLOW_WS_NORMAL ||
                               it->style->white_space == FLOW_WS_NOWRAP))
                tabs = tab_interval_for(l, ifc->style);
            text_segments(l, s, it, it->text, it->len, it->style, tabs);
            break;
        }
        case FI_ATOMIC: {
            const flow_style_t *st = it->style;
            Replaced r = {0, 0, false};
            if (it->content == NULL)
                r = replaced_size(l, it->node, st, cw);
            if (r.alt) {
                const char *alt = os64_html_attr(it->node, "alt")->value;
                text_segments(l, s, it, alt, (uint32_t)os64_strlen(alt), st, tabs);
                break;
            }
            Seg *g = seg_add(l, s, SG_ATOM);
            if (g == NULL)
                return;
            g->item = it;
            g->style = st;
            g->atom = r;
            g->ml = len(st->margin[FLOW_LEFT], cw);
            g->mr = len(st->margin[FLOW_RIGHT], cw);
            g->mt = len(st->margin[FLOW_TOP], cw);
            g->mb = len(st->margin[FLOW_BOTTOM], cw);
            int64_t fw = st->border_width[FLOW_LEFT] + st->border_width[FLOW_RIGHT] +
                         len(st->padding[FLOW_LEFT], cw) + len(st->padding[FLOW_RIGHT], cw);
            int64_t fh = st->border_width[FLOW_TOP] + st->border_width[FLOW_BOTTOM] +
                         len(st->padding[FLOW_TOP], cw) + len(st->padding[FLOW_BOTTOM], cw);
            g->frame_w = fw;
            g->frame_h = fh;
            // An atom with content of its own (a marquee) is as wide as
            // the line; its height comes from laying it out.
            if (it->content != NULL)
                r.w = cw - fw - g->ml - g->mr;
            g->atom = r;
            g->w = g->ml + fw + r.w + g->mr;
            bool wrap = wraps(st->white_space) && !quirk_no_wrap_round(l, ifc, it);
            g->wrap_after = wrap;
            if (wrap && s->n >= 2 && s->v[s->n - 2].kind != SG_BREAK)
                s->v[s->n - 2].wrap_after = true;
            break;
        }
        case FI_OPEN:
        case FI_CLOSE: {
            Seg *g = seg_add(l, s, it->kind == FI_OPEN ? SG_OPEN : SG_CLOSE);
            if (g != NULL) {
                g->item = it;
                g->style = it->style;
            }
            break;
        }
        case FI_BREAK: {
            Seg *g = seg_add(l, s, SG_BREAK);
            if (g != NULL) {
                g->item = it;
                g->style = it->style;
            }
            break;
        }
        case FI_WBR: {
            Seg *g = seg_add(l, s, SG_WBR);
            if (g != NULL) {
                g->item = it;
                g->wrap_after = true;
            }
            break;
        }
        case FI_MARKER: {
            Fonts f;
            if (!fonts_for(l, it->style, &f))
                return;
            os64_text_run_t *run = run_of(l, &f, it->text, it->len, 0, 0);
            if (run == NULL)
                return;
            os64_text_run_view_t v;
            int64_t w = os64_text_run_view(run, &v) == OS64_FONT_OK ? v.advance_x : 0;
            os64_text_run_release(run);
            Seg *g = seg_add(l, s, SG_MARKER);
            if (g == NULL)
                return;
            g->item = it;
            g->text = it->text;
            g->style = it->style;
            g->b1 = g->s1 = it->len;
            g->w = w;
            break;
        }
        }
    }
}

// ── Inline formatting: lines ────────────────────────────────────────────

// Greedy line breaking (CSS 2.1 §9.4.2): as much as fits, breaking at the
// LAST soft wrap opportunity before what does not; a word wider than the
// line goes on a line of its own and overflows it (`overflow-wrap:
// normal`). Returns the segment after the line; `*forced` says a break
// ended it.
static size_t break_line(const Segs *s, size_t start, int64_t avail, bool *forced)
{
    int64_t pen = 0;
    bool content = false;
    size_t brk = SIZE_MAX;
    *forced = false;
    for (size_t j = start; j < s->n; j++) {
        const Seg *g = &s->v[j];
        if (g->kind == SG_BREAK) {
            *forced = true;
            return j + 1;
        }
        if (g->kind == SG_WORD || g->kind == SG_ATOM || g->kind == SG_MARKER) {
            if (brk != SIZE_MAX && pen + g->w > avail)
                return brk + 1;
            pen += g->w;
            if (g->kind != SG_WORD || g->b1 > g->b0)
                content = true;
        }
        // An opportunity before any content is no place to break: the line
        // it would end holds nothing.
        if (g->wrap_after && content)
            brk = j;
        if (g->kind == SG_WORD)
            pen += g->sw;
    }
    return s->n;
}

typedef struct {
    int64_t top, bottom;    // relative to the baseline, y down
} Extent;

static void extend(Extent *e, bool *any, int64_t top, int64_t bottom)
{
    if (!*any) {
        e->top = top;
        e->bottom = bottom;
        *any = true;
        return;
    }
    if (top < e->top)
        e->top = top;
    if (bottom > e->bottom)
        e->bottom = bottom;
}

// An inline box's height is its line-height, the content area centred in
// it with half the leading above and half below (§10.8.1).
static Extent font_box(int64_t ascent, int64_t descent, int64_t line_height)
{
    int64_t half = (line_height - ascent - descent) / 2;
    return (Extent){-ascent - half, descent + (line_height - ascent - descent - half)};
}

static FFrag *frag_add(L *l, FLine *line, f_frag_kind_t kind)
{
    FFrag *f = alloc(l, sizeof(*f));
    if (f == NULL)
        return NULL;
    f->kind = kind;
    f->link = -1;
    if (line->last_frag != NULL)
        line->last_frag->next = f;
    else
        line->frags = f;
    line->last_frag = f;
    return f;
}

static FSpan *span_add(L *l, FLine *line, const FInline *inl, int64_t x0, int64_t x1)
{
    FSpan *sp = alloc(l, sizeof(*sp));
    if (sp == NULL)
        return NULL;
    sp->inl = inl;
    sp->x0 = x0;
    sp->x1 = x1;
    if (line->last_span != NULL)
        line->last_span->next = sp;
    else
        line->spans = sp;
    line->last_span = sp;
    return sp;
}

static void block(L *l, const FStyles *styles, FBox *b, int64_t cbx, int64_t cbw, Cursor *cur);
static void translate(FBox *b, int64_t dx, int64_t dy);

// Open inline boxes carried from one line to the next.
typedef struct {
    const FInline **v;
    size_t n, cap;
} Open;

static bool open_push(L *l, Open *o, const FInline *inl)
{
    if (o->n == o->cap) {
        size_t cap = o->cap != 0 ? o->cap * 2 : 8;
        const FInline **grown = os64_realloc(o->v, cap * sizeof(*grown));
        if (grown == NULL) {
            fail(l);
            return false;
        }
        o->v = grown;
        o->cap = cap;
    }
    o->v[o->n++] = inl;
    return true;
}

static void lines(L *l, const FStyles *styles, FBox *ifc, int64_t cx, int64_t cw, Cursor *cur)
{
    Segs s = {0};
    segments(l, ifc, cw, &s);
    Fonts bf;
    bool have_block_font = fonts_for(l, ifc->style, &bf);
    Open open = {0};
    // The x each open inline's piece on the current line starts at.
    int64_t *open_x = NULL;
    size_t open_x_cap = 0;

    flow_text_align_t align = ifc->style->text_align;
    bool justify = align == FLOW_ALIGN_JUSTIFY || align == FLOW_ALIGN_HTML_JUSTIFY;

    for (size_t start = 0; start < s.n && !l->failed;) {
        bool forced;
        size_t end = break_line(&s, start, cw, &forced);
        // An inline that closes right where a soft break falls closes on
        // this line, not as an empty piece on the next.
        if (!forced)
            while (end < s.n && s.v[end].kind == SG_CLOSE)
                end++;
        bool last = end >= s.n;

        FLine *line = alloc(l, sizeof(*line));
        if (line == NULL)
            break;
        line->x = cx;
        line->w = cw;
        if (ifc->last_line != NULL)
            ifc->last_line->next = line;
        else
            ifc->lines = line;
        ifc->last_line = line;

        // The last segment with content, whose collapsible spaces go.
        size_t tail = SIZE_MAX;
        for (size_t j = start; j < end; j++)
            if (s.v[j].kind == SG_ATOM || s.v[j].kind == SG_MARKER ||
                (s.v[j].kind == SG_WORD && s.v[j].b1 > s.v[j].b0))
                tail = j;

        if (open_x_cap < open.cap) {
            int64_t *grown = os64_realloc(open_x, open.cap * sizeof(*grown));
            if (grown == NULL) {
                fail(l);
                break;
            }
            open_x = grown;
            open_x_cap = open.cap;
        }
        for (size_t k = 0; k < open.n; k++)
            open_x[k] = 0;

        // Horizontal: lay the fragments down from 0; alignment shifts
        // them after, once the line's width is known.
        int64_t pen = 0, used = 0;
        size_t gaps = 0;
        bool content = false, has_text = false, has_break = false;
        Extent brk_box = {0, 0};
        Extent ext = {0, 0};
        bool any = false;
        // The strut: the block's own font's box, so a line of small text is
        // as tall as its neighbours. Quirks and limited-quirks modes make
        // none (the Quirks standard's 3.4) — the sliced-image fix.
        if (have_block_font && !l->any_quirks) {
            Extent strut = font_box(bf.info.ascent, bf.info.descent, bf.info.line_height);
            extend(&ext, &any, strut.top, strut.bottom);
        }

        for (size_t j = start; j < end && !l->failed; j++) {
            Seg *g = &s.v[j];
            switch (g->kind) {
            case SG_WORD: {
                // A run of words from one text is laid down as ONE fragment
                // — per word on a justified line, so the gaps can grow.
                // Collapsible space leading a line is dropped, and so is
                // collapsible space trailing it; pre-wrap's trailing space
                // stays but HANGS, counting for nothing.
                if (!content && g->b1 == g->b0 && g->collapsible)
                    break;
                bool spread = justify && !last && !forced;
                // A fragment is one run, so it stops short of the run cap.
                size_t k = j;
                if (!spread)
                    while (k + 1 < end && s.v[k + 1].kind == SG_WORD &&
                           s.v[k + 1].item == g->item && s.v[k + 1].text == g->text &&
                           s.v[k + 1].s1 - g->b0 <= OS64_TEXT_BYTES_MAX)
                        k++;
                const Seg *e = &s.v[k];
                bool at_tail = tail == SIZE_MAX || k >= tail;
                uint32_t b0 = g->b0;
                if (!content && g->collapsible)
                    while (b0 < g->b1 && g->text[b0] == ' ')
                        b0++;
                uint32_t b1 = e->s1;
                if (spread || (at_tail && e->collapsible))
                    b1 = e->b1;
                Fonts f;
                if (b1 > b0 && fonts_for(l, g->style, &f)) {
                    bool pre = g->style->white_space == FLOW_WS_PRE ||
                               g->style->white_space == FLOW_WS_PRE_WRAP;
                    int64_t tabs = 0;
                    for (uint32_t t = b0; t < b1 && pre; t++)
                        if (g->text[t] == '\t') {
                            tabs = tab_interval_for(l, ifc->style);
                            break;
                        }
                    // Tabs stop where the LINE's stops are, whatever this
                    // fragment starts after.
                    os64_text_run_t *run = run_of(l, &f, g->text + b0, b1 - b0,
                                                  pre ? -pen : 0, tabs);
                    if (run == NULL || !keep_run(l, run))
                        break;
                    FFrag *fr = frag_add(l, line, FF_TEXT);
                    if (fr == NULL)
                        break;
                    os64_text_run_view_t v;
                    os64_text_run_view(run, &v);
                    fr->item = g->item;
                    fr->node = g->item->node;
                    fr->style = g->style;
                    fr->run = run;
                    fr->text = g->text;
                    fr->begin = b0;
                    fr->end = b1;
                    fr->x = pen;
                    fr->w = v.advance_x;
                    fr->h = v.ascent + v.descent;
                    fr->y = -v.ascent;
                    Paint p = paint_for(l, styles, g->item->node,
                                        g->item->kind == FI_ATOMIC ? g->item->node : NULL);
                    fr->decoration = p.decoration;
                    fr->decoration_color = p.color;
                    fr->link = p.link;
                    Extent fb = font_box(v.ascent, v.descent, v.line_height);
                    extend(&ext, &any, fb.top, fb.bottom);
                    pen += v.advance_x;
                    used = at_tail && e->hang && !spread ? pen - e->sw : pen;
                    has_text = true;
                    content = true;
                }
                if (spread && !at_tail && e->s1 > e->b1) {
                    pen += e->sw;
                    gaps++;
                }
                j = k;
                break;
            }
            case SG_ATOM: {
                FFrag *fr = frag_add(l, line, FF_ATOMIC);
                if (fr == NULL)
                    break;
                fr->item = g->item;
                fr->node = g->item->node;
                fr->style = g->style;
                fr->x = pen + g->ml;
                fr->w = g->frame_w + g->atom.w;
                int64_t content_h = g->atom.h;
                if (g->item->content != NULL) {
                    // Laid out where it will not collide, measured, and
                    // moved into place once the line is placed.
                    Cursor c = {0, 0, -1};
                    block(l, styles, g->item->content, 0, g->atom.w + g->frame_w, &c);
                    content_h = g->item->content->placed ? g->item->content->h - g->frame_h : 0;
                }
                fr->h = g->frame_h + content_h;
                Paint p = paint_for(l, styles, g->item->node, g->item->node);
                fr->link = g->item->link >= 0 ? g->item->link : p.link;
                int64_t hm = g->mt + fr->h + g->mb;
                // The baseline of a replaced box is its bottom margin edge.
                int64_t top = -hm;
                switch (g->style->vertical_align) {
                case FLOW_VALIGN_MIDDLE: {
                    int64_t xh = g->style->font_size / 2;
                    top = -xh / 2 - hm / 2;
                    break;
                }
                case FLOW_VALIGN_HTML_MIDDLE:
                    top = -hm / 2;
                    break;
                case FLOW_VALIGN_TEXT_TOP: {
                    Fonts f;
                    top = fonts_for(l, g->style, &f) ? -f.info.ascent : -hm;
                    break;
                }
                default:
                    break;
                }
                fr->y = top + g->mt;
                if (g->style->vertical_align != FLOW_VALIGN_TOP &&
                    g->style->vertical_align != FLOW_VALIGN_BOTTOM)
                    extend(&ext, &any, top, top + hm);
                pen += g->w;
                used = pen;
                content = true;
                break;
            }
            case SG_MARKER: {
                Fonts f;
                if (!fonts_for(l, g->style, &f))
                    break;
                os64_text_run_t *run = run_of(l, &f, g->text, g->b1, 0, 0);
                if (run == NULL || !keep_run(l, run))
                    break;
                FFrag *fr = frag_add(l, line, FF_MARKER);
                if (fr == NULL)
                    break;
                os64_text_run_view_t v;
                os64_text_run_view(run, &v);
                fr->item = g->item;
                fr->node = g->item->node;
                fr->style = g->style;
                fr->run = run;
                fr->text = g->text;
                fr->end = g->b1;
                fr->x = pen;
                fr->w = v.advance_x;
                fr->h = v.ascent + v.descent;
                fr->y = -v.ascent;
                Extent fb = font_box(v.ascent, v.descent, v.line_height);
                extend(&ext, &any, fb.top, fb.bottom);
                pen += v.advance_x;
                used = pen;
                content = true;
                has_text = true;
                break;
            }
            case SG_OPEN:
                if (open_push(l, &open, g->item->inl)) {
                    if (open_x_cap < open.cap) {
                        int64_t *grown = os64_realloc(open_x, open.cap * sizeof(*grown));
                        if (grown == NULL) {
                            fail(l);
                            break;
                        }
                        open_x = grown;
                        open_x_cap = open.cap;
                    }
                    open_x[open.n - 1] = pen;
                }
                break;
            case SG_CLOSE:
                // Inlines close innermost first; the piece ends here.
                for (size_t k = open.n; k > 0; k--)
                    if (open.v[k - 1] == g->item->inl) {
                        span_add(l, line, open.v[k - 1], open_x[k - 1], pen);
                        for (size_t m = k - 1; m + 1 < open.n; m++) {
                            open.v[m] = open.v[m + 1];
                            open_x[m] = open_x[m + 1];
                        }
                        open.n--;
                        break;
                    }
                break;
            case SG_BREAK: {
                // A forced break holds its line open with its own font's box
                // — that is `a<br><br>b`'s blank line — but in quirks mode
                // only when nothing else is on the line: a <br> between two
                // sliced images must not open a gap under the first.
                Fonts f;
                if (fonts_for(l, g->style, &f)) {
                    brk_box = font_box(f.info.ascent, f.info.descent, f.info.line_height);
                    has_break = true;
                }
                break;
            }
            case SG_WBR:
                break;
            }
        }
        // Pieces still open run to the line's end and carry on.
        for (size_t k = 0; k < open.n && !l->failed; k++)
            span_add(l, line, open.v[k], open_x[k], pen);

        // No-quirks: every inline box on the line holds its own font's box
        // open, empty or not (in quirks mode an empty one does not, the
        // Quirks standard's 3.3).
        if (!l->any_quirks)
            for (FSpan *sp = line->spans; sp != NULL; sp = sp->next) {
                Fonts f;
                if (fonts_for(l, sp->inl->style, &f)) {
                    Extent fb = font_box(f.info.ascent, f.info.descent, f.info.line_height);
                    extend(&ext, &any, fb.top, fb.bottom);
                }
            }

        if (has_break && (!l->any_quirks || (!has_text && !content)))
            extend(&ext, &any, brk_box.top, brk_box.bottom);

        // Vertical (§10.8): the line is as tall as what sits on its
        // baseline, then as tall as anything aligned to its top or bottom.
        bool empty = !has_text && !content && !has_break;
        int64_t height = any ? ext.bottom - ext.top : 0;
        int64_t baseline_off = any ? -ext.top : 0;
        for (FFrag *fr = line->frags; fr != NULL; fr = fr->next) {
            if (fr->kind != FF_ATOMIC)
                continue;
            flow_vertical_align_t va = fr->style->vertical_align;
            if (va != FLOW_VALIGN_TOP && va != FLOW_VALIGN_BOTTOM)
                continue;
            int64_t hm = len(fr->style->margin[FLOW_TOP], cw) + fr->h +
                         len(fr->style->margin[FLOW_BOTTOM], cw);
            if (hm > height) {
                if (va == FLOW_VALIGN_BOTTOM)
                    baseline_off += hm - height;
                height = hm;
            }
        }
        if (empty)
            height = 0;

        // Resolve the margins waiting above the first line with height.
        if (height > 0) {
            cur->y += cur->pm;
            cur->pm = 0;
            if (cur->first < 0)
                cur->first = cur->y;
        }
        line->y = cur->y;
        line->h = height;
        line->baseline = line->y + baseline_off;

        // Horizontal alignment, now that the line's width is known.
        int64_t shift = 0, extra = cw - used;
        switch (align) {
        case FLOW_ALIGN_RIGHT: case FLOW_ALIGN_HTML_RIGHT:
            shift = extra > 0 ? extra : 0;
            break;
        case FLOW_ALIGN_CENTER: case FLOW_ALIGN_HTML_CENTER:
            shift = extra > 0 ? extra / 2 : 0;
            break;
        default:
            break;
        }
        int64_t per_gap = justify && !last && !forced && gaps > 0 && extra > 0
                              ? extra / (int64_t)gaps : 0;
        size_t gap_index = 0;
        for (FFrag *fr = line->frags; fr != NULL; fr = fr->next) {
            int64_t dx = shift + per_gap * (int64_t)gap_index;
            if (per_gap > 0 && fr->kind == FF_TEXT)
                gap_index++;
            fr->x += cx + dx;
            if (fr->kind == FF_ATOMIC) {
                flow_vertical_align_t va = fr->style->vertical_align;
                int64_t mt = len(fr->style->margin[FLOW_TOP], cw);
                if (va == FLOW_VALIGN_TOP)
                    fr->y = line->y + mt;
                else if (va == FLOW_VALIGN_BOTTOM)
                    fr->y = line->y + height - fr->h - len(fr->style->margin[FLOW_BOTTOM], cw);
                else
                    fr->y += line->baseline;
                if (fr->item->content != NULL)
                    translate(fr->item->content, fr->x - fr->item->content->x,
                              fr->y - fr->item->content->y);
            } else {
                fr->y += line->baseline;
            }
            fr->baseline = line->baseline;
        }
        for (FSpan *sp = line->spans; sp != NULL; sp = sp->next) {
            sp->x0 += cx + shift;
            sp->x1 += cx + shift;
            Fonts f;
            if (fonts_for(l, sp->inl->style, &f)) {
                sp->top = line->baseline - f.info.ascent;
                sp->bottom = line->baseline + f.info.descent;
            }
        }
        cur->y += height;
        if (height > 0 && cur->first < 0)
            cur->first = line->y;
        start = end;
    }
    os64_free(s.v);
    os64_free(open.v);
    os64_free(open_x);
}

// ── Block formatting ────────────────────────────────────────────────────

static void translate(FBox *b, int64_t dx, int64_t dy)
{
    if (dx == 0 && dy == 0)
        return;
    b->x += dx;
    b->y += dy;
    for (FLine *ln = b->lines; ln != NULL; ln = ln->next) {
        ln->x += dx;
        ln->y += dy;
        ln->baseline += dy;
        for (FFrag *fr = ln->frags; fr != NULL; fr = fr->next) {
            fr->x += dx;
            fr->y += dy;
            fr->baseline += dy;
            if (fr->kind == FF_ATOMIC && fr->item->content != NULL)
                translate(fr->item->content, dx, dy);
        }
        for (FSpan *sp = ln->spans; sp != NULL; sp = sp->next) {
            sp->x0 += dx;
            sp->x1 += dx;
            sp->top += dy;
            sp->bottom += dy;
        }
    }
    if (b->marker_frag != NULL) {
        b->marker_frag->x += dx;
        b->marker_frag->y += dy;
        b->marker_frag->baseline += dy;
    }
    for (FBox *c = b->first; c != NULL; c = c->next)
        translate(c, dx, dy);
}

// A box whose margins never collapse with its content's: the root, a
// table and its cells and captions, a replaced block, and an atom's
// content — each starts a block formatting context of its own (§9.4.1,
// §8.3.1).
static bool bfc_root(const FBox *b)
{
    return b->parent == NULL || b->kind == FB_TABLE || b->kind == FB_CELL ||
           b->kind == FB_CAPTION || b->kind == FB_REPLACED ||
           (b->kind == FB_BLOCK && b->node != NULL && b->style->display == FLOW_DISPLAY_INLINE_BLOCK);
}

// The HTML alignments reach a block child whose margins are both set, that
// is narrower than its container, and that has no align attribute of its
// own (§15.3.3's "align descendants").
static int64_t html_align_shift(const FBox *b, int64_t rem)
{
    if (rem <= 0 || b->parent == NULL || b->node == NULL ||
        os64_html_attr(b->node, "align") != NULL)
        return 0;
    switch (b->parent->style->text_align) {
    case FLOW_ALIGN_HTML_CENTER: return rem / 2;
    case FLOW_ALIGN_HTML_RIGHT: return rem;
    default: return 0;
    }
}

// §10.3.3: margin + border + padding + width = the containing block, auto
// margins sharing what a set width leaves over.
static int64_t widths(FBox *b, int64_t cbw, int64_t forced_w, int64_t *ml_out)
{
    const flow_style_t *s = b->style;
    for (int i = 0; i < 4; i++) {
        b->border[i] = s->border_width[i];
        b->padding[i] = len(s->padding[i], cbw);
    }
    int64_t frame = b->border[FLOW_LEFT] + b->border[FLOW_RIGHT] + b->padding[FLOW_LEFT] +
                    b->padding[FLOW_RIGHT];
    bool ml_auto = s->margin[FLOW_LEFT].kind == FLOW_LENGTH_AUTO;
    bool mr_auto = s->margin[FLOW_RIGHT].kind == FLOW_LENGTH_AUTO;
    int64_t ml = ml_auto ? 0 : len(s->margin[FLOW_LEFT], cbw);
    int64_t mr = mr_auto ? 0 : len(s->margin[FLOW_RIGHT], cbw);
    bool w_auto = forced_w < 0 && s->width.kind == FLOW_LENGTH_AUTO;
    int64_t w;
    if (w_auto) {
        w = cbw - ml - mr - frame;
        if (w < 0)
            w = 0;
    } else {
        w = forced_w >= 0 ? forced_w : len(s->width, cbw);
        int64_t rem = cbw - (ml + mr + frame + w);
        if (ml_auto && mr_auto)
            ml = rem > 0 ? rem / 2 : 0;
        else if (ml_auto)
            ml = rem;
        else if (!mr_auto)
            ml += html_align_shift(b, rem);
    }
    *ml_out = ml;
    return w;
}

// The first line with height anywhere inside a box, in order.
static void first_line_of(FBox *b, FLine **out)
{
    for (FLine *ln = b->lines; ln != NULL; ln = ln->next)
        if (ln->h > 0) {
            *out = ln;
            return;
        }
    for (FBox *c = b->first; c != NULL && *out == NULL; c = c->next)
        first_line_of(c, out);
}

// An outside marker: right-aligned against the item's content edge, on
// its first line's baseline — or where one would be — in the item's font.
static void place_marker(L *l, FBox *b, int64_t content_x, int64_t content_y)
{
    Fonts f;
    if (!fonts_for(l, b->style, &f))
        return;
    os64_text_run_t *run = run_of(l, &f, b->marker, b->marker_len, 0, 0);
    if (run == NULL || !keep_run(l, run))
        return;
    FFrag *fr = alloc(l, sizeof(*fr));
    if (fr == NULL)
        return;
    os64_text_run_view_t v;
    os64_text_run_view(run, &v);
    FLine *first = NULL;
    first_line_of(b, &first);
    fr->kind = FF_MARKER;
    fr->node = b->node;
    fr->style = b->style;
    fr->run = run;
    fr->text = b->marker;
    fr->end = b->marker_len;
    fr->link = -1;
    fr->w = v.advance_x;
    fr->h = v.ascent + v.descent;
    fr->x = content_x - fr->w;
    fr->baseline = first != NULL ? first->baseline : content_y + v.ascent;
    fr->y = fr->baseline - v.ascent;
    b->marker_frag = fr;
}

// A block container's block-level children, in order. A TABLE is laid out
// here as a block too: its row groups, rows and cells stack one under
// another, each cell as wide as the table. The automatic table layout
// (LAYOUT.md § Tables) is not applied.
static void children(L *l, const FStyles *styles, FBox *b, int64_t cx, int64_t cw, Cursor *in)
{
    for (FBox *c = b->first; c != NULL && !l->failed; c = c->next) {
        if (c->kind == FB_COLUMN_GROUP || c->kind == FB_COLUMN) {
            // Columns have no box of their own to stack; they sit, empty,
            // at the table's top.
            c->placed = true;
            c->x = cx;
            c->y = in->y;
            for (FBox *k = c->first; k != NULL; k = k->next) {
                k->placed = true;
                k->x = cx;
                k->y = in->y;
            }
            continue;
        }
        block(l, styles, c, cx, cw, in);
    }
}

// A block-level box in its parent's flow. On entry `cur` is the point
// below the previous box with the margins still pending there; on exit it
// is the point below this one, with this box's bottom margin (and, when it
// adjoins, its last child's) pending.
static void block(L *l, const FStyles *styles, FBox *b, int64_t cbx, int64_t cbw, Cursor *cur)
{
    if (l->failed)
        return;
    const flow_style_t *s = b->style;
    Replaced rep = {0, 0, false};
    int64_t forced_w = -1;
    if (b->kind == FB_REPLACED) {
        rep = replaced_size(l, b->node, s, cbw);
        forced_w = rep.w;
    }
    int64_t ml;
    int64_t cw = widths(b, cbw, forced_w, &ml);
    int64_t mt = len(s->margin[FLOW_TOP], cbw), mb = len(s->margin[FLOW_BOTTOM], cbw);
    bool root = bfc_root(b);
    bool top_open = !root && b->border[FLOW_TOP] == 0 && b->padding[FLOW_TOP] == 0;
    bool height_auto = s->height.kind != FLOW_LENGTH_PX;
    bool bottom_open = !root && height_auto && b->border[FLOW_BOTTOM] == 0 &&
                       b->padding[FLOW_BOTTOM] == 0;

    b->x = cbx + ml;
    b->w = b->border[FLOW_LEFT] + b->padding[FLOW_LEFT] + cw + b->padding[FLOW_RIGHT] +
           b->border[FLOW_RIGHT];
    int64_t cx = b->x + b->border[FLOW_LEFT] + b->padding[FLOW_LEFT];
    int64_t pm = max64(cur->pm, mt);

    Cursor in;
    int64_t y_border = 0;
    if (top_open) {
        in = (Cursor){cur->y, pm, -1};
    } else {
        y_border = cur->y + pm;
        in = (Cursor){y_border + b->border[FLOW_TOP] + b->padding[FLOW_TOP], 0, -1};
        in.first = in.y;
    }
    int64_t content_top = in.y;

    if (b->kind == FB_REPLACED) {
        in.y += rep.h;
    } else if (b->ifc) {
        lines(l, styles, b, cx, cw, &in);
    } else {
        children(l, styles, b, cx, cw, &in);
    }
    if (l->failed)
        return;

    // Nothing inside resolved a position: the box is empty, and its top
    // and bottom margins collapse together with whatever adjoins them.
    if (top_open && in.first < 0 && height_auto && bottom_open) {
        b->placed = true;
        b->y = cur->y + pm;
        b->h = 0;
        cur->pm = max64(pm, max64(in.pm, mb));
        if (b->marker != NULL)
            place_marker(l, b, cx, b->y);
        return;
    }
    if (top_open) {
        y_border = in.first >= 0 ? in.first : cur->y + pm;
        content_top = y_border;
    }
    int64_t content_h, pm_out;
    if (!height_auto) {
        content_h = s->height.value;
        pm_out = mb;
    } else if (bottom_open) {
        content_h = in.y - content_top;
        pm_out = max64(in.pm, mb);
    } else {
        content_h = in.y + in.pm - content_top;
        pm_out = mb;
    }
    if (content_h < 0)
        content_h = 0;
    b->placed = true;
    b->y = y_border;
    b->h = b->border[FLOW_TOP] + b->padding[FLOW_TOP] + content_h + b->padding[FLOW_BOTTOM] +
           b->border[FLOW_BOTTOM];
    cur->y = b->y + b->h;
    cur->pm = pm_out;
    if (cur->first < 0)
        cur->first = y_border;
    if (b->marker != NULL)
        place_marker(l, b, cx, y_border + b->border[FLOW_TOP] + b->padding[FLOW_TOP]);
}

// ── The layout ──────────────────────────────────────────────────────────

static int64_t right_edge(const FBox *b)
{
    int64_t r = b->placed ? b->x + b->w : 0;
    for (const FLine *ln = b->lines; ln != NULL; ln = ln->next)
        for (const FFrag *fr = ln->frags; fr != NULL; fr = fr->next) {
            r = max64(r, fr->x + fr->w);
            if (fr->kind == FF_ATOMIC && fr->item->content != NULL)
                r = max64(r, right_edge(fr->item->content));
        }
    for (const FBox *c = b->first; c != NULL; c = c->next)
        r = max64(r, right_edge(c));
    return r;
}

FLayout *f_layout(FBoxes *boxes, const os64_html_document_t *doc, const os64_page_t *model,
                  const flow_env_t *env, int32_t width)
{
    if (boxes == NULL || env == NULL || width < 0)
        return NULL;
    FLayout *out = os64_calloc(1, sizeof(*out));
    if (out == NULL)
        return NULL;
    out->env = env;
    out->quirks = doc != NULL ? doc->quirks : OS64_HTML_NO_QUIRKS;
    out->model = model;
    out->boxes = boxes;
    out->incomplete = boxes->incomplete;
    L l = {out, env, model, out->quirks == OS64_HTML_QUIRKS,
           out->quirks != OS64_HTML_NO_QUIRKS, false};
    int64_t w = (int64_t)width * 64;
    out->width = w;
    if (boxes->root != NULL) {
        Cursor cur = {0, 0, -1};
        block(&l, boxes->styles, boxes->root, 0, w, &cur);
        out->height = cur.y + cur.pm;
        out->width = max64(w, right_edge(boxes->root));
    }
    return out;
}

void f_layout_free(FLayout *layout)
{
    if (layout == NULL)
        return;
    for (size_t i = 0; i < layout->nruns; i++)
        os64_text_run_release(layout->runs[i]);
    os64_free(layout->runs);
    f_arena_free(&layout->arena);
    os64_free(layout);
}
