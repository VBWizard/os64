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
// THE DOCUMENT AND THE MODEL MUST OUTLIVE WHATEVER IS BUILT FROM THEM. A
// style points into the tree for the family names a page wrote; nothing
// is copied that can be pointed at.

#include "html/html.h"
#include "page/page.h"
#include "os64/font_backend.h"
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
} flow_length_kind_t;

typedef struct {
    flow_length_kind_t kind;
    int32_t value;
} flow_length_t;

// Box sides, in CSS's own order.
enum { FLOW_TOP = 0, FLOW_RIGHT = 1, FLOW_BOTTOM = 2, FLOW_LEFT = 3 };

// ── The computed style ──────────────────────────────────────────────────
//
// EVERY FIELD IS ONE CSS PROPERTY'S COMPUTED VALUE, in the property's own
// units and with its own initial value, because a cascade arriving later is
// a second producer of this same struct (LAYOUT.md § The ruling). A field
// is here when the first producer — the Rendering chapter and the
// presentational attributes — can set it; a property nothing sets yet
// arrives with the producer that sets it, and no field here changes its
// meaning when one does.

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
} flow_white_space_t;

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
typedef enum { FLOW_CLEAR_NONE = 0, FLOW_CLEAR_LEFT, FLOW_CLEAR_RIGHT, FLOW_CLEAR_BOTH } flow_clear_t;

typedef struct {
    flow_display_t display;

    flow_family_list_t family;
    uint16_t font_weight;           // 100..900: 400 normal, 700 bold
    flow_font_style_t font_style;
    flow_unit_t font_size;

    uint32_t color;                 // XRGB
    bool has_background;            // false = transparent
    uint32_t background;            // XRGB

    flow_length_t margin[4];        // PX, PERCENT or AUTO
    flow_length_t padding[4];       // PX or PERCENT
    // CSS computes a border's width to 0 when its style is none or hidden,
    // so a width here is a width that will be drawn.
    flow_unit_t border_width[4];
    flow_border_style_t border_style[4];
    uint32_t border_color[4];       // XRGB; `currentColor` resolved
    flow_length_t width, height;    // AUTO, PX or PERCENT

    flow_text_align_t text_align;
    flow_vertical_align_t vertical_align;
    flow_white_space_t white_space;
    // This element's own decorations. CSS draws an ancestor's across its
    // inline descendants too, in the ancestor's colour, which is a fact
    // layout derives and not a property of the descendant.
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
} flow_style_t;

// ── What the face hands in ──────────────────────────────────────────────

typedef struct {
    void *ctx;
    // Fonts: the face's resolver; on the host, the fake backend. `families`
    // is a style's list — names as the page wrote them, then a generic.
    os64_font_status_t (*fonts)(void *ctx, const flow_family_list_t *families,
                                bool bold, bool italic, uint32_t px,
                                os64_text_font_t *const **list, size_t *count,
                                os64_font_face_info_t *primary);
    // Replaced sizes: the face's oracle. False = unknown.
    bool (*replaced_size)(void *ctx, const os64_html_node_t *node, int32_t *w, int32_t *h);
    os64_text_context_t *text;      // the page context the runs are laid out on
    uint32_t viewport_font_px;      // `medium`; 16 unless the face says otherwise
    flow_generic_t default_generic; // the family a page that names none is drawn in
    uint32_t ink, link_ink, paper;  // XRGB; the dumps name these, never print them
} flow_env_t;

// ── The door ────────────────────────────────────────────────────────────

typedef struct flow_tree flow_tree_t;

// Styles, boxes and lays out the page at `width` CSS pixels. NULL on no
// memory only; otherwise a tree whose `incomplete` says whether it is
// whole. Every call is a whole rebuild (LAYOUT.md, ruling 2).
flow_tree_t *flow_layout(const os64_html_document_t *doc, const os64_page_t *model,
                         int32_t width, const flow_env_t *env);
void flow_free(flow_tree_t *tree);

// The page's size in whole pixels: its height, and its width — at least
// the width laid out at, more where a word or a table would not fit.
int32_t flow_height(const flow_tree_t *tree);
int32_t flow_width(const flow_tree_t *tree);
bool flow_incomplete(const flow_tree_t *tree);

// The laid-out tree as text, for the harness and a probe in the guest.
// Like snprintf: answers the length the whole dump needs, writes what fits.
int64_t flow_dump(const flow_tree_t *tree, char *out, size_t cap);

#pragma GCC visibility pop

#endif
