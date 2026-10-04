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
// Recursion follows the box tree, whose depth the build holds to
// F_DEPTH_MAX (LAYOUT.md § Bounds).

#include "internal.h"

typedef struct {
    FLayout *out;
    const flow_env_t *env;
    const os64_page_t *model;
    bool quirks;        // full quirks
    bool any_quirks;    // quirks or limited quirks
    bool failed;
    // The list items whose outside markers have no line yet, outermost
    // first: the next line with content is the first line of every one of
    // them (an item that starts with a nested list shares the nested item's
    // first line), and holds each marker's font box. They nest no deeper
    // than the build does.
    const FBox *markers[F_DEPTH_MAX];
    int32_t nmarkers;
    size_t scratch;     // the working memory live now (Scratch, below)
} L;

// Vertical margins that adjoin collapse to the largest positive one plus
// the most negative (CSS 2.1 § 8.3.1), so a set of them is held as those
// two and summed where a box is placed.
typedef struct {
    int64_t pos, neg;   // pos >= 0, neg <= 0
} Margins;

static const Margins kNoMargins = {0, 0};

static int64_t max64(int64_t a, int64_t b)
{
    return a > b ? a : b;
}

static int64_t min64(int64_t a, int64_t b)
{
    return a < b ? a : b;
}

static Margins margin_of(int64_t m)
{
    return m >= 0 ? (Margins){m, 0} : (Margins){0, m};
}

static Margins margins_join(Margins a, Margins b)
{
    return (Margins){max64(a.pos, b.pos), min64(a.neg, b.neg)};
}

static int64_t margins_sum(Margins m)
{
    return m.pos + m.neg;
}

// The flow of one block formatting context: where the next box goes, the
// vertical margins adjoining that point that no box has resolved yet, and
// where the first content inside the current container was placed, if any
// has been.
typedef struct {
    int64_t y;
    Margins pm;
    int64_t first;      // -1 until content resolves the pending margins
} Cursor;

// The longest length layout resolves, in 26.6: as many pixels as a face
// can read. A page can nest percentages of percentages (`width=1000000%`
// three tables deep) past anything int64_t holds; held here, a length times
// a percentage, and the sum of every length on a path through the page,
// stay far inside it.
#define LEN_MAX ((int64_t)INT32_MAX * 64)

static int64_t hold_len(int64_t v)
{
    return v > LEN_MAX ? LEN_MAX : v < -LEN_MAX ? -LEN_MAX : v;
}

// a * b / c for a, b >= 0 and c > 0, exact where it fits and held to
// LEN_MAX where it does not: a picture's shape scales one side by the
// other, and the face may report a side as long as an int32_t. The 128-bit
// product is one instruction; its division is done here by hand, because
// dividing an __int128 calls a libgcc routine userland does not link.
static int64_t mul_div(int64_t a, int64_t b, int64_t c)
{
    int64_t p;
    if (!__builtin_mul_overflow(a, b, &p))
        return hold_len(p / c);
    unsigned __int128 n = (unsigned __int128)(uint64_t)a * (uint64_t)b;
    uint64_t hi = (uint64_t)(n >> 64), lo = (uint64_t)n;
    if (hi >= (uint64_t)c)
        return LEN_MAX;             // the quotient needs more than 64 bits
    uint64_t q = 0, r = hi;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((lo >> i) & 1);
        if (r >= (uint64_t)c) {
            r -= (uint64_t)c;
            q |= (uint64_t)1 << i;
        }
    }
    return q > (uint64_t)LEN_MAX ? LEN_MAX : (int64_t)q;
}

// A length against a containing block's width; auto reads as 0, and the
// caller that cares asks first. A percentage is a count of 1/64 percent,
// so it divides by 6400, taken in two parts that cannot overflow, and a
// calc()'s pixel offset is added to it.
static int64_t len(flow_length_t l, int64_t base)
{
    switch (l.kind) {
    case FLOW_LENGTH_PX: return hold_len(l.value);
    case FLOW_LENGTH_PERCENT: {
        int64_t b = hold_len(base);
        return hold_len(b / 6400 * l.value + b % 6400 * l.value / 6400 + hold_len(l.offset));
    }
    case FLOW_LENGTH_AUTO: case FLOW_LENGTH_FIT_CONTENT: case FLOW_LENGTH_CONTENT: break;
    }
    return 0;
}

int64_t f_len(flow_length_t l, int64_t base)
{
    return len(l, base);
}

// A width or height the page gave, as the content box's: a border-box one
// less its frame (CSS Sizing 3 § 4.1), and never below zero.
static int64_t content_of(const flow_style_t *s, flow_length_t v, int64_t base, int64_t frame)
{
    int64_t x = len(v, base) - (s->box_sizing == FLOW_BORDER_BOX ? frame : 0);
    return x > 0 ? x : 0;
}

// A content width held to max-width and then min-width (§10.4): when the
// two disagree, the minimum wins. Measured against no containing block
// (`base` 0, an intrinsic width), a percentage limit binds nothing.
static bool binds(flow_length_t limit, int64_t base)
{
    return limit.kind == FLOW_LENGTH_PX || (limit.kind == FLOW_LENGTH_PERCENT && base > 0);
}

static int64_t clamp_width(const flow_style_t *s, int64_t w, int64_t base, int64_t frame)
{
    if (binds(s->max_width, base))
        w = min64(w, content_of(s, s->max_width, base, frame));
    if (binds(s->min_width, base))
        w = max64(w, content_of(s, s->min_width, base, frame));
    return w;
}

// The same for a height. Only a length binds: a percentage would need a
// containing block of known height, and libflow sizes heights by content.
static int64_t clamp_height(const flow_style_t *s, int64_t h, int64_t frame)
{
    if (s->max_height.kind == FLOW_LENGTH_PX)
        h = min64(h, content_of(s, s->max_height, 0, frame));
    if (s->min_height.kind == FLOW_LENGTH_PX)
        h = max64(h, content_of(s, s->min_height, 0, frame));
    return h;
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
    if (l->scratch > l->out->arena.cap || size > l->out->arena.cap - l->scratch) {
        fail(l);
        return NULL;
    }
    l->out->arena.cap -= l->scratch;
    void *p = f_arena_alloc(&l->out->arena, size);
    l->out->arena.cap += l->scratch;
    if (p == NULL)
        fail(l);
    return p;
}

// ── Scratch ─────────────────────────────────────────────────────────────
//
// A table's working memory — its columns, holds, spans and rows, freed
// when it is placed — and a flex container's list of its items, charged
// to the same budget as the arena
// (flow_env_t.max_arena_bytes). Nesting keeps a chain of it live at once,
// a cell laying out the next table while its own table's columns wait, and
// columns can be DECLARED: ten `<col span=1000>`s a level are ten thousand
// columns in a few bytes. So a page can multiply it by its depth. Each
// block carries its size, so a free gives its bytes back. A formatting
// context's segments are not charged: they are its own words, one each,
// and nested contexts hold disjoint content, so no chain of them can
// outgrow the page.
typedef struct {
    size_t bytes;
    _Alignas(16) unsigned char data[];
} Scratch;

static Scratch *scratch_of(void *p)
{
    return p != NULL ? (Scratch *)((unsigned char *)p - offsetof(Scratch, data)) : NULL;
}

static void *scratch_realloc(L *l, void *p, size_t bytes)
{
    Scratch *old = scratch_of(p);
    size_t had = old != NULL ? old->bytes : 0;
    size_t cap = l->out->arena.cap, used = l->out->arena.reserved + l->scratch - had;
    if (l->failed || bytes > SIZE_MAX - sizeof(Scratch) || used > cap || bytes > cap - used) {
        fail(l);
        return NULL;
    }
    Scratch *b = os64_realloc(old, sizeof(Scratch) + bytes);
    if (b == NULL) {
        fail(l);
        return NULL;
    }
    b->bytes = bytes;
    l->scratch = l->scratch - had + bytes;
    if (l->scratch > l->out->scratch_peak)
        l->out->scratch_peak = l->scratch;
    return b->data;
}

static void *scratch_calloc(L *l, size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size) {
        fail(l);
        return NULL;
    }
    void *p = scratch_realloc(l, NULL, count * size);
    if (p != NULL)
        os64_memset(p, 0, count * size);
    return p;
}

static void scratch_free(L *l, void *p)
{
    Scratch *b = scratch_of(p);
    if (b == NULL)
        return;
    l->scratch -= b->bytes;
    os64_free(b);
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
// start, relative to the run's own origin. The stops repeat every
// interval, so the origin is handed over reduced modulo it: the same stops,
// in the engine's 32-bit coordinates however far along the line the run
// starts.
static os64_text_run_t *run_of(L *l, const Fonts *f, const char *text, uint32_t n,
                               int64_t tab_origin, int64_t tab_interval)
{
    if (l->failed)
        return NULL;
    int64_t interval = tab_interval > 0 ? tab_interval : 64 * 8;
    os64_text_layout_t opt = {
        .encoding = OS64_TEXT_UTF8_WESTERN_V1,
        .fonts = f->list,
        .font_count = f->count,
        .tab_origin = (os64_font_pos_t)(tab_origin % interval),
        .tab_interval = (os64_font_pos_t)interval,
    };
    os64_text_run_t *run = NULL;
    if (os64_text_layout(l->env->text, (const uint8_t *)text, n, &opt, &run) != OS64_FONT_OK) {
        fail(l);
        return NULL;
    }
    return run;
}

// The widest a run may be: the engine's coordinates are signed 32-bit 26.6,
// and a text inside the byte cap can still be wider than that (a million
// 48px M's). Half the range, so a run's end never nears it.
#define RUN_ADVANCE_MAX ((int64_t)INT32_MAX / 2)

// A measuring run, or NULL with the engine's reason, failing nothing: the
// caller decides whether LIMIT means "try less".
static os64_text_run_t *run_try(L *l, const Fonts *f, const char *text, uint32_t n,
                                int64_t tab_interval, os64_font_status_t *why)
{
    os64_text_layout_t opt = {
        .encoding = OS64_TEXT_UTF8_WESTERN_V1,
        .fonts = f->list,
        .font_count = f->count,
        .tab_origin = 0,
        .tab_interval = (os64_font_pos_t)(tab_interval > 0 ? tab_interval : 64 * 8),
    };
    os64_text_run_t *run = NULL;
    *why = os64_text_layout(l->env->text, (const uint8_t *)text, n, &opt, &run);
    return *why == OS64_FONT_OK ? run : NULL;
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
    bool none;              // not there at all: a missing picture whose alt is empty
} Replaced;

// CSS 2.1 §10.3.2 and §10.6.2: the size the page gave — a percentage
// height reads as unset (LAYOUT.md § Booked) — else the one the face
// measured (scaled to keep its shape when the page gave one side),
// else the chapter's rule for a picture that has not arrived — its alt
// text inline when it has one, nothing when its alt is empty, a small
// icon when it has none — and a sensible box for anything else: a frame's
// 300x150, a control ten ems wide and one line of its own font high.
static bool width_given(const flow_style_t *s)
{
    return s->width.kind == FLOW_LENGTH_PX || s->width.kind == FLOW_LENGTH_PERCENT;
}

static bool height_given(const flow_style_t *s)
{
    return s->height.kind == FLOW_LENGTH_PX;
}

bool flow_replaced_fixed(const flow_style_t *style)
{
    return style != NULL && width_given(style) && height_given(style);
}

static Replaced replaced_size(L *l, const os64_html_node_t *node, const flow_style_t *s,
                              int64_t cbw)
{
    Replaced r = {0, 0, false, false};
    bool w_set = width_given(s);
    bool h_set = height_given(s);
    int64_t fw = s->border_width[FLOW_LEFT] + s->border_width[FLOW_RIGHT] +
                 len(s->padding[FLOW_LEFT], cbw) + len(s->padding[FLOW_RIGHT], cbw);
    int64_t fh = s->border_width[FLOW_TOP] + s->border_width[FLOW_BOTTOM] +
                 len(s->padding[FLOW_TOP], cbw) + len(s->padding[FLOW_BOTTOM], cbw);
    int64_t w = content_of(s, s->width, cbw, fw), h = h_set ? content_of(s, s->height, 0, fh) : 0;
    int32_t iw = 0, ih = 0;
    bool known = l->env->replaced_size != NULL &&
                 l->env->replaced_size(l->env->ctx, node, &iw, &ih) && iw >= 0 && ih >= 0;
    int64_t kw = f_css_units(l->env, iw), kh = f_css_units(l->env, ih);
    if (known) {
        if (w_set && h_set) {
            r.w = w;
            r.h = h;
        } else if (w_set) {
            r.w = w;
            r.h = kw > 0 ? mul_div(w, kh, kw) : kh;
        } else if (h_set) {
            r.h = h;
            r.w = kh > 0 ? mul_div(h, kw, kh) : kw;
        } else {
            r.w = kw;
            r.h = kh;
        }
        // §10.4's limits, the picture's own ratio kept in whichever
        // dimension the page left to it (`img { max-width: 100% }`). The
        // rest of §10.4's table — both limits binding at once against the
        // ratio — is booked (GARB.md § Booked).
        int64_t held = clamp_width(s, r.w, cbw, fw);
        if (held != r.w) {
            if (!h_set && r.w > 0)
                r.h = r.h * held / r.w;
            r.w = held;
        }
        held = clamp_height(s, r.h, fh);
        if (held != r.h) {
            if (!w_set && r.h > 0)
                r.w = r.w * held / r.h;
            r.h = held;
        }
        return r;
    }
    // A picture whose size is not known yet is guessed square from the
    // side the page gave, until it arrives; anything else keeps its own
    // default on the side the page left unset (a frame's 300 by 150).
    bool img = node->ns == OS64_HTML_NS_HTML && node->tag == OS64_HTML_TAG_IMG;
    if (img && (w_set || h_set)) {
        r.w = w_set ? w : h;
        r.h = h_set ? h : w;
        return r;
    }
    if (img) {
        const os64_html_attr_t *alt = os64_html_attr(node, "alt");
        if (alt != NULL && alt->value != NULL && alt->value[0] != '\0') {
            r.alt = true;
        } else if (alt == NULL) {
            r.w = r.h = f_css_units(l->env, 16);
        } else {
            // An empty alt says the picture is decoration: it takes no space.
            r.none = true;
        }
        return r;
    }
    bool frame = node->ns == OS64_HTML_NS_HTML &&
                 (node->tag == OS64_HTML_TAG_IFRAME || node->tag == OS64_HTML_TAG_FRAME);
    if (frame) {
        r.w = f_css_units(l->env, 300);
        r.h = f_css_units(l->env, 150);
    } else {
        Fonts f;
        r.w = (int64_t)s->font_size * 10;
        r.h = fonts_for(l, s, &f) ? f.info.line_height : s->font_size;
    }
    if (w_set)
        r.w = w;
    if (h_set)
        r.h = h;
    return r;
}

// Content size `size` on `axis` (0 across, 1 down) as the page would have
// written it: a border box's includes its frame, so `box-sizing` goes on
// meaning what it did for the limits a copied style keeps.
static flow_length_t replaced_given(const flow_style_t *s, int axis, int64_t size, int64_t cbw)
{
    int64_t frame = axis == 0
        ? s->border_width[FLOW_LEFT] + s->border_width[FLOW_RIGHT] +
              len(s->padding[FLOW_LEFT], cbw) + len(s->padding[FLOW_RIGHT], cbw)
        : s->border_width[FLOW_TOP] + s->border_width[FLOW_BOTTOM] +
              len(s->padding[FLOW_TOP], cbw) + len(s->padding[FLOW_BOTTOM], cbw);
    flow_length_t given;
    os64_memset(&given, 0, sizeof(given));
    given.kind = FLOW_LENGTH_PX;
    given.value = (int32_t)min64(max64(0, size) + (s->box_sizing == FLOW_BORDER_BOX ? frame : 0),
                                 INT32_MAX);
    return given;
}

// A picture's own size on one axis (0 across, 1 down), content box: as
// if the page had given it no size and no limits on that axis, the size
// on the other axis carried through the picture's ratio — the page's, or,
// `across` >= 0, that content size (a line stretched it there). What a
// flex base of `content`, or of `auto` with no size, and an automatic
// minimum read (Flexbox 1 § 9.2, § 4.5): a size the page gave on the flex
// axis is the algorithm's to weigh, never the content's.
static int64_t replaced_own(L *l, const FBox *b, int axis, int64_t cbw, int64_t across)
{
    flow_style_t own = *b->style;
    flow_length_t none;
    os64_memset(&none, 0, sizeof(none));
    if (axis == 0)
        own.width = own.min_width = own.max_width = none;
    else
        own.height = own.min_height = own.max_height = none;
    if (across >= 0 && axis == 0) {
        own.height = replaced_given(&own, 1, across, cbw);
        own.min_height = own.max_height = none;
    } else if (across >= 0) {
        own.width = replaced_given(&own, 0, across, cbw);
        own.min_width = own.max_width = none;
    }
    Replaced r = replaced_size(l, b->node, &own, cbw);
    return axis == 0 ? r.w : r.h;
}

// A picture's size across the other axis when it is given content size
// `size` on `axis` (0 across, 1 down) and the page gave it none there:
// through its own ratio, held to that axis's limits as the page wrote them.
// A used size, where replaced_own is a content one.
static int64_t replaced_for(L *l, const FBox *b, int axis, int64_t size, int64_t cbw)
{
    flow_style_t own = *b->style;
    flow_length_t none;
    os64_memset(&none, 0, sizeof(none));
    if (axis == 0) {
        own.width = replaced_given(&own, 0, size, cbw);
        own.min_width = own.max_width = none;
    } else {
        own.height = replaced_given(&own, 1, size, cbw);
        own.min_height = own.max_height = none;
    }
    Replaced r = replaced_size(l, b->node, &own, cbw);
    return axis == 0 ? r.h : r.w;
}

// ── Relative offsets ────────────────────────────────────────────────────

// A containing block's height when it does not depend on its content — a
// height in pixels on the nearest box with an element — or -1: what a
// percentage top or bottom is of (CSS 2.1 § 9.3.2, where the other case
// computes it to auto).
static int64_t definite_height(const FBox *b)
{
    while (b != NULL && b->node == NULL)
        b = b->parent;
    if (b == NULL || b->style->height.kind != FLOW_LENGTH_PX)
        return -1;
    const flow_style_t *s = b->style;
    int64_t frame = s->border_width[FLOW_TOP] + s->border_width[FLOW_BOTTOM] +
                    len(s->padding[FLOW_TOP], 0) + len(s->padding[FLOW_BOTTOM], 0);
    return content_of(s, s->height, 0, frame);
}

// How far a relative box moves from where the flow put it (CSS 2.1 §
// 9.4.3): `left` over `right`, `top` over `bottom`, percentages of its
// containing block — a vertical one only against a definite height (`cbh`
// >= 0). A sticky box is laid out where the flow puts it, and the door
// moves it as the page scrolls (flow_box_doc_offset).
static void rel_offset(const flow_style_t *s, int64_t cbw, int64_t cbh, int64_t *dx, int64_t *dy)
{
    *dx = *dy = 0;
    if (s->position != FLOW_POSITION_RELATIVE)
        return;
    if (s->inset[FLOW_LEFT].kind != FLOW_LENGTH_AUTO)
        *dx = len(s->inset[FLOW_LEFT], cbw);
    else if (s->inset[FLOW_RIGHT].kind != FLOW_LENGTH_AUTO)
        *dx = -len(s->inset[FLOW_RIGHT], cbw);
    bool top = s->inset[FLOW_TOP].kind != FLOW_LENGTH_AUTO;
    flow_length_t v = top ? s->inset[FLOW_TOP] : s->inset[FLOW_BOTTOM];
    if (v.kind == FLOW_LENGTH_AUTO || (v.kind == FLOW_LENGTH_PERCENT && cbh < 0))
        return;
    *dy = top ? len(v, cbh) : -len(v, cbh);
}

// ── Decorations and links ───────────────────────────────────────────────

typedef struct {
    uint8_t decoration;
    FDecorationColors colors;
    int32_t link;
} Paint;

// What a fragment is drawn with that its own style does not say: the
// decorations propagated to it and the link it sits in, which pass 1
// answered for its element (FStyled). An atom's own fragment is decorated
// by what surrounds it — the edge an atom makes keeps its ancestors'
// decorations off its CONTENT, not off the atom — unless it is a table in
// full quirks, which keeps them off both.
static Paint paint_for(const L *l, const FStyles *styles, const os64_html_node_t *n,
                       const os64_html_node_t *atom)
{
    const os64_html_node_t *e = n;
    while (e != NULL && e->kind != OS64_HTML_ELEMENT)
        e = e->parent;
    const FStyled *st = e != NULL ? f_style_of(styles, e) : NULL;
    if (st == NULL)
        return (Paint){0, {0, 0}, -1};
    Paint p = {st->decoration, st->decoration_colors, st->link};
    bool quirks_table = l->quirks && e->ns == OS64_HTML_NS_HTML && e->tag == OS64_HTML_TAG_TABLE;
    if (e == atom && !quirks_table) {
        const FStyled *up = f_style_of(styles, e->parent);
        if (up != NULL)
            f_decoration_inherit(&p.decoration, &p.colors, up->decoration,
                                 up->decoration_colors);
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

typedef enum { SG_WORD, SG_ATOM, SG_OPEN, SG_CLOSE, SG_BREAK, SG_WBR, SG_MARKER,
               SG_PLACEHOLDER } SegKind;

// What a line holds nothing of — no width, no opportunity, nothing that
// ends a run of white space: an inline box's edges, and where an absolute
// box was written.
static bool seg_is_edge(SegKind k)
{
    return k == SG_OPEN || k == SG_CLOSE || k == SG_PLACEHOLDER;
}

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
    bool keep_round;        // ATOM: a quirks-mode picture, no opportunity either side
    // WORD whose spaces hold a tab: the stops (the block's interval) and a
    // space's advance, so the spaces are measured where the line puts them
    // — a tab's width depends on the pen it starts from.
    int64_t tab_interval, space_w;
    Replaced atom;          // ATOM: its content box
    int64_t frame_w, frame_h, ml, mr, mt, mb;   // ATOM: border+padding, margins
} Seg;

typedef struct {
    Seg *v;
    size_t n, cap;
    // Something that draws nothing (a missing picture with no alt, an alt
    // that collapsed to nothing) stood after a collapsible space: the next
    // text's leading space is the same space and collapses into it. Pass 2
    // could not know — the picture's absence is the face's, at layout.
    bool drop_space;
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
    if (!seg_is_edge(kind))
        s->drop_space = false;
    return g;
}

// Whether the text before the end of the segments, across inline edges,
// ends in a space that collapses.
static bool ends_in_space(const Segs *s)
{
    for (size_t k = s->n; k > 0; k--) {
        const Seg *p = &s->v[k - 1];
        if (seg_is_edge(p->kind))
            continue;
        return p->kind == SG_WORD && p->collapsible && p->s1 > p->b1;
    }
    return false;
}

// A space's advance in these fonts, for spaces with no other measure.
static int64_t space_advance(L *l, const Fonts *f)
{
    os64_font_status_t why;
    os64_text_run_t *run = run_try(l, f, " ", 1, 0, &why);
    if (run == NULL)
        return 0;
    int64_t w = caret_x(run, 1);
    os64_text_run_release(run);
    return w;
}

// A text's words: measured ONCE, then cut at its spaces. A text longer than
// the engine takes in one run (OS64_TEXT_BYTES_MAX) is measured in WINDOWS
// cut after a space, so a word never straddles two; a window with no space
// in it is cut at a character boundary, and the word it splits stays one
// word — two segments with no opportunity between them. Never a refusal.
// Spaces holding a tab are the exception to "once": a tab is as wide as
// the way to the line's next stop, so the breaker walks them where the
// line puts them (spaces_width).
static void text_segments(L *l, Segs *s, const FItem *it, const char *text, uint32_t n,
                          const flow_style_t *style, int64_t tab_interval)
{
    Fonts f;
    if (n == 0 || !fonts_for(l, style, &f))
        return;
    flow_white_space_t ws = style->white_space;
    bool collapsible = f_ws_collapses(ws);
    bool whole = ws == FLOW_WS_PRE || ws == FLOW_WS_NOWRAP;
    uint32_t lead = 0;
    if (s->drop_space && collapsible)
        while (lead < n && text[lead] == ' ')
            lead++;
    s->drop_space = false;
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
        // a million-byte window costs tens of megabytes of the budget. One
        // too WIDE for the engine's coordinates is halved at a character
        // boundary until it fits, so long text is still never refused.
        os64_text_run_t *run = NULL;
        for (;;) {
            os64_font_status_t why;
            run = l->failed ? NULL : run_try(l, &f, text + w0, w1 - w0, tab_interval, &why);
            if (run != NULL || l->failed || why != OS64_FONT_LIMIT)
                break;
            uint32_t half = w0 + (w1 - w0) / 2;
            while (half > w0 && ((unsigned char)text[half] & 0xC0) == 0x80)
                half--;
            if (half == w0)
                break;
            w1 = half;
        }
        if (run == NULL) {
            fail(l);
            return;
        }
        if (whole) {
            // No opportunity inside: one word per window, its trailing
            // collapsible space still removable at a line's end.
            uint32_t b0 = w0 == 0 ? lead : w0;
            uint32_t end = w1;
            if (collapsible && w1 == n)
                while (end > b0 && text[end - 1] == ' ')
                    end--;
            Seg *g = seg_add(l, s, SG_WORD);
            if (g != NULL) {
                *g = (Seg){.kind = SG_WORD, .item = it, .text = text, .style = style,
                           .b0 = b0, .b1 = end, .s1 = w1, .collapsible = collapsible};
                int64_t x0 = caret_x(run, b0 - w0);
                g->w = caret_x(run, end - w0) - x0;
                g->sw = caret_x(run, w1 - w0) - x0 - g->w;
            }
        } else {
            for (uint32_t at = w0 == 0 ? lead : w0; at < w1;) {
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
                for (uint32_t t = b1; t < at; t++)
                    if (text[t] == '\t')
                        g->tab_interval = tab_interval > 0 ? tab_interval : 64 * 8;
                for (uint32_t t = b1; t < at && g->tab_interval > 0; t++)
                    if (text[t] == ' ') {
                        g->space_w = caret_x(run, t + 1 - w0) - caret_x(run, t - w0);
                        break;
                    }
                if (g->tab_interval > 0 && g->space_w == 0)
                    g->space_w = space_advance(l, &f);
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

typedef struct {
    int64_t min, max;
} Intr;

static Intr intrinsic(L *l, FBox *b);
static int64_t hframe(const FBox *b, int64_t base);
static int64_t vframe(const FBox *b, int64_t base);

static void segments(L *l, FBox *ifc, int64_t cw, Segs *s)
{
    l->out->measures++;
    int64_t tabs = 0;
    for (const FItem *it = ifc->items; it != NULL && !l->failed; it = it->next) {
        switch (it->kind) {
        case FI_TEXT: {
            if (tabs == 0 && !f_ws_collapses(it->style->white_space))
                tabs = tab_interval_for(l, ifc->style);
            text_segments(l, s, it, it->text, it->len, it->style, tabs);
            break;
        }
        case FI_ATOMIC: {
            const flow_style_t *st = it->style;
            Replaced r = {0, 0, false, false};
            if (it->content == NULL)
                r = replaced_size(l, it->node, st, cw);
            if (r.alt) {
                // The alt text stands where the picture would, as text:
                // under collapsing white-space its spaces collapse too.
                const char *alt = os64_html_attr(it->node, "alt")->value;
                uint32_t n = (uint32_t)os64_strlen(alt);
                if (f_ws_collapses(st->white_space)) {
                    char *copy = alloc(l, n);
                    if (copy == NULL)
                        break;
                    // Against the text beside it too, across inline edges
                    // (`a <b><img alt=" b"></b>`): a space after a space the
                    // text before already ends in, or before one the next
                    // text starts with, is the same space.
                    bool space = ends_in_space(s);
                    uint32_t m = f_collapse_white(alt, n, copy, &space);
                    const FItem *next = it->next;
                    while (next != NULL && (next->kind == FI_OPEN || next->kind == FI_CLOSE ||
                                            next->kind == FI_PLACEHOLDER))
                        next = next->next;
                    if (m > 0 && copy[m - 1] == ' ' && next != NULL && next->kind == FI_TEXT &&
                        next->len > 0 && next->text[0] == ' ' &&
                        f_ws_collapses(next->style->white_space))
                        m--;
                    alt = copy;
                    n = m;
                }
                if (n == 0) {
                    s->drop_space = ends_in_space(s);
                    break;
                }
                text_segments(l, s, it, alt, n, st, tabs);
                break;
            }
            if (r.none) {
                s->drop_space = ends_in_space(s);
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
            // An atom with content of its own is laid out inside it, which
            // gives its height. A marquee is as wide as the line; any other
            // inline-block is its set width, or shrinks to fit (CSS 2.1 §
            // 10.3.9): its min-content width at least, its max-content at
            // most, and between them the room the line has. Measured for a
            // parent's intrinsic widths (cw 0), that is its min-content.
            if (it->content != NULL) {
                int64_t room = max64(0, cw - fw - g->ml - g->mr);
                if (it->node->tag == OS64_HTML_TAG_MARQUEE) {
                    r.w = room;
                } else if (width_given(st)) {
                    r.w = content_of(st, st->width, cw, fw);
                } else {
                    // intrinsic() answers border-box widths with the frame's
                    // percentages at 0 (the containing block is not known
                    // there); take off that frame, not the line's, to have
                    // the content's own preferred widths.
                    Intr in = intrinsic(l, it->content);
                    int64_t f0 = hframe(it->content, 0);
                    r.w = min64(max64(in.min - f0, room), in.max - f0);
                    r.w = max64(0, r.w);
                }
                r.w = clamp_width(st, r.w, cw, fw);
            }
            g->atom = r;
            g->w = g->ml + fw + r.w + g->mr;
            g->keep_round = quirk_no_wrap_round(l, ifc, it);
            bool wrap = f_ws_wraps(st->white_space) && !g->keep_round;
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
            // A break opportunity where its own white-space allows one: a
            // wbr inside `nobr` is `normal` (pass 1), one in a nowrap or
            // pre run is not.
            Seg *g = seg_add(l, s, SG_WBR);
            if (g != NULL) {
                g->item = it;
                g->wrap_after = f_ws_wraps(it->style->white_space);
            }
            break;
        }
        case FI_PLACEHOLDER: {
            if (it->absolute == NULL)
                break;
            Seg *g = seg_add(l, s, SG_PLACEHOLDER);
            if (g != NULL) {
                g->item = it;
                g->style = it->style;
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
            // The marker ends in its suffix space, and the break after it is
            // the item's to allow: the item is the nearest box holding both
            // the space and the text that follows (CSS Text 3 § 5).
            g->wrap_after = it->len > 0 && it->text[it->len - 1] == ' ' &&
                            f_ws_wraps(it->style->white_space);
            break;
        }
        }
    }
    // A quirks-mode picture keeps no opportunity either side of it, and
    // the whitespace between two slices is one of its sides: the
    // opportunity before it ends whatever precedes it, and the one after it
    // is the whitespace that follows. Inline edges hold none of their own.
    for (size_t j = 0; j < s->n; j++) {
        if (!s->v[j].keep_round)
            continue;
        for (size_t k = j; k > 0; k--) {
            Seg *p = &s->v[k - 1];
            if (seg_is_edge(p->kind))
                continue;
            if (p->kind != SG_BREAK)
                p->wrap_after = false;
            break;
        }
        for (size_t k = j + 1; k < s->n; k++) {
            Seg *n = &s->v[k];
            if (seg_is_edge(n->kind))
                continue;
            if (n->kind == SG_WORD && n->b1 == n->b0)
                n->wrap_after = false;
            break;
        }
    }
}

// ── Inline formatting: lines ────────────────────────────────────────────

// A word's spaces, starting at line position `x`: measured once where they
// hold no tab, and walked from `x` where they do, each tab to the line's
// next stop as the text engine places it (the line starts at 0).
static int64_t spaces_width(const Seg *g, int64_t x)
{
    if (g->tab_interval <= 0)
        return g->sw;
    int64_t at = x;
    for (uint32_t t = g->b1; t < g->s1; t++) {
        if (g->text[t] == '\t') {
            int64_t k = at / g->tab_interval;
            if (at < 0 && at % g->tab_interval != 0)
                k--;
            at = (k + 1) * g->tab_interval;
        } else {
            at += g->space_w;
        }
    }
    return at - x;
}

// Greedy line breaking (CSS 2.1 §9.4.2): as much as fits, breaking at the
// LAST soft wrap opportunity before what does not; a word wider than the
// line goes on a line of its own and overflows it (`overflow-wrap:
// normal`). Returns the segment after the line; `*forced` says a break
// ended it.
static size_t break_line(const Segs *s, size_t start, int64_t indent, int64_t avail,
                         bool *forced)
{
    // The pen starts where the line does, after a first line's indent, so
    // a tab reaches the same stop here as where the line is laid down.
    int64_t pen = indent;
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
            pen += spaces_width(g, pen);
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
// An inline box's extent about its baseline (CSS 2.1 § 10.8.1): its
// ascent and descent with the leading — line-height less the two — split
// above and below. `normal` is the face's own line height; a leading may
// be negative, and then the box is shorter than its glyphs.
static Extent font_box(const flow_style_t *s, int64_t ascent, int64_t descent, int64_t normal)
{
    int64_t line_height = s->line_height.kind == FLOW_LINE_NUMBER
                              ? (int64_t)s->font_size * s->line_height.value / 1000
                        : s->line_height.kind == FLOW_LINE_PX ? s->line_height.value : normal;
    int64_t half = (line_height - ascent - descent) / 2;
    return (Extent){-ascent - half, descent + (line_height - ascent - descent - half)};
}

static FFrag *frag_add(L *l, FLine *line, f_frag_kind_t kind, int64_t rel_x, int64_t rel_y)
{
    FFrag *f = alloc(l, sizeof(*f));
    if (f == NULL)
        return NULL;
    f->kind = kind;
    f->link = -1;
    f->rel_x = rel_x;
    f->rel_y = rel_y;
    if (line->last_frag != NULL)
        line->last_frag->next = f;
    else
        line->frags = f;
    line->last_frag = f;
    return f;
}

static FSpan *span_add(L *l, FLine *line, FInline *inl, int64_t x0, int64_t x1, int64_t rel_x,
                       int64_t rel_y)
{
    FSpan *sp = alloc(l, sizeof(*sp));
    if (sp == NULL)
        return NULL;
    sp->inl = inl;
    sp->x0 = x0;
    sp->x1 = x1;
    sp->rel_x = rel_x;
    sp->rel_y = rel_y;
    // A positioned inline's first and last pieces bound it as a containing
    // block (CSS 2.1 § 10.1, 4.1).
    if (inl->pos != NULL) {
        if (inl->first_span == NULL)
            inl->first_span = sp;
        inl->last_span = sp;
    }
    if (line->last_span != NULL)
        line->last_span->next = sp;
    else
        line->spans = sp;
    line->last_span = sp;
    return sp;
}

static void block(L *l, const FStyles *styles, FBox *b, int64_t cbx, int64_t cbw, Cursor *cur);
static void translate(FBox *b, int64_t dx, int64_t dy);
static void table(L *l, const FStyles *styles, FBox *t, int64_t cbx, int64_t cbw, Cursor *cur);
static void positioned_done(L *l, const FStyles *styles, FBox *b, int64_t cbw);
static int64_t fit_content(L *l, FBox *b, int64_t cbw);
static void unplace(FBox *b);

// Open inline boxes carried from one line to the next.
typedef struct {
    FInline **v;
    size_t n, cap;
} Open;

static bool open_push(L *l, Open *o, FInline *inl)
{
    if (o->n == o->cap) {
        size_t cap = o->cap != 0 ? o->cap * 2 : 8;
        FInline **grown = os64_realloc(o->v, cap * sizeof(*grown));
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

// The baseline of a box's last line with content, in its flow (CSS 2.1 §
// 10.8.1: an inline-block's baseline), or a flex or grid container's;
// false for a box with none. A table inside is not looked into.
static bool first_baseline_in(const FBox *b, int64_t *out);

static bool last_baseline(const FBox *b, int64_t *out)
{
    bool found = false;
    // A flex container's last baseline is read as its first (flex_baseline),
    // and so is a grid container's (grid_baseline).
    if (b->flex || b->grid)
        return first_baseline_in(b, out);
    if (b->ifc) {
        for (const FLine *ln = b->lines; ln != NULL; ln = ln->next)
            if (ln->h > 0) {
                *out = ln->baseline;
                found = true;
            }
        return found;
    }
    for (const FBox *c = b->first; c != NULL; c = c->next)
        if (c->kind == FB_BLOCK && !c->out_of_flow && last_baseline(c, out))
            found = true;
    return found;
}

// The indent of a context's first line (CSS Text 3 § 8.1): an element's
// own, and an anonymous block's only when it holds its parent's first
// line — the one before any block child.
static int64_t first_line_indent(const FBox *ifc, int64_t cw)
{
    if (ifc->node == NULL && (ifc->parent == NULL || ifc->parent->first != ifc))
        return 0;
    return len(ifc->style->text_indent, cw);
}

// How many of a justified line's gaps end at or before pen position `x`:
// how many widenings a fragment or an inline box's edge there moves by.
static int64_t gaps_before(const int64_t *gap_at, size_t gaps, int64_t x)
{
    int64_t n = 0;
    for (size_t i = 0; i < gaps && gap_at[i] <= x; i++)
        n++;
    return n;
}

static void lines(L *l, const FStyles *styles, FBox *ifc, int64_t cx, int64_t cw, Cursor *cur)
{
    // Pass 2 stopped inside this context: its last line was broken without
    // the items that would have followed, so it is not a line to trust.
    bool cut = ifc->unfinished;
    Segs s = {0};
    segments(l, ifc, cw, &s);
    Fonts bf;
    bool have_block_font = fonts_for(l, ifc->style, &bf);
    Open open = {0};
    // The x each open inline's piece on the current line starts at.
    int64_t *open_x = NULL;
    size_t open_x_cap = 0;
    int64_t *gap_at = NULL;             // where each justified gap ends, in pen x
    size_t gap_cap = 0;
    // The relative inlines open here move what is inside them: the sum of
    // their offsets, carried across lines as the open inlines are (their
    // containing block is this context's, whose width is `cw`).
    int64_t rel_x = 0, rel_y = 0, cbh = definite_height(ifc);

    flow_text_align_t align = ifc->style->text_align;
    bool justify = align == FLOW_ALIGN_JUSTIFY || align == FLOW_ALIGN_HTML_JUSTIFY;
    // text-indent: the first line of an element's own text starts this far
    // in, and has that much less room.
    int64_t indent = first_line_indent(ifc, cw);

    for (size_t start = 0; start < s.n && !l->failed;) {
        bool forced;
        size_t end = break_line(&s, start, indent, cw, &forced);
        // An inline that closes right where a break falls closes on this
        // line, not as an empty piece on the next.
        while (end < s.n && s.v[end].kind == SG_CLOSE)
            end++;
        bool last = end >= s.n;

        // A line joins its context only once it is whole — every fragment
        // and span placed — so a layout that stops partway leaves no half
        // line in the tree, only the lines before it.
        FLine *line = alloc(l, sizeof(*line));
        if (line == NULL)
            break;
        line->x = cx;
        line->y = cur->y;
        line->baseline = cur->y;
        line->w = cw;

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

        // Horizontal: lay the fragments down from 0, or from the first
        // line's indent; alignment shifts them after, once the line's
        // width is known. Where each gap a justified line may grow ends is
        // kept, so what comes after it — a fragment, an inline box's edge
        // — moves by the gaps before it.
        int64_t pen = indent, used = indent;
        indent = 0;
        size_t gaps = 0;
        bool content = false, has_text = false, has_break = false;
        Extent brk_box = {0, 0};
        Extent ext = {0, 0};
        bool any = false;
        // The strut: the block's own font's box, so a line of small text is
        // as tall as its neighbours. Quirks and limited-quirks modes make
        // none (the Quirks standard's 3.4) — the sliced-image fix.
        if (have_block_font && !l->any_quirks) {
            Extent strut = font_box(ifc->style, bf.info.ascent, bf.info.descent, bf.info.line_height);
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
                size_t k = j;
                // It stops short of the run cap, and of the widest run the
                // engine's coordinates hold.
                int64_t wide = g->w + g->sw;
                if (!spread)
                    while (k + 1 < end && s.v[k + 1].kind == SG_WORD &&
                           s.v[k + 1].item == g->item && s.v[k + 1].text == g->text &&
                           s.v[k + 1].s1 - g->b0 <= OS64_TEXT_BYTES_MAX &&
                           wide + s.v[k + 1].w + s.v[k + 1].sw <= RUN_ADVANCE_MAX) {
                        wide += s.v[k + 1].w + s.v[k + 1].sw;
                        k++;
                    }
                const Seg *e = &s.v[k];
                bool at_tail = tail == SIZE_MAX || k >= tail;
                uint32_t b0 = g->b0;
                if (!content && g->collapsible)
                    while (b0 < g->b1 && g->text[b0] == ' ')
                        b0++;
                uint32_t b1 = e->s1;
                if (spread || (at_tail && e->collapsible))
                    b1 = e->b1;
                // The tab stops first: they ask for the block's fonts, and
                // a font list is good only until the next request.
                bool pre = g->style->white_space == FLOW_WS_PRE ||
                           g->style->white_space == FLOW_WS_PRE_WRAP;
                int64_t tabs = 0;
                for (uint32_t t = b0; t < b1 && pre; t++)
                    if (g->text[t] == '\t') {
                        tabs = tab_interval_for(l, ifc->style);
                        break;
                    }
                Fonts f;
                if (b1 > b0 && fonts_for(l, g->style, &f)) {
                    // Tabs stop where the LINE's stops are, whatever this
                    // fragment starts after.
                    os64_text_run_t *run = run_of(l, &f, g->text + b0, b1 - b0,
                                                  pre ? -pen : 0, tabs);
                    if (run == NULL || !keep_run(l, run))
                        break;
                    FFrag *fr = frag_add(l, line, FF_TEXT, rel_x, rel_y);
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
                    fr->decoration_colors = p.colors;
                    fr->link = p.link;
                    Extent fb = font_box(g->style, v.ascent, v.descent, v.line_height);
                    extend(&ext, &any, fb.top, fb.bottom);
                    // Pre-wrap spaces ending a line hang: they are drawn, and
                    // count for nothing — not the line's width, not the page's.
                    if (at_tail && e->hang && !spread && e->b1 > b0)
                        fr->hang = v.advance_x - caret_x(run, e->b1 - b0);
                    else if (at_tail && e->hang && !spread)
                        fr->hang = v.advance_x;
                    pen += v.advance_x;
                    used = pen - fr->hang;
                    has_text = true;
                    content = true;
                }
                if (spread && !at_tail && e->s1 > e->b1) {
                    pen += spaces_width(e, pen);
                    if (gaps == gap_cap) {
                        size_t cap = gap_cap ? gap_cap * 2 : 16;
                        int64_t *grown = os64_realloc(gap_at, cap * sizeof(*grown));
                        if (grown == NULL) {
                            fail(l);
                            break;
                        }
                        gap_at = grown;
                        gap_cap = cap;
                    }
                    gap_at[gaps++] = pen;
                }
                j = k;
                break;
            }
            case SG_ATOM: {
                // A relative atom moves by its own offset too.
                int64_t own_x, own_y;
                rel_offset(g->style, cw, cbh, &own_x, &own_y);
                FFrag *fr = frag_add(l, line, FF_ATOMIC, rel_x + own_x, rel_y + own_y);
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
                    // moved into place once the line is placed. The block
                    // is the element's own box, against the line it was
                    // measured on — its percentages are of the line's
                    // width, once — with the width the line gave it.
                    Cursor c = {0, kNoMargins, -1};
                    g->item->content->atom_sized = true;
                    g->item->content->atom_w = g->atom.w;
                    block(l, styles, g->item->content, 0, cw, &c);
                    content_h = g->item->content->placed ? g->item->content->h - g->frame_h : 0;
                }
                fr->h = g->frame_h + content_h;
                Paint p = paint_for(l, styles, g->item->node, g->item->node);
                fr->link = g->item->link >= 0 ? g->item->link : p.link;
                int64_t hm = g->mt + fr->h + g->mb;
                // The baseline of a replaced box is its bottom margin edge;
                // an inline-block's is its last line's, where it has one,
                // unless it is a scroll container (CSS 2.1 § 10.8.1, in CSS
                // Inline 3's terms, where `overflow: clip` makes none): one a
                // sheet makes so, or a marquee, which the chapter makes
                // `overflow: hidden !important` — asked by its tag, because
                // this chapter does not give the marquee that clip (it would
                // cut the text a still marquee lets overflow; booked).
                int64_t top = -hm, base = 0;
                bool scroller = f_overflow_scrolls(g->style->overflow_x) ||
                                f_overflow_scrolls(g->style->overflow_y) ||
                                g->item->node->tag == OS64_HTML_TAG_MARQUEE;
                if (g->item->content != NULL && g->item->content->placed && !scroller &&
                    last_baseline(g->item->content, &base))
                    top = -(g->mt + base - g->item->content->y);
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
                FFrag *fr = frag_add(l, line, FF_MARKER, rel_x, rel_y);
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
                // In its item's line, so in the item's link and under its
                // decorations, as the text beside it is.
                Paint mp = paint_for(l, styles, g->item->node, NULL);
                fr->decoration = mp.decoration;
                fr->decoration_colors = mp.colors;
                fr->link = mp.link;
                Extent fb = font_box(g->style, v.ascent, v.descent, v.line_height);
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
                    FInline *inl = g->item->inl;
                    rel_offset(inl->style, cw, cbh, &inl->rel_x, &inl->rel_y);
                    rel_x += inl->rel_x;
                    rel_y += inl->rel_y;
                }
                break;
            case SG_CLOSE:
                // Inlines close innermost first; the piece ends here.
                for (size_t k = open.n; k > 0; k--)
                    if (open.v[k - 1] == g->item->inl) {
                        span_add(l, line, open.v[k - 1], open_x[k - 1], pen, rel_x, rel_y);
                        rel_x -= open.v[k - 1]->rel_x;
                        rel_y -= open.v[k - 1]->rel_y;
                        for (size_t m = k - 1; m + 1 < open.n; m++) {
                            open.v[m] = open.v[m + 1];
                            open_x[m] = open_x[m + 1];
                        }
                        open.n--;
                        break;
                    }
                break;
            case SG_PLACEHOLDER: {
                // Where the out-of-flow box stood: its static position, read
                // once the line is placed (and moved with it after).
                FFrag *fr = frag_add(l, line, FF_PLACEHOLDER, rel_x, rel_y);
                if (fr == NULL)
                    break;
                fr->item = g->item;
                fr->node = g->item->node;
                fr->style = g->style;
                fr->x = pen;
                g->item->absolute->place = fr;
                break;
            }
            case SG_BREAK: {
                // A forced break holds its line open with its own font's box
                // — that is `a<br><br>b`'s blank line — but in quirks mode
                // only when nothing else is on the line: a <br> between two
                // sliced images must not open a gap under the first.
                Fonts f;
                if (fonts_for(l, g->style, &f)) {
                    brk_box = font_box(g->style, f.info.ascent, f.info.descent, f.info.line_height);
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
            span_add(l, line, open.v[k], open_x[k], pen, rel_x, rel_y);
        // A line only joins its context whole (above): one pass 3 could
        // not finish is dropped.
        if (l->failed)
            break;

        // No-quirks: every inline box on the line holds its own font's box
        // open, empty or not, on a line that is not itself empty (in quirks
        // mode an empty one does not, the Quirks standard's 3.3).
        if (!l->any_quirks)
            for (FSpan *sp = line->spans; sp != NULL; sp = sp->next) {
                Fonts f;
                if (fonts_for(l, sp->inl->style, &f)) {
                    Extent fb = font_box(sp->inl->style, f.info.ascent, f.info.descent, f.info.line_height);
                    extend(&ext, &any, fb.top, fb.bottom);
                }
            }

        if (has_break && (!l->any_quirks || (!has_text && !content)))
            extend(&ext, &any, brk_box.top, brk_box.bottom);

        // Vertical (§10.8): the line is as tall as what sits on its
        // baseline, then as tall as anything aligned to its top or bottom.
        // A line with no text, no break and nothing inline with a margin,
        // padding or border of its own is zero-height (CSS 2.1 § 9.4.2):
        // `<p><span></span></p>` makes no line at all.
        bool framed = false;
        for (FSpan *sp = line->spans; sp != NULL && !framed; sp = sp->next)
            for (int k = 0; k < 4 && !framed; k++) {
                const flow_style_t *ss = sp->inl->style;
                framed = len(ss->margin[k], cw) != 0 || len(ss->padding[k], cw) != 0 ||
                         (ss->border_width[k] != 0 && ss->border_style[k] != FLOW_BORDER_NONE &&
                          ss->border_style[k] != FLOW_BORDER_HIDDEN);
            }
        bool empty = !has_text && !content && !has_break && !framed;
        // An outside marker stands on its item's first line with content,
        // so that line is as tall as the marker's font as well — in quirks
        // mode a line of nothing but a small picture has no strut of its
        // own to hold it.
        if (l->nmarkers > 0 && !empty) {
            for (int32_t k = 0; k < l->nmarkers; k++) {
                Fonts mf;
                if (fonts_for(l, l->markers[k]->style, &mf)) {
                    Extent mb = font_box(l->markers[k]->style, mf.info.ascent, mf.info.descent, mf.info.line_height);
                    extend(&ext, &any, mb.top, mb.bottom);
                }
            }
            l->nmarkers = 0;
        }
        int64_t height = any ? ext.bottom - ext.top : 0;
        int64_t baseline_off = any ? -ext.top : 0;
        // A box aligned to the top or the bottom asks only that the line be
        // as tall as it, so the line is the tallest of the three, whatever
        // their order (§10.8: as short as it can be). Where the baseline
        // then sits is the one freedom left: what a bottom-aligned box adds
        // goes above it, what a top-aligned box adds below — each alone is
        // what every engine does, and together the bottom's share is taken
        // first.
        int64_t tallest_top = 0, tallest_bottom = 0;
        for (FFrag *fr = line->frags; fr != NULL; fr = fr->next) {
            if (fr->kind != FF_ATOMIC)
                continue;
            flow_vertical_align_t va = fr->style->vertical_align;
            if (va != FLOW_VALIGN_TOP && va != FLOW_VALIGN_BOTTOM)
                continue;
            int64_t hm = len(fr->style->margin[FLOW_TOP], cw) + fr->h +
                         len(fr->style->margin[FLOW_BOTTOM], cw);
            if (va == FLOW_VALIGN_TOP)
                tallest_top = max64(tallest_top, hm);
            else
                tallest_bottom = max64(tallest_bottom, hm);
        }
        if (tallest_bottom > height)
            baseline_off += tallest_bottom - height;
        height = max64(height, max64(tallest_top, tallest_bottom));
        if (empty)
            height = 0;

        // Resolve the margins waiting above the first line with height.
        if (height > 0) {
            cur->y += margins_sum(cur->pm);
            cur->pm = kNoMargins;
            if (cur->first < 0)
                cur->first = cur->y;
        }
        // An empty line resolves no margins, but it sits where a line would.
        line->y = height > 0 ? cur->y : cur->y + margins_sum(cur->pm);
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
        // Whether anything in the flow stands before a fragment on this line:
        // text, an atom, a marker — not a placeholder, not an inline's edge.
        bool flow_before = false;
        for (FFrag *fr = line->frags; fr != NULL; fr = fr->next) {
            if (fr->kind == FF_PLACEHOLDER && !fr->style->specified_inline) {
                // A box that was a block would have broken the line where
                // something in the flow preceded it, so its static position
                // starts the next line; with nothing before it, it starts
                // this one. Either way at the content edge (Chrome: ruling
                // 5, and the probe's case 3).
                fr->x = cx + fr->rel_x;
                fr->y = line->y + (flow_before ? height : 0) + fr->rel_y;
                fr->baseline = line->baseline;
                continue;
            }
            if (fr->kind != FF_PLACEHOLDER)
                flow_before = true;
            fr->x += cx + shift + per_gap * gaps_before(gap_at, gaps, fr->x) + fr->rel_x;
            if (fr->kind == FF_ATOMIC) {
                flow_vertical_align_t va = fr->style->vertical_align;
                int64_t mt = len(fr->style->margin[FLOW_TOP], cw);
                if (va == FLOW_VALIGN_TOP)
                    fr->y = line->y + mt;
                else if (va == FLOW_VALIGN_BOTTOM)
                    fr->y = line->y + height - fr->h - len(fr->style->margin[FLOW_BOTTOM], cw);
                else
                    fr->y += line->baseline;
                fr->y += fr->rel_y;
                if (fr->item->content != NULL)
                    translate(fr->item->content, fr->x - fr->item->content->x,
                              fr->y - fr->item->content->y);
            } else if (fr->kind == FF_PLACEHOLDER) {
                // One that was inline stands where the pen was, at the
                // line's top.
                fr->y = line->y + fr->rel_y;
            } else {
                fr->y += line->baseline + fr->rel_y;
            }
            fr->baseline = line->baseline;
        }
        for (FSpan *sp = line->spans; sp != NULL; sp = sp->next) {
            sp->x0 += cx + shift + per_gap * gaps_before(gap_at, gaps, sp->x0) + sp->rel_x;
            sp->x1 += cx + shift + per_gap * gaps_before(gap_at, gaps, sp->x1) + sp->rel_x;
            Fonts f;
            if (fonts_for(l, sp->inl->style, &f)) {
                sp->top = line->baseline - f.info.ascent + sp->rel_y;
                sp->bottom = line->baseline + f.info.descent + sp->rel_y;
            }
        }
        if (l->failed)
            break;
        if (ifc->last_line != NULL)
            ifc->last_line->next = line;
        else
            ifc->lines = line;
        ifc->last_line = line;
        cur->y += height;
        if (height > 0 && cur->first < 0)
            cur->first = line->y;
        start = end;
    }
    if (cut && ifc->last_line != NULL) {
        FLine *last = ifc->last_line;
        // An out-of-flow box whose place is on the line dropped has none.
        for (FFrag *fr = last->frags; fr != NULL; fr = fr->next)
            if (fr->kind == FF_PLACEHOLDER)
                fr->item->absolute->place = NULL;
        last->frags = last->last_frag = NULL;
        last->spans = last->last_span = NULL;
        last->unfinished = true;
    }
    os64_free(s.v);
    os64_free(open.v);
    os64_free(open_x);
    os64_free(gap_at);
}

// ── Block formatting ────────────────────────────────────────────────────

static void translate(FBox *b, int64_t dx, int64_t dy)
{
    if (dx == 0 && dy == 0)
        return;
    b->x += dx;
    b->y += dy;
    if (b->has_baseline)
        b->baseline += dy;
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
// table and its cells and captions, a replaced block, an atom's content,
// an out-of-flow box, a block whose overflow scrolls or hides (CSS
// Overflow 3 § 3; `clip` does not), and a flex or grid container and each
// of its items (Flexbox 1 § 3, § 4; Grid 2 § 5, § 6) — each starts a
// formatting context of its own (§9.4.1, §8.3.1).
static bool bfc_root(const FBox *b)
{
    const flow_style_t *s = b->style;
    bool scroller = b->node != NULL &&
                    (f_overflow_scrolls(s->overflow_x) || f_overflow_scrolls(s->overflow_y));
    return b->parent == NULL || b->kind == FB_TABLE || b->kind == FB_CELL ||
           b->kind == FB_CAPTION || b->kind == FB_REPLACED || scroller || b->out_of_flow ||
           (b->kind == FB_BLOCK && b->node != NULL && f_display_atomic(s->display)) || b->flex ||
           b->grid || (b->parent != NULL && (b->parent->flex || b->parent->grid));
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
// margins sharing what a set width leaves over — and zero when it leaves
// nothing, the box overflowing to the right rather than off the left.
// §10.4: a width that breaks max-width or min-width is worked again with
// the limit as the width the page set — which is how `max-width: 40em;
// margin: 0 auto` centres.
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
    if (w_auto)
        w = max64(0, cbw - ml - mr - frame);
    else
        w = forced_w >= 0 ? forced_w : content_of(s, s->width, cbw, frame);
    if (forced_w < 0) {
        int64_t held = clamp_width(s, w, cbw, frame);
        if (held != w) {
            w = held;
            w_auto = false;
        }
    }
    if (!w_auto) {
        int64_t rem = cbw - (ml + mr + frame + w);
        if (ml_auto && mr_auto)
            ml = rem > 0 ? rem / 2 : 0;
        else if (ml_auto)
            ml = rem > 0 ? rem : 0;
        else if (!mr_auto)
            ml += html_align_shift(b, rem);
    }
    *ml_out = ml;
    return w;
}

// A table's first header group, which is shown first, and its first footer
// group, which is shown last, whatever their places in the source (CSS 2.1
// § 17.2); every other group and row keeps its order between them.
static void table_ends(const FBox *t, FBox **head, FBox **foot)
{
    *head = *foot = NULL;
    for (FBox *c = t->first; c != NULL; c = c->next) {
        if (c->kind != FB_ROW_GROUP)
            continue;
        if (*head == NULL && c->style->display == FLOW_DISPLAY_TABLE_HEADER_GROUP)
            *head = c;
        if (*foot == NULL && c->style->display == FLOW_DISPLAY_TABLE_FOOTER_GROUP)
            *foot = c;
    }
}

// The first baseline inside a box, in the order it is shown: a line's, or
// a table row's as its layout decided it — a nested table's baseline is its
// first row's, the one that row's cells share, not whatever line its first
// cell holds. A table's rows are walked header group first and footer
// group last, and its captions not at all.
static flow_place_t cross_align(const flow_style_t *container, const flow_style_t *item);

// A flex container's first baseline (Flexbox 1 § 8.5): in a row, that of
// an item on its first line lined up by baseline; else its startmost
// item's, on its first line — first in `order`, not in the tree, and in a
// reverse direction, which puts the last at the start, last. An item with
// no line of text gives one from its border box's bottom edge, as it does
// when it is lined up by baseline.
static bool flex_baseline(const FBox *b, int64_t *out)
{
    const flow_style_t *s = b->style;
    bool row = s->flex_direction == FLOW_FLEX_ROW || s->flex_direction == FLOW_FLEX_ROW_REVERSE;
    bool reverse = s->flex_direction == FLOW_FLEX_ROW_REVERSE ||
                   s->flex_direction == FLOW_FLEX_COLUMN_REVERSE;
    const FBox *pick = NULL;
    for (const FBox *c = b->first; c != NULL; c = c->next) {
        if (c->out_of_flow)
            continue;
        const flow_style_t *cs = c->style;
        if (c->flex_line != 0)
            continue;               // a wrapped container's baseline is its first line's
        if (row && cross_align(s, cs) == FLOW_PLACE_BASELINE &&
            cs->margin[FLOW_TOP].kind != FLOW_LENGTH_AUTO &&
            cs->margin[FLOW_BOTTOM].kind != FLOW_LENGTH_AUTO) {
            pick = c;
            break;
        }
        if (pick == NULL || (reverse ? cs->order >= pick->style->order
                                     : cs->order < pick->style->order))
            pick = c;
    }
    if (pick == NULL || !pick->placed)
        return false;
    if (!first_baseline_in(pick, out))
        *out = pick->y + pick->h;
    return true;
}

// A grid container's first baseline (Grid 2 § 10.8): its first item's in
// grid order — the lowest row, then the leftmost column, then `order`,
// then the tree's — made from its border box's bottom edge when it holds
// no text. Items lined up by baseline are read as `start` (GRID.md §
// Booked), so none takes part.
static bool grid_baseline(const FBox *b, int64_t *out)
{
    const FBox *pick = NULL;
    for (const FBox *c = b->first; c != NULL; c = c->next)
        if (!c->out_of_flow && c->placed &&
            (pick == NULL || c->grid_row < pick->grid_row ||
             (c->grid_row == pick->grid_row &&
              (c->grid_col < pick->grid_col ||
               (c->grid_col == pick->grid_col && c->style->order < pick->style->order)))))
            pick = c;
    if (pick == NULL)
        return false;
    if (!first_baseline_in(pick, out))
        *out = pick->y + pick->h;
    return true;
}

static bool first_baseline_in(const FBox *b, int64_t *out)
{
    if (!b->placed)
        return false;
    if (b->flex)
        return flex_baseline(b, out);
    if (b->grid)
        return grid_baseline(b, out);
    for (const FLine *ln = b->lines; ln != NULL; ln = ln->next)
        if (ln->h > 0) {
            *out = ln->baseline;
            return true;
        }
    if (b->kind == FB_ROW) {
        if (b->has_baseline)
            *out = b->baseline;
        return b->has_baseline;
    }
    if (b->kind == FB_TABLE) {
        FBox *head, *foot;
        table_ends(b, &head, &foot);
        if (head != NULL && first_baseline_in(head, out))
            return true;
        for (const FBox *c = b->first; c != NULL; c = c->next)
            if ((c->kind == FB_ROW_GROUP || c->kind == FB_ROW) && c != head && c != foot &&
                first_baseline_in(c, out))
                return true;
        return foot != NULL && first_baseline_in(foot, out);
    }
    for (const FBox *c = b->first; c != NULL; c = c->next)
        if (!c->out_of_flow && first_baseline_in(c, out))
            return true;
    return false;
}

// An outside marker: right-aligned against the item's content edge, on
// its first line's baseline — or where one would be — in the item's font.
static void place_marker(L *l, const FStyles *styles, FBox *b, int64_t content_x,
                         int64_t content_y)
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
    int64_t first = 0;
    bool has_first = first_baseline_in(b, &first);
    fr->kind = FF_MARKER;
    fr->node = b->node;
    fr->style = b->style;
    fr->run = run;
    fr->text = b->marker;
    fr->end = b->marker_len;
    // The marker belongs to its item, so it is in whatever link the item
    // sits in. It takes none of the item's decorations: an outside marker
    // is not in the line they are drawn across.
    fr->link = paint_for(l, styles, b->node, NULL).link;
    fr->w = v.advance_x;
    fr->h = v.ascent + v.descent;
    fr->x = content_x - fr->w;
    Extent fb = font_box(b->style, v.ascent, v.descent, v.line_height);
    fr->baseline = has_first ? first : content_y - fb.top;
    fr->y = fr->baseline - v.ascent;
    b->marker_frag = fr;
}

// A block container's block-level children, in order. An out-of-flow one
// takes no part: it records its static position — where the flow has
// reached, below the previous sibling's own margin and not the margin that
// sibling will collapse to with the next, which has not been met yet
// (POSITION.md, ruling 5) — and is laid out once its containing block is
// finished.
static void children(L *l, const FStyles *styles, FBox *b, int64_t cx, int64_t cw, Cursor *in)
{
    for (FBox *c = b->first; c != NULL && !l->failed; c = c->next) {
        if (c->out_of_flow) {
            c->x = cx;
            c->y = in->y + margins_sum(in->pm);
            c->static_known = true;
            continue;
        }
        block(l, styles, c, cx, cw, in);
    }
}

// ── Flexible boxes (FLEX.md) ────────────────────────────────────────────
//
// Items along a main axis, on one line or, wrapping, on several. Every size
// the algorithm needs before an item is laid out is a width, which
// intrinsic() knows without laying anything out, so a ROW is resolved
// first and each item laid out once, at the width it was given, and moved
// to its line and its place on the cross axis after. A COLUMN's items are
// laid out first, at their cross size, and growing or shrinking sets a
// height, which moves nothing inside. Both have one exception: an item
// that is a flex container itself places its items again against the
// height it is given (refit) — moved and sized, never laid out again.

typedef struct {
    FBox *box;
    // Main-axis sizes, border box: the flex base size, the hypothetical
    // size, the limits, and what resolving gave it.
    int64_t base, hyp, min, max, target;
    int64_t frame;          // main-axis border and padding
    int64_t m0, m1;         // main-axis margins, left/top then right/bottom
    bool auto0, auto1;      // which of those are auto
    int32_t grow, shrink;   // thousandths
    bool frozen;
} FlexItem;

// `v` shared among `n` weights by cumulative rounding, so the parts add up
// to it exactly.
static void share(int64_t v, const int64_t *weights, int64_t *parts, int32_t n)
{
    double total = 0, sofar = 0;
    for (int32_t i = 0; i < n; i++)
        total += (double)weights[i];
    int64_t given = 0;
    for (int32_t i = 0; i < n; i++) {
        sofar += (double)weights[i];
        int64_t upto = total > 0 ? (int64_t)((double)v * sofar / total + (v < 0 ? -0.5 : 0.5)) : 0;
        parts[i] = upto - given;
        given = upto;
    }
}

// The same, by real weights: a shrink weight is a factor in thousandths
// times a base in 26.6, and a small factor on a base under a pixel, divided
// down to whole units, is nothing.
static void share_by(int64_t v, const double *weights, int64_t *parts, int32_t n)
{
    double total = 0, sofar = 0;
    for (int32_t i = 0; i < n; i++)
        total += weights[i];
    int64_t given = 0;
    for (int32_t i = 0; i < n; i++) {
        sofar += weights[i];
        int64_t upto = total > 0 ? (int64_t)((double)v * sofar / total + (v < 0 ? -0.5 : 0.5)) : 0;
        parts[i] = upto - given;
        given = upto;
    }
}

static int64_t outer(const FlexItem *it, int64_t size)
{
    return it->m0 + size + it->m1;
}

// Flexbox 1 § 9.7: the free space on a line shared out by flex-grow, or
// taken back by flex-shrink weighted by each item's inner base size; an
// item a share carries past a limit is frozen there and the rest share
// again. The line is `idx[0..n)`, and `space` its inner main size less its
// gaps. `parts` and `w` are working memory for n shares and n weights.
static void resolve_flex(FlexItem *items, const int32_t *idx, int32_t n, int64_t space,
                         int64_t *parts, double *w)
{
    int64_t hyp_sum = 0;
    for (int32_t i = 0; i < n; i++)
        hyp_sum += outer(&items[idx[i]], items[idx[i]].hyp);
    bool growing = hyp_sum < space;
    for (int32_t i = 0; i < n; i++) {
        FlexItem *it = &items[idx[i]];
        int32_t f = growing ? it->grow : it->shrink;
        it->frozen = f == 0 || (growing && it->base > it->hyp) || (!growing && it->base < it->hyp);
        it->target = it->frozen ? it->hyp : it->base;
    }
    int64_t initial = space;
    for (int32_t i = 0; i < n; i++)
        initial -= outer(&items[idx[i]], items[idx[i]].frozen ? items[idx[i]].target
                                                              : items[idx[i]].base);
    for (int32_t round = 0; round <= n; round++) {
        int64_t rem = space, factors = 0;
        bool any = false;
        for (int32_t i = 0; i < n; i++) {
            const FlexItem *it = &items[idx[i]];
            rem -= outer(it, it->frozen ? it->target : it->base);
            if (!it->frozen) {
                any = true;
                factors += growing ? it->grow : it->shrink;
            }
        }
        if (!any)
            break;
        // Factors that sum below one share only that fraction.
        if (factors < 1000) {
            int64_t part = initial * factors / 1000;
            if ((part < 0 ? -part : part) < (rem < 0 ? -rem : rem))
                rem = part;
        }
        for (int32_t i = 0; i < n; i++) {
            const FlexItem *it = &items[idx[i]];
            w[i] = it->frozen ? 0
                 : growing ? (double)it->grow
                           : (double)it->shrink * (double)max64(0, it->base - it->frame);
        }
        share_by(rem, w, parts, n);
        int64_t violation = 0;
        for (int32_t i = 0; i < n; i++) {
            FlexItem *it = &items[idx[i]];
            if (it->frozen)
                continue;
            int64_t want = it->base + parts[i];
            int64_t held = want < it->min ? it->min : want > it->max ? it->max : want;
            parts[i] = held - want;         // this item's violation
            it->target = held;
            violation += held - want;
        }
        for (int32_t i = 0; i < n; i++) {
            FlexItem *it = &items[idx[i]];
            if (it->frozen)
                continue;
            if (violation == 0 || (violation > 0 && parts[i] > 0) ||
                (violation < 0 && parts[i] < 0))
                it->frozen = true;
        }
    }
}

// One line of items: `count` of them from `first` in `order` order, how
// tall (a row) or wide (a column) it is, how far above its baselines its
// items reach, and where it starts on the cross axis.
typedef struct {
    int32_t first, count;
    int64_t cross, asc, pos;
} FlexLine;

// The lines (§ 9.3): items in `order` order, a new line wherever the next
// one's outer hypothetical size would pass `room`, each line at least one
// item; one line when the container does not wrap. The count.
static int32_t form_lines(const FlexItem *items, const int32_t *ord, int32_t n, int64_t room,
                          int64_t gap, bool wraps, FlexLine *lines)
{
    int32_t nl = 0;
    int64_t used = 0;
    for (int32_t i = 0; i < n; i++) {
        int64_t o = outer(&items[ord[i]], items[ord[i]].hyp);
        if (nl == 0 || (wraps && lines[nl - 1].count > 0 && used + gap + o > room)) {
            lines[nl].first = i;
            lines[nl].count = 0;
            nl++;
            used = o;
        } else {
            used += gap + o;
        }
        lines[nl - 1].count++;
    }
    return nl;
}

// The main-axis auto margins of a line's items take its positive free
// space, equally; false on no memory. `free` is left what they did not take.
static bool auto_margins(L *l, FlexItem *items, const int32_t *idx, int32_t n, int64_t *free,
                         int64_t *w)
{
    int32_t autos = 0;
    for (int32_t i = 0; i < n; i++)
        autos += items[idx[i]].auto0 + items[idx[i]].auto1;
    if (*free <= 0 || autos == 0)
        return true;
    int64_t *parts = scratch_calloc(l, (size_t)n * 2, sizeof(*parts));
    if (parts == NULL)
        return false;
    for (int32_t i = 0; i < 2 * n; i++)
        w[i] = (i % 2 == 0 ? items[idx[i / 2]].auto0 : items[idx[i / 2]].auto1) ? 1 : 0;
    share(*free, w, parts, 2 * n);
    for (int32_t i = 0; i < n; i++) {
        items[idx[i]].m0 += parts[2 * i];
        items[idx[i]].m1 += parts[2 * i + 1];
    }
    scratch_free(l, parts);
    *free = 0;
    return true;
}

// The items in `order` order — each item's `key` — stably: tree order
// among equals.
static void order_items(const int64_t *key, int32_t *ord, int32_t *tmp, int32_t n)
{
    if (n < 2)
        return;
    int32_t h = n / 2;
    order_items(key, ord, tmp, h);
    order_items(key, ord + h, tmp, n - h);
    int32_t i = 0, j = h, k = 0;
    while (i < h && j < n)
        tmp[k++] = key[ord[j]] < key[ord[i]] ? ord[j++] : ord[i++];
    while (i < h)
        tmp[k++] = ord[i++];
    while (j < n)
        tmp[k++] = ord[j++];
    os64_memcpy(ord, tmp, (size_t)n * sizeof(*ord));
}

// Where `justify-content` puts the k-th of n items, past where the ones
// before it and the gaps end, given the line's free space: the reverse
// directions read `start` and `end` against the writing mode, which does
// not reverse, and `left` and `right` against the page in a row, and as
// `start` in a column.
static int64_t justify_extra(flow_place_t j, bool reverse, bool column, int64_t free,
                             int32_t k, int32_t n)
{
    switch (j) {
    case FLOW_PLACE_START: j = reverse ? FLOW_PLACE_FLEX_END : FLOW_PLACE_FLEX_START; break;
    case FLOW_PLACE_END: j = reverse ? FLOW_PLACE_FLEX_START : FLOW_PLACE_FLEX_END; break;
    // Physical sides: on a column, whose axis is not the inline one, both
    // are `start` (Box Alignment 3 § 5.1), which column-reverse puts at
    // flex-end.
    case FLOW_PLACE_LEFT:
        j = !reverse ? FLOW_PLACE_FLEX_START : FLOW_PLACE_FLEX_END;
        break;
    case FLOW_PLACE_RIGHT:
        j = column ? (reverse ? FLOW_PLACE_FLEX_END : FLOW_PLACE_FLEX_START)
                   : (reverse ? FLOW_PLACE_FLEX_START : FLOW_PLACE_FLEX_END);
        break;
    default: break;
    }
    // Negative free space is never spread (FLEX.md, decision 4).
    switch (j) {
    case FLOW_PLACE_FLEX_END: return free;
    case FLOW_PLACE_CENTER: return free / 2;
    case FLOW_PLACE_SPACE_BETWEEN: return free > 0 && n > 1 ? free * k / (n - 1) : 0;
    case FLOW_PLACE_SPACE_AROUND: return free > 0 ? free * (2 * k + 1) / (2 * n) : 0;
    case FLOW_PLACE_SPACE_EVENLY: return free > 0 ? free * (k + 1) / (n + 1) : 0;
    default: return 0;
    }
}

// An item's cross alignment: its own, or its container's; `normal` is
// `stretch`. The answer is PHYSICAL: flex-start is the top (a row's line)
// or the left (a column's). `start` and `end` are the writing mode's,
// which wrap-reverse leaves alone; `flex-start` and `flex-end` are the
// cross axis's, which wrap-reverse turns round.
static flow_place_t cross_align(const flow_style_t *container, const flow_style_t *item)
{
    flow_place_t a = item->align_self != FLOW_PLACE_AUTO ? item->align_self
                                                         : container->align_items;
    bool back = container->flex_wrap == FLOW_FLEX_WRAP_REVERSE;
    switch (a) {
    case FLOW_PLACE_NORMAL: case FLOW_PLACE_AUTO: return FLOW_PLACE_STRETCH;
    case FLOW_PLACE_START: case FLOW_PLACE_SELF_START: return FLOW_PLACE_FLEX_START;
    case FLOW_PLACE_END: case FLOW_PLACE_SELF_END: return FLOW_PLACE_FLEX_END;
    case FLOW_PLACE_FLEX_START: return back ? FLOW_PLACE_FLEX_END : FLOW_PLACE_FLEX_START;
    case FLOW_PLACE_FLEX_END: return back ? FLOW_PLACE_FLEX_START : FLOW_PLACE_FLEX_END;
    default: return a;
    }
}

static const int64_t kNoLimit = INT64_MAX / 4;

// Where the lines go on the cross axis (§ 9.4, § 8.4): `total` is the
// container's inner cross size. `align-content: normal` is `stretch`,
// which shares what the lines leave among them; the rest place them as
// justify-content places items. The places are from the cross axis's
// start, which the caller mirrors when `back` (wrap-reverse). A
// single-line container's one line is as big as the container. False on
// no memory.
static bool place_lines(L *l, FlexLine *lines, int32_t nl, int64_t total, int64_t gap,
                        flow_place_t how, bool wraps, bool back)
{
    if (!wraps) {
        lines[0].cross = total;
        lines[0].pos = 0;
        return true;
    }
    int64_t free = total - gap * (nl - 1);
    for (int32_t i = 0; i < nl; i++)
        free -= lines[i].cross;
    if (how == FLOW_PLACE_NORMAL || how == FLOW_PLACE_STRETCH) {
        if (free > 0) {
            int64_t *w = scratch_calloc(l, (size_t)nl * 2, sizeof(*w));
            if (w == NULL)
                return false;
            for (int32_t i = 0; i < nl; i++)
                w[i] = 1;
            share(free, w, w + nl, nl);
            for (int32_t i = 0; i < nl; i++)
                lines[i].cross += w[nl + i];
            scratch_free(l, w);
            free = 0;
        }
        how = FLOW_PLACE_FLEX_START;
    } else if (how == FLOW_PLACE_BASELINE) {
        how = FLOW_PLACE_FLEX_START;
    } else if (back && (how == FLOW_PLACE_START || how == FLOW_PLACE_END)) {
        // `start` and `end` are the writing mode's: the caller mirrors
        // these places for wrap-reverse, so they are mirrored here first.
        how = how == FLOW_PLACE_START ? FLOW_PLACE_FLEX_END : FLOW_PLACE_FLEX_START;
    }
    int64_t at = 0;
    for (int32_t i = 0; i < nl; i++) {
        lines[i].pos = at + justify_extra(how, false, true, free, i, nl);
        at += lines[i].cross + gap;
    }
    return true;
}

static bool sized_later(const FBox *b);

// The relative offset `block` laid an item out at: moved to its place
// after, it keeps the offset on top of the place (CSS 2.1 § 9.4.3).
static void laid_offset(const FBox *c, int64_t cbw, int64_t *dx, int64_t *dy)
{
    *dx = *dy = 0;
    if (c->positioned && !c->out_of_flow)
        rel_offset(c->style, cbw, definite_height(c->parent), dx, dy);
}

// An item's height before its container stretched or flexed it: a height
// the page gave, or its content's, held to its limits; border box. What a
// container sizes it from, however often it places it.
static int64_t natural_h(const FBox *c)
{
    const flow_style_t *cs = c->style;
    int64_t vf = c->border[FLOW_TOP] + c->padding[FLOW_TOP] + c->padding[FLOW_BOTTOM] +
                 c->border[FLOW_BOTTOM];
    if (c->kind == FB_TABLE)
        return c->h;                // a table's height is its rows'
    int64_t h = cs->height.kind == FLOW_LENGTH_PX ? content_of(cs, cs->height, 0, vf)
                                                  : c->content_h - vf;
    return clamp_height(cs, max64(0, h), vf) + vf;
}

// An item's width as it was laid out, border box, before a wrapping
// column's line stretched it.
static int64_t natural_w(const FBox *c)
{
    if (!c->flex_sized)
        return c->w;
    return c->flex_w + c->border[FLOW_LEFT] + c->padding[FLOW_LEFT] + c->padding[FLOW_RIGHT] +
           c->border[FLOW_RIGHT];
}

static int64_t content_width(const FBox *c)
{
    return c->w - c->border[FLOW_LEFT] - c->padding[FLOW_LEFT] - c->padding[FLOW_RIGHT] -
           c->border[FLOW_RIGHT];
}

// The end of a flex or grid container's items. Placed whole, each item's
// absolute boxes are laid out against its final size (sized_later), and
// first, the items of an item that is itself a flex or grid container,
// whose own placement left them for now because this one could still
// resize it. Stopped, what was
// laid out is withdrawn: an item there has only its provisional place,
// before its container moved and sized it, and a tree that stopped holds
// no item its container had not finished with.
static void items_done(L *l, const FStyles *styles, FBox *b, int64_t cbw)
{
    for (FBox *c = b->first; c != NULL; c = c->next) {
        if (c->out_of_flow)
            continue;
        if (l->failed) {
            unplace(c);
            continue;
        }
        if (c->flex || c->grid)
            items_done(l, styles, c, content_width(c));
        positioned_done(l, styles, c, cbw);
    }
}

// Whether an item is stretched across its line: `stretch`, no auto margin
// across, and no size the page gave across — a row's item its height, a
// column's its width. A table is not: its size is its rows' and columns'.
static bool stretches_across(const flow_style_t *s, const FBox *c)
{
    const flow_style_t *cs = c->style;
    bool row = s->flex_direction == FLOW_FLEX_ROW || s->flex_direction == FLOW_FLEX_ROW_REVERSE;
    int side0 = row ? FLOW_TOP : FLOW_LEFT, side1 = row ? FLOW_BOTTOM : FLOW_RIGHT;
    bool size_given = row ? cs->height.kind == FLOW_LENGTH_PX
                          : cs->width.kind == FLOW_LENGTH_PX || cs->width.kind == FLOW_LENGTH_PERCENT;
    return cross_align(s, cs) == FLOW_PLACE_STRETCH && !size_given && c->kind != FB_TABLE &&
           cs->margin[side0].kind != FLOW_LENGTH_AUTO && cs->margin[side1].kind != FLOW_LENGTH_AUTO;
}

// A column item's left margin across a content box `cw` wide, its border
// box `w` wide: auto margins take the room first, else its alignment
// places it (a stretched one fills it, and is not sized here).
static int64_t column_ml(const flow_style_t *s, const FBox *c, int64_t cw, int64_t w)
{
    const flow_style_t *cs = c->style;
    bool ml_auto = cs->margin[FLOW_LEFT].kind == FLOW_LENGTH_AUTO;
    bool mr_auto = cs->margin[FLOW_RIGHT].kind == FLOW_LENGTH_AUTO;
    int64_t ml = ml_auto ? 0 : len(cs->margin[FLOW_LEFT], cw);
    int64_t mr = mr_auto ? 0 : len(cs->margin[FLOW_RIGHT], cw);
    int64_t room = cw - (ml + w + mr);
    flow_place_t a = cross_align(s, cs);
    if ((ml_auto || mr_auto) && room > 0)
        ml += ml_auto && mr_auto ? room / 2 : ml_auto ? room : 0;
    else if (!ml_auto && !mr_auto && a == FLOW_PLACE_FLEX_END)
        ml += room;
    else if (!ml_auto && !mr_auto && a == FLOW_PLACE_CENTER)
        ml += room / 2;
    return ml;
}

// One flex container's items and what placing them needs.
typedef struct {
    FBox *b;
    bool column, reverse, wraps, wrap_back;
    int64_t cx, cw, top;
    int32_t n;
    FlexItem *items;
    int32_t *ord;
    int64_t *w;
    double *weights;
    FlexLine *lines;
} FlexRun;

static void refit(L *l, const FStyles *styles, FBox *c);

// An item's height set by its container: an item that is itself a flex
// container places its own items again, against it (refit).
static void set_item_h(L *l, const FStyles *styles, FBox *c, int64_t h)
{
    if (c->h == h)
        return;
    c->h = h;
    refit(l, styles, c);
}

// A row's lines, as laying its items out formed them (FBox.flex_line):
// in `order` order, each line's items together. The count.
static int32_t row_lines(const FlexRun *r)
{
    int32_t nl = 0;
    for (int32_t i = 0; i < r->n; i++) {
        int32_t li = r->items[r->ord[i]].box->flex_line;
        if (nl == 0 || li != r->items[r->ord[r->lines[nl - 1].first]].box->flex_line) {
            r->lines[nl].first = i;
            r->lines[nl].count = 0;
            nl++;
        }
        r->lines[nl - 1].count++;
    }
    return nl;
}

// Everything after the items are laid out, against the container's content
// height `given` (-1: not known, the items' own): a row's line sizes and
// cross alignment; a column's lines, flexing, main placement and, wrapped,
// cross placement. It can run again (refit): it reads only what laying the
// items out left in their boxes — their natural sizes, not what an earlier
// placement gave them — and moves each item to an absolute place. `*used`
// is the content height it came to, `*natural` the one its items alone
// ask. False on no memory.
static bool flex_place(L *l, const FStyles *styles, FlexRun *r, int64_t given, int64_t *used,
                       int64_t *natural)
{
    FBox *b = r->b;
    const flow_style_t *s = b->style;
    FlexItem *items = r->items;
    FlexLine *lines = r->lines;
    int32_t n = r->n, *ord = r->ord;
    int64_t cx = r->cx, cw = r->cw, top = r->top;
    bool column = r->column, wraps = r->wraps, wrap_back = r->wrap_back;
    int64_t vframe_b = b->border[FLOW_TOP] + b->padding[FLOW_TOP] + b->padding[FLOW_BOTTOM] +
                       b->border[FLOW_BOTTOM];
    // The gaps: along the main axis between items, across it between
    // lines. A percentage is of the container's size on that axis, which
    // counts as nothing where it is not known yet.
    int64_t main_def = column ? given : cw;
    int64_t cross_def = column ? cw : given;
    flow_length_t gm = column ? s->row_gap : s->column_gap;
    flow_length_t gc = column ? s->column_gap : s->row_gap;
    int64_t gap = gm.kind == FLOW_LENGTH_PERCENT && main_def < 0 ? 0 : len(gm, max64(0, main_def));
    int64_t cgap = gc.kind == FLOW_LENGTH_PERCENT && cross_def < 0 ? 0 : len(gc, max64(0, cross_def));

    if (!column) {
        // Each line's cross size: its tallest item, the baseline-aligned
        // ones by what they reach above and below their shared baseline.
        int32_t nl = row_lines(r);
        int64_t sum = cgap * (nl - 1);
        for (int32_t li = 0; li < nl; li++) {
            const int32_t *idx = ord + lines[li].first;
            int64_t line = 0, desc = 0;
            lines[li].asc = 0;
            for (int32_t i = 0; i < lines[li].count; i++) {
                FBox *c = items[idx[i]].box;
                const flow_style_t *cs = c->style;
                int64_t mt = len(cs->margin[FLOW_TOP], cw), mb = len(cs->margin[FLOW_BOTTOM], cw);
                int64_t h = natural_h(c), o = mt + h + mb;
                // An item with an auto margin across takes no part in the
                // baseline (§ 8.3): it is only as tall as it is.
                if (cross_align(s, cs) == FLOW_PLACE_BASELINE &&
                    cs->margin[FLOW_TOP].kind != FLOW_LENGTH_AUTO &&
                    cs->margin[FLOW_BOTTOM].kind != FLOW_LENGTH_AUTO) {
                    int64_t bl, asc = mt + h;
                    if (first_baseline_in(c, &bl))
                        asc = mt + (bl - c->y);
                    lines[li].asc = max64(lines[li].asc, asc);
                    desc = max64(desc, o - asc);
                } else {
                    line = max64(line, o);
                }
            }
            lines[li].cross = max64(line, lines[li].asc + desc);
            sum += lines[li].cross;
        }
        *natural = sum;
        int64_t total = given >= 0 ? given : clamp_height(s, sum, vframe_b);
        if (!place_lines(l, lines, nl, total, cgap, s->align_content, wraps, wrap_back))
            return false;
        for (int32_t li = 0; li < nl; li++) {
            const int32_t *idx = ord + lines[li].first;
            int64_t line = lines[li].cross;
            int64_t line_top = wrap_back ? top + total - lines[li].pos - line : top + lines[li].pos;
            for (int32_t i = 0; i < lines[li].count; i++) {
                FBox *c = items[idx[i]].box;
                const flow_style_t *cs = c->style;
                bool at_auto = cs->margin[FLOW_TOP].kind == FLOW_LENGTH_AUTO;
                bool ab_auto = cs->margin[FLOW_BOTTOM].kind == FLOW_LENGTH_AUTO;
                int64_t mt = len(cs->margin[FLOW_TOP], cw), mb = len(cs->margin[FLOW_BOTTOM], cw);
                int64_t vf = c->border[FLOW_TOP] + c->padding[FLOW_TOP] + c->padding[FLOW_BOTTOM] +
                             c->border[FLOW_BOTTOM];
                int64_t h = natural_h(c), room = line - (mt + h + mb), dy = 0;
                flow_place_t a = cross_align(s, cs);   // physical
                if (at_auto || ab_auto) {
                    // Auto margins on the cross axis take its free space.
                    if (room > 0)
                        dy = at_auto && ab_auto ? room / 2 : at_auto ? room : 0;
                } else if (a == FLOW_PLACE_STRETCH) {
                    if (cs->height.kind != FLOW_LENGTH_PX && c->kind != FB_TABLE)
                        h = max64(vf, clamp_height(cs, max64(0, line - mt - mb - vf), vf) + vf);
                    // One that does not fill its line sits at the cross
                    // start, which wrap-reverse puts at the bottom.
                    if (wrap_back)
                        dy = line - (mt + h + mb);
                } else if (a == FLOW_PLACE_FLEX_END) {
                    dy = room;
                } else if (a == FLOW_PLACE_CENTER) {
                    dy = room / 2;
                } else if (a == FLOW_PLACE_BASELINE) {
                    int64_t bl, asc = mt + h;
                    if (first_baseline_in(c, &bl))
                        asc = mt + (bl - c->y);
                    dy = lines[li].asc - asc;
                }
                set_item_h(l, styles, c, h);
                int64_t rdx, rdy;
                laid_offset(c, cw, &rdx, &rdy);
                translate(c, 0, line_top + mt + dy + rdy - c->y);
            }
        }
        *used = total;
        return !l->failed;
    }

    // A column: each item's flex base and limits, from what laying it out
    // found (§ 9.2), then the lines, each resolved against the height.
    for (int32_t i = 0; i < n; i++) {
        FlexItem *it = &items[i];
        FBox *c = it->box;
        const flow_style_t *cs = c->style;
        it->frame = c->border[FLOW_TOP] + c->padding[FLOW_TOP] + c->padding[FLOW_BOTTOM] +
                    c->border[FLOW_BOTTOM];
        it->auto0 = cs->margin[FLOW_TOP].kind == FLOW_LENGTH_AUTO;
        it->auto1 = cs->margin[FLOW_BOTTOM].kind == FLOW_LENGTH_AUTO;
        it->m0 = it->auto0 ? 0 : len(cs->margin[FLOW_TOP], cw);
        it->m1 = it->auto1 ? 0 : len(cs->margin[FLOW_BOTTOM], cw);
        it->grow = cs->flex_grow;
        it->shrink = cs->flex_shrink;
        // `auto` is the item's height, given or its content's; `content`,
        // and a percentage of a height not known, its content's alone.
        // Neither is held to the item's limits: those hold the
        // hypothetical size, and the base weighs the shrinking.
        // A picture's content is its own height, not the one it was laid
        // out at (replaced_own) — or, stretched across a single line, the
        // one its ratio gives that width (§ 9.8: a stretched size is
        // definite; a wrapping column's line width is not known yet).
        int64_t content = c->content_h;
        if (c->kind == FB_REPLACED)
            content = replaced_own(l, c, 1, cw, !wraps && stretches_across(s, c) ? c->flex_w : -1) +
                      it->frame;
        flow_length_t fb = cs->flex_basis;
        if (fb.kind == FLOW_LENGTH_PX || (fb.kind == FLOW_LENGTH_PERCENT && given >= 0))
            it->base = content_of(cs, fb, max64(0, given), it->frame) + it->frame;
        else if (fb.kind == FLOW_LENGTH_AUTO && cs->height.kind == FLOW_LENGTH_PX)
            it->base = content_of(cs, cs->height, 0, it->frame) + it->frame;
        else
            it->base = content;
        it->max = cs->max_height.kind == FLOW_LENGTH_PX
                      ? content_of(cs, cs->max_height, 0, it->frame) + it->frame : kNoLimit;
        if (cs->min_height.kind == FLOW_LENGTH_PX)
            it->min = content_of(cs, cs->min_height, 0, it->frame) + it->frame;
        else if (f_overflow_scrolls(cs->overflow_x) || f_overflow_scrolls(cs->overflow_y))
            it->min = it->frame;
        else
            // The automatic minimum (§ 4.5): its content's height, no more
            // than a height the page gave (natural_h is that, when it gave
            // one), or its limit.
            it->min = min64(min64(content, natural_h(c)), it->max);
        it->min = max64(it->min, it->frame);
        it->max = max64(it->max, it->min);
        it->hyp = it->base < it->min ? it->min : it->base > it->max ? it->max : it->base;
    }
    // A column breaks into lines only where its height is known, or
    // limited: with neither, it is as tall as all of its items.
    // A min-height over the max-height wins, as it does for the height.
    int64_t room = given >= 0 ? given
                 : s->max_height.kind == FLOW_LENGTH_PX
                     ? clamp_height(s, content_of(s, s->max_height, 0, vframe_b), vframe_b)
                     : kNoLimit;
    int32_t nl = form_lines(items, ord, n, room, gap, wraps, lines);
    for (int32_t li = 0; li < nl; li++)
        for (int32_t i = 0; i < lines[li].count; i++)
            items[ord[lines[li].first + i]].box->flex_line = li;
    int64_t size = 0;
    for (int32_t li = 0; li < nl; li++) {
        int64_t sum = gap * (lines[li].count - 1);
        for (int32_t i = 0; i < lines[li].count; i++)
            sum += outer(&items[ord[lines[li].first + i]], items[ord[lines[li].first + i]].hyp);
        size = max64(size, sum);
    }
    *natural = size;
    size = given >= 0 ? given : clamp_height(s, size, vframe_b);
    for (int32_t li = 0; li < nl; li++) {
        const int32_t *idx = ord + lines[li].first;
        int32_t m = lines[li].count;
        int64_t space = size - gap * (m - 1);
        resolve_flex(items, idx, m, space, r->w, r->weights);
        int64_t free = space;
        lines[li].cross = 0;
        for (int32_t i = 0; i < m; i++) {
            FlexItem *it = &items[idx[i]];
            const flow_style_t *cs = it->box->style;
            set_item_h(l, styles, it->box, it->target);
            // A picture the page gave no width takes one from the height
            // it was flexed to, through its ratio, and is placed across
            // again (a wrapping column places it with its line, below) —
            // unless a single line stretches it, which decided its width
            // first.
            FBox *c = it->box;
            if (c->kind == FB_REPLACED && c->flex_sized && cs->width.kind == FLOW_LENGTH_AUTO &&
                (wraps || !stretches_across(s, c))) {
                c->flex_w = replaced_for(l, c, 1, c->h - it->frame, cw);
                c->w = hframe(c, cw) + c->flex_w;
                if (!wraps) {
                    c->flex_ml = column_ml(s, c, cw, c->w);
                    int64_t rdx, rdy;
                    laid_offset(c, cw, &rdx, &rdy);
                    translate(c, cx + c->flex_ml + rdx - c->x, 0);
                }
            }
            free -= outer(it, it->target);
            int64_t mx = len(cs->margin[FLOW_LEFT], cw) + len(cs->margin[FLOW_RIGHT], cw);
            lines[li].cross = max64(lines[li].cross, natural_w(it->box) + mx);
        }
        if (l->failed || !auto_margins(l, items, idx, m, &free, r->w))
            return false;
        int64_t along = 0;
        for (int32_t i = 0; i < m; i++) {
            FlexItem *it = &items[idx[i]];
            int64_t o = outer(it, it->target);
            int64_t pos = along + justify_extra(s->justify_content, r->reverse, true, free, i, m);
            int64_t mb_top = r->reverse ? top + size - pos - o : top + pos;
            int64_t rdx, rdy;
            laid_offset(it->box, cw, &rdx, &rdy);
            translate(it->box, 0, mb_top + it->m0 + rdy - it->box->y);
            along += o + gap;
        }
    }
    if (wraps) {
        // Across: the lines side by side, and each item in its line — a
        // stretched one's box widened to the line, its content as it was
        // laid out (FLEX.md § Booked).
        if (!place_lines(l, lines, nl, cw, cgap, s->align_content, true, wrap_back))
            return false;
        for (int32_t li = 0; li < nl; li++) {
            const int32_t *idx = ord + lines[li].first;
            int64_t line = lines[li].cross;
            int64_t line_left = wrap_back ? cx + cw - lines[li].pos - line : cx + lines[li].pos;
            for (int32_t i = 0; i < lines[li].count; i++) {
                FBox *c = items[idx[i]].box;
                const flow_style_t *cs = c->style;
                bool ml_auto = cs->margin[FLOW_LEFT].kind == FLOW_LENGTH_AUTO;
                bool mr_auto = cs->margin[FLOW_RIGHT].kind == FLOW_LENGTH_AUTO;
                int64_t ml = ml_auto ? 0 : len(cs->margin[FLOW_LEFT], cw);
                int64_t mr = mr_auto ? 0 : len(cs->margin[FLOW_RIGHT], cw);
                int64_t nw = natural_w(c), room = line - (ml + nw + mr), dx = 0;
                c->w = nw;
                flow_place_t a = cross_align(s, cs);   // physical
                if (ml_auto || mr_auto) {
                    if (room > 0)
                        dx = ml_auto && mr_auto ? room / 2 : ml_auto ? room : 0;
                } else if (a == FLOW_PLACE_STRETCH) {
                    // Widened to the line, held to its width limits; one
                    // that does not fill it sits at the cross start, which
                    // wrap-reverse puts at the right.
                    int64_t hf = hframe(c, cw);
                    if (cs->width.kind == FLOW_LENGTH_AUTO && room > 0)
                        c->w = hf + clamp_width(cs, nw - hf + room, cw, hf);
                    if (wrap_back)
                        dx = line - (ml + c->w + mr);
                } else if (a == FLOW_PLACE_FLEX_END) {
                    dx = room;
                } else if (a == FLOW_PLACE_CENTER) {
                    dx = room / 2;
                }
                int64_t rdx, rdy;
                laid_offset(c, cw, &rdx, &rdy);
                translate(c, line_left + ml + dx + rdx - c->x, 0);
            }
        }
    }
    *used = size;
    return true;
}

// A flex container's working memory for its `n` items, in `order` order.
// False on no memory, with whatever was allocated left for flex_run_free.
static bool flex_run_open(L *l, FBox *b, int32_t n, FlexRun *r)
{
    const flow_style_t *s = b->style;
    os64_memset(r, 0, sizeof(*r));
    r->b = b;
    r->n = n;
    r->column = s->flex_direction == FLOW_FLEX_COLUMN ||
                s->flex_direction == FLOW_FLEX_COLUMN_REVERSE;
    r->reverse = s->flex_direction == FLOW_FLEX_ROW_REVERSE ||
                 s->flex_direction == FLOW_FLEX_COLUMN_REVERSE;
    r->wraps = s->flex_wrap != FLOW_FLEX_NOWRAP;
    r->wrap_back = s->flex_wrap == FLOW_FLEX_WRAP_REVERSE;
    r->items = scratch_calloc(l, (size_t)n, sizeof(*r->items));
    r->ord = scratch_calloc(l, (size_t)n * 2, sizeof(*r->ord));
    r->w = scratch_calloc(l, (size_t)n * 2, sizeof(*r->w));
    r->weights = scratch_calloc(l, (size_t)n, sizeof(*r->weights));
    r->lines = scratch_calloc(l, (size_t)n, sizeof(*r->lines));
    if (r->items == NULL || r->ord == NULL || r->w == NULL || r->weights == NULL ||
        r->lines == NULL)
        return false;
    int32_t k = 0;
    bool reordered = false;
    for (FBox *c = b->first; c != NULL && k < n; c = c->next)
        if (!c->out_of_flow) {
            r->items[k].box = c;
            r->ord[k] = k;
            reordered |= c->style->order != 0;
            k++;
        }
    if (reordered) {
        for (int32_t i = 0; i < n; i++)
            r->w[i] = r->items[i].box->style->order;
        order_items(r->w, r->ord, r->ord + n, n);
    }
    return true;
}

static void flex_run_free(L *l, FlexRun *r)
{
    scratch_free(l, r->items);
    scratch_free(l, r->ord);
    scratch_free(l, r->w);
    scratch_free(l, r->weights);
    scratch_free(l, r->lines);
}

static int32_t flex_items(const FBox *b)
{
    int32_t n = 0;
    for (const FBox *c = b->first; c != NULL; c = c->next)
        n += !c->out_of_flow;
    return n;
}

// A flex container's items, from `in->y` down, content box `cx`..`cx + cw`.
// On exit `in->y` is the content's bottom, and `*natural` the height its
// items alone ask, whatever height it was given: its content's, for the
// container it is an item of (FBox.content_h).
static void flex(L *l, const FStyles *styles, FBox *b, int64_t cx, int64_t cw, Cursor *in,
                 int64_t *natural)
{
    const flow_style_t *s = b->style;
    int64_t top = in->y;
    *natural = 0;
    int64_t vframe_b = b->border[FLOW_TOP] + b->padding[FLOW_TOP] + b->padding[FLOW_BOTTOM] +
                       b->border[FLOW_BOTTOM];
    bool h_def = s->height.kind == FLOW_LENGTH_PX || b->abs_h_set;
    int64_t h_given = b->abs_h_set ? b->abs_h : content_of(s, s->height, 0, vframe_b);
    if (h_def)
        h_given = clamp_height(s, h_given, vframe_b);

    for (FBox *c = b->first; c != NULL; c = c->next)
        if (c->out_of_flow) {
            // Its static position: the content box's start corner (FLEX.md
            // § Booked has the rest of Flexbox 1 § 4.1).
            c->x = cx;
            c->y = top;
            c->static_known = true;
        }
    in->first = top;
    int32_t n = flex_items(b);
    if (n == 0)
        return;
    FlexRun r;
    int64_t *at = NULL;
    if (!flex_run_open(l, b, n, &r))
        goto done;
    r.cx = cx;
    r.cw = cw;
    r.top = top;
    FlexItem *items = r.items;
    int32_t *ord = r.ord;
    FlexLine *lines = r.lines;

    if (!r.column) {
        // ── Rows: measure, collect lines, resolve each, then lay each item
        // out once, at its width and its place along its line; placing
        // them moves them across (flex_place).
        at = scratch_calloc(l, (size_t)n, sizeof(*at));
        if (at == NULL)
            goto done;
        flow_length_t gm = s->column_gap;
        int64_t gap = gm.kind == FLOW_LENGTH_PERCENT && cw < 0 ? 0 : len(gm, max64(0, cw));
        for (int32_t i = 0; i < n; i++) {
            FlexItem *it = &items[i];
            FBox *c = it->box;
            const flow_style_t *cs = c->style;
            it->frame = hframe(c, cw);
            it->auto0 = cs->margin[FLOW_LEFT].kind == FLOW_LENGTH_AUTO;
            it->auto1 = cs->margin[FLOW_RIGHT].kind == FLOW_LENGTH_AUTO;
            it->m0 = it->auto0 ? 0 : len(cs->margin[FLOW_LEFT], cw);
            it->m1 = it->auto1 ? 0 : len(cs->margin[FLOW_RIGHT], cw);
            it->grow = cs->flex_grow;
            it->shrink = cs->flex_shrink;
            (void)intrinsic(l, c);          // measures content_min, content_max
            // The content's widths with its frame as it is here: intrinsic()
            // measured a percentage padding against nothing.
            int64_t own_min = c->content_min - hframe(c, 0) + it->frame;
            int64_t own_max = c->content_max - hframe(c, 0) + it->frame;
            // A picture stretched to the one line a container's height
            // fixes has that height, which its ratio carries into its width
            // (§ 9.8: a stretched size is definite).
            if (c->kind == FB_REPLACED && h_def && !r.wraps && stretches_across(s, c)) {
                int64_t vf = vframe(c, cw);
                int64_t ch = clamp_height(cs, max64(0, h_given - len(cs->margin[FLOW_TOP], cw) -
                                                           len(cs->margin[FLOW_BOTTOM], cw) - vf), vf);
                own_min = own_max = replaced_own(l, c, 0, cw, ch) + it->frame;
            }
            bool width_given = cs->width.kind == FLOW_LENGTH_PX || cs->width.kind == FLOW_LENGTH_PERCENT;
            flow_length_t fb = cs->flex_basis;
            if (fb.kind == FLOW_LENGTH_PX || fb.kind == FLOW_LENGTH_PERCENT)
                it->base = content_of(cs, fb, cw, it->frame) + it->frame;
            else if (fb.kind == FLOW_LENGTH_AUTO && width_given)
                it->base = content_of(cs, cs->width, cw, it->frame) + it->frame;
            else
                // `content`, and `auto` with no width: the content's own
                // max-content width, before a limit the page gave, which
                // holds the hypothetical size and not the base (§ 9.2.3).
                it->base = own_max;
            it->max = binds(cs->max_width, cw) ? content_of(cs, cs->max_width, cw, it->frame) + it->frame
                                                : kNoLimit;
            if (binds(cs->min_width, cw)) {
                it->min = content_of(cs, cs->min_width, cw, it->frame) + it->frame;
            } else if (f_overflow_scrolls(cs->overflow_x) || f_overflow_scrolls(cs->overflow_y)) {
                it->min = it->frame;
            } else {
                // The automatic minimum (§ 4.5): the content's min-content
                // width, no more than a width the page gave, or its limit.
                it->min = own_min;
                if (width_given)
                    it->min = min64(it->min, content_of(cs, cs->width, cw, it->frame) + it->frame);
                it->min = min64(it->min, it->max);
            }
            it->min = max64(it->min, it->frame);
            it->max = max64(it->max, it->min);
            it->hyp = it->base < it->min ? it->min : it->base > it->max ? it->max : it->base;
        }
        int32_t nl = form_lines(items, ord, n, cw, gap, r.wraps, lines);
        for (int32_t li = 0; li < nl; li++) {
            const int32_t *idx = ord + lines[li].first;
            int32_t m = lines[li].count;
            int64_t space = cw - gap * (m - 1);
            resolve_flex(items, idx, m, space, r.w, r.weights);
            int64_t free = space;
            for (int32_t i = 0; i < m; i++)
                free -= outer(&items[idx[i]], items[idx[i]].target);
            if (!auto_margins(l, items, idx, m, &free, r.w))
                goto done;
            // Each item's margin box, from the main start, in `order` order.
            int64_t along = 0;
            for (int32_t i = 0; i < m; i++) {
                FlexItem *it = &items[idx[i]];
                int64_t o = outer(it, it->target);
                int64_t pos = along + justify_extra(s->justify_content, r.reverse, false, free, i, m);
                at[idx[i]] = r.reverse ? cx + cw - pos - o : cx + pos;
                it->box->flex_line = li;
                along += o + gap;
            }
        }
        // Laid out in tree order, each once, at its width and its place
        // along the line.
        for (int32_t i = 0; i < n && !l->failed; i++) {
            FlexItem *it = &items[i];
            FBox *c = it->box;
            c->flex_sized = true;
            c->flex_w = max64(0, it->target - it->frame);
            c->flex_ml = it->m0;
            Cursor cur = {top, kNoMargins, -1};
            block(l, styles, c, at[i], cw, &cur);
        }
    } else {
        // ── Columns: lay each item out at its cross size; placing them
        // collects the lines, resolves each and moves the items to their
        // places (flex_place).
        for (int32_t i = 0; i < n && !l->failed; i++) {
            FlexItem *it = &items[i];
            FBox *c = it->box;
            const flow_style_t *cs = c->style;
            flow_place_t a = cross_align(s, cs);
            bool ml_auto = cs->margin[FLOW_LEFT].kind == FLOW_LENGTH_AUTO;
            bool mr_auto = cs->margin[FLOW_RIGHT].kind == FLOW_LENGTH_AUTO;
            bool width_given = cs->width.kind == FLOW_LENGTH_PX || cs->width.kind == FLOW_LENGTH_PERCENT;
            // A wrapping column's lines are as wide as their widest item,
            // which is not known before the items are laid out: each is
            // laid out at its own width, and moved across after. A
            // stretched picture is sized here too: laid out as a block, it
            // would take its own width rather than the line's — which a
            // wrapping column's is not, until its lines are formed.
            bool stretched = !r.wraps && a == FLOW_PLACE_STRETCH && !ml_auto && !mr_auto &&
                             !width_given;
            if (!stretched || c->kind == FB_REPLACED) {
                // Sized as it would be on its own, or stretched, then placed
                // across.
                int64_t frame = hframe(c, cw);
                int64_t across = cw - len(cs->margin[FLOW_LEFT], cw) - len(cs->margin[FLOW_RIGHT], cw);
                int64_t cwid = width_given ? clamp_width(cs, content_of(cs, cs->width, cw, frame), cw, frame)
                             : stretched ? clamp_width(cs, max64(0, across - frame), cw, frame)
                             : fit_content(l, c, cw);
                c->flex_sized = true;
                c->flex_w = cwid;
                // Wrapped, its line places it across (flex_place).
                c->flex_ml = r.wraps ? (ml_auto ? 0 : len(cs->margin[FLOW_LEFT], cw))
                                     : column_ml(s, c, cw, frame + cwid);
            }
            Cursor cur = {top, kNoMargins, -1};
            block(l, styles, c, cx, cw, &cur);
        }
    }
    if (l->failed)
        goto done;
    int64_t used;
    if (flex_place(l, styles, &r, h_def ? h_given : -1, &used, natural))
        in->y = top + used;
done:
    if (l->failed || !sized_later(b))
        items_done(l, styles, b, cw);
    if (l->failed)
        in->y = top;
    scratch_free(l, at);
    flex_run_free(l, &r);
}

// ── Grid layout (GRID.md) ───────────────────────────────────────────────
//
// Items placed into an area of whole tracks. The columns need only widths,
// which intrinsic() knows without laying anything out; the rows need
// heights. So: place, size the columns, lay each item out once at its
// columns' width, size the rows from the heights, and move each item into
// its rows — a stretched one's box heightened, nothing inside it moved but
// the items of one that is a flex or grid container, placed again against
// its new height (refit).
// Axis 0 is the columns, axis 1 the rows; a line is counted from 0.

typedef struct {
    FBox *box;
    int32_t at[2][2];       // [axis][start, end): the tracks it spans
    bool fixed[2];          // its lines on that axis are definite
    int32_t span[2];
    // What it asks of the tracks it spans on the axis being sized, margin
    // box: its min-content and max-content sizes, and its minimum
    // contribution — what an `auto` minimum is held to (§ 6.6).
    int64_t cmin, cmax, cauto;
} GItem;

typedef struct {
    flow_track_t fn;
    int64_t base, limit;    // limit kNoLimit: none yet
    int64_t pos, size;      // after sizing: from the content edge
    bool fit_repeat;        // in an auto-fit repeat: collapsed when empty
    bool used;              // an item spans it
} GTrack;

typedef struct {
    GTrack *t[2];
    int32_t n[2];
    GItem *items;           // tree order
    int32_t *ord;           // `order` order: the placement's
    int32_t ni;
    int64_t gap[2];
} GPlan;

// Whether a sizing function, in room `avail` (-1 when not known), is a
// length after all: a percentage of a size not known is `auto` (§ 7.2.1).
static bool breadth_fixed(flow_breadth_t b, int64_t avail)
{
    return b.kind == FLOW_TRACK_FIXED && (b.len.kind != FLOW_LENGTH_PERCENT || avail >= 0);
}

// The size an auto-repeat counts a track at (§ 7.2.3.2): its maximum when
// that is a length, no less than its minimum; else its minimum when that
// is a length; else nothing.
static int64_t repeat_size(const flow_track_t *t, int64_t avail)
{
    if (!t->fit && breadth_fixed(t->max, avail))
        return max64(len(t->max.len, avail), breadth_fixed(t->min, avail) ? len(t->min.len, avail) : 0);
    return breadth_fixed(t->min, avail) ? len(t->min.len, avail) : 0;
}

// How many times a template's auto-repeat fills `avail` (-1: once), held
// so that the whole list stays within the bound; 1 without one too.
// `one_px` is one CSS pixel in the tree's units, at the zoom.
static int32_t repeats(const flow_tracks_t *t, int64_t avail, int64_t gap, int64_t one_px)
{
    if (t->repeat_n == 0 || avail < 0)
        return 1;
    int64_t others = 0, rep = 0;
    int32_t nothers = t->n - t->repeat_n;
    for (int32_t i = 0; i < t->n; i++) {
        bool in = i >= t->repeat_at && i < t->repeat_at + t->repeat_n;
        // Counted at 1 CSS px at least (§ 7.2.3.2), or a track of nothing
        // would repeat without end; its size is not changed.
        int64_t size = repeat_size(&t->tracks[i], avail);
        *(in ? &rep : &others) += in ? max64(size, one_px) : size;
    }
    int64_t per = rep + gap * t->repeat_n;
    int64_t room = avail - others - gap * (nothers - 1);
    int64_t k = per > 0 ? room / per : 1;
    int64_t most = max64(1, (F_GRID_MAX - nothers) / t->repeat_n);
    return (int32_t)(k < 1 ? 1 : k > most ? most : k);
}

// A template's tracks with its auto-repeat written out `reps` times.
static int32_t template_count(const flow_tracks_t *t, int32_t reps)
{
    int64_t n = t->n + (int64_t)(reps - 1) * t->repeat_n;
    return (int32_t)(n > F_GRID_MAX ? F_GRID_MAX : n);
}

// The i-th track of the template written out (i < template_count), and
// whether it is one of an auto-fit's repeats.
static flow_track_t template_track(const flow_tracks_t *t, int32_t reps, int32_t i, bool *fit_repeat)
{
    *fit_repeat = false;
    if (t->repeat_n == 0 || i < t->repeat_at)
        return t->tracks[i];
    int32_t past = i - t->repeat_at;
    if (past < reps * t->repeat_n) {
        *fit_repeat = t->repeat_fit;
        return t->tracks[t->repeat_at + past % t->repeat_n];
    }
    return t->tracks[i - (reps - 1) * t->repeat_n];
}

// A named line (§ 8.3.1): an area's edge — `name` the side asked, or
// `name-start` and `name-end` either one — or false.
static bool named_line(const flow_style_t *s, const flow_grid_line_t *ln, int axis, bool end,
                       int32_t *out)
{
    for (int pass = 0; pass < 2; pass++) {
        uint32_t n = ln->name_len;
        bool want_end = end;
        if (pass == 1) {
            if (n > 6 && os64_memcmp(ln->name + n - 6, "-start", 6) == 0) {
                n -= 6;
                want_end = false;
            } else if (n > 4 && os64_memcmp(ln->name + n - 4, "-end", 4) == 0) {
                n -= 4;
                want_end = true;
            } else {
                return false;
            }
        }
        for (int32_t i = 0; i < s->ngrid_areas; i++) {
            const flow_grid_area_t *a = &s->grid_areas[i];
            if (a->name_len != n || os64_memcmp(a->name, ln->name, n) != 0)
                continue;
            *out = axis == 0 ? (want_end ? a->col1 : a->col0) : (want_end ? a->row1 : a->row0);
            return true;
        }
    }
    return false;
}

// One line of an item's placement, from 0; -1 for auto, and a span in
// `*span`. A name no area has is a line no explicit line is named after,
// so it is the first implicit line past the explicit grid, every implicit
// line taking the name (§ 8.3.1).
static int32_t line_at(const flow_style_t *s, const flow_grid_line_t *ln, int axis, bool end,
                       int32_t nexp, int32_t *span)
{
    int32_t at;
    switch (ln->kind) {
    case FLOW_GRID_LINE_NUMBER:
        // A negative number counts back from the explicit grid's end; a
        // line before its start is its start (GRID.md § Booked).
        at = ln->n > 0 ? ln->n - 1 : nexp + 1 + ln->n;
        return at < 0 ? 0 : at > F_GRID_MAX ? F_GRID_MAX : at;
    case FLOW_GRID_LINE_SPAN:
        *span = ln->n;
        return -1;
    case FLOW_GRID_LINE_NAME:
        return named_line(s, ln, axis, end, &at) ? at : min64(nexp + 1, F_GRID_MAX);
    case FLOW_GRID_LINE_AUTO:
        break;
    }
    return -1;
}

// An item's placement on one axis (§ 8.3.1): definite lines, or a span to
// be placed. Lines the wrong way round swap; two alike are one track.
static void resolve_lines(const flow_style_t *s, GItem *it, int axis, int32_t nexp)
{
    const flow_style_t *cs = it->box->style;
    int32_t span0 = 0, span1 = 0;
    int32_t a = line_at(s, axis == 0 ? &cs->grid_column_start : &cs->grid_row_start, axis, false,
                        nexp, &span0);
    int32_t e = line_at(s, axis == 0 ? &cs->grid_column_end : &cs->grid_row_end, axis, true,
                        nexp, &span1);
    it->fixed[axis] = a >= 0 || e >= 0;
    if (a >= 0 && e >= 0) {
        if (e < a) {
            int32_t t = a;
            a = e;
            e = t;
        }
        if (e == a)
            e = a + 1;
    } else if (a >= 0) {
        e = a + (span1 > 0 ? span1 : 1);
    } else if (e >= 0) {
        a = e - (span0 > 0 ? span0 : 1);
        if (a < 0)
            a = 0;
        if (e <= a)
            e = a + 1;
    }
    // Two spans: the end's is dropped.
    it->span[axis] = it->fixed[axis] ? e - a : span0 > 0 ? span0 : span1 > 0 ? span1 : 1;
    if (it->span[axis] > F_GRID_MAX)
        it->span[axis] = F_GRID_MAX;
    if (it->fixed[axis]) {
        // Held to the bound: the last tracks.
        if (e > F_GRID_MAX) {
            e = F_GRID_MAX;
            a = min64(a, e - 1);
        }
        it->at[axis][0] = a;
        it->at[axis][1] = e;
        it->span[axis] = e - a;
    }
}

// The occupied cells, `a` across by `b` down in the placement's own axes:
// F_GRID_MAX cells to a B line, B lines added as they are needed.
typedef struct {
    uint8_t *cell;
    int32_t nb;
} Occ;

static bool occ_reach(L *l, Occ *o, int32_t nb)
{
    if (nb <= o->nb)
        return true;
    int32_t want = max64(nb, min64(F_GRID_MAX, (int64_t)o->nb * 2 + 8));
    uint8_t *grown = scratch_realloc(l, o->cell, (size_t)want * F_GRID_MAX);
    if (grown == NULL)
        return false;
    os64_memset(grown + (size_t)o->nb * F_GRID_MAX, 0, (size_t)(want - o->nb) * F_GRID_MAX);
    o->cell = grown;
    o->nb = want;
    return true;
}

// Whether an area is free; when not, `*past` is the A line just past the
// last occupied cell in it — no area of its size starting from `a` up to
// there, in these B lines, is free either.
static bool occ_free(const Occ *o, int32_t a, int32_t b, int32_t sa, int32_t sb, int32_t *past)
{
    for (int32_t j = a + sa - 1; j >= a; j--)
        for (int32_t i = b; i < b + sb && i < o->nb; i++)
            if (o->cell[(size_t)i * F_GRID_MAX + j]) {
                *past = j + 1;
                return false;
            }
    return true;
}

static bool occ_mark(L *l, Occ *o, int32_t a, int32_t b, int32_t sa, int32_t sb)
{
    if (!occ_reach(l, o, b + sb))
        return false;
    for (int32_t i = b; i < b + sb; i++)
        os64_memset(o->cell + (size_t)i * F_GRID_MAX + a, 1, (size_t)sa);
    return true;
}

// The first B line from `b` with a free cell among the first `na`: no
// area can start above it, and it only moves down as items are placed,
// so a dense search starts there rather than at the top every time.
static int32_t occ_open(const Occ *o, int32_t b, int32_t na)
{
    for (; b < o->nb; b++)
        for (int32_t a = 0; a < na; a++)
            if (!o->cell[(size_t)b * F_GRID_MAX + a])
                return b;
    return b;
}

// The first free A line in B line `b` for an area `sa` by `sb`, from `a`;
// -1 when the line has none.
static int32_t occ_scan(const Occ *o, int32_t a, int32_t b, int32_t sa, int32_t sb, int32_t na)
{
    int32_t past;
    while (a + sa <= na) {
        if (occ_free(o, a, b, sa, sb, &past))
            return a;
        a = past;
    }
    return -1;
}

static void set_area(GItem *it, int axis, int32_t at)
{
    it->at[axis][0] = at;
    it->at[axis][1] = at + it->span[axis];
}

// § 8.5: the items with both positions first, then — along the flow's
// first axis `A`, its lines stepping down `B` — those fixed on B only,
// then the rest at a cursor (`dense`: from the start for each). The count
// of tracks on A, in `*na`. False on no memory.
static bool place_items(L *l, const flow_style_t *s, GPlan *p, const int32_t *nexp, int32_t *na_out)
{
    int A = s->grid_auto_flow_column ? 1 : 0, B = 1 - A;
    bool dense = s->grid_dense;
    Occ o = {NULL, 0};
    int32_t *cursor = scratch_calloc(l, F_GRID_MAX, sizeof(*cursor));
    bool ok = cursor != NULL;
    for (int32_t k = 0; k < p->ni && ok; k++) {
        GItem *it = &p->items[p->ord[k]];
        if (it->fixed[A] && it->fixed[B])
            ok = occ_mark(l, &o, it->at[A][0], it->at[B][0], it->span[A], it->span[B]);
    }
    // Fixed on B: the first A line in its B line free of everything, past
    // what this step put there before it — which may add tracks on A.
    for (int32_t k = 0; k < p->ni && ok; k++) {
        GItem *it = &p->items[p->ord[k]];
        if (it->fixed[A] || !it->fixed[B])
            continue;
        int32_t b = it->at[B][0], sa = it->span[A];
        int32_t a = occ_scan(&o, dense ? 0 : cursor[b], b, sa, it->span[B], F_GRID_MAX);
        if (a < 0)
            a = F_GRID_MAX - sa;        // held to the last tracks
        set_area(it, A, a);
        cursor[b] = a + sa;
        ok = occ_mark(l, &o, a, b, sa, it->span[B]);
    }
    int32_t na = nexp[A];
    for (int32_t k = 0; k < p->ni; k++) {
        const GItem *it = &p->items[k];
        na = max64(na, it->fixed[A] || it->fixed[B] ? it->at[A][1] : it->span[A]);
    }
    na = min64(na, F_GRID_MAX);         // 0 with no items and no explicit tracks
    int32_t ca = 0, cb = 0, open = 0;
    for (int32_t k = 0; k < p->ni && ok; k++) {
        GItem *it = &p->items[p->ord[k]];
        if (it->fixed[B])
            continue;
        int32_t sa = min64(it->span[A], na), sb = it->span[B], a, b, past;
        it->span[A] = sa;
        if (it->fixed[A]) {
            a = it->at[A][0];
            if (dense) {
                b = open = occ_open(&o, open, na);
            } else {
                if (a < ca)
                    cb++;
                b = cb;
            }
            while (b + sb <= F_GRID_MAX && !occ_free(&o, a, b, sa, sb, &past))
                b++;
        } else {
            if (dense) {
                ca = 0;
                cb = open = occ_open(&o, open, na);
            }
            b = cb;
            a = -1;
            for (; b + sb <= F_GRID_MAX; b++) {
                a = occ_scan(&o, b == cb ? ca : 0, b, sa, sb, na);
                if (a >= 0)
                    break;
            }
            if (a < 0)
                a = 0;
            set_area(it, A, a);
        }
        if (b + sb > F_GRID_MAX)
            b = F_GRID_MAX - sb;        // held to the last tracks
        set_area(it, B, b);
        if (!dense) {
            ca = a;
            cb = b;
        }
        ok = occ_mark(l, &o, a, b, sa, sb);
    }
    scratch_free(l, cursor);
    scratch_free(l, o.cell);
    *na_out = na;
    return ok;
}

// The explicit grid, the items placed in it, and the implicit tracks they
// need, in room `cw` by `ch` (-1: not known); false on no memory, the plan
// then partly filled — plan_free takes it either way.
static bool grid_plan(L *l, FBox *b, int64_t cw, int64_t ch, GPlan *p)
{
    const flow_style_t *s = b->style;
    os64_memset(p, 0, sizeof(*p));
    int64_t room[2] = {cw, ch};
    const flow_length_t gaps[2] = {s->column_gap, s->row_gap};
    for (int ax = 0; ax < 2; ax++) {
        bool pct = gaps[ax].kind == FLOW_LENGTH_PERCENT;
        p->gap[ax] = pct && room[ax] < 0 ? 0 : len(gaps[ax], max64(0, room[ax]));
    }
    const flow_tracks_t *tmpl[2] = {&s->grid_template_columns, &s->grid_template_rows};
    const flow_tracks_t *autos[2] = {&s->grid_auto_columns, &s->grid_auto_rows};
    int32_t reps[2], ntmpl[2], nexp[2];
    // An auto-repeat down the rows fills a height the page gave, or else
    // its limit.
    int64_t fill[2] = {cw, ch};
    if (ch < 0 && s->max_height.kind == FLOW_LENGTH_PX)
        fill[1] = content_of(s, s->max_height, 0, vframe(b, max64(0, cw)));
    for (int ax = 0; ax < 2; ax++) {
        reps[ax] = repeats(tmpl[ax], fill[ax], p->gap[ax], f_css_units(l->env, 1));
        ntmpl[ax] = template_count(tmpl[ax], reps[ax]);
        nexp[ax] = max64(ntmpl[ax], ax == 0 ? s->grid_area_cols : s->grid_area_rows);
    }

    for (FBox *c = b->first; c != NULL; c = c->next)
        if (!c->out_of_flow)
            p->ni++;
    p->items = scratch_calloc(l, (size_t)max64(1, p->ni), sizeof(*p->items));
    p->ord = scratch_calloc(l, (size_t)max64(1, p->ni) * 2, sizeof(*p->ord));
    int64_t *keys = scratch_calloc(l, (size_t)max64(1, p->ni), sizeof(*keys));
    if (p->items == NULL || p->ord == NULL || keys == NULL) {
        scratch_free(l, keys);
        return false;
    }
    int32_t k = 0;
    bool reordered = false;
    for (FBox *c = b->first; c != NULL; c = c->next) {
        if (c->out_of_flow)
            continue;
        GItem *it = &p->items[k];
        it->box = c;
        for (int ax = 0; ax < 2; ax++)
            resolve_lines(s, it, ax, nexp[ax]);
        p->ord[k] = k;
        keys[k] = c->style->order;
        reordered |= c->style->order != 0;
        k++;
    }
    if (reordered)
        order_items(keys, p->ord, p->ord + p->ni, p->ni);
    scratch_free(l, keys);
    int32_t na;
    int A = s->grid_auto_flow_column ? 1 : 0;
    if (!place_items(l, s, p, nexp, &na))
        return false;
    int32_t count[2];
    count[A] = na;
    count[1 - A] = nexp[1 - A];
    for (int32_t i = 0; i < p->ni; i++)
        count[1 - A] = max64(count[1 - A], p->items[i].at[1 - A][1]);
    for (int ax = 0; ax < 2; ax++) {
        // An axis with no tracks has none: room for one is only so the
        // allocation is never of nothing.
        int32_t n = (int32_t)min64(count[ax], F_GRID_MAX);
        p->t[ax] = scratch_calloc(l, (size_t)max64(1, n), sizeof(GTrack));
        if (p->t[ax] == NULL)
            return false;
        p->n[ax] = n;
        for (int32_t i = 0; i < n; i++) {
            GTrack *t = &p->t[ax][i];
            if (i < ntmpl[ax]) {
                t->fn = template_track(tmpl[ax], reps[ax], i, &t->fit_repeat);
            } else if (autos[ax]->n > 0) {
                t->fn = autos[ax]->tracks[(i - ntmpl[ax]) % autos[ax]->n];
            } else {
                t->fn.min.kind = t->fn.max.kind = FLOW_TRACK_AUTO;
            }
        }
        for (int32_t j = 0; j < p->ni; j++)
            for (int32_t i = p->items[j].at[ax][0]; i < p->items[j].at[ax][1] && i < n; i++)
                p->t[ax][i].used = true;
    }
    return true;
}

static void plan_free(L *l, GPlan *p)
{
    scratch_free(l, p->items);
    scratch_free(l, p->ord);
    scratch_free(l, p->t[0]);
    scratch_free(l, p->t[1]);
}

// An auto-fit repeat that no item spans is no track at all (§ 7.2.3.2),
// and the gaps beside it go with it.
static bool track_gone(const GTrack *t)
{
    return t->fit_repeat && !t->used;
}

// The tracks placed end to end from 0, `gap` between those not gone.
static void space_tracks(GTrack *t, int32_t n, int64_t gap)
{
    int64_t at = 0;
    bool first = true;
    for (int32_t i = 0; i < n; i++) {
        if (!track_gone(&t[i]) && !first)
            at += gap;
        t[i].pos = at;
        at += t[i].size;
        first &= track_gone(&t[i]);
    }
}

static bool track_flexible(const GTrack *t)
{
    return !track_gone(t) && t->fn.max.kind == FLOW_TRACK_FR;
}

// The tracks from `a` to `e` and the gaps between them.
static int64_t span_sum(const GTrack *t, int32_t a, int32_t e, int64_t gap, bool limits)
{
    int64_t sum = 0;
    int32_t seen = 0;
    for (int32_t i = a; i < e; i++) {
        if (track_gone(&t[i]))
            continue;
        sum += limits && t[i].limit != kNoLimit ? t[i].limit : t[i].base;
        seen++;
    }
    return sum + gap * max64(0, seen - 1);
}

// `extra` shared by `w` among the tracks from `a` to `e`, into their
// bases, or their limits — a limit not set yet taken as the base first,
// and a fit-content one held to its length.
static void grow_tracks(GTrack *t, int32_t a, int32_t e, const int64_t *w, int64_t *parts,
                        int64_t extra, bool limits, int64_t avail)
{
    share(extra, w + a, parts + a, e - a);
    for (int32_t i = a; i < e; i++) {
        if (w[i] == 0)
            continue;
        if (!limits) {
            t[i].base += parts[i];
            continue;
        }
        int64_t from = t[i].limit == kNoLimit ? t[i].base : t[i].limit;
        t[i].limit = from + parts[i];
        if (t[i].fn.fit && breadth_fixed(t[i].fn.max, avail))
            t[i].limit = max64(t[i].base, min64(t[i].limit, len(t[i].fn.max.len, avail)));
    }
}

// § 12 on one axis, as GRID.md step 3 reads it: `avail` the room (-1: not
// known, as when a width is being asked for); `stretch` the room `auto`
// tracks stretch into (-1: none). Each track's `size` and `pos` on exit.
// False on no memory.
static bool size_tracks(L *l, GTrack *t, int32_t n, GItem *items, int32_t ni, int axis,
                        int64_t avail, int64_t gap, int64_t stretch)
{
    int64_t *w = scratch_calloc(l, (size_t)n * 2, sizeof(*w));
    int32_t *key = scratch_calloc(l, (size_t)ni * 2 + F_GRID_MAX + 2, sizeof(*key));
    if (w == NULL || key == NULL) {
        scratch_free(l, w);
        scratch_free(l, key);
        return false;
    }
    int64_t *parts = w + n;
    int32_t visible = 0;
    for (int32_t i = 0; i < n; i++) {
        GTrack *tr = &t[i];
        tr->base = 0;
        tr->limit = kNoLimit;
        if (track_gone(tr)) {
            tr->limit = 0;
            continue;
        }
        visible++;
        if (breadth_fixed(tr->fn.min, avail))
            tr->base = max64(0, len(tr->fn.min.len, avail));
        if (!tr->fn.fit && breadth_fixed(tr->fn.max, avail))
            tr->limit = max64(tr->base, len(tr->fn.max.len, avail));
    }
    int64_t gaps = gap * max64(0, visible - 1);

    // The items, by how many tracks they span, fewest first; those that
    // span a flexible track after all the rest (§ 12.5). Sorted by count,
    // stably, so each item is looked at once.
    int32_t *order = key + ni, *count = key + 2 * ni;
    for (int32_t j = 0; j < ni; j++) {
        int32_t a = items[j].at[axis][0], e = min64(items[j].at[axis][1], n);
        key[j] = e - a;
        for (int32_t i = a; i < e; i++)
            if (track_flexible(&t[i]))
                key[j] = F_GRID_MAX + 1;
        count[key[j]]++;
    }
    for (int32_t k = 1; k <= F_GRID_MAX + 1; k++)
        count[k] += count[k - 1];
    for (int32_t j = ni - 1; j >= 0; j--)
        order[--count[key[j]]] = j;
    for (int32_t o = 0; o < ni; o++) {
        GItem *it = &items[order[o]];
        int32_t a = it->at[axis][0], e = min64(it->at[axis][1], n);
        bool flexible = key[order[o]] == F_GRID_MAX + 1;
        // The bases: an intrinsic minimum takes the item's minimum
        // contribution for `auto`, its min-content or max-content for
        // those; a flexible track shares by its factor.
        int64_t want_min = 0;
        for (int32_t i = a; i < e; i++) {
            const GTrack *tr = &t[i];
            bool intrinsic_min = !track_gone(tr) && !breadth_fixed(tr->fn.min, avail);
            w[i] = !intrinsic_min || (flexible && !track_flexible(tr)) ? 0
                   : flexible ? max64(1, tr->fn.max.fr) : 1;
            if (w[i] != 0)
                want_min = max64(want_min, tr->fn.min.kind == FLOW_TRACK_MAX_CONTENT ? it->cmax
                                         : tr->fn.min.kind == FLOW_TRACK_MIN_CONTENT ? it->cmin
                                                                                      : it->cauto);
        }
        int64_t extra = want_min - span_sum(t, a, e, gap, false);
        if (extra > 0)
            grow_tracks(t, a, e, w, parts, extra, false, avail);
        if (flexible)
            continue;
        // The limits: an intrinsic maximum takes the item's
        // max-content, or its min-content for `min-content`.
        int64_t want_max = 0;
        for (int32_t i = a; i < e; i++) {
            const GTrack *tr = &t[i];
            w[i] = !track_gone(tr) && (tr->fn.fit || !breadth_fixed(tr->fn.max, avail)) ? 1 : 0;
            if (w[i] != 0)
                want_max = max64(want_max, tr->fn.max.kind == FLOW_TRACK_MIN_CONTENT ? it->cmin
                                                                                      : it->cmax);
        }
        extra = want_max - span_sum(t, a, e, gap, true);
        if (extra > 0)
            grow_tracks(t, a, e, w, parts, extra, true, avail);
    }
    // A limit nothing set is the base; none is below it.
    for (int32_t i = 0; i < n; i++)
        if (t[i].limit == kNoLimit || t[i].limit < t[i].base)
            t[i].limit = t[i].base;

    // Maximize (§ 12.6): the room left grows the tracks to their limits,
    // equally; with no room known, every track is at its limit.
    int64_t sum = 0;
    for (int32_t i = 0; i < n; i++)
        sum += t[i].base;
    if (avail < 0) {
        for (int32_t i = 0; i < n; i++)
            if (!track_flexible(&t[i]))
                t[i].base = t[i].limit;
    } else {
        int64_t free = avail - sum - gaps;
        while (free > 0) {
            int32_t open = 0;
            for (int32_t i = 0; i < n; i++) {
                w[i] = !track_flexible(&t[i]) && t[i].base < t[i].limit ? 1 : 0;
                open += (int32_t)w[i];
            }
            if (open == 0)
                break;
            share(free, w, parts, n);
            for (int32_t i = 0; i < n; i++) {
                if (w[i] == 0)
                    continue;
                int64_t grown = min64(t[i].base + parts[i], t[i].limit);
                free -= grown - t[i].base;
                t[i].base = grown;
            }
        }
    }

    // Flexible tracks (§ 12.7): the room left shared by their factors — a
    // track whose base is more than its share keeps its base, and the rest
    // share again; a sum of factors below 1 takes only that part of it.
    // With no room known, one fr is the most any flexible track or item
    // asks of it. A factor is in thousandths and a size may be a sum of a
    // thousand tracks, so their products go through mul_div, held rather
    // than wrapped.
    int32_t nflex = 0;
    for (int32_t i = 0; i < n; i++)
        nflex += track_flexible(&t[i]);
    if (nflex > 0 && avail >= 0) {
        for (int32_t i = 0; i < n; i++)
            w[i] = track_flexible(&t[i]) ? max64(0, t[i].fn.max.fr) : 0;
        for (bool again = true; again;) {
            again = false;
            int64_t left = avail - gaps, frs = 0;
            for (int32_t i = 0; i < n; i++) {
                if (w[i] == 0)
                    left -= t[i].base;          // not flexible, or held at its base
                frs += w[i];
            }
            if (frs == 0 || left <= 0)
                break;
            int64_t per = mul_div(left, 1000, max64(frs, 1000));     // one fr
            for (int32_t i = 0; i < n; i++)
                if (w[i] != 0 && t[i].base > mul_div(per, w[i], 1000)) {
                    w[i] = 0;
                    again = true;
                }
            if (again)
                continue;
            int64_t whole = frs >= 1000 ? left : mul_div(left, frs, 1000);
            share(whole, w, parts, n);
            for (int32_t i = 0; i < n; i++)
                if (w[i] != 0)
                    t[i].base = max64(t[i].base, parts[i]);
        }
    } else if (nflex > 0) {
        int64_t per = 0;
        for (int32_t i = 0; i < n; i++)
            if (track_flexible(&t[i]))
                per = max64(per, t[i].fn.max.fr > 1000 ? mul_div(t[i].base, 1000, t[i].fn.max.fr)
                                                       : t[i].base);
        for (int32_t j = 0; j < ni; j++) {
            int32_t a = items[j].at[axis][0], e = min64(items[j].at[axis][1], n);
            int64_t frs = 0, fixed = gap * max64(0, e - a - 1);
            for (int32_t i = a; i < e; i++) {
                if (track_flexible(&t[i]))
                    frs += t[i].fn.max.fr;
                else
                    fixed += t[i].base;
            }
            if (frs > 0 && items[j].cmax > fixed)
                per = max64(per, mul_div(items[j].cmax - fixed, 1000, max64(frs, 1000)));
        }
        for (int32_t i = 0; i < n; i++)
            if (track_flexible(&t[i]))
                t[i].base = max64(t[i].base, mul_div(per, t[i].fn.max.fr, 1000));
    }
    scratch_free(l, key);

    // Stretch (§ 12.8): what room is left goes to the `auto` tracks.
    if (stretch >= 0) {
        int64_t free = stretch - gaps;
        int32_t autos = 0;
        for (int32_t i = 0; i < n; i++) {
            free -= t[i].base;
            w[i] = !track_gone(&t[i]) && t[i].fn.max.kind == FLOW_TRACK_AUTO && !t[i].fn.fit;
            autos += (int32_t)w[i];
        }
        if (free > 0 && autos > 0) {
            share(free, w, parts, n);
            for (int32_t i = 0; i < n; i++)
                t[i].base += w[i] != 0 ? parts[i] : 0;
        }
    }

    for (int32_t i = 0; i < n; i++)
        t[i].size = track_gone(&t[i]) ? 0 : t[i].base;
    space_tracks(t, n, gap);
    scratch_free(l, w);
    return true;
}

// The tracks' extent, gaps included.
static int64_t tracks_extent(const GTrack *t, int32_t n)
{
    return n > 0 ? t[n - 1].pos + t[n - 1].size : 0;
}

// `justify-content` or `align-content` on the tracks (§ 10.5), the room
// they leave being `free`: `normal` and `stretch` are start, the auto
// tracks having taken the room already. Overflowing tracks are centred
// or ended all the same.
static void place_tracks(GTrack *t, int32_t n, flow_place_t how, int64_t free)
{
    switch (how) {
    case FLOW_PLACE_LEFT: case FLOW_PLACE_FLEX_START: case FLOW_PLACE_SELF_START:
        how = FLOW_PLACE_START;
        break;
    case FLOW_PLACE_RIGHT: case FLOW_PLACE_FLEX_END: case FLOW_PLACE_SELF_END:
        how = FLOW_PLACE_END;
        break;
    default: break;
    }
    int32_t visible = 0;
    for (int32_t i = 0; i < n; i++)
        visible += !track_gone(&t[i]);
    if (visible == 0)
        return;
    int32_t k = 0;
    for (int32_t i = 0; i < n; i++) {
        t[i].pos += justify_extra(how, false, false, free, min64(k, visible - 1), visible);
        k += !track_gone(&t[i]);
    }
}

// Where an item goes across its area on one axis: `justify-self` or
// `align-self` over the container's `-items`; `normal` stretches all but
// a replaced box, which starts (Box Alignment 3 § 6).
static flow_place_t self_align(flow_place_t self, flow_place_t items, const FBox *c)
{
    flow_place_t a = self != FLOW_PLACE_AUTO ? self : items;
    switch (a) {
    case FLOW_PLACE_NORMAL: case FLOW_PLACE_AUTO:
        return c->kind == FB_REPLACED ? FLOW_PLACE_START : FLOW_PLACE_STRETCH;
    case FLOW_PLACE_FLEX_START: case FLOW_PLACE_SELF_START: case FLOW_PLACE_LEFT:
    case FLOW_PLACE_BASELINE:
        return FLOW_PLACE_START;
    case FLOW_PLACE_FLEX_END: case FLOW_PLACE_SELF_END: case FLOW_PLACE_RIGHT:
        return FLOW_PLACE_END;
    default:
        return a;
    }
}

// The widths the items ask of the columns: their intrinsic widths and
// their horizontal margins, and — for an `auto` minimum — no more than a
// min-width asks, or nothing but their frame when they scroll.
static void column_asks(L *l, GPlan *p, bool min_only)
{
    for (int32_t j = 0; j < p->ni && !l->failed; j++) {
        GItem *it = &p->items[j];
        FBox *c = it->box;
        const flow_style_t *cs = c->style;
        Intr ci = intrinsic(l, c);
        int64_t m = len(cs->margin[FLOW_LEFT], 0) + len(cs->margin[FLOW_RIGHT], 0);
        int64_t frame = hframe(c, 0);
        it->cmin = ci.min + m;
        it->cmax = (min_only ? ci.min : ci.max) + m;
        // Its minimum contribution (§ 6.6): a width the page gave is its
        // size whatever its minimum; else its automatic minimum, a
        // min-width the page gave, only its frame when it scrolls, or its
        // content's.
        if (cs->width.kind == FLOW_LENGTH_PX)
            it->cauto = it->cmin;
        else if (cs->min_width.kind == FLOW_LENGTH_PX)
            it->cauto = content_of(cs, cs->min_width, 0, frame) + frame + m;
        else if (f_overflow_scrolls(cs->overflow_x) || f_overflow_scrolls(cs->overflow_y))
            it->cauto = frame + m;
        else
            it->cauto = it->cmin;
    }
}

// A grid container's content min-content and max-content widths: its
// columns sized with no room given, its items asking their min-content
// widths, and then their max-content ones.
static Intr grid_intrinsic(L *l, FBox *b)
{
    Intr r = {0, 0};
    GPlan p;
    if (grid_plan(l, b, -1, -1, &p)) {
        for (int pass = 0; pass < 2 && !l->failed; pass++) {
            column_asks(l, &p, pass == 0);
            if (!size_tracks(l, p.t[0], p.n[0], p.items, p.ni, 0, -1, p.gap[0], -1))
                break;
            *(pass == 0 ? &r.min : &r.max) = tracks_extent(p.t[0], p.n[0]);
        }
    } else {
        fail(l);
    }
    plan_free(l, &p);
    r.max = max64(r.max, r.min);
    return r;
}

// A grid container's items, from `in->y` down, content box `cx`..`cx + cw`.
// On exit `in->y` is the content's bottom.
// A grid item's left margin across an area `aw` wide, its border box `w`
// wide: auto margins take the room first, else `justify-self` places it.
static int64_t grid_ml(const flow_style_t *s, const FBox *c, int64_t aw, int64_t w)
{
    const flow_style_t *cs = c->style;
    bool ml_auto = cs->margin[FLOW_LEFT].kind == FLOW_LENGTH_AUTO;
    bool mr_auto = cs->margin[FLOW_RIGHT].kind == FLOW_LENGTH_AUTO;
    int64_t ml = ml_auto ? 0 : len(cs->margin[FLOW_LEFT], aw);
    int64_t mr = mr_auto ? 0 : len(cs->margin[FLOW_RIGHT], aw);
    int64_t room = aw - (ml + w + mr);
    flow_place_t a = self_align(cs->justify_self, s->justify_items, c);
    if (ml_auto || mr_auto) {
        if (room > 0)
            ml += ml_auto && mr_auto ? room / 2 : ml_auto ? room : 0;
    } else if (a == FLOW_PLACE_END) {
        ml += room;
    } else if (a == FLOW_PLACE_CENTER) {
        ml += room / 2;
    }
    return ml;
}

// A grid's plan and its columns, sized and placed in the width `cw`; `ch`
// its content height, -1 when not known. False on no memory, the plan
// then partly filled — plan_free takes it either way.
static bool grid_open(L *l, FBox *b, int64_t cw, int64_t ch, GPlan *p)
{
    const flow_style_t *s = b->style;
    if (!grid_plan(l, b, cw, ch, p)) {
        fail(l);
        return false;
    }
    GTrack *cols = p->t[0];
    bool spread_x = s->justify_content == FLOW_PLACE_NORMAL ||
                    s->justify_content == FLOW_PLACE_STRETCH;
    column_asks(l, p, false);
    if (!size_tracks(l, cols, p->n[0], p->items, p->ni, 0, cw, p->gap[0], spread_x ? cw : -1))
        return false;
    place_tracks(cols, p->n[0], s->justify_content, cw - tracks_extent(cols, p->n[0]));
    return !l->failed;
}

// Everything after a grid's items are laid out, in its content box from
// `cx`, `top`, against its content height `given` (-1: not known): the
// rows, sized from the items' heights, and each item moved into its area —
// down into its rows and stretched, across into its columns. It can run
// again (refit):
// it reads the items' natural heights, never what an earlier placement
// gave them, and moves each item to an absolute place. `*used` is the
// content height it came to, `*natural` the rows' extent with no height
// given. False on no memory.
static bool grid_place(L *l, const FStyles *styles, FBox *b, GPlan *p, int64_t cx, int64_t top,
                       int64_t given, int64_t *used, int64_t *natural)
{
    const flow_style_t *s = b->style;
    GTrack *cols = p->t[0], *rows = p->t[1];
    int64_t vframe_b = b->border[FLOW_TOP] + b->padding[FLOW_TOP] + b->padding[FLOW_BOTTOM] +
                       b->border[FLOW_BOTTOM];
    for (int32_t j = 0; j < p->ni; j++) {
        GItem *it = &p->items[j];
        const flow_style_t *cs = it->box->style;
        int64_t aw = cols[it->at[0][1] - 1].pos + cols[it->at[0][1] - 1].size -
                     cols[it->at[0][0]].pos;
        int64_t m = len(cs->margin[FLOW_TOP], aw) + len(cs->margin[FLOW_BOTTOM], aw);
        const FBox *c = it->box;
        int64_t vf = c->border[FLOW_TOP] + c->padding[FLOW_TOP] + c->padding[FLOW_BOTTOM] +
                     c->border[FLOW_BOTTOM];
        it->cmin = it->cmax = m + natural_h(c);
        // Its minimum contribution, as column_asks has it across: a height
        // the page gave; else a min-height the page gave, nothing but its
        // frame when it scrolls, or its content's.
        if (cs->height.kind == FLOW_LENGTH_PX)
            it->cauto = it->cmin;
        else if (cs->min_height.kind == FLOW_LENGTH_PX)
            it->cauto = content_of(cs, cs->min_height, 0, vf) + vf + m;
        else if (f_overflow_scrolls(cs->overflow_x) || f_overflow_scrolls(cs->overflow_y))
            it->cauto = vf + m;
        else
            it->cauto = it->cmin;
    }
    // The rows as the items alone size them; then, when there is a height
    // to fill — one the page gave, or a min-height rows stretch into —
    // sized again against it.
    if (!size_tracks(l, rows, p->n[1], p->items, p->ni, 1, -1, p->gap[1], -1))
        return false;
    *natural = tracks_extent(rows, p->n[1]);
    bool spread_y = s->align_content == FLOW_PLACE_NORMAL || s->align_content == FLOW_PLACE_STRETCH;
    int64_t floor_h = s->min_height.kind == FLOW_LENGTH_PX
                          ? content_of(s, s->min_height, 0, vframe_b) : -1;
    int64_t room_y = given >= 0 ? given : floor_h;
    if ((given >= 0 || (spread_y && room_y >= 0)) &&
        !size_tracks(l, rows, p->n[1], p->items, p->ni, 1, given, p->gap[1],
                     spread_y ? room_y : -1))
        return false;
    int64_t total = given >= 0 ? given : clamp_height(s, tracks_extent(rows, p->n[1]), vframe_b);
    // A percentage row gap counted as nothing while the height was being
    // found is of that height now (Box Alignment 3 § 8.3: a cyclic gap).
    if (given < 0 && s->row_gap.kind == FLOW_LENGTH_PERCENT)
        space_tracks(rows, p->n[1], len(s->row_gap, total));
    place_tracks(rows, p->n[1], s->align_content, total - tracks_extent(rows, p->n[1]));

    // Each item moved into its area: down into its rows, and across into
    // its columns (a plan made again for a refit may put it in others).
    for (int32_t j = 0; j < p->ni; j++) {
        GItem *it = &p->items[j];
        FBox *c = it->box;
        const flow_style_t *cs = c->style;
        const GTrack *r0 = &rows[it->at[1][0]], *r1 = &rows[it->at[1][1] - 1];
        int64_t ah = r1->pos + r1->size - r0->pos;
        int64_t aw = cols[it->at[0][1] - 1].pos + cols[it->at[0][1] - 1].size -
                     cols[it->at[0][0]].pos;
        bool at_auto = cs->margin[FLOW_TOP].kind == FLOW_LENGTH_AUTO;
        bool ab_auto = cs->margin[FLOW_BOTTOM].kind == FLOW_LENGTH_AUTO;
        int64_t mt = len(cs->margin[FLOW_TOP], aw), mb = len(cs->margin[FLOW_BOTTOM], aw);
        int64_t vf = c->border[FLOW_TOP] + c->padding[FLOW_TOP] + c->padding[FLOW_BOTTOM] +
                     c->border[FLOW_BOTTOM];
        int64_t h = natural_h(c), room = ah - (mt + h + mb), dy = 0;
        flow_place_t a = self_align(cs->align_self, s->align_items, c);
        if (at_auto || ab_auto) {
            if (room > 0)
                dy = at_auto && ab_auto ? room / 2 : at_auto ? room : 0;
        } else if (a == FLOW_PLACE_STRETCH) {
            if (cs->height.kind != FLOW_LENGTH_PX && c->kind != FB_TABLE)
                h = max64(vf, clamp_height(cs, max64(0, ah - mt - mb - vf), vf) + vf);
        } else if (a == FLOW_PLACE_END) {
            dy = room;
        } else if (a == FLOW_PLACE_CENTER) {
            dy = room / 2;
        }
        set_item_h(l, styles, c, h);
        // A relative item's offsets are of its grid area, its containing
        // block (Grid 2 § 9), both ways.
        int64_t rdx = 0, rdy = 0;
        if (c->positioned && !c->out_of_flow)
            rel_offset(cs, aw, ah, &rdx, &rdy);
        // Across, its place in this area's width: the width it was laid out
        // at, placed again by its alignment. A table placed itself, and
        // keeps where it was laid out in its area (area_dx).
        int64_t ax = c->kind == FB_TABLE ? c->area_dx : grid_ml(s, c, aw, c->w) + rdx;
        translate(c, cx + cols[it->at[0][0]].pos + ax - c->x,
                  top + r0->pos + mt + dy + rdy - c->y);
        c->grid_row = it->at[1][0];
        c->grid_col = it->at[0][0];
    }
    *used = total;
    return !l->failed;
}

// A grid container's items, from `in->y` down, content box `cx`..`cx + cw`.
// On exit `in->y` is the content's bottom, and `*natural` the height its
// items alone ask (FBox.content_h), whatever height it was given.
static void grid_layout(L *l, const FStyles *styles, FBox *b, int64_t cx, int64_t cw, Cursor *in,
                        int64_t *natural)
{
    const flow_style_t *s = b->style;
    int64_t top = in->y;
    *natural = 0;
    int64_t vframe_b = b->border[FLOW_TOP] + b->padding[FLOW_TOP] + b->padding[FLOW_BOTTOM] +
                       b->border[FLOW_BOTTOM];
    bool h_def = s->height.kind == FLOW_LENGTH_PX || b->abs_h_set;
    int64_t h_given = b->abs_h_set ? b->abs_h : content_of(s, s->height, 0, vframe_b);
    if (h_def)
        h_given = clamp_height(s, h_given, vframe_b);
    for (FBox *c = b->first; c != NULL; c = c->next)
        if (c->out_of_flow) {
            // Its static position: the content box's start corner (GRID.md
            // § Booked has the grid area as its containing block).
            c->x = cx;
            c->y = top;
            c->static_known = true;
        }
    in->first = top;
    GPlan p;
    if (!grid_open(l, b, cw, h_def ? h_given : -1, &p))
        goto done;
    GTrack *cols = p.t[0];

    // Each item laid out once, in tree order, at its columns' width — all
    // of it when it stretches — and its place across them.
    for (int32_t j = 0; j < p.ni && !l->failed; j++) {
        GItem *it = &p.items[j];
        FBox *c = it->box;
        const flow_style_t *cs = c->style;
        const GTrack *c0 = &cols[it->at[0][0]], *c1 = &cols[it->at[0][1] - 1];
        int64_t ax = cx + c0->pos, aw = max64(0, c1->pos + c1->size - c0->pos);
        Cursor cur = {top, kNoMargins, -1};
        if (c->kind != FB_TABLE) {
            bool ml_auto = cs->margin[FLOW_LEFT].kind == FLOW_LENGTH_AUTO;
            bool mr_auto = cs->margin[FLOW_RIGHT].kind == FLOW_LENGTH_AUTO;
            int64_t ml = ml_auto ? 0 : len(cs->margin[FLOW_LEFT], aw);
            int64_t mr = mr_auto ? 0 : len(cs->margin[FLOW_RIGHT], aw);
            int64_t frame = hframe(c, aw);
            flow_place_t a = self_align(cs->justify_self, s->justify_items, c);
            bool width_given = cs->width.kind == FLOW_LENGTH_PX ||
                               cs->width.kind == FLOW_LENGTH_PERCENT;
            int64_t cwid;
            if (width_given)
                cwid = clamp_width(cs, content_of(cs, cs->width, aw, frame), aw, frame);
            else if (c->kind == FB_REPLACED && a != FLOW_PLACE_STRETCH)
                cwid = replaced_size(l, c->node, cs, aw).w;
            else if (a == FLOW_PLACE_STRETCH && !ml_auto && !mr_auto)
                cwid = clamp_width(cs, max64(0, aw - ml - mr - frame), aw, frame);
            else
                cwid = fit_content(l, c, aw);
            c->flex_sized = true;
            c->flex_w = cwid;
            c->flex_ml = grid_ml(s, c, aw, frame + cwid);
        }
        block(l, styles, c, ax, aw, &cur);
        c->area_dx = c->x - ax;
    }
    if (l->failed)
        goto done;
    int64_t used;
    if (grid_place(l, styles, b, &p, cx, top, h_def ? h_given : -1, &used, natural))
        in->y = top + used;
done:
    if (l->failed || !sized_later(b))
        items_done(l, styles, b, cw);
    if (l->failed)
        in->y = top;
    plan_free(l, &p);
}

// A flex or grid container whose height its own container changed after
// it was laid out (stretched, or flexed down a column): its items, laid out
// once already, are placed again against the height it now has. Nothing
// inside them is laid out again (FLEX.md, decision 1).
static void refit(L *l, const FStyles *styles, FBox *c)
{
    if (l->failed || !(c->flex || c->grid))
        return;
    int64_t vf = c->border[FLOW_TOP] + c->padding[FLOW_TOP] + c->padding[FLOW_BOTTOM] +
                 c->border[FLOW_BOTTOM];
    int64_t given = max64(0, c->h - vf), used, natural;
    int64_t top = c->y + c->border[FLOW_TOP] + c->padding[FLOW_TOP];
    if (c->grid) {
        GPlan p;
        if (grid_open(l, c, content_width(c), given, &p))
            (void)grid_place(l, styles, c, &p, c->x + c->border[FLOW_LEFT] + c->padding[FLOW_LEFT],
                             top, given, &used, &natural);
        plan_free(l, &p);
        return;
    }
    int32_t n = flex_items(c);
    if (n == 0)
        return;
    FlexRun r;
    if (flex_run_open(l, c, n, &r)) {
        r.cx = c->x + c->border[FLOW_LEFT] + c->padding[FLOW_LEFT];
        r.cw = content_width(c);
        r.top = top;
        (void)flex_place(l, styles, &r, given, &used, &natural);
    }
    flex_run_free(l, &r);
}

static void block_at(L *l, const FStyles *styles, FBox *b, int64_t cbx, int64_t cbw, Cursor *cur);

// A flex or grid item's size is its container's to finish — stretched
// across a line or down its rows, grown or shrunk down a column — after it
// is laid out, so the absolute boxes it contains wait for that
// (items_done), or they would be placed against a size the item no longer
// has. An item that is a flex or grid container waits with its own items'
// for the same reason: its container may yet place them again (refit).
static bool sized_later(const FBox *b)
{
    return b->parent != NULL && (b->parent->flex || b->parent->grid) && !b->out_of_flow;
}

// A block-level box in its parent's flow. On entry `cur` is the point
// below the previous box with the margins still pending there; on exit it
// is the point below this one, with this box's bottom margin (and, when it
// adjoins, its last child's) pending.
//
// A RELATIVE box (CSS 2.1 § 9.4.3) is laid out at its offset from where the
// flow puts it — everything inside it with it — and the flow goes on as if
// it had not moved: the cursor it hands back is the one it would have. Laid
// out there from the start rather than moved once finished, so whatever
// finishes inside it is where it will stay (LAYOUT.md § Proof, relation b).
// A cell is placed by its table, and moves once placed (positioned_done).
static void block(L *l, const FStyles *styles, FBox *b, int64_t cbx, int64_t cbw, Cursor *cur)
{
    if (l->failed)
        return;
    int64_t dx = 0, dy = 0;
    if (b->positioned && !b->out_of_flow)
        rel_offset(b->style, cbw, definite_height(b->parent), &dx, &dy);
    if (dx == 0 && dy == 0) {
        block_at(l, styles, b, cbx, cbw, cur);
        return;
    }
    Cursor moved = *cur;
    moved.y += dy;
    if (moved.first >= 0)
        moved.first += dy;
    block_at(l, styles, b, cbx + dx, cbw, &moved);
    *cur = moved;
    cur->y -= dy;
    if (cur->first >= 0)
        cur->first -= dy;
}

static void block_at(L *l, const FStyles *styles, FBox *b, int64_t cbx, int64_t cbw, Cursor *cur)
{
    if (b->kind == FB_TABLE) {
        table(l, styles, b, cbx, cbw, cur);
        return;
    }
    const flow_style_t *s = b->style;
    Replaced rep = {0, 0, false, false};
    int64_t forced_w = -1;
    if (b->kind == FB_REPLACED) {
        rep = replaced_size(l, b->node, s, cbw);
        forced_w = rep.w;
    } else if (b->atom_sized) {
        forced_w = b->atom_w;
    } else if (b->abs_sized) {
        forced_w = b->abs_w;
    } else if (s->width.kind == FLOW_LENGTH_FIT_CONTENT) {
        forced_w = fit_content(l, b, cbw);
    }
    // A flex item's container sized it, a replaced one included — whose
    // height, when the page gave none, follows the width it was given by
    // its own ratio.
    if (b->flex_sized) {
        forced_w = b->flex_w;
        if (b->kind == FB_REPLACED && !height_given(s))
            rep.h = replaced_for(l, b, 0, b->flex_w, cbw);
    }
    int64_t ml;
    int64_t cw = widths(b, cbw, forced_w, &ml);
    // An out-of-flow box's own equations placed it (absolute, below), and
    // a flex item's container did (flex, below).
    if (b->abs_sized)
        ml = b->abs_ml;
    if (b->flex_sized)
        ml = b->flex_ml;
    b->laid++;
    int64_t mt = len(s->margin[FLOW_TOP], cbw), mb = len(s->margin[FLOW_BOTTOM], cbw);
    bool root = bfc_root(b);
    bool top_open = !root && b->border[FLOW_TOP] == 0 && b->padding[FLOW_TOP] == 0;
    bool height_auto = s->height.kind != FLOW_LENGTH_PX && !b->abs_h_set;
    // §8.3.1: a bottom margin meets the last child's, and a box's two
    // margins meet each other, only across a height of auto and a
    // min-height of zero.
    bool min_height = s->min_height.kind == FLOW_LENGTH_PX && s->min_height.value > 0;
    bool bottom_open = !root && height_auto && !min_height && b->border[FLOW_BOTTOM] == 0 &&
                       b->padding[FLOW_BOTTOM] == 0;
    int64_t vframe_px = b->border[FLOW_TOP] + b->padding[FLOW_TOP] + b->padding[FLOW_BOTTOM] +
                        b->border[FLOW_BOTTOM];

    b->x = cbx + ml;
    b->w = b->border[FLOW_LEFT] + b->padding[FLOW_LEFT] + cw + b->padding[FLOW_RIGHT] +
           b->border[FLOW_RIGHT];
    int64_t cx = b->x + b->border[FLOW_LEFT] + b->padding[FLOW_LEFT];
    Margins pm = margins_join(cur->pm, margin_of(mt));

    Cursor in;
    int64_t y_border = 0;
    if (top_open) {
        in = (Cursor){cur->y, pm, -1};
    } else {
        y_border = cur->y + margins_sum(pm);
        in = (Cursor){y_border + b->border[FLOW_TOP] + b->padding[FLOW_TOP], kNoMargins, -1};
        in.first = in.y;
    }
    int64_t content_top = in.y;
    int64_t asked = -1;                 // a flex or grid container's items' own height

    // An outside marker waits for its item's first line with content.
    bool waiting = b->marker != NULL && l->nmarkers < F_DEPTH_MAX;
    if (waiting)
        l->markers[l->nmarkers++] = b;
    if (b->kind == FB_REPLACED) {
        in.y += rep.h;
    } else if (b->flex) {
        flex(l, styles, b, cx, cw, &in, &asked);
    } else if (b->grid) {
        grid_layout(l, styles, b, cx, cw, &in, &asked);
    } else if (b->ifc) {
        lines(l, styles, b, cx, cw, &in);
    } else {
        children(l, styles, b, cx, cw, &in);
    }
    // A marker still waiting when its item ends had no line with content
    // (an empty item holds a line of its own below); it stops waiting.
    // Inner items have taken theirs off already, so it is on top.
    if (waiting && l->nmarkers > 0 && l->markers[l->nmarkers - 1] == b)
        l->nmarkers--;
    if (l->failed) {
        // Layout stopped inside this box: it holds what was laid out.
        b->placed = true;
        b->unfinished = true;
        b->y = top_open ? (in.first >= 0 ? in.first : cur->y + margins_sum(pm)) : y_border;
        b->h = max64(0, in.y - b->y);
        // And the parent's cursor stops at its bottom, so every box above
        // holds what was laid out and the page can be scrolled to it.
        if (cur->first < 0)
            cur->first = b->y;
        cur->y = b->y + b->h;
        cur->pm = kNoMargins;
        return;
    }

    // An item with nothing in it still shows its marker, and the marker
    // stands on a line: the item holds one line of its own font, as a line
    // with content would, margins above resolved there.
    if (b->marker != NULL && (top_open ? in.first < 0 : in.y == content_top)) {
        Fonts f;
        if (fonts_for(l, s, &f)) {
            Extent fb = font_box(s, f.info.ascent, f.info.descent, f.info.line_height);
            in.y += margins_sum(in.pm);
            in.pm = kNoMargins;
            if (in.first < 0)
                in.first = in.y;
            in.y += fb.bottom - fb.top;
        }
    }

    // Nothing inside resolved a position: the box is empty, and its top
    // and bottom margins collapse together with whatever adjoins them.
    if (top_open && in.first < 0 && height_auto && bottom_open) {
        b->placed = true;
        b->y = cur->y + margins_sum(pm);
        b->h = b->content_h = 0;
        cur->pm = margins_join(pm, margins_join(in.pm, margin_of(mb)));
        if (b->marker != NULL)
            place_marker(l, styles, b, cx, b->y);
        if (l->failed)
            b->unfinished = true;
        else if (!sized_later(b))
            positioned_done(l, styles, b, cbw);
        return;
    }
    if (top_open) {
        y_border = in.first >= 0 ? in.first : cur->y + margins_sum(pm);
        content_top = y_border;
    }
    int64_t flowed = bottom_open ? in.y - content_top : in.y + margins_sum(in.pm) - content_top;
    // A flex or grid container's content height is what its items ask,
    // whatever height it was given and gave them.
    b->content_h = vframe_px + max64(0, asked >= 0 ? asked : flowed);
    int64_t content_h;
    Margins pm_out;
    if (!height_auto) {
        content_h = b->abs_h_set ? b->abs_h : content_of(s, s->height, 0, vframe_px);
        pm_out = margin_of(mb);
    } else if (bottom_open) {
        content_h = in.y - content_top;
        pm_out = margins_join(in.pm, margin_of(mb));
    } else {
        content_h = in.y + margins_sum(in.pm) - content_top;
        pm_out = margin_of(mb);
    }
    // Content taller than a max-height is drawn past it.
    content_h = clamp_height(s, max64(0, content_h), vframe_px);
    b->placed = true;
    b->y = y_border;
    b->h = b->border[FLOW_TOP] + b->padding[FLOW_TOP] + content_h + b->padding[FLOW_BOTTOM] +
           b->border[FLOW_BOTTOM];
    cur->y = b->y + b->h;
    cur->pm = pm_out;
    if (cur->first < 0)
        cur->first = y_border;
    if (b->marker != NULL)
        place_marker(l, styles, b, cx, y_border + b->border[FLOW_TOP] + b->padding[FLOW_TOP]);
    // The marker is the last of a box's own work: a layout that stopped
    // there stopped inside this box.
    if (l->failed)
        b->unfinished = true;
    else if (!sized_later(b))
        positioned_done(l, styles, b, cbw);
}

// ── Positioned layout (POSITION.md) ─────────────────────────────────────
//
// An ABSOLUTE box is laid out once its containing block is finished — its
// height is part of the equations — as a block formatting context of its
// own, in document coordinates like everything else, where the tree walks
// that follow never visit it. A RELATIVE box is laid out at its offset from
// where the flow puts it (block, above), or, a table cell, moved there once
// its table has placed it; a relative inline's pieces and content move as
// their lines are placed (lines(), above).

typedef struct {
    int64_t x, y, w, h;
} Rect;

// A box's padding box: what it is the containing block of (CSS 2.1 § 10.1).
static Rect padding_rect(const FBox *b)
{
    return (Rect){b->x + b->border[FLOW_LEFT], b->y + b->border[FLOW_TOP],
                  max64(0, b->w - b->border[FLOW_LEFT] - b->border[FLOW_RIGHT]),
                  max64(0, b->h - b->border[FLOW_TOP] - b->border[FLOW_BOTTOM])};
}

// A positioned inline's: the box bounding the padding boxes of its first
// and last pieces (CSS 2.1 § 10.1, 4.1) — its left padding opens the first,
// its right padding closes the last — once its home has placed them all.
// One with no piece at all is its home's padding box.
static Rect inline_rect(const FPos *p)
{
    const FSpan *f = p->inl->first_span, *e = p->inl->last_span;
    Rect home = padding_rect(p->home);
    if (f == NULL)
        return home;
    const flow_style_t *s = p->style;
    int64_t pl = len(s->padding[FLOW_LEFT], home.w), pr = len(s->padding[FLOW_RIGHT], home.w);
    int64_t pt = len(s->padding[FLOW_TOP], home.w), pb = len(s->padding[FLOW_BOTTOM], home.w);
    int64_t x0 = min64(f->x0 - pl, e->x0), x1 = max64(f->x1, e->x1 + pr);
    int64_t y0 = min64(f->top, e->top) - pt, y1 = max64(f->bottom, e->bottom) + pb;
    return (Rect){x0, y0, max64(0, x1 - x0), max64(0, y1 - y0)};
}

// A width that shrinks to fit (CSS 2.1 § 10.3.5's formula, CSS Sizing 3 §
// 3.2's fit-content): the content's min-content width at least, its
// max-content at most, and between them the room `avail` leaves.
static int64_t shrink_to_fit(L *l, FBox *b, int64_t avail)
{
    Intr in = intrinsic(l, b);
    int64_t f0 = hframe(b, 0);
    return max64(0, min64(max64(in.min - f0, avail), in.max - f0));
}

// `width: fit-content` in the flow: the room its containing block leaves
// after its margins that are set and its frame, then its limits.
static int64_t fit_content(L *l, FBox *b, int64_t cbw)
{
    const flow_style_t *s = b->style;
    int64_t frame = hframe(b, cbw);
    int64_t margins = len(s->margin[FLOW_LEFT], cbw) + len(s->margin[FLOW_RIGHT], cbw);
    return clamp_width(s, shrink_to_fit(l, b, cbw - margins - frame), cbw, frame);
}

// One axis of an absolute box's equations: the two insets and the two
// margins along it (0 where auto), which of them are auto, and the
// containing block's length.
typedef struct {
    bool auto0, auto1, mauto0, mauto1;
    int64_t in0, in1, m0, m1;
    int64_t cb;
} Axis;

// What an auto size comes to in the room it is given: shrink-to-fit across,
// the laid-out content's height down.
typedef struct {
    L *l;
    FBox *b;
    int64_t content_h;
} Fit;

static int64_t fit_size(const Fit *f, bool across, int64_t room)
{
    return across ? shrink_to_fit(f->l, f->b, room) : f->content_h;
}

// CSS 2.1 § 10.3.7 across and § 10.6.4 down, which are the same equation
// — inset + margin + frame + size + margin + inset = the containing block
// — with the same cases: the size given or auto (-1), `stat` the static
// position's offset from the containing block's start. Answers the border
// edge's offset from that start, and the used size and start margin.
static int64_t solve_axis(Axis ax, int64_t stat, int64_t frame, int64_t size, const Fit *fit,
                          bool across, int64_t *size_out, int64_t *m0_out)
{
    int64_t in0 = ax.in0, in1 = ax.in1, m0 = ax.m0, m1 = ax.m1;
    if (ax.auto0 && ax.auto1 && size < 0) {
        // All three auto: from the static position, as big as it fits.
        in0 = stat;
        size = fit_size(fit, across, ax.cb - in0 - m0 - m1 - frame);
    } else if (!ax.auto0 && !ax.auto1 && size >= 0) {
        // None auto: auto margins share what is left — across, a start
        // margin never below zero; over-constrained, the end inset gives.
        int64_t rem = ax.cb - in0 - in1 - m0 - m1 - frame - size;
        if (ax.mauto0 && ax.mauto1) {
            m0 = across && rem < 0 ? 0 : rem / 2;
            m1 = rem - m0;
        } else if (ax.mauto0) {
            m0 = rem;
        } else if (ax.mauto1) {
            m1 = rem;
        }
    } else if (ax.auto0 && size < 0) {
        size = fit_size(fit, across, ax.cb - in1 - m0 - m1 - frame);
        in0 = ax.cb - in1 - m1 - frame - size - m0;
    } else if (ax.auto0 && ax.auto1) {
        in0 = stat;
    } else if (size < 0 && ax.auto1) {
        size = fit_size(fit, across, ax.cb - in0 - m0 - m1 - frame);
    } else if (ax.auto0) {
        in0 = ax.cb - in1 - m1 - frame - size - m0;
    } else if (size < 0) {
        size = max64(0, ax.cb - in0 - in1 - m0 - m1 - frame);
    }
    *size_out = size;
    *m0_out = m0;
    return in0 + m0;
}

static Axis axis_of(const flow_style_t *s, int side0, int side1, int64_t cb, int64_t pct_base)
{
    Axis ax = {.cb = cb};
    ax.auto0 = s->inset[side0].kind == FLOW_LENGTH_AUTO;
    ax.auto1 = s->inset[side1].kind == FLOW_LENGTH_AUTO;
    ax.in0 = len(s->inset[side0], cb);
    ax.in1 = len(s->inset[side1], cb);
    ax.mauto0 = s->margin[side0].kind == FLOW_LENGTH_AUTO;
    ax.mauto1 = s->margin[side1].kind == FLOW_LENGTH_AUTO;
    // Every margin's percentage is of the containing block's WIDTH.
    ax.m0 = len(s->margin[side0], pct_base);
    ax.m1 = len(s->margin[side1], pct_base);
    return ax;
}

// A table is laid out as the flow would lay it out in the room its insets
// leave, then moved to where they put it; its captions move with it.
static void absolute_table(L *l, const FStyles *styles, FBox *t, Rect cb, int64_t sx, int64_t sy)
{
    const flow_style_t *s = t->style;
    Axis h = axis_of(s, FLOW_LEFT, FLOW_RIGHT, cb.w, cb.w);
    Axis v = axis_of(s, FLOW_TOP, FLOW_BOTTOM, cb.h, cb.w);
    int64_t room = max64(0, cb.w - (h.auto0 ? 0 : h.in0) - (h.auto1 ? 0 : h.in1));
    Cursor c = {0, kNoMargins, -1};
    table(l, styles, t, 0, room, &c);
    if (l->failed)
        return;
    // table() gave it its left margin inside the room: t->x.
    int64_t x = !h.auto0 ? cb.x + h.in0 + t->x
              : !h.auto1 ? cb.x + cb.w - h.in1 - h.m1 - t->w
              : sx + h.m0;
    int64_t y = !v.auto0 ? cb.y + v.in0 + v.m0
              : !v.auto1 ? cb.y + cb.h - v.in1 - v.m1 - t->h
              : sy + v.m0;
    translate(t, x - t->x, y - t->y);
}

// An out-of-flow box — absolute, or fixed, whose containing block is the
// viewport — against its containing block's padding box `cb`: the
// width first (§ 10.3.8 for a replaced box, whose width is its own), held
// to its limits and solved again with the one it is held to (§ 10.4); the
// height when nothing about it waits on the content; then the box laid
// out at that width; then the vertical equation with the height it came to.
// A box with no static position — its line was dropped — is not placed.
static void absolute(L *l, const FStyles *styles, FBox *a, Rect cb)
{
    int64_t sx, sy;
    if (a->place != NULL) {
        sx = a->place->x;
        sy = a->place->y;
    } else if (a->static_known) {
        sx = a->x;
        sy = a->y;
    } else {
        return;
    }
    if (a->kind == FB_TABLE) {
        absolute_table(l, styles, a, cb, sx, sy);
        return;
    }
    const flow_style_t *s = a->style;
    Axis h = axis_of(s, FLOW_LEFT, FLOW_RIGHT, cb.w, cb.w);
    Axis v = axis_of(s, FLOW_TOP, FLOW_BOTTOM, cb.h, cb.w);
    int64_t hf = hframe(a, cb.w), vf = vframe(a, cb.w);
    Fit fit = {l, a, 0};
    bool replaced = a->kind == FB_REPLACED;
    Replaced rep = {0, 0, false, false};
    int64_t w = -1, hgt = -1;
    if (replaced) {
        rep = replaced_size(l, a->node, s, cb.w);
        w = rep.w;
        hgt = rep.h;
    } else if (width_given(s)) {
        w = content_of(s, s->width, cb.w, hf);
    } else if (s->width.kind == FLOW_LENGTH_FIT_CONTENT) {
        w = shrink_to_fit(l, a, cb.w - (h.auto0 ? 0 : h.in0) - (h.auto1 ? 0 : h.in1) - h.m0 -
                                    h.m1 - hf);
    }
    int64_t used_w, ml;
    int64_t x = solve_axis(h, sx - cb.x, hf, w, &fit, true, &used_w, &ml);
    if (!replaced) {
        int64_t held = clamp_width(s, used_w, cb.w, hf);
        if (held != used_w)
            x = solve_axis(h, sx - cb.x, hf, held, &fit, true, &used_w, &ml);
        // A percentage height resolves here: the containing block's is known.
        if (s->height.kind == FLOW_LENGTH_PX || s->height.kind == FLOW_LENGTH_PERCENT)
            hgt = content_of(s, s->height, cb.h, vf);
        else if (!v.auto0 && !v.auto1)
            hgt = max64(0, cb.h - v.in0 - v.in1 - v.m0 - v.m1 - vf);
        if (hgt >= 0) {
            hgt = clamp_height(s, hgt, vf);
            a->abs_h_set = true;
            a->abs_h = hgt;
        }
        a->abs_w = used_w;
    }
    a->abs_sized = true;
    a->abs_ml = ml;
    Cursor c = {0, kNoMargins, -1};
    block(l, styles, a, cb.x + x - ml, cb.w, &c);
    if (l->failed || !a->placed)
        return;
    fit.content_h = a->h - vf;
    int64_t used_h, mt;
    int64_t y = solve_axis(v, sy - cb.y, vf, hgt >= 0 ? a->h - vf : -1, &fit, false, &used_h, &mt);
    translate(a, 0, cb.y + y - a->y);
}

// The out-of-flow boxes filed under one containing block, in tree order. ALL
// OR NOTHING: one whose layout stops is not placed, and its containing
// block — `owner`, the box whose work just finished — is not finished.
static void absolutes(L *l, const FStyles *styles, FPos *first, Rect cb, FBox *owner)
{
    for (FPos *a = first; a != NULL && !l->failed; a = a->abs_next) {
        absolute(l, styles, a->box, cb);
        if (l->failed) {
            unplace(a->box);
            owner->unfinished = true;
        }
    }
}

// A box's own work is done, so what it is the containing block of can be
// laid out: the out-of-flow boxes of the positioned inlines whose pieces are
// all in it, then its own. A relative CELL, which its table placed, then
// moves with all of that inside it; any other relative box was laid out
// where it moved to (block). `cbw` is its containing block's width, what
// a percentage offset is of.
static void positioned_done(L *l, const FStyles *styles, FBox *b, int64_t cbw)
{
    for (FPos *p = b->homed; p != NULL && !l->failed; p = p->home_next)
        absolutes(l, styles, p->abs_first, inline_rect(p), b);
    if (b->pos != NULL && !l->failed)
        absolutes(l, styles, b->pos->abs_first, padding_rect(b), b);
    if (b->kind == FB_CELL && b->positioned && !l->failed) {
        int64_t dx, dy;
        rel_offset(b->style, cbw, definite_height(b->parent), &dx, &dy);
        translate(b, dx, dy);
    }
}

// ── Tables (CSS 2.1 §17.5, the automatic layout of §17.5.2.2) ────────────
//
// MEASURE, THEN PLACE — Netscape 1.1's second pass. Every cell says how
// narrow it can be (the widest thing in it that cannot break) and how wide
// it wants to be (its content on one line); the columns take those, the
// table picks its width from them, the columns share the width, and only
// then is each cell laid out, once, at the width it got. A cell's two
// widths are found by reading its content, not by laying it out, and kept
// on the box, so a table nested forty deep costs each cell a constant
// number of measurements and not three to the power of its depth.

static Intr table_intrinsic(L *l, FBox *t);

static int64_t hframe(const FBox *b, int64_t base)
{
    const flow_style_t *s = b->style;
    return s->border_width[FLOW_LEFT] + s->border_width[FLOW_RIGHT] +
           len(s->padding[FLOW_LEFT], base) + len(s->padding[FLOW_RIGHT], base);
}

static int64_t vframe(const FBox *b, int64_t base)
{
    const flow_style_t *s = b->style;
    return s->border_width[FLOW_TOP] + s->border_width[FLOW_BOTTOM] +
           len(s->padding[FLOW_TOP], base) + len(s->padding[FLOW_BOTTOM], base);
}

// An inline formatting context's two widths: its widest word — a run of
// segments with no opportunity between them, across nodes, spaces inside
// the run included — and its widest line when only forced breaks break it.
// A trailing space counts in neither, and a tab is as wide as the way to
// the line's next stop.
static Intr ifc_intrinsic(L *l, FBox *ifc)
{
    Segs s = {0};
    segments(l, ifc, 0, &s);
    Intr r = {0, 0};
    // The first line's indent is part of its first word and of the line,
    // a percentage of a width not known yet counting as nothing.
    int64_t word = max64(0, first_line_indent(ifc, 0)), line = word, space = 0;
    for (size_t i = 0; i < s.n; i++) {
        const Seg *g = &s.v[i];
        switch (g->kind) {
        case SG_WORD:
        case SG_ATOM:
        case SG_MARKER: {
            // An atom with content of its own is measured from it: segments
            // made at no width give it none, and it lays out as wide as the
            // line it is given, so what it can be and wants to be are its
            // content's, with its margins round them.
            int64_t wmin = g->w, wmax = g->w;
            if (g->kind == SG_ATOM && g->item->content != NULL) {
                Intr ci = intrinsic(l, g->item->content);
                wmin = g->ml + ci.min + g->mr;
                wmax = g->ml + ci.max + g->mr;
            }
            word += wmin;
            if (wmax > 0 || g->kind != SG_WORD) {
                line += space + wmax;
                space = 0;
            }
            if (g->kind == SG_WORD && g->s1 > g->b1) {
                int64_t sw = spaces_width(g, line + space);
                // Spaces no opportunity follows are inside the word (a
                // quirks-mode row of slices and the whitespace between).
                if (!g->wrap_after)
                    word += sw;
                if (g->collapsible || g->hang)
                    space += sw;
                else
                    line += sw;
            }
            break;
        }
        case SG_BREAK:
            r.max = max64(r.max, line);
            line = space = 0;
            break;
        default:
            break;
        }
        if (g->wrap_after || g->kind == SG_BREAK) {
            r.min = max64(r.min, word);
            word = 0;
        }
    }
    r.min = max64(r.min, word);
    r.max = max64(r.max, max64(line, r.min));
    os64_free(s.v);
    return r;
}

// A box's border-box min-content and max-content widths (percentages of a
// containing block that is not known yet count as nothing, as CSS says).
static Intr intrinsic(L *l, FBox *b)
{
    if (b->intrinsic_known)
        return (Intr){b->intrinsic_min, b->intrinsic_max};
    Intr r = {0, 0};
    const flow_style_t *s = b->style;
    b->intrinsic_computed++;
    if (b->kind == FB_TABLE) {
        r = table_intrinsic(l, b);
        b->content_min = r.min;
        b->content_max = r.max;
    } else if (b->kind == FB_REPLACED) {
        Replaced rep = replaced_size(l, b->node, s, 0);
        r.min = r.max = rep.w + hframe(b, 0);
        b->content_min = b->content_max = replaced_own(l, b, 0, 0, -1) + hframe(b, 0);
    } else {
        if (b->flex && s->flex_direction != FLOW_FLEX_COLUMN &&
            s->flex_direction != FLOW_FLEX_COLUMN_REVERSE) {
            // A row's (FLEX.md, decision 3): its items side by side and
            // the gaps between, and at least its widest item's min-content
            // when it wraps, all of them side by side when it does not. An
            // item that cannot shrink asks at least its flex basis, when
            // that is a length; one that can asks its content, as Chrome
            // measures it (FLEX.md, decision 3).
            int32_t n = 0;
            bool wraps = s->flex_wrap != FLOW_FLEX_NOWRAP;
            for (FBox *c = b->first; c != NULL && !l->failed; c = c->next) {
                if (c->out_of_flow)
                    continue;
                Intr ci = intrinsic(l, c);
                const flow_style_t *cs = c->style;
                if (cs->flex_shrink == 0 && cs->flex_basis.kind == FLOW_LENGTH_PX) {
                    // The basis as the item will be: held to its limits.
                    int64_t frame = hframe(c, 0);
                    int64_t basis = content_of(cs, cs->flex_basis, 0, frame) + frame;
                    if (cs->max_width.kind == FLOW_LENGTH_PX)
                        basis = min64(basis, content_of(cs, cs->max_width, 0, frame) + frame);
                    if (cs->min_width.kind == FLOW_LENGTH_PX)
                        basis = max64(basis, content_of(cs, cs->min_width, 0, frame) + frame);
                    ci.min = max64(ci.min, basis);
                    ci.max = max64(ci.max, basis);
                }
                int64_t m = len(c->style->margin[FLOW_LEFT], 0) + len(c->style->margin[FLOW_RIGHT], 0);
                r.max += ci.max + m;
                r.min = wraps ? max64(r.min, ci.min + m) : r.min + ci.min + m;
                n++;
            }
            int64_t gap = s->column_gap.kind == FLOW_LENGTH_PX ? len(s->column_gap, 0) : 0;
            if (n > 1) {
                r.max += gap * (n - 1);
                if (!wraps)
                    r.min += gap * (n - 1);
            }
        } else if (b->grid) {
            r = grid_intrinsic(l, b);
        } else if (b->ifc) {
            r = ifc_intrinsic(l, b);
        } else {
            for (FBox *c = b->first; c != NULL && !l->failed; c = c->next) {
                if (c->out_of_flow)
                    continue;
                Intr ci = intrinsic(l, c);
                int64_t m = len(c->style->margin[FLOW_LEFT], 0) + len(c->style->margin[FLOW_RIGHT], 0);
                r.min = max64(r.min, ci.min + m);
                r.max = max64(r.max, ci.max + m);
            }
        }
        // A width the page set is both, for anything but a cell, whose
        // width is its column's business; so is a limit in pixels (a
        // percentage of a width not known yet binds nothing).
        int64_t frame = hframe(b, 0);
        b->content_min = r.min + frame;
        b->content_max = r.max + frame;
        if (b->kind != FB_CELL) {
            if (s->width.kind == FLOW_LENGTH_PX)
                r.min = r.max = content_of(s, s->width, 0, frame);
            if (s->max_width.kind == FLOW_LENGTH_PX) {
                r.min = min64(r.min, content_of(s, s->max_width, 0, frame));
                r.max = min64(r.max, content_of(s, s->max_width, 0, frame));
            }
            if (s->min_width.kind == FLOW_LENGTH_PX) {
                r.min = max64(r.min, content_of(s, s->min_width, 0, frame));
                r.max = max64(r.max, content_of(s, s->min_width, 0, frame));
            }
        }
        r.min += frame;
        r.max += frame;
    }
    b->intrinsic_known = true;
    b->intrinsic_min = r.min;
    b->intrinsic_max = r.max;
    return r;
}

// ── The grid ────────────────────────────────────────────────────────────

// HTML's own limits on a cell's reach (the standard's clamps), and a limit
// of our own on how many columns a grid may have, so a page of spans
// costs what its cells cost: a cell that would start past it is not laid
// out.
#define COLSPAN_MAX 1000
#define ROWSPAN_MAX 65534
#define COLUMNS_MAX 10000

typedef struct {
    FBox *cell;
    int32_t row, col, rows, cols;
} GCell;

typedef struct {
    FBox *box;
    int32_t group_end;      // the first row past this row's group
} GRow;

typedef struct {
    GRow *rows;
    int32_t nrows, cap_rows;
    GCell *cells;
    int32_t ncells, cap_cells;
    int32_t ncols;
    // The rowspans' holds: per column, the first row a hold no longer
    // reaches (0: never held), as the leaves of a min-tree, so the next free
    // slot is found in log(columns) and not by walking every held column —
    // a page of long holds over wide columns would otherwise cost rows x
    // columns. As many leaves as the columns holds reach, to a power of two,
    // grown as they reach further: a page of small tables pays for small
    // trees. NULL until the table's first rowspan.
    int64_t *holds;
    int32_t hold_leaves;
} Grid;

// The first column from `c` that no hold reaches on row `r`.
static int32_t hold_free(const int64_t *t, int32_t node, int32_t lo, int32_t hi, int32_t c,
                         int64_t r)
{
    if (hi <= c || t[node] > r)
        return -1;
    if (hi - lo == 1)
        return lo;
    int32_t mid = lo + (hi - lo) / 2;
    int32_t k = hold_free(t, 2 * node, lo, mid, c, r);
    return k >= 0 ? k : hold_free(t, 2 * node + 1, mid, hi, c, r);
}

static int32_t next_free(const Grid *g, int32_t c, int64_t r)
{
    if (g->holds == NULL || c >= g->hold_leaves)
        return c;
    int32_t k = hold_free(g->holds, 1, 0, g->hold_leaves, c, r);
    return k >= 0 ? k : g->hold_leaves;
}

// Room in the tree for column `k`: the leaves doubled until it has one, the
// old leaves kept and the levels above them made again.
static bool hold_room(L *l, Grid *g, int32_t k)
{
    if (g->holds != NULL && k < g->hold_leaves)
        return true;
    int32_t n = g->hold_leaves > 0 ? g->hold_leaves : 16;
    while (n <= k)
        n *= 2;
    int64_t *t = scratch_calloc(l, 2 * (size_t)n, sizeof(int64_t));
    if (t == NULL) {
        fail(l);
        return false;
    }
    for (int32_t j = 0; j < g->hold_leaves; j++)
        t[n + j] = g->holds[g->hold_leaves + j];
    for (int32_t at = n - 1; at >= 1; at--)
        t[at] = t[2 * at] < t[2 * at + 1] ? t[2 * at] : t[2 * at + 1];
    scratch_free(l, g->holds);
    g->holds = t;
    g->hold_leaves = n;
    return true;
}

// Column `k` held until row `until`, if that is longer than it was.
static void hold_until(Grid *g, int32_t k, int64_t until)
{
    int64_t *t = g->holds;
    int32_t at = g->hold_leaves + k;
    if (until <= t[at])
        return;
    t[at] = until;
    for (at /= 2; at >= 1; at /= 2) {
        int64_t m = t[2 * at] < t[2 * at + 1] ? t[2 * at] : t[2 * at + 1];
        if (t[at] == m)
            break;
        t[at] = m;
    }
}

static bool grow(L *l, void **v, int32_t *cap, int32_t want, size_t size)
{
    if (want <= *cap)
        return true;
    int32_t cap2 = *cap != 0 ? *cap : 16;
    while (cap2 < want)
        cap2 *= 2;
    void *p = scratch_realloc(l, *v, (size_t)cap2 * size);
    if (p == NULL) {
        fail(l);
        return false;
    }
    os64_memset((char *)p + (size_t)*cap * size, 0, (size_t)(cap2 - *cap) * size);
    *v = p;
    *cap = cap2;
    return true;
}

static int32_t span_attr(const FBox *cell, const char *name, int32_t max, int32_t zero)
{
    if (cell->node == NULL)
        return 1;
    const os64_html_attr_t *a = os64_html_attr(cell->node, name);
    int32_t v;
    if (a == NULL || !f_parse_nonnegative(a->value, &v))
        return 1;
    if (v == 0)
        return zero;
    return v > max ? max : v;
}

static void add_rows(L *l, Grid *g, FBox *group, FBox *first_row_alone)
{
    int32_t start = g->nrows;
    for (FBox *r = group != NULL ? group->first : first_row_alone; r != NULL; r = r->next) {
        if (group == NULL && r->kind != FB_ROW)
            break;
        if (r->kind != FB_ROW)
            continue;
        if (!grow(l, (void **)&g->rows, &g->cap_rows, g->nrows + 1, sizeof(GRow)))
            return;
        g->rows[g->nrows++] = (GRow){r, 0};
    }
    for (int32_t i = start; i < g->nrows; i++)
        g->rows[i].group_end = g->nrows;
}

// The rows in the order they are drawn — the first header group first,
// the first footer group last, every other group and every row that has
// none in between — and every cell given its slot.
static bool grid_build(L *l, FBox *t, Grid *g)
{
    os64_memset(g, 0, sizeof(*g));
    FBox *head, *foot;
    table_ends(t, &head, &foot);
    if (head != NULL)
        add_rows(l, g, head, NULL);
    for (FBox *c = t->first; c != NULL && !l->failed; c = c->next) {
        if (c->kind == FB_ROW_GROUP && c != head && c != foot) {
            add_rows(l, g, c, NULL);
        } else if (c->kind == FB_ROW) {
            // A run of rows with no group of their own is one group.
            add_rows(l, g, NULL, c);
            while (c->next != NULL && c->next->kind == FB_ROW)
                c = c->next;
        }
    }
    if (foot != NULL)
        add_rows(l, g, foot, NULL);
    if (l->failed)
        return false;

    for (int32_t r = 0; r < g->nrows && !l->failed; r++) {
        int32_t c = 0;
        for (FBox *cell = g->rows[r].box->first; cell != NULL; cell = cell->next) {
            if (cell->kind != FB_CELL)
                continue;
            c = next_free(g, c, r);
            int32_t cs = span_attr(cell, "colspan", COLSPAN_MAX, 1);
            int32_t rs = span_attr(cell, "rowspan", ROWSPAN_MAX, g->rows[r].group_end - r);
            if (r + rs > g->rows[r].group_end)
                rs = g->rows[r].group_end - r;
            if (c >= COLUMNS_MAX)
                continue;
            if (c + cs > COLUMNS_MAX)
                cs = COLUMNS_MAX - c;
            if (!grow(l, (void **)&g->cells, &g->cap_cells, g->ncells + 1, sizeof(GCell)))
                return false;
            if (rs > 1 && !hold_room(l, g, c + cs - 1))
                return false;
            g->cells[g->ncells++] = (GCell){cell, r, c, rs, cs};
            // A hold only ever lengthens: a cell whose span crosses a
            // longer hold leaves it as it was.
            if (rs > 1)
                for (int32_t k = c; k < c + cs; k++)
                    hold_until(g, k, r + rs);
            c += cs;
            if (c > g->ncols)
                g->ncols = c;
        }
    }
    // Columns the table declares count even where no cell reaches.
    int32_t declared = 0;
    for (FBox *c = t->first; c != NULL; c = c->next) {
        if (c->kind == FB_COLUMN)
            declared += span_attr(c, "span", COLSPAN_MAX, 1);
        if (c->kind == FB_COLUMN_GROUP) {
            int32_t in = 0;
            for (FBox *k = c->first; k != NULL; k = k->next)
                in += span_attr(k, "span", COLSPAN_MAX, 1);
            declared += in > 0 ? in : span_attr(c, "span", COLSPAN_MAX, 1);
        }
    }
    if (declared > COLUMNS_MAX)
        declared = COLUMNS_MAX;
    if (declared > g->ncols)
        g->ncols = declared;
    return !l->failed;
}

static void grid_free(L *l, Grid *g)
{
    scratch_free(l, g->rows);
    scratch_free(l, g->cells);
    scratch_free(l, g->holds);
}

// ── Columns ─────────────────────────────────────────────────────────────

typedef struct {
    int64_t min, max;
    int64_t fixed;          // a width the page gave, or -1
    int64_t pct;            // a percentage the page gave, in 1/64ths, or -1 (a span's share)
    int64_t width, x;
} Col;

static int64_t spacing_h(const FBox *t)
{
    return t->style->border_collapse == FLOW_COLLAPSE_BORDERS ? 0 : t->style->border_spacing[0];
}

static int64_t spacing_v(const FBox *t)
{
    return t->style->border_collapse == FLOW_COLLAPSE_BORDERS ? 0 : t->style->border_spacing[1];
}

// Spread `want` over columns [c, c+n) beyond what they already hold, in
// proportion to what each wants at most — Netscape's rule, and every
// engine's since — or evenly when none wants anything.
static void spread(Col *cols, int32_t c, int32_t n, int64_t want, bool to_max)
{
    int64_t have = 0, weight = 0;
    for (int32_t k = c; k < c + n; k++) {
        have += to_max ? cols[k].max : cols[k].min;
        weight += cols[k].max;
    }
    if (want <= have)
        return;
    int64_t extra = want - have, given = 0;
    for (int32_t k = c; k < c + n; k++) {
        int64_t share = weight > 0 ? mul_div(extra, cols[k].max, weight) : extra / n;
        if (k == c + n - 1)
            share = extra - given;
        given += share;
        if (to_max)
            cols[k].max += share;
        else
            cols[k].min += share;
        if (cols[k].max < cols[k].min)
            cols[k].max = cols[k].min;
    }
}

// A spanning cell's percentage over its columns: what the columns that
// have percentages already hold counts toward it, and the rest goes to the
// ones that have none, in proportion to what they want at most, or evenly
// (the automatic table layout's rule for spans, as for pixels).
static void spread_pct(Col *cols, int32_t c, int32_t n, int64_t pct)
{
    int64_t have = 0, weight = 0;
    int32_t free_cols = 0;
    for (int32_t k = c; k < c + n; k++) {
        if (cols[k].pct >= 0) {
            have += cols[k].pct;
        } else {
            weight += cols[k].max;
            free_cols++;
        }
    }
    if (pct <= have || free_cols == 0)
        return;
    int64_t extra = pct - have, given = 0;
    int32_t seen = 0;
    for (int32_t k = c; k < c + n; k++) {
        if (cols[k].pct >= 0)
            continue;
        int64_t share = weight > 0 ? mul_div(extra, cols[k].max, weight) : extra / free_cols;
        if (++seen == free_cols)
            share = extra - given;
        given += share;
        cols[k].pct = share;
    }
}

// The columns' widths as constraints: one-column cells and `col` widths
// first, then spanning cells, narrowest span first.
static Col *columns_of(L *l, FBox *t, Grid *g, int64_t hsp)
{
    Col *cols = scratch_calloc(l, (size_t)(g->ncols > 0 ? g->ncols : 1), sizeof(Col));
    if (cols == NULL) {
        fail(l);
        return NULL;
    }
    for (int32_t k = 0; k < g->ncols; k++)
        cols[k].fixed = cols[k].pct = -1;
    // A `col` sets the width of the columns it spans, in pixels or as a
    // percentage; one with no width takes its group's, and a group with no
    // `col` in it sets its own `span` of columns (HTML 4's default width).
    int32_t k = 0;
    for (FBox *c = t->first; c != NULL && k < g->ncols; c = c->next) {
        if (c->kind != FB_COLUMN && c->kind != FB_COLUMN_GROUP)
            continue;
        bool lone_group = c->kind == FB_COLUMN_GROUP && c->first == NULL;
        for (FBox *col = c->kind == FB_COLUMN || lone_group ? c : c->first; col != NULL;
             col = c->kind == FB_COLUMN || lone_group ? NULL : col->next) {
            int32_t n = span_attr(col, "span", COLSPAN_MAX, 1);
            flow_length_t w = col->style->width;
            if (w.kind == FLOW_LENGTH_AUTO && col != c)
                w = c->style->width;
            for (int32_t j = 0; j < n && k < g->ncols; j++, k++) {
                if (w.kind == FLOW_LENGTH_PX)
                    cols[k].fixed = w.value;
                else if (w.kind == FLOW_LENGTH_PERCENT)
                    cols[k].pct = w.value;
            }
        }
    }

    GCell *spanning = NULL;
    int32_t nspanning = 0;
    for (int32_t i = 0; i < g->ncells && !l->failed; i++) {
        GCell *gc = &g->cells[i];
        Intr ci = intrinsic(l, gc->cell);
        const flow_style_t *s = gc->cell->style;
        int64_t frame = hframe(gc->cell, 0);
        int64_t fixed = s->width.kind == FLOW_LENGTH_PX ? s->width.value + frame : -1;
        // A set width is what the cell wants at most, never less than it
        // can be; in quirks mode a nowrap cell's set width is also what it
        // can be at least (the Quirks standard's 3.9).
        int64_t cmin = ci.min, cmax = ci.max;
        if (fixed >= 0) {
            cmax = max64(cmin, fixed);
            if (l->quirks && os64_html_attr(gc->cell->node, "nowrap") != NULL)
                cmin = cmax;
        }
        if (gc->cols == 1) {
            Col *col = &cols[gc->col];
            col->min = max64(col->min, cmin);
            col->max = max64(col->max, cmax);
            if (fixed >= 0)
                col->fixed = max64(col->fixed, fixed);
            // The percentage only: a column has no room for a calc()'s
            // fixed part (GARB.md § Booked).
            if (s->width.kind == FLOW_LENGTH_PERCENT)
                col->pct = max64(col->pct, s->width.value);
        } else {
            if (spanning == NULL) {
                spanning = scratch_realloc(l, NULL, (size_t)g->ncells * sizeof(GCell));
                if (spanning == NULL) {
                    fail(l);
                    break;
                }
            }
            spanning[nspanning++] = *gc;
        }
    }
    for (int32_t i = 0; i < g->ncols; i++) {
        if (cols[i].fixed >= 0)
            cols[i].max = max64(cols[i].min, max64(cols[i].max, cols[i].fixed));
        if (cols[i].max < cols[i].min)
            cols[i].max = cols[i].min;
    }
    // Narrowest span first, sorted by counting, with a count per span up to
    // the widest there is: the order costs the cells, not their square, and
    // a table's buckets are its own spans' (COLSPAN_MAX's for every table
    // was a page of small tables zeroing four kilobytes each).
    int32_t widest = 0;
    for (int32_t i = 0; i < nspanning; i++)
        widest = spanning[i].cols > widest ? spanning[i].cols : widest;
    int32_t *starts = nspanning > 0 ? scratch_calloc(l, (size_t)widest + 2, sizeof(int32_t)) : NULL;
    GCell *ordered = nspanning > 0 ? scratch_realloc(l, NULL, (size_t)nspanning * sizeof(GCell)) : NULL;
    if (nspanning > 0 && (starts == NULL || ordered == NULL))
        fail(l);
    if (ordered != NULL && starts != NULL) {
        for (int32_t i = 0; i < nspanning; i++)
            starts[spanning[i].cols + 1]++;
        for (int32_t v = 1; v <= widest + 1; v++)
            starts[v] += starts[v - 1];
        for (int32_t i = 0; i < nspanning; i++)
            ordered[starts[spanning[i].cols]++] = spanning[i];
    }
    for (int32_t i = 0; i < nspanning && ordered != NULL && !l->failed; i++) {
        GCell *gc = &ordered[i];
        Intr ci = intrinsic(l, gc->cell);
        const flow_style_t *s = gc->cell->style;
        int64_t gaps = (int64_t)(gc->cols - 1) * hsp;
        int64_t cmax = ci.max;
        if (s->width.kind == FLOW_LENGTH_PX)
            cmax = max64(ci.min, s->width.value + hframe(gc->cell, 0));
        spread(cols, gc->col, gc->cols, ci.min - gaps, false);
        spread(cols, gc->col, gc->cols, cmax - gaps, true);
        if (s->width.kind == FLOW_LENGTH_PERCENT)
            spread_pct(cols, gc->col, gc->cols, s->width.value);
    }
    scratch_free(l, starts);
    scratch_free(l, ordered);
    scratch_free(l, spanning);
    return cols;
}

static Intr grid_sums(const Col *cols, int32_t n, int64_t hsp)
{
    Intr r = {(int64_t)(n + 1) * hsp, (int64_t)(n + 1) * hsp};
    if (n == 0)
        r.min = r.max = 0;
    for (int32_t k = 0; k < n; k++) {
        r.min += cols[k].min;
        r.max += cols[k].max;
    }
    return r;
}

static Intr table_intrinsic(L *l, FBox *t)
{
    Intr r = {0, 0};
    Grid g;
    if (!grid_build(l, t, &g)) {
        grid_free(l, &g);
        return r;
    }
    int64_t hsp = spacing_h(t);
    Col *cols = columns_of(l, t, &g, hsp);
    if (cols != NULL) {
        r = grid_sums(cols, g.ncols, hsp);
        r.min += hframe(t, 0);
        r.max += hframe(t, 0);
        // `table { box-sizing: border-box }`: a set width is the border box.
        if (t->style->width.kind == FLOW_LENGTH_PX) {
            r.min = max64(r.min, t->style->width.value);
            r.max = r.min;
        }
        for (FBox *c = t->first; c != NULL; c = c->next)
            if (c->kind == FB_CAPTION) {
                Intr ci = intrinsic(l, c);
                r.min = max64(r.min, ci.min);
                r.max = max64(r.max, ci.min);
            }
    }
    scratch_free(l, cols);
    grid_free(l, &g);
    return r;
}

// Share the grid's width among the columns: a percentage column its share
// of it, a fixed column its width (each kind giving back toward its least
// when the others' least leaves it less room), and the rest from each auto
// column's least toward its most in proportion to how far apart the two
// are; what is left over goes to the auto columns, else to the others.
// Never below a column's least: a table overflows before it squashes a word.
static void distribute(Col *cols, int32_t n, int64_t w)
{
    if (n == 0)
        return;
    int64_t pct_total = 0;
    for (int32_t k = 0; k < n; k++)
        if (cols[k].pct >= 0)
            pct_total += cols[k].pct;
    int64_t taken = 0, amin = 0, amax = 0;
    int32_t nauto = 0;
    for (int32_t k = 0; k < n; k++) {
        Col *c = &cols[k];
        if (c->pct >= 0) {
            int64_t pct = pct_total > 100 * 64 ? c->pct * (100 * 64) / pct_total : c->pct;
            c->width = max64(c->min, mul_div(w, pct, 100 * 64));
            taken += c->width;
        } else if (c->fixed >= 0) {
            c->width = max64(c->min, c->fixed);
            taken += c->width;
        } else {
            amin += c->min;
            amax += c->max;
            nauto++;
        }
    }
    // The grid never outgrows the table, whatever kind its columns are
    // (an auto-width table takes its containing block's width when its
    // preferences do not fit it). Each kind is held in turn to the room the
    // others' least leaves it, giving back toward its own least in
    // proportion to what each column can give: the percentage columns first,
    // then the set widths. The table is never narrower than every column's
    // least together, so each kind always has its own least's room.
    int64_t pct_want = 0, pct_least = 0, fixed_want = 0, fixed_least = 0;
    for (int32_t k = 0; k < n; k++) {
        if (cols[k].pct >= 0) {
            pct_want += cols[k].width;
            pct_least += cols[k].min;
        } else if (cols[k].fixed >= 0) {
            fixed_want += cols[k].width;
            fixed_least += cols[k].min;
        }
    }
    int64_t pct_room = w - fixed_least - amin;
    if (pct_want > pct_room && pct_want > pct_least) {
        int64_t give = max64(0, pct_room - pct_least);
        for (int32_t k = 0; k < n; k++)
            if (cols[k].pct >= 0)
                cols[k].width = cols[k].min + mul_div(cols[k].width - cols[k].min, give,
                                                      pct_want - pct_least);
    }
    int64_t pct_taken = 0;
    for (int32_t k = 0; k < n; k++)
        if (cols[k].pct >= 0)
            pct_taken += cols[k].width;
    taken = pct_taken + fixed_want;
    int64_t room = w - pct_taken - amin;
    if (fixed_want > room && fixed_want > fixed_least) {
        int64_t give = max64(0, room - fixed_least);
        for (int32_t k = 0; k < n; k++)
            if (cols[k].pct < 0 && cols[k].fixed >= 0)
                cols[k].width = cols[k].min + mul_div(cols[k].width - cols[k].min, give,
                                                      fixed_want - fixed_least);
        taken = pct_taken;
        for (int32_t k = 0; k < n; k++)
            if (cols[k].pct < 0 && cols[k].fixed >= 0)
                taken += cols[k].width;
    }
    int64_t a = w - taken;
    if (nauto > 0) {
        for (int32_t k = 0; k < n; k++) {
            Col *c = &cols[k];
            if (c->pct >= 0 || c->fixed >= 0)
                continue;
            if (a >= amax)
                c->width = c->max;
            else if (a > amin && amax > amin)
                c->width = c->min + mul_div(c->max - c->min, a - amin, amax - amin);
            else
                c->width = c->min;
        }
    }
    int64_t sum = 0;
    for (int32_t k = 0; k < n; k++)
        sum += cols[k].width;
    int64_t extra = w - sum;
    if (extra <= 0)
        return;
    // What is left: to the auto columns in proportion to what they want,
    // else to all of them in proportion to their widths.
    int64_t weight = 0;
    int32_t last = -1;
    for (int32_t k = 0; k < n; k++)
        if (nauto == 0 || (cols[k].pct < 0 && cols[k].fixed < 0)) {
            weight += nauto > 0 ? cols[k].max : cols[k].width;
            last = k;
        }
    int64_t given = 0;
    int32_t eligible = 0;
    for (int32_t k = 0; k < n; k++)
        if (nauto == 0 || (cols[k].pct < 0 && cols[k].fixed < 0))
            eligible++;
    for (int32_t k = 0; k < n; k++) {
        if (!(nauto == 0 || (cols[k].pct < 0 && cols[k].fixed < 0)))
            continue;
        int64_t wk = nauto > 0 ? cols[k].max : cols[k].width;
        int64_t share = weight > 0 ? mul_div(extra, wk, weight) : extra / eligible;
        if (k == last)
            share = extra - given;
        given += share;
        cols[k].width += share;
    }
}

// ── Cells and rows ──────────────────────────────────────────────────────

static void translate_content(FBox *b, int64_t dy)
{
    for (FLine *ln = b->lines; ln != NULL; ln = ln->next) {
        ln->y += dy;
        ln->baseline += dy;
        for (FFrag *fr = ln->frags; fr != NULL; fr = fr->next) {
            fr->y += dy;
            fr->baseline += dy;
            if (fr->kind == FF_ATOMIC && fr->item->content != NULL)
                translate(fr->item->content, 0, dy);
        }
        for (FSpan *sp = ln->spans; sp != NULL; sp = sp->next) {
            sp->top += dy;
            sp->bottom += dy;
        }
    }
    for (FBox *c = b->first; c != NULL; c = c->next)
        translate(c, 0, dy);
}

// A cell laid out, once, at the width its columns give it, its top at 0:
// the height its content needs, border and padding included.
static int64_t cell_layout(L *l, const FStyles *styles, FBox *cell, int64_t x, int64_t w,
                           int64_t base)
{
    const flow_style_t *s = cell->style;
    for (int i = 0; i < 4; i++) {
        cell->border[i] = s->border_width[i];
        cell->padding[i] = len(s->padding[i], base);
    }
    int64_t cw = w - cell->border[FLOW_LEFT] - cell->border[FLOW_RIGHT] -
                 cell->padding[FLOW_LEFT] - cell->padding[FLOW_RIGHT];
    if (cw < 0)
        cw = 0;
    cell->x = x;
    cell->w = w;
    cell->y = 0;
    int64_t top = cell->border[FLOW_TOP] + cell->padding[FLOW_TOP];
    Cursor in = {top, kNoMargins, top};
    int64_t cx = x + cell->border[FLOW_LEFT] + cell->padding[FLOW_LEFT];
    if (cell->ifc)
        lines(l, styles, cell, cx, cw, &in);
    else
        children(l, styles, cell, cx, cw, &in);
    int64_t content = in.y + margins_sum(in.pm) - top;
    int64_t vf = vframe(cell, base);
    int64_t h = vf + (content > 0 ? content : 0);
    // A set height is a least height. In quirks mode it counts the border
    // and padding (the Quirks standard's 3.13); otherwise the content alone.
    if (s->height.kind == FLOW_LENGTH_PX)
        h = max64(h, l->quirks ? s->height.value : s->height.value + vf);
    cell->placed = true;
    cell->h = h;
    return h;
}

// A cell's baseline, from its top, as laid out before its row stretches
// it (CSS 2.1 § 17.5.3): its first line's, or where there is none, the
// bottom of its content edge.
static int64_t first_baseline(FBox *cell)
{
    int64_t first = 0;
    return first_baseline_in(cell, &first)
               ? first - cell->y
               : cell->h - cell->border[FLOW_BOTTOM] - cell->padding[FLOW_BOTTOM];
}

static void unplace(FBox *b)
{
    b->placed = false;
    for (FBox *c = b->first; c != NULL; c = c->next)
        unplace(c);
}

static void table_body(L *l, const FStyles *styles, FBox *t, int64_t cbx, int64_t cbw,
                       Cursor *cur);

static bool table_is_empty(const FBox *t)
{
    for (const FBox *c = t->first; c != NULL; c = c->next) {
        if (c->kind == FB_CAPTION || c->kind == FB_ROW)
            return false;
        if (c->kind == FB_ROW_GROUP)
            for (const FBox *r = c->first; r != NULL; r = r->next)
                if (r->kind == FB_ROW)
                    return false;
    }
    return true;
}

// The table: its width and its columns', its captions, its rows and their
// cells, placed in its containing block like any block. One whose build or
// layout stopped partway (memory, the budget, the depth) keeps its place,
// empty: its columns
// would come from only the cells it has, so no line in it breaks where the
// whole table's would, and none of it is real yet.
static void table(L *l, const FStyles *styles, FBox *t, int64_t cbx, int64_t cbw, Cursor *cur)
{
    // A table with no rows and no captions in quirks mode is nothing at all
    // (the Quirks standard's 3.10): no width, no height, no border, and the
    // flow goes on as if it were not there.
    if (l->quirks && table_is_empty(t)) {
        for (FBox *c = t->first; c != NULL; c = c->next)
            unplace(c);
        t->placed = true;
        t->x = cbx;
        t->y = cur->y;
        t->w = t->h = 0;
        for (int i = 0; i < 4; i++)
            t->border[i] = t->padding[i] = 0;
        return;
    }
    int64_t y = cur->y + margins_sum(margins_join(cur->pm,
                                                  margin_of(len(t->style->margin[FLOW_TOP], cbw))));
    if (t->unfinished) {
        if (cur->first < 0)
            cur->first = y;
        cur->y = y;
        cur->pm = kNoMargins;
    } else {
        table_body(l, styles, t, cbx, cbw, cur);
        if (!l->failed)
            positioned_done(l, styles, t, cbw);
        if (!l->failed)
            return;
    }
    for (FBox *c = t->first; c != NULL; c = c->next)
        unplace(c);
    t->placed = true;
    t->unfinished = true;
    if (t->w == 0)
        t->x = cbx;
    t->y = y;
    t->h = 0;
}

// Row heights under range adds and range sums: two Fenwick trees, in
// uint64_t because the second accumulates value x row terms that may pass
// int64_t on their way even when every sum asked for fits; the arithmetic
// is exact modulo 2^64, so a sum that fits comes back exact.
typedef struct {
    uint64_t *a, *b;
    int32_t n;
} RowSums;

static void rowsums_point(uint64_t *t, int32_t n, int32_t i, uint64_t v)
{
    for (; i <= n; i += i & -i)
        t[i] += v;
}

static uint64_t rowsums_prefix_of(const uint64_t *t, int32_t i)
{
    uint64_t v = 0;
    for (; i > 0; i -= i & -i)
        v += t[i];
    return v;
}

// Rows [r0, r1] each grow by v.
static void rowsums_add(RowSums *s, int32_t r0, int32_t r1, int64_t v)
{
    uint64_t u = (uint64_t)v;
    rowsums_point(s->a, s->n, r0 + 1, u);
    rowsums_point(s->a, s->n, r1 + 2, (uint64_t)0 - u);
    rowsums_point(s->b, s->n, r0 + 1, u * (uint64_t)r0);
    rowsums_point(s->b, s->n, r1 + 2, (uint64_t)0 - u * (uint64_t)(r1 + 1));
}

static uint64_t rowsums_prefix(const RowSums *s, int32_t count)
{
    return rowsums_prefix_of(s->a, count) * (uint64_t)count - rowsums_prefix_of(s->b, count);
}

// The heights of rows [r0, r1] together.
static int64_t rowsums_sum(const RowSums *s, int32_t r0, int32_t r1)
{
    return (int64_t)(rowsums_prefix(s, r1 + 1) - rowsums_prefix(s, r0));
}

static bool rowsums_init(L *l, RowSums *s, const int64_t *rh, int32_t n)
{
    s->n = n + 1;
    s->a = scratch_calloc(l, (size_t)n + 2, sizeof(uint64_t));
    s->b = scratch_calloc(l, (size_t)n + 2, sizeof(uint64_t));
    if (s->a == NULL || s->b == NULL) {
        fail(l);
        return false;
    }
    for (int32_t r = 0; r < n; r++)
        rowsums_add(s, r, r, rh[r]);
    return true;
}

static void table_body(L *l, const FStyles *styles, FBox *t, int64_t cbx, int64_t cbw,
                       Cursor *cur)
{
    const flow_style_t *s = t->style;
    int64_t hsp = spacing_h(t), vsp = spacing_v(t);
    Grid g;
    if (!grid_build(l, t, &g)) {
        grid_free(l, &g);
        return;
    }
    Col *cols = columns_of(l, t, &g, hsp);
    if (cols == NULL) {
        grid_free(l, &g);
        return;
    }
    Intr sums = grid_sums(cols, g.ncols, hsp);
    int64_t frame = hframe(t, cbw);
    int64_t tmin = sums.min + frame, tmax = sums.max + frame;
    int64_t margins = (s->margin[FLOW_LEFT].kind == FLOW_LENGTH_AUTO ? 0 : len(s->margin[FLOW_LEFT], cbw)) +
                      (s->margin[FLOW_RIGHT].kind == FLOW_LENGTH_AUTO ? 0 : len(s->margin[FLOW_RIGHT], cbw));
    int64_t used;
    if (width_given(s))
        used = max64(len(s->width, cbw), tmin);
    else
        used = max64(tmin, tmax < cbw - margins ? tmax : cbw - margins);
    for (FBox *c = t->first; c != NULL; c = c->next)
        if (c->kind == FB_CAPTION)
            used = max64(used, intrinsic(l, c).min);

    // Place the table box by the block width equation, with its width now
    // fixed: border-box, so the content width is what the frame leaves.
    int64_t ml;
    widths(t, cbw, used - frame, &ml);
    t->x = cbx + ml;
    t->w = used;
    int64_t mt = len(s->margin[FLOW_TOP], cbw), mb = len(s->margin[FLOW_BOTTOM], cbw);
    int64_t y = cur->y + margins_sum(margins_join(cur->pm, margin_of(mt)));
    if (cur->first < 0)
        cur->first = y;

    // Top captions, as wide as the table.
    Cursor cc = {y, kNoMargins, y};
    for (FBox *c = t->first; c != NULL && !l->failed; c = c->next)
        if (c->kind == FB_CAPTION && c->style->caption_side == FLOW_CAPTION_TOP)
            block(l, styles, c, t->x, t->w, &cc);
    y = cc.y + margins_sum(cc.pm);

    distribute(cols, g.ncols, used - frame - (int64_t)(g.ncols + 1) * hsp);
    int64_t gx = t->x + t->border[FLOW_LEFT] + t->padding[FLOW_LEFT];
    int64_t x = gx + hsp;
    for (int32_t k = 0; k < g.ncols; k++) {
        cols[k].x = x;
        x += cols[k].width + hsp;
    }

    // Each cell once, at its width; rows as tall as their tallest
    // one-row cell; a spanning cell's excess spread evenly over its rows.
    int64_t *rh = scratch_calloc(l, (size_t)(g.nrows > 0 ? g.nrows : 1), sizeof(int64_t));
    int64_t *ry = scratch_calloc(l, (size_t)(g.nrows > 0 ? g.nrows : 1), sizeof(int64_t));
    if (rh == NULL || ry == NULL) {
        fail(l);
        scratch_free(l, rh);
        scratch_free(l, ry);
        scratch_free(l, cols);
        grid_free(l, &g);
        return;
    }
    int64_t inner_w = used - frame;
    for (int32_t r = 0; r < g.nrows; r++)
        if (g.rows[r].box->style->height.kind == FLOW_LENGTH_PX)
            rh[r] = g.rows[r].box->style->height.value;
    // Each cell laid out at its width, and its baseline taken then, before
    // any row stretches it. A row is as tall as its tallest cell, and as
    // tall as its baseline cells need: the most any has above the shared
    // baseline plus the most any has below it (§17.5.3) — sized before the
    // cells are moved, so a moved cell never reaches into the next row.
    int64_t *base = scratch_calloc(l, (size_t)(g.ncells > 0 ? g.ncells : 1), sizeof(int64_t));
    int64_t *row_base = scratch_calloc(l, (size_t)(g.nrows > 0 ? g.nrows : 1), sizeof(int64_t));
    int64_t *below = scratch_calloc(l, (size_t)(g.nrows > 0 ? g.nrows : 1), sizeof(int64_t));
    if (base == NULL || row_base == NULL || below == NULL)
        fail(l);
    for (int32_t i = 0; i < g.ncells && !l->failed; i++) {
        GCell *gc = &g.cells[i];
        int64_t w = (int64_t)(gc->cols - 1) * hsp;
        for (int32_t k = gc->col; k < gc->col + gc->cols; k++)
            w += cols[k].width;
        int64_t h = cell_layout(l, styles, gc->cell, cols[gc->col].x, w, inner_w);
        if (gc->rows == 1)
            rh[gc->row] = max64(rh[gc->row], h);
        if (!l->failed)
            base[i] = first_baseline(gc->cell);
        if (!l->failed && gc->cell->style->vertical_align == FLOW_VALIGN_BASELINE)
            row_base[gc->row] = max64(row_base[gc->row], base[i]);
    }
    for (int32_t i = 0; i < g.ncells && !l->failed; i++) {
        GCell *gc = &g.cells[i];
        if (gc->rows == 1 && gc->cell->style->vertical_align == FLOW_VALIGN_BASELINE)
            below[gc->row] = max64(below[gc->row], gc->cell->h - base[i]);
    }
    for (int32_t r = 0; r < g.nrows && !l->failed; r++)
        if (below[r] > 0 || row_base[r] > 0)
            rh[r] = max64(rh[r], row_base[r] + below[r]);
    // Rowspans: what each spanning cell's rows already have, and its excess
    // spread over them, in a range-add, range-sum tree over the row heights
    // — a cell costs log(rows), not its span, which a first row of long
    // spans over thousands of empty rows made rows x cells.
    RowSums rs = {0};
    bool spanning = false;
    for (int32_t i = 0; i < g.ncells && !spanning; i++)
        spanning = g.cells[i].rows > 1;
    if (spanning && !l->failed && !rowsums_init(l, &rs, rh, g.nrows))
        spanning = false;
    for (int32_t i = 0; i < g.ncells && spanning && !l->failed; i++) {
        GCell *gc = &g.cells[i];
        if (gc->rows == 1)
            continue;
        int32_t last = gc->row + gc->rows - 1;
        int64_t have = (int64_t)(gc->rows - 1) * vsp + rowsums_sum(&rs, gc->row, last);
        // A spanning baseline cell moves down to its first row's baseline,
        // and the rows it spans hold it where it goes.
        int64_t need = gc->cell->h;
        if (gc->cell->style->vertical_align == FLOW_VALIGN_BASELINE)
            need += row_base[gc->row] - base[i];
        if (need > have) {
            int64_t extra = need - have, each = extra / gc->rows;
            rowsums_add(&rs, gc->row, last, each);
            rowsums_add(&rs, last, last, extra - each * gc->rows);
        }
    }
    if (spanning && !l->failed)
        for (int32_t r = 0; r < g.nrows; r++)
            rh[r] = rowsums_sum(&rs, r, r);
    scratch_free(l, rs.a);
    scratch_free(l, rs.b);
    // A row group's set height is its rows' least together: what they lack
    // of it goes to them in proportion to their heights, else evenly, the
    // rule a set table height follows.
    for (int32_t r0 = 0; r0 < g.nrows && !l->failed;) {
        int32_t r1 = g.rows[r0].group_end;
        const FBox *group = g.rows[r0].box->parent;
        if (group != NULL && group->kind == FB_ROW_GROUP &&
            group->style->height.kind == FLOW_LENGTH_PX) {
            int64_t have = (int64_t)(r1 - r0 - 1) * vsp;
            for (int32_t r = r0; r < r1; r++)
                have += rh[r];
            int64_t total = have - (int64_t)(r1 - r0 - 1) * vsp;
            if (group->style->height.value > have) {
                int64_t extra = group->style->height.value - have, given = 0;
                for (int32_t r = r0; r < r1; r++) {
                    int64_t share = total > 0 ? mul_div(extra, rh[r], total) : extra / (r1 - r0);
                    if (r == r1 - 1)
                        share = extra - given;
                    given += share;
                    rh[r] += share;
                }
            }
        }
        r0 = r1 > r0 ? r1 : r0 + 1;
    }
    int64_t top = y + t->border[FLOW_TOP] + t->padding[FLOW_TOP];
    int64_t grid_h = g.nrows > 0 ? (int64_t)(g.nrows + 1) * vsp : 0;
    for (int32_t r = 0; r < g.nrows; r++)
        grid_h += rh[r];
    // A set height (border-box) is a least height, rows or none; rows share
    // the rest in proportion to their heights.
    int64_t vf = vframe(t, cbw);
    if (s->height.kind == FLOW_LENGTH_PX && s->height.value - vf > grid_h && g.nrows > 0) {
        int64_t extra = s->height.value - vf - grid_h, total = grid_h - (int64_t)(g.nrows + 1) * vsp;
        int64_t given = 0;
        for (int32_t r = 0; r < g.nrows; r++) {
            int64_t share = total > 0 ? mul_div(extra, rh[r], total) : extra / g.nrows;
            if (r == g.nrows - 1)
                share = extra - given;
            given += share;
            rh[r] += share;
        }
        grid_h += extra;
    } else if (s->height.kind == FLOW_LENGTH_PX && s->height.value - vf > grid_h) {
        // No rows to share it: a set height is still the table's least.
        grid_h = s->height.value - vf;
    }
    int64_t yy = top + vsp;
    for (int32_t r = 0; r < g.nrows; r++) {
        ry[r] = yy;
        yy += rh[r] + vsp;
    }

    // Cells to their rows, and their content to where `valign` puts it —
    // baseline cells sharing their row's lowest first baseline.
    for (int32_t i = 0; i < g.ncells && !l->failed; i++) {
        GCell *gc = &g.cells[i];
        FBox *cell = gc->cell;
        int64_t natural = cell->h;
        // The rows it spans, from their tops: no walk over the span.
        int32_t last = gc->row + gc->rows - 1;
        int64_t h = ry[last] + rh[last] - ry[gc->row];
        translate(cell, 0, ry[gc->row] - cell->y);
        cell->h = h;
        int64_t shift = 0;
        switch (cell->style->vertical_align) {
        case FLOW_VALIGN_MIDDLE: shift = (h - natural) / 2; break;
        case FLOW_VALIGN_BOTTOM: shift = h - natural; break;
        case FLOW_VALIGN_BASELINE: shift = row_base[gc->row] - base[i]; break;
        default: break;
        }
        if (shift > 0) {
            translate_content(cell, shift);
            base[i] += shift;
        }
    }
    // A cell is finished once its row has stretched it and valign has
    // placed its content: only now is it a containing block's final rect.
    for (int32_t i = 0; i < g.ncells && !l->failed; i++)
        positioned_done(l, styles, g.cells[i].cell, inner_w);
    // Each row's baseline, for a table this one is nested in: the one its
    // baseline cells share, else its first cell's where valign put it, else
    // (no cells) its bottom.
    for (int32_t i = 0; i < g.ncells && !l->failed; i++) {
        GCell *gc = &g.cells[i];
        if (gc->cell->style->vertical_align == FLOW_VALIGN_BASELINE) {
            g.rows[gc->row].box->has_baseline = true;
            g.rows[gc->row].box->baseline = ry[gc->row] + row_base[gc->row];
        }
    }
    for (int32_t i = 0; i < g.ncells && !l->failed; i++) {
        FBox *row = g.rows[g.cells[i].row].box;
        if (!row->has_baseline) {
            row->has_baseline = true;
            row->baseline = ry[g.cells[i].row] + base[i];
        }
    }
    // A row with no cells has its bottom as its baseline, so a table whose
    // first row is empty still takes its baseline from that row.
    for (int32_t r = 0; r < g.nrows && !l->failed; r++)
        if (!g.rows[r].box->has_baseline) {
            g.rows[r].box->has_baseline = true;
            g.rows[r].box->baseline = ry[r] + rh[r];
        }
    scratch_free(l, base);
    scratch_free(l, row_base);
    scratch_free(l, below);

    // Rows, row groups and columns: the rectangles their cells make.
    int64_t row_x = gx + hsp, row_w = 0;
    for (int32_t k = 0; k < g.ncols; k++)
        row_w += cols[k].width + (k > 0 ? hsp : 0);
    for (int32_t r = 0; r < g.nrows; r++) {
        FBox *row = g.rows[r].box;
        row->placed = true;
        row->x = row_x;
        row->w = row_w;
        row->y = ry[r];
        row->h = rh[r];
    }
    int64_t cursor_y = top + vsp;
    for (FBox *c = t->first; c != NULL; c = c->next) {
        if (c->kind == FB_ROW_GROUP) {
            int64_t y0 = -1, y1 = -1;
            for (FBox *row = c->first; row != NULL; row = row->next)
                if (row->kind == FB_ROW && row->placed) {
                    if (y0 < 0 || row->y < y0)
                        y0 = row->y;
                    y1 = max64(y1, row->y + row->h);
                }
            c->placed = true;
            c->x = row_x;
            c->w = row_w;
            c->y = y0 >= 0 ? y0 : cursor_y;
            c->h = y0 >= 0 ? y1 - y0 : 0;
            if (y0 >= 0)
                cursor_y = y1 + vsp;
        }
    }
    // A column is as tall as the rows; a column group spans its columns.
    int64_t cols_y = g.nrows > 0 ? ry[0] : top;
    int64_t cols_h = g.nrows > 0 ? ry[g.nrows - 1] + rh[g.nrows - 1] - cols_y : 0;
    int32_t k = 0;
    for (FBox *c = t->first; c != NULL; c = c->next) {
        if (c->kind != FB_COLUMN && c->kind != FB_COLUMN_GROUP)
            continue;
        int32_t k0 = k;
        if (c->kind == FB_COLUMN_GROUP && c->first == NULL)
            k += span_attr(c, "span", COLSPAN_MAX, 1);
        for (FBox *col = c->kind == FB_COLUMN ? c : c->first; col != NULL;
             col = c->kind == FB_COLUMN ? NULL : col->next) {
            int32_t n = span_attr(col, "span", COLSPAN_MAX, 1);
            int32_t j0 = k;
            k += n;
            col->placed = true;
            col->x = j0 < g.ncols ? cols[j0].x : x;
            int32_t j1 = k < g.ncols ? k : g.ncols;
            col->w = j1 > j0 ? cols[j1 - 1].x + cols[j1 - 1].width - col->x : 0;
            col->y = cols_y;
            col->h = cols_h;
        }
        if (c->kind == FB_COLUMN_GROUP) {
            int32_t j1 = k < g.ncols ? k : g.ncols;
            c->placed = true;
            c->x = k0 < g.ncols ? cols[k0].x : x;
            c->w = j1 > k0 ? cols[j1 - 1].x + cols[j1 - 1].width - c->x : 0;
            c->y = cols_y;
            c->h = cols_h;
        }
    }

    // The table box itself.
    t->placed = true;
    t->y = y;
    t->h = vf + grid_h;
    t->content_h = t->h;

    // Bottom captions, then the flow goes on below it all.
    cc = (Cursor){t->y + t->h, kNoMargins, t->y + t->h};
    for (FBox *c = t->first; c != NULL && !l->failed; c = c->next)
        if (c->kind == FB_CAPTION && c->style->caption_side == FLOW_CAPTION_BOTTOM)
            block(l, styles, c, t->x, t->w, &cc);
    cur->y = cc.y + margins_sum(cc.pm);
    cur->pm = margin_of(mb);

    scratch_free(l, rh);
    scratch_free(l, ry);
    scratch_free(l, cols);
    grid_free(l, &g);
}

// ── The layout ──────────────────────────────────────────────────────────

// What is not placed is not on the page — a failed table's unplaced cells
// keep their lines, and those reach no edge. A positioned child is not
// reached through its parent: its edges are the list's (page_extent).
static int64_t right_edge(const FBox *b)
{
    if (!b->placed)
        return 0;
    int64_t r = b->x + b->w;
    // What a box clips on an axis is not drawn past it there — scrolled to
    // inside it, if it scrolls — so it widens nothing (CSS Overflow 3 §
    // 2.2). The viewport's overflow clips nothing: it is the page's.
    if (f_box_clips(b, true))
        return r;
    for (const FLine *ln = b->lines; ln != NULL; ln = ln->next)
        for (const FFrag *fr = ln->frags; fr != NULL; fr = fr->next) {
            r = max64(r, fr->x + fr->w - fr->hang);
            if (fr->kind == FF_ATOMIC && fr->item->content != NULL)
                r = max64(r, right_edge(fr->item->content));
        }
    for (const FBox *c = b->first; c != NULL; c = c->next)
        if (!c->positioned)
            r = max64(r, right_edge(c));
    return r;
}

// How far down anything is drawn: a box's own bottom, and anything inside
// it that overflows a height the page set — unless the box clips it.
static int64_t bottom_edge(const FBox *b)
{
    if (!b->placed)
        return 0;
    int64_t r = b->y + b->h;
    if (f_box_clips(b, false))
        return r;
    for (const FLine *ln = b->lines; ln != NULL; ln = ln->next)
        for (const FFrag *fr = ln->frags; fr != NULL; fr = fr->next) {
            r = max64(r, fr->y + fr->h);
            if (fr->kind == FF_ATOMIC && fr->item->content != NULL)
                r = max64(r, bottom_edge(fr->item->content));
        }
    for (const FBox *c = b->first; c != NULL; c = c->next)
        if (!c->positioned)
            r = max64(r, bottom_edge(c));
    return r;
}

// What clips a box: its tree parent in the flow, its containing block out
// of it — an inline one's home, since an inline clips nothing — and NULL at
// the initial containing block or the root (POSITION.md: clips follow
// containing blocks).
static const FBox *clip_parent(const FBox *b)
{
    if (!b->out_of_flow)
        return b->parent;
    const FPos *cb = b->pos != NULL ? b->pos->cb : NULL;
    return cb == NULL ? NULL : cb->kind == FP_BOX ? cb->box : cb->home;
}

// The page's extent takes each positioned box's far edges as it takes any
// box's — cut where a box on its clip chain cuts it, as right_edge and
// bottom_edge are — and never anything left of or above the origin, which
// is drawn where it reaches the glass and cannot be scrolled to
// (POSITION.md, ruling 2). Found by walking the boxes, an inline-block's
// content included, and not the entries: a build that stopped can leave a
// positioned box without one. A fixed box and everything in it are left
// out: it is where it is on the glass, not on the page, and the page is
// never scrolled to reach it.
static void page_extent(const FBox *b, int64_t *width, int64_t *height)
{
    if (!b->placed || (b->out_of_flow && b->style->position == FLOW_POSITION_FIXED))
        return;
    if (b->positioned) {
        int64_t r = right_edge(b), bottom = bottom_edge(b);
        for (const FBox *c = clip_parent(b); c != NULL; c = clip_parent(c)) {
            if (f_box_clips(c, true))
                r = min64(r, c->x + c->w);
            if (f_box_clips(c, false))
                bottom = min64(bottom, c->y + c->h);
        }
        *width = max64(*width, r);
        *height = max64(*height, bottom);
    }
    for (const FLine *ln = b->lines; ln != NULL; ln = ln->next)
        for (const FFrag *fr = ln->frags; fr != NULL; fr = fr->next)
            if (fr->kind == FF_ATOMIC && fr->item->content != NULL)
                page_extent(fr->item->content, width, height);
    for (const FBox *c = b->first; c != NULL; c = c->next)
        page_extent(c, width, height);
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
    out->arena.cap = f_arena_budget(env);
    out->quirks = doc != NULL ? doc->quirks : OS64_HTML_NO_QUIRKS;
    out->model = model;
    out->boxes = boxes;
    // Whole means whole from the bytes up: a page the parser or the model
    // stopped short of is a prefix however well it lays out.
    out->incomplete = boxes->incomplete || (doc != NULL && doc->refusal != 0) ||
                      (model != NULL && os64_page_incomplete(model));
    L l = {.out = out, .env = env, .model = model, .quirks = out->quirks == OS64_HTML_QUIRKS,
           .any_quirks = out->quirks != OS64_HTML_NO_QUIRKS};
    int64_t w = (int64_t)width * 64;
    out->width = w;
    if (boxes->root != NULL) {
        Cursor cur = {0, kNoMargins, -1};
        block(&l, boxes->styles, boxes->root, 0, w, &cur);
        // A layout that stopped has no cursor worth reading: the page is as
        // tall as what it placed.
        out->height = l.failed ? bottom_edge(boxes->root)
                               : max64(cur.y + margins_sum(cur.pm), bottom_edge(boxes->root));
        // The initial containing block is the viewport, as tall as the
        // window (CSS 2.1 § 10.1) — or, with no window height known, the
        // page — and so is every fixed box's, whose coordinates are the
        // viewport's own and the same numbers; the root is the owner when
        // one of those boxes stops.
        if (!l.failed) {
            int64_t vh = env->viewport_height > 0 ? f_css_units(env, env->viewport_height)
                                                  : out->height;
            absolutes(&l, boxes->styles, boxes->icb_first, (Rect){0, 0, w, vh}, boxes->root);
        }
        out->width = max64(w, right_edge(boxes->root));
        page_extent(boxes->root, &out->width, &out->height);
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
