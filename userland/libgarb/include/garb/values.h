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
    // Transforms 1 § 4-5 and 2 § 5-6. `transform`: the KEYWORD none, or a
    // FUNCTIONS list. `transform-origin`: a LENGTH whose three items are x,
    // y (a length-percentage, its keywords as the percentages they are)
    // and z (a length). `translate`: none, or a LENGTH whose one to three
    // items are x, y (length-percentages) and z (a length). `scale`: none,
    // or a NUMBER whose one to three items are each a NUMBER or a
    // PERCENTAGE. `rotate`: none, an ANGLE about z, or a NUMBER whose four
    // items are an axis's x, y and z and the ANGLE.
    GARB_TRANSFORM, GARB_TRANSFORM_ORIGIN, GARB_TRANSLATE, GARB_ROTATE, GARB_SCALE,
    // Compatibility § 3.4.6.1: the colour a glyph is FILLED with, a COLOR
    // or the KEYWORD currentcolor. Its name is the prefixed one; there is
    // no other.
    GARB_TEXT_FILL_COLOR,
    // Animations 1 § 4 (PILE3.md § Animations): each a list, one item per
    // animation. `animation-name`: the KEYWORD none, or a STRING — the name
    // as written, whether an identifier or a string, since names are
    // compared codepoint for codepoint. `-duration`: a TIME, or the KEYWORD
    // auto (Animations 2), which a clock reads as 0s. `-delay`: a TIME.
    // `-timing-function`: an easing — the KEYWORD linear, ease, ease-in,
    // ease-out, ease-in-out, step-start or step-end, or a FUNCTION
    // cubic-bezier (four NUMBERs) or steps (a NUMBER, then its position
    // KEYWORD when one is written). `-iteration-count`: the KEYWORD
    // infinite or a NUMBER. The direction, play state and fill mode: a
    // KEYWORD.
    GARB_ANIMATION_NAME, GARB_ANIMATION_DURATION, GARB_ANIMATION_TIMING_FUNCTION,
    GARB_ANIMATION_ITERATION_COUNT, GARB_ANIMATION_DIRECTION, GARB_ANIMATION_PLAY_STATE,
    GARB_ANIMATION_DELAY, GARB_ANIMATION_FILL_MODE,
    // Will Change 1 § 2: the KEYWORD auto, or what it names, each a STRING
    // in lowercase (a list, as the animations' are).
    GARB_WILL_CHANGE,
    // CSS UI 4 § 5.1.1: the KEYWORD a pointer over the box is shaped as; the
    // pictures (urls, and image-set()s of urls and strings) and their
    // hotspots written before it are read against their grammar, and only
    // the keyword they fall back to is kept.
    GARB_CURSOR,
    // CSS UI 4 § 6.1: a KEYWORD.
    GARB_USER_SELECT,
    // CSS Text 3 § 5.1 and § 5.4, each a KEYWORD; `word-wrap` is the
    // second's legacy name.
    GARB_WORD_BREAK, GARB_OVERFLOW_WRAP,
    // CSS Text 3 § 7.2: the KEYWORD normal, or a LENGTH.
    GARB_LETTER_SPACING,
    // CSS Overflow 4 § 4.1's one-value form, clip or ellipsis: a KEYWORD.
    GARB_TEXT_OVERFLOW,
    // CSS UI 4 § 3.2-3.5: a width (a LENGTH, or the KEYWORD thin, medium or
    // thick), a style (a KEYWORD, a border's but hidden, or auto), a colour
    // (a COLOR, or the KEYWORD auto) and an offset (a LENGTH).
    GARB_OUTLINE_WIDTH, GARB_OUTLINE_STYLE, GARB_OUTLINE_COLOR, GARB_OUTLINE_OFFSET,
    // CSS Images 3 § 4.5-4.6: a KEYWORD, and a position as a LENGTH whose
    // two items are x and y, each a length-percentage, its keywords as the
    // percentages they are (`right 10px` a calc(100% - 10px)).
    GARB_OBJECT_FIT, GARB_OBJECT_POSITION,
    // CSS Sizing 4 § 4.1: the KEYWORD auto, or a RATIO.
    GARB_ASPECT_RATIO,
    // SVG solid paint values. Resource paints are retained for explicit refusal
    // by a renderer without paint servers; lengths may use SVG user units.
    GARB_FILL, GARB_STROKE, GARB_FILL_OPACITY, GARB_STROKE_OPACITY,
    GARB_FILL_RULE, GARB_STROKE_WIDTH, GARB_STROKE_LINECAP,
    GARB_STROKE_LINEJOIN, GARB_STROKE_MITERLIMIT,
    GARB_STROKE_DASHARRAY, GARB_VECTOR_EFFECT, GARB_COLOR_INTERPOLATION,
    GARB_NPROPS
} garb_prop_t;

// The longhand's name, and whether it is inherited by default.
const char *garb_prop_name(garb_prop_t prop);
bool garb_prop_inherited(garb_prop_t prop);
// Whether the Modern Web arc added it: a property so marked is known only
// to a reading whose `modern_off` is clear (garb_reading_t, garb_env_t), so
// the arc's switch (MODERN_WEB.md, rule 4) takes it away whole, as if
// libgarb had never read it.
bool garb_prop_modern(garb_prop_t prop);
// Every name a declaration may set a property by, each once, for whatever
// lists them (an inline style object's members): the longhands' in
// garb_prop_t's order, then the shorthands', the aliases' and the
// flow-relative names' (CSS Logical 1). NULL past
// the last. garb_prop_known says whether a reading with `modern_off` reads
// a name: a marked name, and a shorthand all of whose longhands are
// marked, are not read with it.
const char *garb_prop_name_at(int32_t k);
bool garb_prop_known(const char *name, size_t len, bool modern_off);

// ── Typed values ────────────────────────────────────────────────────────

typedef enum {
    GARB_V_KEYWORD,         // `keyword`: lowercase, from libgarb's own table
    GARB_V_WIDE,            // a CSS-wide keyword: inherit, initial, unset, revert
    GARB_V_LENGTH,          // `number` in `unit`
    GARB_V_PERCENTAGE,      // `number` percent
    GARB_V_NUMBER,
    GARB_V_COLOR,
    GARB_V_URL,             // `text`, as written: resolved against the SHEET by whoever fetches
    GARB_V_STRING,          // `text`: a family name, a list marker, an animation's name
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
    // A comma-separated list of two or more: a background longhand's
    // layers (Backgrounds 3 § 3.1), the top layer first, or an animation
    // longhand's animations in the order `animation-name` lists them —
    // one value per item in `items`. A list of one is that one value,
    // never this.
    GARB_V_LAYERS,
    // `number` in `unit`, one of the angle units (Values 4 § 7.1). Where a
    // grammar takes a unitless 0 for an angle it is read as 0deg.
    GARB_V_ANGLE,
    // Functions applied in order, left to right, in `items`: a transform
    // list (Transforms 1 § 4), each a FUNCTION whose `keyword` is its name
    // in lowercase and whose items are its arguments — LENGTH, PERCENTAGE,
    // NUMBER, CALC, ANGLE, or perspective()'s KEYWORD none.
    GARB_V_FUNCTIONS,
    // `number` in `unit`, GARB_U_S or GARB_U_MS (Values 4 § 7.3).
    GARB_V_TIME,
    // A <ratio> (Values 4 § 5.7): its width and height in `items`, two
    // NUMBERs, neither negative, a calc() worked out; `keyword` "auto" when
    // aspect-ratio wrote auto beside it, NULL otherwise.
    GARB_V_RATIO,
} garb_vkind_t;

typedef enum {
    GARB_U_PX, GARB_U_EM, GARB_U_REM, GARB_U_EX, GARB_U_CH, GARB_U_VW, GARB_U_VH, GARB_U_VMIN,
    GARB_U_VMAX, GARB_U_PT, GARB_U_PC, GARB_U_CM, GARB_U_MM, GARB_U_IN, GARB_U_Q,
    // A grid track's share of the free space (Grid 2 § 7.2.4), which only
    // a track list reads: no other length accepts it.
    GARB_U_FR,
    // An ANGLE's: no length accepts them.
    GARB_U_DEG, GARB_U_GRAD, GARB_U_RAD, GARB_U_TURN,
    // A TIME's.
    GARB_U_S, GARB_U_MS,
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
    double number;          // LENGTH, PERCENTAGE, NUMBER, ANGLE, TIME
    garb_unit_t unit;       // LENGTH, ANGLE, TIME
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
// The most functions one transform list is read with; a value with more is
// invalid.
#define GARB_TRANSFORMS_MAX 64

// The domain a page means (PILE3.md § Animations, both slices, Contracts).
// A number past its limit is held at the limit where it is first read,
// never refused: refusing would change which declaration wins, and that is
// the cascade's question, not arithmetic's. Within these limits what a page
// writes is worked in a double; what libflow builds from it — a transform
// list multiplied out — may leave one, and is answered where it is built.
//
// The domain bounds how large a number is, never how small: scale(1e-300)
// is read as written.
//
// A time, in milliseconds: at 1e12 ms (31 years) a double still resolves a
// tenth of a microsecond.
#define GARB_TIME_MS_MAX 1e12
// An iteration count (`infinite` is its own keyword): at 1e15 a double
// still counts every iteration, and a count times a time is finite, so a
// finite count is never read as an infinite one.
#define GARB_ITERATIONS_MAX 1e15
// A cubic-bezier()'s y controls. Its x controls are already in [0, 1].
#define GARB_EASING_Y_MAX 1e6
// A transform function's numbers (scale, matrix(), rotate3d()'s axis) and
// its angles, an angle in degrees whatever its unit: at 1e15 a double
// still resolves an eighth of a degree.
#define GARB_TRANSFORM_NUMBER_MAX 1e15
#define GARB_ANGLE_DEG_MAX 1e15

// `v` held to ±`limit`; a NaN, which only a calc() of infinities makes, is
// 0, as Values 4 censors a top-level calculation's NaN.
static inline double garb_hold(double v, double limit)
{
    return v > limit ? limit : v < -limit ? -limit : v == v ? v : 0;
}

// A declaration read against its property's grammar: the longhands it sets
// into `out` (a shorthand sets them all, the ones it leaves out to their
// initial values), how many answered; 0 for a property libgarb does not
// read; GARB_DECL_INVALID when the value does not fit the grammar; and
// GARB_DECL_HELD for a value holding var() or env(), which is not read
// here — the cascade reads it once they are replaced (garb_decl_has_var
// asks the same question first). Values that must live on (a family name,
// a url) point into `owner`'s arena.
#define GARB_DECL_INVALID (-1)
#define GARB_DECL_HELD    (-2)
// How a declaration is read. `quirks` lets the few properties quirks mode
// allows take a length without a unit. `modern_off` makes every name the
// Modern Web arc added (garb_prop_modern, and the shorthands and aliases
// marked beside them) one libgarb does not read, answered 0 as any such
// name is, and reads a shorthand as an engine without its marked longhands
// would: a part of its value only one of them takes makes it invalid.
// Zero for each is a standards-mode reading that knows them all.
typedef struct {
    bool quirks;
    bool modern_off;
} garb_reading_t;
int32_t garb_read_declaration(garb_parsed_t *owner, const garb_decl_t *decl, garb_reading_t how,
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
