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
    GARB_V_IMAGE,           // an image this library does not draw yet (a gradient): `text` is its function
} garb_vkind_t;

typedef enum {
    GARB_U_PX, GARB_U_EM, GARB_U_REM, GARB_U_EX, GARB_U_CH, GARB_U_VW, GARB_U_VH, GARB_U_VMIN,
    GARB_U_VMAX, GARB_U_PT, GARB_U_PC, GARB_U_CM, GARB_U_MM, GARB_U_IN, GARB_U_Q,
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
    const char *text;       // URL, STRING, IMAGE
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

// A declaration read against its property's grammar: the longhands it sets
// into `out` (a shorthand sets them all, the ones it leaves out to their
// initial values), how many answered, 0 for a property libgarb does not
// read, -1 when the value is INVALID. `quirks` lets the few properties
// quirks mode allows take a length without a unit. Values that must live
// on (a family name, a url) point into `owner`'s arena. A value holding
// var() or env() is not read here: garb_decl_has_var says so, and the
// cascade reads it once they are replaced.
int32_t garb_read_declaration(garb_parsed_t *owner, const garb_decl_t *decl, bool quirks,
                              garb_set_t *out);
bool garb_decl_has_var(const garb_decl_t *decl);
// Whether a declaration names a custom property (`--name`), whose value is
// its component values as written.
bool garb_decl_is_custom(const garb_decl_t *decl);

// Just a colour (CSS Color 4 § 4-8), as component values: the suite's
// colour files, and anything else that holds exactly one.
bool garb_read_color(const garb_value_t *v, int32_t n, garb_color_t *out);

// A value written back in canonical text, for dumps and the harness. Like
// snprintf.
size_t garb_val_dump(const garb_val_t *v, char *out, size_t cap);

#pragma GCC visibility pop

#endif
