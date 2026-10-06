#ifndef GARB_VALUES_H
#define GARB_VALUES_H

// Values (GARB.md, G2b): what a declaration SAYS, checked against its
// property's grammar and turned into typed values — a shorthand into the
// longhands it sets. What a value COMPUTES to (an em against the font, a
// percentage against its containing block) is libflow's, which already
// computes the Rendering chapter's.
//
// A declaration that does not fit its property's grammar is INVALID, and
// the cascade drops it as if it were never written, so an earlier valid one
// still stands — the rule that lets a 2026 sheet write a fallback before a
// value an older browser cannot read. The one exception is a value holding
// var() or env(): whether it fits cannot be known until what it names is,
// so it is kept as written and judged per element (the cascade's, G2c).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "garb/garb.h"

#pragma GCC visibility push(default)

// ── The longhands libgarb reads ─────────────────────────────────────────

typedef enum {
    GARB_DISPLAY,
    GARB_COLOR,
    GARB_BACKGROUND_COLOR,
    GARB_BACKGROUND_IMAGE,
    GARB_BACKGROUND_REPEAT,
    GARB_BACKGROUND_POSITION_X,
    GARB_BACKGROUND_POSITION_Y,
    // cover or contain as a KEYWORD, or a LENGTH whose two items are the
    // width and the height, each a length-percentage or the KEYWORD auto.
    GARB_BACKGROUND_SIZE,
    GARB_BACKGROUND_ORIGIN,
    GARB_BACKGROUND_CLIP,
    GARB_MARGIN_TOP, GARB_MARGIN_RIGHT, GARB_MARGIN_BOTTOM, GARB_MARGIN_LEFT,
    GARB_PADDING_TOP, GARB_PADDING_RIGHT, GARB_PADDING_BOTTOM, GARB_PADDING_LEFT,
    GARB_BORDER_TOP_WIDTH, GARB_BORDER_RIGHT_WIDTH, GARB_BORDER_BOTTOM_WIDTH, GARB_BORDER_LEFT_WIDTH,
    GARB_BORDER_TOP_STYLE, GARB_BORDER_RIGHT_STYLE, GARB_BORDER_BOTTOM_STYLE, GARB_BORDER_LEFT_STYLE,
    GARB_BORDER_TOP_COLOR, GARB_BORDER_RIGHT_COLOR, GARB_BORDER_BOTTOM_COLOR, GARB_BORDER_LEFT_COLOR,
    GARB_WIDTH, GARB_HEIGHT, GARB_MIN_WIDTH, GARB_MAX_WIDTH, GARB_MIN_HEIGHT, GARB_MAX_HEIGHT,
    GARB_BOX_SIZING,
    GARB_FONT_FAMILY, GARB_FONT_SIZE, GARB_FONT_WEIGHT, GARB_FONT_STYLE, GARB_FONT_VARIANT,
    GARB_LINE_HEIGHT,
    GARB_TEXT_ALIGN, GARB_VERTICAL_ALIGN, GARB_WHITE_SPACE, GARB_TEXT_DECORATION_LINE,
    GARB_TEXT_INDENT, GARB_TEXT_TRANSFORM,
    GARB_VISIBILITY,
    GARB_LIST_STYLE_TYPE, GARB_LIST_STYLE_POSITION, GARB_LIST_STYLE_IMAGE,
    GARB_BORDER_SPACING, GARB_BORDER_COLLAPSE, GARB_CAPTION_SIDE,
    GARB_FLOAT, GARB_CLEAR,
    GARB_OVERFLOW_X, GARB_OVERFLOW_Y,
    GARB_POSITION,
    GARB_TOP, GARB_RIGHT, GARB_BOTTOM, GARB_LEFT,
    GARB_Z_INDEX, GARB_OPACITY,
    GARB_POINTER_EVENTS,
    GARB_FLEX_DIRECTION, GARB_FLEX_WRAP,
    GARB_JUSTIFY_CONTENT, GARB_ALIGN_ITEMS, GARB_ALIGN_SELF, GARB_ALIGN_CONTENT,
    GARB_FLEX_GROW, GARB_FLEX_SHRINK, GARB_FLEX_BASIS, GARB_ORDER,
    GARB_ROW_GAP, GARB_COLUMN_GAP,
    GARB_GRID_TEMPLATE_COLUMNS, GARB_GRID_TEMPLATE_ROWS, GARB_GRID_TEMPLATE_AREAS,
    GARB_GRID_AUTO_COLUMNS, GARB_GRID_AUTO_ROWS, GARB_GRID_AUTO_FLOW,
    GARB_GRID_COLUMN_START, GARB_GRID_COLUMN_END, GARB_GRID_ROW_START, GARB_GRID_ROW_END,
    GARB_JUSTIFY_ITEMS, GARB_JUSTIFY_SELF,
    // Backgrounds 3 § 5.1: each corner's horizontal and vertical radius.
    GARB_BORDER_TOP_LEFT_RADIUS, GARB_BORDER_TOP_RIGHT_RADIUS,
    GARB_BORDER_BOTTOM_RIGHT_RADIUS, GARB_BORDER_BOTTOM_LEFT_RADIUS,
    // Backgrounds 3 § 7.1 and Text Decoration 3 § 4: `none`, or a list.
    GARB_BOX_SHADOW, GARB_TEXT_SHADOW,
    // Images 3 § 5.3, with the spellings older pages wrote for crisp
    // pixels kept as they were written.
    GARB_IMAGE_RENDERING,
    GARB_CLIP,
    GARB_NPROPS
} garb_prop_t;

// The longhand's name, and whether it is inherited by default.
const char *garb_prop_name(garb_prop_t prop);
bool garb_prop_inherited(garb_prop_t prop);

// ── Typed values ────────────────────────────────────────────────────────

typedef enum {
    GARB_V_KEYWORD,         // `keyword`: lowercase, from libgarb's own table
    GARB_V_WIDE,            // a CSS-wide keyword: inherit, initial, unset, revert
    GARB_V_LENGTH,          // `number` in `unit`
    GARB_V_PERCENTAGE,      // `number` percent
    GARB_V_NUMBER,
    GARB_V_COLOR,
    GARB_V_URL,             // `text`, as written: resolved against the SHEET by whoever fetches
    GARB_V_STRING,          // `text`: a family name, a list marker
    GARB_V_CALC,            // `calc`: a length-percentage (or number) to work out at compute time
    // An image named by its function (`text`). A linear or radial gradient
    // carries itself in `items` (comma): items[0] its geometry, then its
    // stops. Linear: a NUMBER, the angle in degrees in [0, 360) (taken round
    // the turn in its own unit, so no unit overflows it), or a KEYWORD, "to top",
    // "to top right" and the rest. Radial: a KEYWORD "circle" or "ellipse"
    // whose items are its size — an extent KEYWORD, or a length-percentage
    // and a second one or "auto" — and its centre, x then y. A stop: a COLOR
    // whose items are its one position, or none (a stop written with two is
    // two stops); a hint: a length-percentage alone. Others (conic,
    // image-set) carry nothing.
    GARB_V_IMAGE,
    // A grid track list (Grid 2 § 7.2): its tracks in `items` — each a
    // LENGTH (in GARB_U_FR for a flexible one), a PERCENTAGE, a CALC, a
    // KEYWORD (auto, min-content, max-content) or a FUNCTION. Its line
    // names are read and not kept (GRID.md, decision 3).
    GARB_V_TRACKS,
    // A function a grammar keeps whole: `keyword` its name — minmax,
    // fit-content, repeat, and `span`, which is a keyword with its number
    // or name and is written so — and its arguments in `items`; repeat's
    // first is its count (a NUMBER, or the KEYWORD auto-fill or auto-fit)
    // and its second the TRACKS repeated.
    GARB_V_FUNCTION,
    // A background longhand written for more than one layer (Backgrounds 3
    // § 3.1): one value per layer in `items`, the top layer first. A list
    // of one is that one value, never this.
    GARB_V_LAYERS,
} garb_vkind_t;

typedef enum {
    GARB_U_PX, GARB_U_EM, GARB_U_REM, GARB_U_EX, GARB_U_CH, GARB_U_VW, GARB_U_VH, GARB_U_VMIN,
    GARB_U_VMAX, GARB_U_PT, GARB_U_PC, GARB_U_CM, GARB_U_MM, GARB_U_IN, GARB_U_Q,
    // A grid track's share of the free space (Grid 2 § 7.2.4), which only
    // a track list reads: no other length accepts it.
    GARB_U_FR,
} garb_unit_t;

// sRGB, channels 0..255 kept as written (Color 4 serializes 209.525 as
// that), alpha 0..1. `current` is currentColor, whose value is the
// element's `color` — a compute-time question.
typedef struct {
    double r, g, b, a;
    bool current;
} garb_color_t;

typedef struct garb_calc garb_calc_t;

typedef struct garb_val {
    garb_vkind_t kind;
    const char *keyword;    // KEYWORD, WIDE
    double number;          // LENGTH, PERCENTAGE, NUMBER
    garb_unit_t unit;       // LENGTH
    garb_color_t color;     // COLOR
    const char *text;       // URL, STRING (a grid area's or line's name too), IMAGE
    size_t len;
    const garb_calc_t *calc;
    // A list: font-family's names (`comma`), border-spacing's two lengths,
    // text-decoration-line's lines.
    const struct garb_val *items;
    int32_t nitems;
    bool comma;
} garb_val_t;

// calc(), min(), max(), clamp() (Values 3 and 4), as a tree: a SUM of
// terms, a PRODUCT, one of the three comparisons, or a LEAF — a number, a
// percentage or a length. Its type was checked when it was parsed.
typedef enum { GARB_CALC_LEAF, GARB_CALC_SUM, GARB_CALC_PRODUCT, GARB_CALC_MIN, GARB_CALC_MAX,
               GARB_CALC_CLAMP } garb_calc_op_t;

struct garb_calc {
    garb_calc_op_t op;
    garb_val_t leaf;                        // LEAF: a NUMBER, PERCENTAGE or LENGTH
    const garb_calc_t *const *args;         // the rest
    const bool *invert;                     // SUM: a term subtracted; PRODUCT: a divisor
    int32_t nargs;
};

// One longhand set by a declaration.
typedef struct {
    garb_prop_t prop;
    garb_val_t value;
} garb_set_t;

#define GARB_SETS_MAX 32    // the most longhands a shorthand here sets
// The most tracks one grid track list, or rows one grid-template-areas,
// is read with; a value with more is invalid.
#define GARB_TRACKS_MAX 256

// A declaration read against its property's grammar: the longhands it sets
// into `out` (a shorthand sets them all, the ones it leaves out to their
// initial values), how many answered; 0 for a property libgarb does not
// read; GARB_DECL_INVALID when the value does not fit the grammar; and
// GARB_DECL_HELD for a value holding var() or env(), which is not read
// here — the cascade reads it once they are replaced (garb_decl_has_var
// asks the same question first). `quirks` lets the few properties quirks
// mode allows take a length without a unit. Values that must live on (a
// family name, a url) point into `owner`'s arena.
#define GARB_DECL_INVALID (-1)
#define GARB_DECL_HELD    (-2)
int32_t garb_read_declaration(garb_parsed_t *owner, const garb_decl_t *decl, bool quirks,
                              garb_set_t *out);
bool garb_decl_has_var(const garb_decl_t *decl);
// Whether a declaration names a custom property (`--name`), whose value is
// its component values as written.
bool garb_decl_is_custom(const garb_decl_t *decl);

// One string of grid-template-areas cut into its cells (Grid 2 § 7.3.1):
// each a name — `text`, `len` its bytes — or a null cell (a run of dots,
// `len` 0); anything else in it makes the string invalid, and the answer
// -1. The count of cells, of which the first `cap` are written.
typedef struct {
    const char *text;
    uint32_t len;
} garb_area_cell_t;
int32_t garb_area_cells(const char *s, uint32_t len, garb_area_cell_t *out, int32_t cap);

// Just a colour (CSS Color 4 § 4-8), as component values: the suite's
// colour files, and anything else that holds exactly one.
bool garb_read_color(const garb_value_t *v, int32_t n, garb_color_t *out);

// A value written back in canonical text, for dumps and the harness. Like
// snprintf.
size_t garb_val_dump(const garb_val_t *v, char *out, size_t cap);

#pragma GCC visibility pop

#endif
