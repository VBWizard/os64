#ifndef FLOW_INTERNAL_H
#define FLOW_INTERNAL_H

#include "flow/flow.h"
#include "os64/mem.h"
#include "os64/str.h"

#define F_ARRAY(a) ((int32_t)(sizeof(a) / sizeof((a)[0])))

// ── Storage ─────────────────────────────────────────────────────────────
//
// Everything a layout builds lives until the layout is freed and is never
// freed piecemeal, so it comes from blocks that are freed together. A
// pointer handed out stays valid: blocks never move.
typedef struct FBlock FBlock;
typedef struct {
    FBlock *blocks;
    // Bytes the blocks hold, and the most they may: flow_env_t's
    // max_arena_bytes for the arenas a page can multiply (boxes, lines). A
    // block past it is refused like one malloc cannot supply, so such a page
    // stops as `incomplete`.
    size_t reserved, cap;
} FArena;

// The byte budget a multiplied arena takes from the environment.
static inline size_t f_arena_budget(const flow_env_t *env)
{
    return env != NULL && env->max_arena_bytes != 0 ? env->max_arena_bytes : FLOW_ARENA_DEFAULT;
}

void *f_arena_alloc(FArena *arena, size_t size);    // zeroed; NULL on no memory or budget
void f_arena_free(FArena *arena);

// A node to the record built for it. Open addressing over a power-of-two
// table, because layout asks "what is this node's style?" once per node
// and the face asks "which box is this node?" once per click.
typedef struct {
    const void **keys;
    void **vals;
    size_t cap, count;
} FMap;

bool f_map_put(FMap *map, const void *key, void *val);  // false on no memory
void *f_map_get(const FMap *map, const void *key);      // NULL when absent
void f_map_free(FMap *map);

// ── The page's attribute grammars (attrs.c) ─────────────────────────────
//
// HTML's microsyntaxes, each the standard's algorithm by name, so a value a
// page wrote means here what it means in every browser.

typedef enum { F_DIM_NONE = 0, F_DIM_PX, F_DIM_PERCENT } f_dim_kind_t;

// "Rules for parsing dimension values": digits, an optional fraction, and a
// `%` that makes it a percentage; anything after that is ignored. The value
// is in 1/64ths — of a pixel or of a percent. `nonzero` is the "(ignoring
// zero)" form: a zero is no value at all.
f_dim_kind_t f_parse_dimension(const char *s, bool nonzero, int32_t *out);
// "Rules for parsing integers" and "…non-negative integers". A value too
// large to mean anything on a page is clamped to F_INT_MAX, which no
// geometry reaches, rather than overflowing into a different number.
#define F_INT_MAX 1000000
bool f_parse_integer(const char *s, int32_t *out);
bool f_parse_nonnegative(const char *s, int32_t *out);
// "Rules for parsing a legacy colour value", `chucknorris` and all: a
// page's colour attribute means the colour every browser draws.
bool f_parse_legacy_color(const char *s, uint32_t *out);

// ASCII case-insensitive, which is what `[attr=value i]` means.
bool f_eq_nocase(const char *a, const char *b);

// ── Pass 1: the computed styles (style.c) ───────────────────────────────

// The colour each decoration is drawn in: the colour of the innermost
// element that asked for it, so `<u><font color=red><s>` underlines in the
// u's colour and strikes through in red. A kind not drawn has no colour
// worth reading.
typedef struct {
    uint32_t underline, line_through;
} FDecorationColors;

// An element's decorations and colours taking on the ones propagated to it:
// the kinds it already draws keep its own colour.
static inline void f_decoration_inherit(uint8_t *bits, FDecorationColors *colors,
                                        uint8_t up_bits, FDecorationColors up)
{
    if (!(*bits & FLOW_DECORATION_UNDERLINE) && (up_bits & FLOW_DECORATION_UNDERLINE))
        colors->underline = up.underline;
    if (!(*bits & FLOW_DECORATION_LINE_THROUGH) && (up_bits & FLOW_DECORATION_LINE_THROUGH))
        colors->line_through = up.line_through;
    *bits |= up_bits;
}

typedef struct {
    flow_style_t style;
    // Pass 2's question, answered bottom-up here: does this element make,
    // somewhere inside its inline formatting, an in-flow block-level box?
    // True for an inline with a block child (or an inline child that holds
    // one), which is what makes its container MIX and the inline SPLIT
    // (LAYOUT.md § Pass 2). Not a style property: no cascade writes it.
    bool holds_block;
    // What walking up from this element would say, answered here once,
    // parent before child, so a later pass asks in constant time and the
    // layout stays linear in the page (LAYOUT.md § Bounds). The link this
    // element is or sits in, or -1. The decorations drawn across its
    // content: its own and every ancestor's up to an atomic inline or (full
    // quirks, the Quirks standard's 3.11) a table, which keep their own and
    // stop the rest — each kind in its own colour (FDecorationColors).
    int32_t link;
    uint8_t decoration;
    FDecorationColors decoration_colors;
    // How many of this element and its ancestors are lists (`ul ol menu
    // dir`), lists or `dl`s, and `li`s: the chapter's descendant selectors
    // for its children, by the same bargain. And whether one of them is a
    // `nobr`, which is what `nobr wbr` asks, and how many are `q`s, which
    // is how deep a quotation's marks nest.
    int32_t lists, lists_or_dls, items, quotes;
    bool in_nobr;
    // An `rp` among this element's children: whether an `rt` in it gets
    // the parentheses a browser that lays out no ruby adds. Set by the
    // child, and read by pass 2 once every child has been styled.
    bool has_rp;
} FStyled;

typedef struct {
    const os64_html_document_t *doc;
    const flow_env_t *env;
    FArena arena;
    FMap map;               // element node -> FStyled
} FStyles;

// ALL OR NOTHING: NULL when memory runs out anywhere. A style is a few
// hundred bytes per element, so a page that cannot have its styles cannot
// have its boxes either; and a partial table would let pass 2 decide a
// container's shape from half its children, which a whole build decides
// differently (LAYOUT.md § Proof, the prefix rule).
FStyles *f_style_build(const os64_html_document_t *doc, const os64_page_t *model,
                       const flow_env_t *env);
// NULL for a node with no style: not an element, or inside a
// `display: none` subtree, which pass 1 does not descend into.
const FStyled *f_style_of(const FStyles *styles, const os64_html_node_t *node);
void f_style_free(FStyles *styles);
// The style of an ANONYMOUS box: its parent's inherited properties and the
// initial value of every other (CSS 2.1 §9.2.1.1) — never a copy of the
// parent's struct, which would give an anonymous block its parent's margins
// and borders a second time.
void f_style_anonymous(const FStyles *styles, const flow_style_t *parent,
                       flow_display_t display, flow_style_t *out);

// One line per styled element, indented by depth: the element, its display,
// then what it CHANGED — an inherited property where it differs from the
// parent's, any other where it differs from its initial value — so a line
// reads as the rules that fired. Like snprintf, it answers the length the
// whole dump needs and writes what fits.
int64_t f_style_dump(const FStyles *styles, char *out, size_t cap);

// ── Pass 2: the box tree (boxes.c) ──────────────────────────────────────
//
// Block-level boxes form the tree. A block container whose content is
// inline holds, instead of children, its INLINE FORMATTING CONTEXT as one
// flat sequence of items — text pieces, the opening and closing edges of
// inline boxes, atomic inlines, breaks — because a line is broken across
// that sequence and not per node: `foo<b>bar</b>` is one word in two nodes
// (LAYOUT.md § Pass 3).

// How deep the box build descends — element levels, the parts of a table,
// and an inline-block's content twice — before it stops and reports the
// page incomplete. Every pass after it recurses along the boxes, so this is
// what bounds the stack a layout needs, whatever nesting the parser was
// told to allow; it is sized to leave a thread's stack room to spare
// (LAYOUT.md § Bounds).
#define F_DEPTH_MAX 512

typedef enum {
    FB_BLOCK = 0,       // a block container; node NULL for an anonymous one
    FB_REPLACED,        // a block-level replaced box: a frame
    FB_TABLE,
    FB_CAPTION,         // a block container
    FB_COLUMN_GROUP,
    FB_COLUMN,
    FB_ROW_GROUP,
    FB_ROW,
    FB_CELL,            // a block container
} f_box_kind_t;

typedef enum {
    FI_TEXT = 0,
    FI_OPEN,            // an inline box begins (or, after a split, continues)
    FI_CLOSE,           // it ends (or is interrupted by a block)
    FI_ATOMIC,          // a replaced inline, or an inline-block with content
    FI_BREAK,           // `br`, or a preserved newline
    FI_WBR,             // a break opportunity and nothing else
    FI_MARKER,          // a list marker drawn inside
} f_item_kind_t;

typedef struct FBox FBox;
typedef struct FLine FLine;
typedef struct FFrag FFrag;

// One record per inline element, shared by every piece a block splits it
// into, so a face paints a split `<a>` as one link.
typedef struct {
    const os64_html_node_t *node;
    const flow_style_t *style;
    int32_t link;                   // libpage's index, or -1
    int32_t pieces;                 // how many OPENs have been emitted
    FBox *open_in;                  // the context its current piece is open in
} FInline;

typedef struct FItem FItem;
struct FItem {
    FItem *next;
    f_item_kind_t kind;
    // TEXT: the text node, or for generated text the element that
    // generated it. OPEN/CLOSE/ATOMIC/BREAK/WBR: the element (NULL for a
    // preserved newline). MARKER: the list item.
    const os64_html_node_t *node;
    const flow_style_t *style;      // what the item is drawn in
    // TEXT, MARKER: the bytes to lay out. For page text, a slice of the
    // node's processed copy starting `offset` bytes in — the range a
    // selection maps back through.
    const char *text;
    uint32_t len, offset;
    bool generated;                 // TEXT the sheet made: a quote mark, a ruby parenthesis
    bool continuation;              // OPEN: an earlier piece exists; CLOSE: a block split it here
    FInline *inl;                   // OPEN, CLOSE
    FBox *content;                  // ATOMIC with a block of its own (a marquee)
    int32_t link, control;          // ATOMIC: libpage's indexes, or -1
};

struct FBox {
    f_box_kind_t kind;
    const os64_html_node_t *node;
    const flow_style_t *style;
    FBox *parent, *first, *last, *next;
    // A block container either has block children or is an inline
    // formatting context with items — never both.
    bool ifc;
    FItem *items, *last_item;
    int32_t nitems;
    bool collapse_space;            // build time: the last text ended in a collapsible space
    // A list item's marker when it is drawn OUTSIDE; an inside marker is
    // an item. NULL when the item has none.
    const char *marker;
    uint32_t marker_len;
    // The link this box IS (a frame) or sits inside, however deep, or -1:
    // every box carries its own, an anonymous one its parent's.
    int32_t link;

    // ── Pass 3's geometry, in 26.6 document coordinates (x from the page's
    // left edge, y from its top). 64-bit, because a long page is taller
    // than 26.6 can count in 32 bits.
    bool placed;                    // false: layout stopped before this box
    // Layout stopped INSIDE this box: its rectangle holds what was laid
    // out before it did (LAYOUT.md § Proof, the allocation sweep).
    bool unfinished;
    int64_t x, y, w, h;             // the border box
    // A table row: the baseline its layout gave it (the one its baseline
    // cells share, else its first cell's, else its bottom), absolute, once
    // it is laid out.
    bool has_baseline;
    int64_t baseline;
    int64_t border[4], padding[4];  // used widths, top right bottom left
    FLine *lines, *last_line;       // an inline formatting context's lines
    FFrag *marker_frag;             // an outside marker, placed
    // The box's min-content and max-content border-box widths, found once
    // per layout and kept: a nested table asks for its cells' widths on
    // every level above it, and without the memo that is exponential.
    bool intrinsic_known;
    int64_t intrinsic_min, intrinsic_max;
};

typedef struct {
    FArena arena;
    const FStyles *styles;          // what the boxes' styles came from
    FBox *root;
    // Memory ran out partway, or the page nests past F_DEPTH_MAX: every
    // box and item present is real, and the build stopped at the first
    // thing it could not make.
    bool incomplete;
} FBoxes;

// ── Pass 3: lines and fragments (layout.c) ──────────────────────────────

typedef enum { FF_TEXT = 0, FF_ATOMIC, FF_MARKER } f_frag_kind_t;

// What one line holds: a text fragment is one run of the text engine; an
// atomic fragment is a replaced box or an inline-block; a marker fragment
// is a list marker's text.
struct FFrag {
    FFrag *next;
    f_frag_kind_t kind;
    const FItem *item;              // TEXT, ATOMIC; a MARKER drawn inside
    const os64_html_node_t *node;
    const flow_style_t *style;
    os64_text_run_t *run;           // TEXT, MARKER: owned, released with the tree
    const char *text;               // TEXT, MARKER: the bytes the run was laid out from
    uint32_t begin, end;            // TEXT: the byte range in the item's text
    int64_t x, y, w, h;             // TEXT, MARKER: the content area; ATOMIC: the border box
    int64_t hang;                   // TEXT: the width at its end of pre-wrap spaces that hang
    int64_t baseline;               // absolute
    uint8_t decoration;             // FLOW_DECORATION_* drawn across it
    FDecorationColors decoration_colors;
    int32_t link;                   // the link it sits in, or -1
};

// One inline box's piece on one line: what a link's underline or a
// background is drawn behind.
typedef struct FSpan FSpan;
struct FSpan {
    FSpan *next;
    const FInline *inl;
    int64_t x0, x1, top, bottom;
};

struct FLine {
    FLine *next;
    int64_t x, y, w, h, baseline;   // w: the content width it was broken to
    FFrag *frags, *last_frag;
    FSpan *spans, *last_span;
};

// NULL only when there is not memory for the tree itself.
FBoxes *f_boxes_build(const os64_html_document_t *doc, const os64_page_t *model,
                      const FStyles *styles, const flow_env_t *env);
void f_boxes_free(FBoxes *boxes);
// CSS white space processing (CSS 2.1 §16.6.1), for every text the page
// is laid out as — a text node's, and a missing picture's alt. Whether a
// white-space value collapses spaces; then `n` bytes of `in` into `out`
// (room for `n`), each run of spaces, tabs and line breaks one space, and
// none at the start when `*space` says the text before ended in one. It
// leaves `*space` saying whether this one does, and returns the length.
bool f_collapsible(flow_white_space_t ws);
uint32_t f_collapse_white(const char *in, uint32_t n, char *out, bool *space);
// One line per box and per item, indented by depth; the same contract as
// f_style_dump.
int64_t f_boxes_dump(const FBoxes *boxes, char *out, size_t cap);

typedef struct {
    const flow_env_t *env;
    os64_html_quirks_t quirks;
    const os64_page_t *model;
    FBoxes *boxes;
    FArena arena;                   // lines, fragments, spans
    // Every run a fragment holds, so freeing the tree releases each once.
    os64_text_run_t **runs;
    size_t nruns, cap_runs;
    int64_t width, height;          // the page's, 26.6
    // How many times an inline formatting context was measured: the cost
    // the harness holds to a linear bound (LAYOUT.md § Bounds).
    uint64_t measures;
    // The text engine or the allocator refused partway: what is placed is
    // real, and nothing after the refusal is.
    bool incomplete;
} FLayout;

// Lays the box tree out at `width` CSS pixels. NULL only when there is not
// memory for the layout record.
FLayout *f_layout(FBoxes *boxes, const os64_html_document_t *doc, const os64_page_t *model,
                  const flow_env_t *env, int32_t width);
void f_layout_free(FLayout *layout);
int64_t f_layout_dump(const FLayout *layout, char *out, size_t cap);

#endif
