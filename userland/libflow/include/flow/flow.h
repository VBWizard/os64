#ifndef OS64_FLOW_H
#define OS64_FLOW_H

// flow.h — a parsed page in, positioned boxes out. LAYOUT.md is the design
// record; this file is the seam it describes.
//
// Below this line libflow decides ALL the geometry of a page and none of
// its meaning: what a link or a control IS comes from libpage, and libflow
// only asks. Above it a face paints, scrolls and hit-tests. Nothing here
// does I/O, blocks, or opens a font file — fonts and the sizes of pictures
// arrive through the callbacks in flow_env_t, which is what lets the whole
// library run on the host against the fake font backend.
//
// THE DOCUMENT, THE MODEL, THE CASCADE AND THE TEXT CONTEXT MUST OUTLIVE
// WHATEVER IS BUILT FROM THEM. A style points into the tree, or into the
// cascade's sheets, for the family names a page wrote, and nothing is
// copied that can be pointed at; every text fragment's run lives on
// `env->text`, and freeing the layout releases each run against it.

#include "html/html.h"
#include "page/page.h"
#include "os64/font_backend.h"
#include "os64/gui.h"
#include "os64/text.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#pragma GCC visibility push(default)

// ── Units ───────────────────────────────────────────────────────────────
//
// A length is CSS pixels in 26.6 fixed point — the text engine's own unit,
// so a font size and a margin computed from it never cross a conversion.
// Layout keeps 26.6 until a box coordinate is written, and rounds there
// once (LAYOUT.md § Rounding).
typedef int32_t flow_unit_t;
#define FLOW_UNITS_PER_PX 64

typedef enum {
    FLOW_LENGTH_AUTO = 0,
    FLOW_LENGTH_PX,         // value in flow_unit_t
    FLOW_LENGTH_PERCENT,    // value in 1/64ths of a percent, resolved at layout
    // A width that shrinks to fit (CSS Sizing 3 § 3.2): no smaller than
    // the content's min-content width, no larger than its max-content, the
    // room between. The Rendering chapter's `dialog` is its producer; an
    // author's `fit-content` is still read as `auto` (GARB.md § Booked).
    FLOW_LENGTH_FIT_CONTENT,
    // `flex-basis: content` (Flexbox 1 § 7.2.3): the item's own content
    // size, whatever width or height it was given — and the sizing
    // keywords a basis is read as that (FLEX.md § Booked). Only a basis
    // is ever this.
    FLOW_LENGTH_CONTENT,
} flow_length_kind_t;

typedef struct {
    flow_length_kind_t kind;
    int32_t value;
    // PERCENT: flow_unit_t added once the percentage is resolved — what a
    // `calc(100% - 2em)` computes to. Zero otherwise.
    flow_unit_t offset;
} flow_length_t;

// Box sides, in CSS's own order.
enum { FLOW_TOP = 0, FLOW_RIGHT = 1, FLOW_BOTTOM = 2, FLOW_LEFT = 3 };

// ── Colours ─────────────────────────────────────────────────────────────
//
// A COLOUR is 0xTTRRGGBB: sRGB, and in the top byte how TRANSPARENT it is —
// 0 opaque, 255 not there at all. Transparency rather than alpha so that
// every plain 0xRRGGBB a face, an attribute or the Rendering chapter writes
// is the opaque colour it always was; and in the colour itself, not beside
// it, so that whatever copies a colour — inheritance, `currentColor`, a
// border or a decoration in its element's colour — copies how translucent
// it is with it (PILE3.md § The blend).
static inline uint8_t flow_alpha(uint32_t colour)
{
    return (uint8_t)(255 - (colour >> 24));
}

// ── The computed style ──────────────────────────────────────────────────
//
// EVERY FIELD IS ONE CSS PROPERTY'S COMPUTED VALUE, in the property's own
// units and with its own initial value, because the page's own sheets
// (libgarb's cascade) are a second producer of this same struct (LAYOUT.md
// § The ruling). A property arrives here with the first producer that can
// set it, and no field changes its meaning when another producer does.

typedef enum {
    FLOW_DISPLAY_INLINE = 0,        // the initial value
    FLOW_DISPLAY_BLOCK,
    FLOW_DISPLAY_LIST_ITEM,
    FLOW_DISPLAY_INLINE_BLOCK,      // an atomic inline: a form control and the like
    FLOW_DISPLAY_TABLE,
    FLOW_DISPLAY_TABLE_CAPTION,
    FLOW_DISPLAY_TABLE_ROW_GROUP,
    FLOW_DISPLAY_TABLE_HEADER_GROUP,
    FLOW_DISPLAY_TABLE_FOOTER_GROUP,
    FLOW_DISPLAY_TABLE_ROW,
    FLOW_DISPLAY_TABLE_CELL,
    FLOW_DISPLAY_TABLE_COLUMN_GROUP,
    FLOW_DISPLAY_TABLE_COLUMN,
    // A flex container (FLEX.md), and a grid container (GRID.md): a block,
    // or an atom, on the outside.
    FLOW_DISPLAY_FLEX,
    FLOW_DISPLAY_INLINE_FLEX,
    FLOW_DISPLAY_GRID,
    FLOW_DISPLAY_INLINE_GRID,
    // The element makes no box and its children are its parent's: `slot`,
    // and an SVG or MathML element, whose HTML descendants still render.
    FLOW_DISPLAY_CONTENTS,
    FLOW_DISPLAY_NONE,
} flow_display_t;

typedef enum { FLOW_GENERIC_SERIF = 0, FLOW_GENERIC_SANS, FLOW_GENERIC_MONO } flow_generic_t;

// A family name as the page wrote it: a slice of an attribute value, NOT
// terminated — a comma follows it in the tree.
typedef struct {
    const char *name;
    uint32_t len;
} flow_family_name_t;

// The names in order, then the generic a resolver falls back to. A
// resolver that matches no name takes the generic.
typedef struct {
    const flow_family_name_t *names;
    uint32_t count;
    flow_generic_t generic;
} flow_family_list_t;

typedef enum { FLOW_FONT_NORMAL = 0, FLOW_FONT_ITALIC } flow_font_style_t;

typedef enum {
    FLOW_BORDER_NONE = 0,
    FLOW_BORDER_HIDDEN,
    FLOW_BORDER_SOLID,
    FLOW_BORDER_INSET,
    FLOW_BORDER_OUTSET,
    FLOW_BORDER_GROOVE,
    FLOW_BORDER_RIDGE,
    FLOW_BORDER_DOTTED,
    FLOW_BORDER_DASHED,
    FLOW_BORDER_DOUBLE,
} flow_border_style_t;

typedef enum {
    FLOW_ALIGN_LEFT = 0,        // the initial value (start, in a left-to-right page)
    FLOW_ALIGN_RIGHT,
    FLOW_ALIGN_CENTER,
    FLOW_ALIGN_JUSTIFY,
    // The HTML alignments: text aligned as the plain value says, AND the
    // element's block-level descendants aligned with it — `<center>`
    // centres the table inside it, which `text-align: center` alone does
    // not. Inherited like any text-align, so the most deeply nested
    // aligner wins, which is the chapter's rule.
    FLOW_ALIGN_HTML_LEFT,
    FLOW_ALIGN_HTML_RIGHT,
    FLOW_ALIGN_HTML_CENTER,
    // `justify` on a cell, a row or a div: text justified, descendants
    // aligned LEFT, as the chapter says.
    FLOW_ALIGN_HTML_JUSTIFY,
} flow_text_align_t;

typedef enum {
    FLOW_VALIGN_BASELINE = 0,
    FLOW_VALIGN_SUB,
    FLOW_VALIGN_SUPER,
    FLOW_VALIGN_TOP,
    FLOW_VALIGN_TEXT_TOP,
    FLOW_VALIGN_MIDDLE,
    FLOW_VALIGN_BOTTOM,
    // `align=middle` or `align=center` on a picture: its vertical middle on
    // the parent's baseline, which is not CSS `middle` (that one adds half
    // an x-height).
    FLOW_VALIGN_HTML_MIDDLE,
} flow_vertical_align_t;

typedef enum {
    FLOW_WS_NORMAL = 0,
    FLOW_WS_PRE,
    FLOW_WS_NOWRAP,
    FLOW_WS_PRE_WRAP,
    FLOW_WS_PRE_LINE,               // spaces collapse, line breaks are kept
} flow_white_space_t;

// `line-height` as it inherits (CSS 2.1 § 10.8.1): `normal` is the face's
// own; a number is a factor of each element's own font size and inherits
// as the number; a length or a percentage computes to pixels and inherits
// as those.
typedef enum { FLOW_LINE_NORMAL = 0, FLOW_LINE_NUMBER, FLOW_LINE_PX } flow_line_height_kind_t;
typedef struct {
    flow_line_height_kind_t kind;
    int32_t value;                  // NUMBER: thousandths; PX: flow_unit_t
} flow_line_height_t;

typedef enum {
    FLOW_TRANSFORM_NONE = 0,
    FLOW_TRANSFORM_UPPERCASE,
    FLOW_TRANSFORM_LOWERCASE,
    FLOW_TRANSFORM_CAPITALIZE,
} flow_text_transform_t;

typedef enum {
    FLOW_LIST_DISC = 0,
    FLOW_LIST_CIRCLE,
    FLOW_LIST_SQUARE,
    FLOW_LIST_DECIMAL,
    FLOW_LIST_LOWER_ALPHA,
    FLOW_LIST_UPPER_ALPHA,
    FLOW_LIST_LOWER_ROMAN,
    FLOW_LIST_UPPER_ROMAN,
    FLOW_LIST_DISCLOSURE_CLOSED,
    FLOW_LIST_DISCLOSURE_OPEN,
    FLOW_LIST_NONE,
} flow_list_style_type_t;

typedef enum { FLOW_LIST_OUTSIDE = 0, FLOW_LIST_INSIDE } flow_list_style_position_t;

enum {
    FLOW_DECORATION_UNDERLINE = 1u << 0,
    FLOW_DECORATION_LINE_THROUGH = 1u << 1,
};

typedef enum { FLOW_VISIBLE = 0, FLOW_HIDDEN, FLOW_COLLAPSE } flow_visibility_t;
typedef enum { FLOW_SEPARATE = 0, FLOW_COLLAPSE_BORDERS } flow_border_collapse_t;
typedef enum { FLOW_CAPTION_TOP = 0, FLOW_CAPTION_BOTTOM } flow_caption_side_t;
typedef enum { FLOW_FLOAT_NONE = 0, FLOW_FLOAT_LEFT, FLOW_FLOAT_RIGHT } flow_float_t;
typedef enum { FLOW_CONTENT_BOX = 0, FLOW_BORDER_BOX } flow_box_sizing_t;
typedef enum { FLOW_REPEAT = 0, FLOW_REPEAT_X, FLOW_REPEAT_Y, FLOW_NO_REPEAT } flow_repeat_t;
typedef enum {
    FLOW_OVERFLOW_VISIBLE = 0,
    FLOW_OVERFLOW_HIDDEN,
    FLOW_OVERFLOW_CLIP,
    FLOW_OVERFLOW_SCROLL,
    FLOW_OVERFLOW_AUTO,
} flow_overflow_t;
typedef enum { FLOW_CLEAR_NONE = 0, FLOW_CLEAR_LEFT, FLOW_CLEAR_RIGHT, FLOW_CLEAR_BOTH } flow_clear_t;
// Flexbox 1 § 5 (FLEX.md): the main axis, and whether items wrap.
typedef enum {
    FLOW_FLEX_ROW = 0,
    FLOW_FLEX_ROW_REVERSE,
    FLOW_FLEX_COLUMN,
    FLOW_FLEX_COLUMN_REVERSE,
} flow_flex_direction_t;
typedef enum { FLOW_FLEX_NOWRAP = 0, FLOW_FLEX_WRAP, FLOW_FLEX_WRAP_REVERSE } flow_flex_wrap_t;
// Box Alignment 3's values, as `justify-content`, `align-items`,
// `align-self` and `align-content` compute them; each property takes the
// ones its grammar allows. NORMAL is every one's initial value but
// `align-self`'s, which is AUTO: its container's `align-items`.
typedef enum {
    FLOW_PLACE_NORMAL = 0,
    FLOW_PLACE_AUTO,
    FLOW_PLACE_STRETCH,
    FLOW_PLACE_FLEX_START,
    FLOW_PLACE_FLEX_END,
    FLOW_PLACE_CENTER,
    FLOW_PLACE_BASELINE,
    FLOW_PLACE_START,
    FLOW_PLACE_END,
    FLOW_PLACE_SELF_START,
    FLOW_PLACE_SELF_END,
    FLOW_PLACE_LEFT,
    FLOW_PLACE_RIGHT,
    FLOW_PLACE_SPACE_BETWEEN,
    FLOW_PLACE_SPACE_AROUND,
    FLOW_PLACE_SPACE_EVENLY,
} flow_place_t;
// Grid 2 § 7.2 (GRID.md): a track's sizing function at one end — a length
// or percentage (FIXED, in `len`), a flexible share (FR, `fr` in
// thousandths), or a keyword — and a track: its minimum and maximum, and
// `fit`, for fit-content(len), whose maximum is then `len` held to the
// content's max-content.
typedef enum {
    FLOW_TRACK_FIXED = 0,
    FLOW_TRACK_FR,
    FLOW_TRACK_AUTO,
    FLOW_TRACK_MIN_CONTENT,
    FLOW_TRACK_MAX_CONTENT,
} flow_track_kind_t;
typedef struct {
    flow_track_kind_t kind;
    flow_length_t len;
    int32_t fr;
} flow_breadth_t;
typedef struct {
    flow_breadth_t min, max;
    bool fit;
} flow_track_t;
// A track list, repeat(n) written out, and where its auto-repeated block is
// — `repeat_n` tracks from `repeat_at`, repeated as often as they fit, or
// none when `repeat_n` is 0; `repeat_fit` for auto-fit, which drops the
// repeated tracks no item is placed in. No tracks at all is `none` in a
// template and `auto` in grid-auto-rows and -columns.
typedef struct {
    const flow_track_t *tracks;
    int32_t n, repeat_at, repeat_n;
    bool repeat_fit;
} flow_tracks_t;
// A grid item's line on one axis (§ 8.3): auto, a line number (negative
// from the explicit grid's end), a span of tracks, or an area's name.
typedef enum {
    FLOW_GRID_LINE_AUTO = 0,
    FLOW_GRID_LINE_NUMBER,
    FLOW_GRID_LINE_SPAN,
    FLOW_GRID_LINE_NAME,
} flow_grid_line_kind_t;
typedef struct {
    flow_grid_line_kind_t kind;
    int32_t n;                      // NUMBER, SPAN
    const char *name;               // NAME: not terminated
    uint32_t name_len;
} flow_grid_line_t;
// A named area of grid-template-areas: its rows and columns, from 0 and
// the end not included.
typedef struct {
    const char *name;
    uint32_t name_len;
    int32_t row0, row1, col0, col1;
} flow_grid_area_t;
// CSS Position 3 § 2 (POSITION.md).
typedef enum {
    FLOW_POSITION_STATIC = 0,
    FLOW_POSITION_RELATIVE,
    FLOW_POSITION_ABSOLUTE,
    FLOW_POSITION_FIXED,
    FLOW_POSITION_STICKY,
} flow_position_t;

typedef struct {
    flow_display_t display;

    flow_family_list_t family;
    uint16_t font_weight;           // 1..1000: 400 normal, 700 bold
    flow_font_style_t font_style;
    flow_unit_t font_size;

    uint32_t color;                 // a colour (above)
    bool has_background;            // false = transparent
    uint32_t background;            // a colour
    // A picture behind the box from the page's own sheets (CSS Backgrounds
    // 3 § 3): its url() as written, not terminated, and the index in the
    // cascade's input of the sheet it was written in (-1 for a `style`
    // attribute), which the face resolves it against and fetches. NULL
    // for none. A `background` ATTRIBUTE's picture is libpage's list, and
    // a sheet's outranks it.
    const char *background_image;
    uint32_t background_image_len;
    int32_t background_sheet;
    flow_repeat_t background_repeat;
    // From the box's top-left corner: PX, or PERCENT of the room the
    // picture leaves (a picture at 100% sits against the far edge).
    flow_length_t background_position[2];

    flow_length_t margin[4];        // PX, PERCENT or AUTO
    flow_length_t padding[4];       // PX or PERCENT
    // CSS computes a border's width to 0 when its style is none or hidden,
    // so a width here is a width that will be drawn.
    flow_unit_t border_width[4];
    flow_border_style_t border_style[4];
    uint32_t border_color[4];       // colours; `currentColor` resolved
    flow_length_t width, height;    // AUTO, PX or PERCENT
    // The limits: PX or PERCENT, AUTO for none (`min-*: auto` is none too,
    // outside flex and grid). A percentage on a height limit binds
    // nothing, as a percentage height does not.
    flow_length_t min_width, max_width, min_height, max_height;
    // Whether width, height and their limits measure the content box or
    // the border box (CSS Sizing 3 § 4.1).
    flow_box_sizing_t box_sizing;
    // Computed as CSS Overflow 3 § 3 says: a visible or clip axis beside
    // one that scrolls or hides becomes auto or hidden.
    flow_overflow_t overflow_x, overflow_y;

    flow_text_align_t text_align;
    flow_vertical_align_t vertical_align;
    flow_white_space_t white_space;
    flow_line_height_t line_height;
    flow_length_t text_indent;      // PX or PERCENT: the first line's; AUTO reads as 0
    flow_text_transform_t text_transform;
    // This element's own decorations. CSS draws an ancestor's across its
    // descendants too, each in the colour of the element that drew it,
    // which is a fact libflow derives and not a property of the descendant.
    uint8_t text_decoration;
    flow_visibility_t visibility;

    flow_list_style_type_t list_style_type;
    flow_list_style_position_t list_style_position;

    flow_unit_t border_spacing[2];  // horizontal, vertical
    flow_border_collapse_t border_collapse;
    flow_caption_side_t caption_side;

    // Recorded so the struct agrees with the cascade about them; the first
    // cut lays a float out where it stands (LAYOUT.md § Booked).
    flow_float_t float_side;
    flow_clear_t clear;

    // Positioning (POSITION.md). The insets in CSS's side order: AUTO, PX,
    // or PERCENT of the containing block.
    flow_position_t position;
    flow_length_t inset[4];
    // `z-index`: an integer when `has_z_index`, else `auto`, the initial.
    bool has_z_index;
    int32_t z_index;
    // In thousandths: 1000 is opaque, the initial value. 0 is not painted,
    // and neither is anything inside it. What is between makes a stacking
    // context that a face composites WHOLE at this opacity — one GROUP,
    // not each thing in it (flow_visit_groups) — but for an inline box
    // and an inline-block, which are painted opaque (PILE3.md § Booked).
    uint16_t opacity;
    // `pointer-events: none` (CSS UI 4 § 5.1), inherited: flow_hit passes
    // through the box to whatever is under it, though a descendant that
    // sets `auto` is still hit. Zero is `auto`, the initial value.
    bool pointer_events_none;
    // A box that leaves the flow — absolute or fixed — has its `display`
    // BLOCKIFIED (CSS 2.1 § 9.7), and this says the display the page gave
    // was inline-level: its static position is where it would have stood
    // on the line, not below it. Not a property: the one fact of the
    // specified display the blockified one loses. A flex item is
    // blockified too (Flexbox 1 § 4), and says the same.
    bool specified_inline;

    // Flexible boxes (FLEX.md). A container's: its main axis, whether its
    // items wrap, and where its free space and its lines go. An item's: its
    // own cross alignment, its flex factors in thousandths, its basis
    // (AUTO, CONTENT, PX or PERCENT of the container's inner main size),
    // and its place among its siblings. A container's gaps between items
    // and between lines: PX or PERCENT, or AUTO for `normal`, which is 0.
    flow_flex_direction_t flex_direction;
    flow_flex_wrap_t flex_wrap;
    flow_place_t justify_content, align_items, align_self, align_content;
    int32_t flex_grow, flex_shrink;
    flow_length_t flex_basis;
    int32_t order;
    flow_length_t row_gap, column_gap;

    // Grids (GRID.md). A container's tracks, explicit and implicit, its
    // named areas — `grid_area_rows` by `grid_area_cols` of them in all —
    // and whether its auto-placement runs down columns, and packs back.
    // An item's four lines, and its place across its area (`justify-self`,
    // AUTO for its container's `justify-items`). The arrays are the
    // styles', and live as long as they do.
    flow_tracks_t grid_template_columns, grid_template_rows;
    flow_tracks_t grid_auto_columns, grid_auto_rows;
    const flow_grid_area_t *grid_areas;
    int32_t ngrid_areas, grid_area_rows, grid_area_cols;
    bool grid_auto_flow_column, grid_dense;
    flow_grid_line_t grid_row_start, grid_row_end, grid_column_start, grid_column_end;
    flow_place_t justify_items, justify_self;
} flow_style_t;

// ── What the face hands in ──────────────────────────────────────────────

typedef struct {
    void *ctx;
    // Fonts: the face's resolver; on the host, the fake backend. `families`
    // is a style's list — names as the page wrote them, then a generic.
    // The list it answers is BORROWED until the next call: libflow lays out
    // what it needs with one list before it asks again, and a run laid out
    // with the fonts retains them for as long as the run lives.
    os64_font_status_t (*fonts)(void *ctx, const flow_family_list_t *families,
                                bool bold, bool italic, uint32_t px,
                                os64_text_font_t *const **list, size_t *count,
                                os64_font_face_info_t *primary);
    // Replaced sizes: the face's oracle. False = unknown.
    bool (*replaced_size)(void *ctx, const os64_html_node_t *node, int32_t *w, int32_t *h);
    os64_text_context_t *text;      // the page context the runs are laid out on
    uint32_t viewport_font_px;      // `medium`; 16 unless the face says otherwise
    flow_generic_t default_generic; // the family a page that names none is drawn in
    uint32_t ink, link_ink, paper;  // opaque colours; the dumps name these, never print them
    // The page's own sheets, cascaded against this document (garb/cascade.h),
    // or NULL for none: their winners are computed after the Rendering
    // chapter and the presentational hints, which they outrank.
    const struct garb_cascade *cascade;
    // What each of the two arenas a page can MULTIPLY may hold, in bytes:
    // the boxes (an inline split round a block reopens every inline open
    // there) and the lines laid out from them, whose budget a table's
    // working memory shares (nested tables hold theirs at once, and
    // columns can be declared by the thousand). 0 is FLOW_ARENA_DEFAULT. The
    // budget is the caller's, as libhtml's max_arena_bytes is; a layout that
    // reaches it stops and says `incomplete`, and what it holds is real.
    size_t max_arena_bytes;
    // The viewport's height in CSS pixels, which is the initial containing
    // block's (CSS 2.1 § 10.1): as tall as the window, not the page, so
    // `position: absolute; bottom: 0` with no positioned ancestor sits at
    // the foot of the first screenful. A cascade (above) must have been
    // judged at this same height, or flow_layout refuses it: one height,
    // so `vh` and the initial containing block can never disagree — whole
    // pixels, so a face cascades at a whole height too. 0 for none known,
    // and the page's own height stands in.
    int32_t viewport_height;
    // Every box laid out as `static`, whatever the page positioned: the
    // page as it reads in document order, one keypress from the page as it
    // was designed to look (POSITION.md § What positioning costs).
    bool static_only;
} flow_env_t;

#define FLOW_ARENA_DEFAULT ((size_t)64 << 20)

// ── The laid-out tree ───────────────────────────────────────────────────

typedef enum {
    FLOW_BOX_BLOCK = 0,     // a block container (node NULL: anonymous)
    FLOW_BOX_REPLACED,      // a block-level replaced box: a frame, or a picture made a block
    FLOW_BOX_TABLE,         // the table's own box; its captions sit outside it
    FLOW_BOX_CAPTION,
    FLOW_BOX_COLUMN_GROUP,
    FLOW_BOX_COLUMN,
    FLOW_BOX_ROW_GROUP,
    FLOW_BOX_ROW,
    FLOW_BOX_CELL,
    FLOW_BOX_LINE,          // a line box
    FLOW_BOX_SPAN,          // one inline box's piece on one line
    FLOW_BOX_TEXT,          // one run of text
    FLOW_BOX_ATOMIC,        // a replaced inline, or an inline-block
    FLOW_BOX_MARKER,        // a list marker's text
} flow_box_kind_t;

typedef struct flow_box flow_box_t;
// A FRAME: what moves a box away from where the flow put it — a sticky
// box's push, or a scroll container's scroll — and what is needed to find
// where it is (flow_box_doc_offset): opaque, the door's own.
typedef struct flow_frame flow_frame_t;

// A point, or an offset: how far the page is scrolled, x and y.
typedef struct {
    int32_t x, y;
} flow_point_t;

// One box of the laid-out tree, read-only. Everything here is whole
// pixels, rounded once, by the painter's rule, from the layout's 26.6 — in
// DOCUMENT coordinates, x from the page's left and y from its top, but for
// a box in a fixed box's subtree (`fixed`), whose coordinates are the
// VIEWPORT's, since it does not move with the page. A box in a FRAME — a
// sticky box's subtree, or a scroll container's content — is where the
// flow put it, and is drawn moved by however far the page's scroll pushes
// the sticky box, or back by however far the container is scrolled.
// flow_box_doc_rect and its siblings answer any box in document
// coordinates, given the scroll: the one place that rule is written.
struct flow_box {
    flow_box_kind_t kind;
    const os64_html_node_t *node;   // NULL for an anonymous box
    const flow_style_t *style;
    // The border box; a TEXT's or MARKER's content area (its baseline less
    // its ascent, ascent plus descent tall); a SPAN's piece of its line.
    os64_gui_rect_t rect;
    // The rect joined with every descendant's: content may overflow its
    // box (a set height too small, a table that will not squash a word, a
    // word wider than its line), so a walk that prunes on `rect` would
    // miss what is drawn.
    os64_gui_rect_t overflow;
    int32_t baseline;               // LINE, TEXT, ATOMIC, MARKER: the line's
    // TEXT, MARKER: the fragment's run, owned by the tree, and its bytes.
    const os64_text_run_t *run;
    const char *text;
    uint32_t length;
    // TEXT: where those bytes are in the node's processed text — what a
    // selection maps a pixel back through — when the node is a TEXT node.
    // Under an ELEMENT they are text the page never wrote there (a `q`'s
    // marks, an `rt`'s parentheses, a picture's alt), and `begin` indexes
    // nothing of the node's.
    uint32_t begin;
    // What of this box may be drawn: the padding boxes of the ancestors
    // whose `overflow` clips, met together, on the axes they clip. When
    // `clipped` is false nothing clips it and `clip` means nothing. In a
    // frame `clip` holds only what clips inside the frame — what clips it
    // from outside stays where it is while it moves, a scroll container's
    // own padding box among them — so the whole of it is
    // flow_box_doc_clip's, and `clipped` says either.
    os64_gui_rect_t clip;
    bool clipped;
    uint8_t decoration;             // TEXT: FLOW_DECORATION_* drawn across it
    // TEXT: each drawn decoration's colour, the colour of the element that
    // drew it — an underline and a line-through may differ.
    uint32_t underline_color, line_through_color;
    int32_t link;                   // libpage's link this box is or sits in, or -1
    int32_t control;                // ATOMIC, REPLACED: libpage's control, or -1
    bool unfinished;                // layout stopped inside it (flow_incomplete)
    // A block-level box whose `position` is not static (a table's row,
    // group or column is laid out static: booked): it stays under its
    // parent, but it is `stacked`, and is in the positioned list
    // (flow_positioned); its tree ancestors' overflow rects leave it out,
    // and its clip is what its containing blocks allow, not its parent
    // (POSITION.md).
    bool positioned;
    // Painted as a layer of its own, in paint order, and never through its
    // tree ancestors: their walks skip it. A positioned box is; so is a
    // block-level box in the flow below full opacity, a stacking context
    // painted whole where CSS 2.1 Appendix E paints a positioned box of
    // z-index 0 — and that one stays in its parent's overflow rect, clip
    // and frame, since it has not left the flow.
    bool stacked;
    // In a `position: fixed` box's subtree, the fixed box included: its
    // rects, clip and baseline are viewport coordinates (above).
    bool fixed;
    // The innermost frame that moves this box, or NULL: the one its
    // containing blocks lead to, so a box in the flow moves with its parent
    // and an absolute one with its containing block, while a fixed box
    // inside a sticky one, the viewport's, does not move with it
    // (POSITION.md, P4). A sticky box's is its own; a scroll container's is
    // the frame it is in, and its content's is its scroll.
    const flow_frame_t *frame;
    // A scroll container's place in the tree's list of them
    // (flow_scroller), or -1: CSS Overflow 3 § 3's, any `overflow` but
    // `visible` and `clip` that is not the viewport's, so `hidden` too,
    // which a script and a fragment link may scroll and a person may not.
    int32_t scroller;
    // `opacity: 0` on it or on an element it is inside: flow_visit does not
    // hand it over. flow_hit still finds it, as a browser's pointer does,
    // unless it is `pointer-events: none`. A face drawing live widgets
    // draws a control's whatever its opacity — a control made invisible is
    // nearly always a custom checkbox's real input, whose styled stand-in
    // cannot follow a click here, and hiding the widget too would leave a
    // form nobody can use — unless the pointer cannot reach it
    // (flow_box_covered), as it cannot a hidden dialog's or search
    // overlay's (POSITION.md, rulings 6 and 9).
    bool unpainted;
    const flow_box_t *parent, *first, *next;
};

// ── The door ────────────────────────────────────────────────────────────

typedef struct flow_tree flow_tree_t;

// Styles, boxes and lays out the page at `width` CSS pixels. NULL when
// memory runs out outside the budget, or when there is no document, no
// environment, no text context or font resolver, the width is negative,
// or the cascade was judged at another viewport height than
// `viewport_height`; otherwise a tree whose `incomplete` says whether it is
// whole — a document libhtml refused partway, or a model libpage could not
// finish, is not, however it lays out. Every call is a whole rebuild
// (LAYOUT.md, ruling 2).
flow_tree_t *flow_layout(const os64_html_document_t *doc, const os64_page_t *model,
                         int32_t width, const flow_env_t *env);
void flow_free(flow_tree_t *tree);

// The page's size in whole pixels: its height, and its width — at least
// the width laid out at, more where a word or a table would not fit —
// each as far as every positioned box reaches, less what a clip on its
// containing-block chain cuts, and nothing above or left of the origin,
// which cannot be scrolled to.
int32_t flow_height(const flow_tree_t *tree);
int32_t flow_width(const flow_tree_t *tree);
bool flow_incomplete(const flow_tree_t *tree);

// The root box: the html element's. NULL for a page with nothing to lay out.
const flow_box_t *flow_root(const flow_tree_t *tree);

// What to add to a box's coordinates to have document ones: the scroll,
// for a box in a fixed subtree, the push of each sticky box it moves with
// at that scroll (CSS Position 3 § 3.4), and less the scroll position of
// each scroll container whose content it is in, else nothing. The one
// rule; the rect and clip functions below apply it.
flow_point_t flow_box_doc_offset(const flow_box_t *box, flow_point_t scroll);
os64_gui_rect_t flow_box_doc_rect(const flow_box_t *box, flow_point_t scroll);
// Its clip, meaningful when `clipped`: its own, met with what clips each
// frame it is in from outside that frame, each where its own frame is.
os64_gui_rect_t flow_box_doc_clip(const flow_box_t *box, flow_point_t scroll);

// ── Scroll containers (CSS Overflow 3 § 2.2; PILE3.md § Scrolling boxes) ─
//
// A scroll container's SCROLL POSITION is the one thing a face may change
// in a laid-out tree: how far its content is scrolled, which every rect
// and clip above, flow_visit and flow_hit answer by. Each starts at 0,0,
// and a new layout starts them all again, so a face that keeps them
// across one sets them again on the new tree, by node.

// The scroll containers, in tree order, each once.
int32_t flow_nscrollers(const flow_tree_t *tree);
const flow_box_t *flow_scroller(const flow_tree_t *tree, int32_t i);
// How far it can be scrolled on each axis: as far as its scrollable
// overflow reaches past its padding box — its content's overflow rects,
// what can be seen of the positioned boxes it moves (cut by the clips
// between them and it, through sticky boxes), and its own end padding past
// what is in its flow — or 0 where nothing reaches past.
flow_point_t flow_scroll_range(const flow_tree_t *tree, int32_t i);
flow_point_t flow_scroll_at(const flow_tree_t *tree, int32_t i);
// Scrolls it to `at`, held to [0, range] on each axis; answers where it
// went.
flow_point_t flow_scroll_set(flow_tree_t *tree, int32_t i, flow_point_t at);
// The scroll container `node` makes, or -1: what a face that keeps a
// position by element finds again in a new layout. Not flow_box_for's
// answer, which for an inline-block is the atom on the line and not the
// container under it.
int32_t flow_scroller_for(const flow_tree_t *tree, const os64_html_node_t *node);
// Scrolls every scroll container `box` is in so that `box` shows, innermost
// first, each asked to show `box` itself where the containers inside have
// moved it (CSSOM View's scroll-into-view walk), never the container inside
// it: its top at the container's top (a fragment's block start), and across
// as little as shows it (inline nearest), its left edge winning when it is
// wider than the view — as Firefox and Chrome reveal a fragment. Each is
// held to its range; the page's own scroll is the face's.
void flow_scroll_reveal(flow_tree_t *tree, const flow_box_t *box);
// The innermost scroll container whose scrolling moves `box`, or -1: its
// containing blocks', which a scroll container's own box is not — asked of
// one, this answers the container it is in. A wheel goes here when it is
// over a box the pointer reaches and that container can move no further.
int32_t flow_box_scroller(const flow_box_t *box);

// Whether the pointer cannot reach `box` at this scroll: flow_hit at its
// centre answers something that is neither it nor inside it — a box
// painted over it, or nothing, since it is hidden or lets the pointer
// through itself. A face that draws live widgets over the page draws a
// control's exactly when this is false (POSITION.md, ruling 9): a text
// field under a fixed header must not draw over the header and take its
// click, while a floating label or an icon in a field's corner, which the
// pointer passes or does not reach at the centre, leaves the field usable.
bool flow_box_covered(const flow_tree_t *tree, const flow_box_t *box, flow_point_t scroll);

// Every box that meets `viewport` but the LINE boxes, which paint nothing of
// their own (flow_hit can still answer one), and the unpainted ones, in
// painting order (CSS 2.1 Appendix E): each stacking context's own
// background, then its members of negative z-index, then its own content —
// the block-level boxes, their backgrounds and borders, then the inline
// content, spans before the text they sit behind, an atom's own content
// where the atom is, its stacked boxes skipped — then its members of
// z-index auto or 0 in tree order, then the positive ones, lowest first;
// a member that is a context painted whole in its turn, and one that is
// not painted by the same two walks over its own subtree. The root is the
// first context. A scroll container's content is walked where its scroll
// moves it. Pruned on OVERFLOW rects: a subtree off the
// viewport costs one test, and a box that meets it costs a test for each
// of its children, lines included. `viewport` is in document coordinates
// and `scroll` is where the page is scrolled to: the rect a face paints is
// often only the view's dirty part, from which the scroll cannot be told,
// and a fixed box meets it wherever the scroll puts it.
void flow_visit(const flow_tree_t *tree, os64_gui_rect_t viewport, flow_point_t scroll,
                void (*visit)(void *ctx, const flow_box_t *box), void *ctx);

// The same walk, told where each GROUP is: a stacking context painted below
// full opacity (flow_style_t.opacity), the root's included. `open` comes
// before the group's first box and `close` after its last, groups nesting;
// `bounds` holds everything the group paints, in document coordinates at
// this scroll — its own boxes' overflow rects and its members', each where
// its frame puts it — so a face can composite it whole: everything inside
// painted as usual, then the group laid over what was under it at its
// opacity (CSS Color 4 § 3.2). A face that blends nothing may leave `open`
// and `close` NULL, which is flow_visit.
typedef struct {
    void (*visit)(void *ctx, const flow_box_t *box);
    void (*open)(void *ctx, const flow_box_t *box, os64_gui_rect_t bounds);
    void (*close)(void *ctx, const flow_box_t *box);
    void *ctx;
} flow_visitor_t;

void flow_visit_groups(const flow_tree_t *tree, os64_gui_rect_t viewport, flow_point_t scroll,
                       const flow_visitor_t *visitor);

// The deepest box whose OWN rect holds (x, y), the last painted winning —
// the stacked boxes backwards, then the ordinary tree — whose clip holds
// the point, and that is drawn (`visibility: visible`) and not
// `pointer-events: none`; NULL for none. A scroll container's content is
// where its scroll moves it, and is not there outside its padding box.
// (x, y) is in document coordinates, the page scrolled to `scroll`. The face
// asks libpage what its node means, and a TEXT's run where in the text the
// pointer is (os64_text_hit).
const flow_box_t *flow_hit(const flow_tree_t *tree, int32_t x, int32_t y, flow_point_t scroll);

// The box under (x, y), in document coordinates, among everything in the
// flow — relative boxes included, out-of-flow ones not — whatever is drawn
// over it, and whatever its `pointer-events`: a place, not a target. What a
// scroll position is a property of: a face that keeps a node in place
// across a new layout anchors on this, or it follows an overlay wherever
// the new layout puts it. It walks the whole in-flow tree: once per
// layout, not per pointer move.
const flow_box_t *flow_hit_in_flow(const flow_tree_t *tree, int32_t x, int32_t y);

// The positioned boxes (flow_box_t.positioned), each once, in the order
// their content is painted (flow_visit's).
int32_t flow_npositioned(const flow_tree_t *tree);
const flow_box_t *flow_positioned(const flow_tree_t *tree, int32_t i);

// A node's first box — where a fragment link scrolls to, where a control's
// widget goes. NULL for a node with none (hidden, display none, or past
// where an incomplete layout stopped).
const flow_box_t *flow_box_for(const flow_tree_t *tree, const os64_html_node_t *node);

// The pictures and the controls that have a box, in tree order, each once:
// PLACEMENT — where a picture that arrived is drawn (its box, or its alt
// text's first run when it is laid out as text), where a control's widget
// goes. A picture that takes no space (a missing one with `alt=""`) has no
// box and is not here, so what to FETCH is libpage's list
// (os64_page_image), which names every picture the page does.
int32_t flow_nimages(const flow_tree_t *tree);
const flow_box_t *flow_image(const flow_tree_t *tree, int32_t i);

// Whether a replaced element drawn in `style` is sized by the page alone —
// a width, and a height in pixels — so that what the oracle answers
// (flow_env_t.replaced_size) cannot change its box. The rule the layout
// sizes by, for a face deciding whether an arrival needs a new layout:
// ask it of the element's box (flow_box_for), not of its attributes, which
// say what the page wrote and not what parsed. False for NULL.
bool flow_replaced_fixed(const flow_style_t *style);
int32_t flow_ncontrols(const flow_tree_t *tree);
const flow_box_t *flow_control(const flow_tree_t *tree, int32_t i);

// The laid-out tree as text, for the harness and a probe in the guest.
// Like snprintf: answers the length the whole dump needs, writes what fits.
int64_t flow_dump(const flow_tree_t *tree, char *out, size_t cap);

#pragma GCC visibility pop

#endif
