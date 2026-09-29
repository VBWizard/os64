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
bool is_delim_v(const garb_value_t *v, char ch);

// Where values that outlive the call are kept, and whether it ran out.
typedef struct {
    os64_arena_t *arena;
    bool short_of_memory;
} Arena;

enum { ACCEPT_LENGTH = 1, ACCEPT_PERCENT = 2, ACCEPT_NUMBER = 4 };

bool vc_dim(VCur *c, int accept, bool negative, bool quirky, Arena *a, garb_val_t *out);
bool vc_color(VCur *c, garb_val_t *out);
bool vc_url(VCur *c, garb_val_t *out);
bool vc_image(VCur *c, garb_val_t *out);
bool read_color_value(const garb_value_t *t, garb_color_t *out);
const garb_calc_t *read_calc(Arena *a, const garb_value_t *f, int accept, int *type);

#endif
