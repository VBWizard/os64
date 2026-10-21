#ifndef GARB_VALUES_INTERNAL_H
#define GARB_VALUES_INTERNAL_H

// The seam between values.c (the primitives) and props.c (the grammars).

#include "internal.h"
#include "garb/values.h"

// A cursor over component values that steps over white space.
typedef struct {
    const garb_value_t *v;
    int32_t n, i;
} VCur;

void vc_skip(VCur *c);
const garb_value_t *vc_peek(VCur *c);
bool vc_done(VCur *c);
// The keyword next, if it is one of `allowed` (a NULL-ended list of
// lowercase words): the list's own spelling, and consumed.
const char *vc_keyword(VCur *c, const char *const *allowed);
bool ieq(const char *a, size_t alen, const char *b);
// Whether `text` is a generic family's keyword (Fonts 4 § 3.1.1), as
// `font-family` reads one and `@font-face` refuses one.
bool garb_generic_family(const char *text, size_t len);
// Whether `text` is a system font's keyword (Fonts 4 § 2.1.3), as the
// `font` shorthand reads one — a family of that name must be quoted.
bool garb_system_font(const char *text, size_t len);
// Whether one identifier of an unquoted family name is a CSS-wide keyword
// or `default`, which no family name may hold in any word (Fonts 4
// § 2.1.1) — for `font-family` and `@font-face` alike.
bool garb_family_word_reserved(const char *text, size_t len);
bool is_delim_v(const garb_value_t *v, char ch);

// Where values that outlive the call are kept, and whether it ran out.
typedef struct {
    os64_arena_t *arena;
    bool short_of_memory;
} Arena;

enum { ACCEPT_LENGTH = 1, ACCEPT_PERCENT = 2, ACCEPT_NUMBER = 4 };

bool vc_dim(VCur *c, int accept, bool negative, bool quirky, Arena *a, garb_val_t *out);
// An angle (Values 4 § 7.1), and a unitless 0 as 0deg where `zero` lets
// it be one. Not inside calc(), which has no angle type here.
bool vc_angle(VCur *c, bool zero, garb_val_t *out);
// A time (Values 4 § 7.3), in s or ms — never a unitless 0, which a time
// does not take — and not negative unless `negative` allows. Not inside
// calc(), which has no time type here.
bool vc_time(VCur *c, bool negative, garb_val_t *out);
bool vc_color(VCur *c, garb_val_t *out);
bool vc_url(VCur *c, garb_val_t *out);
bool vc_image(VCur *c, garb_val_t *out);
bool read_color_value(const garb_value_t *t, garb_color_t *out);
const garb_calc_t *read_calc(Arena *a, const garb_value_t *f, int accept, int *type);
// Whether a longhand's value is read but laid out as something else until
// its slice (props.c's kApproximated): not what @supports means.
bool garb_set_approximated(const garb_set_t *set);
// The same answer by name: the keyword or image function the page wrote
// that is laid out as something else, or NULL.
const char *garb_set_approximation(const garb_set_t *set);

// The longhands a property name sets — itself for a longhand, all of them
// for a shorthand — into `out` (GARB_SETS_MAX long). 0 for a name libgarb
// does not read; with `modern_off`, the names and longhands marked modern
// are not read (garb_reading_t).
int32_t prop_longhands(const char *name, size_t len, bool modern_off, garb_prop_t *out);

#endif
