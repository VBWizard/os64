// media.c — Media Queries Level 4: a query list against the viewport
// (garb/cascade.h).
//
// Level 4's logic has three values: a feature this engine does not know,
// or text it cannot read, is UNKNOWN rather than false, so `not (hover:
// maybe)` does not turn into true. A whole query that ends UNKNOWN is
// false.
//
// What the machine answers, and why: a screen, a mouse (fine pointer, can
// hover), 8 bits a colour channel, 1 dppx, a light scheme, no scripting,
// and reduced motion — yonder runs no CSS animations or transitions, and a
// page that knows so spends nothing on them. So every
// `@media (prefers-reduced-motion: reduce)` block applies, and some pages
// hide more than motion in one (a carousel, a video's autoplay).

#include "values_internal.h"
#include "garb/cascade.h"
#include "os64/str.h"

typedef enum { M_FALSE, M_TRUE, M_UNKNOWN } Tri;

static Tri tri_not(Tri t)
{
    return t == M_UNKNOWN ? M_UNKNOWN : t == M_TRUE ? M_FALSE : M_TRUE;
}

static Tri tri_and(Tri a, Tri b)
{
    if (a == M_FALSE || b == M_FALSE)
        return M_FALSE;
    return a == M_TRUE && b == M_TRUE ? M_TRUE : M_UNKNOWN;
}

static Tri tri_or(Tri a, Tri b)
{
    if (a == M_TRUE || b == M_TRUE)
        return M_TRUE;
    return a == M_FALSE && b == M_FALSE ? M_FALSE : M_UNKNOWN;
}

typedef struct {
    const garb_value_t *v;
    int32_t n, i;
    garb_env_t env;
} MCur;

static const garb_value_t *peek(MCur *c)
{
    while (c->i < c->n && c->v[c->i].kind == GARB_WHITESPACE)
        c->i++;
    return c->i < c->n ? &c->v[c->i] : NULL;
}

static bool word(const garb_value_t *t, const char *w)
{
    return t != NULL && t->kind == GARB_IDENT && ieq(t->text, t->len, w);
}

// A length in CSS pixels. em and rem are the initial font size's (16px),
// as a media query has no element to take one from.
static bool length_px(const garb_value_t *t, garb_env_t env, double *px)
{
    if (t->kind == GARB_NUMBER && t->number == 0) {
        *px = 0;
        return true;
    }
    if (t->kind != GARB_DIMENSION)
        return false;
    static const struct { const char *unit; double scale; int view; } kUnits[] = {
        {"px", 1, 0}, {"em", 16, 0}, {"rem", 16, 0}, {"ex", 8, 0}, {"ch", 8, 0},
        {"in", 96, 0}, {"cm", 96 / 2.54, 0}, {"mm", 96 / 25.4, 0}, {"q", 96 / 101.6, 0},
        {"pt", 96.0 / 72, 0}, {"pc", 16, 0}, {"vw", 0.01, 1}, {"vh", 0.01, 2},
        {"vmin", 0.01, 3}, {"vmax", 0.01, 4},
    };
    for (size_t k = 0; k < sizeof(kUnits) / sizeof(kUnits[0]); k++)
        if (ieq(t->unit, t->unit_len, kUnits[k].unit)) {
            double base = kUnits[k].view == 1 ? env.width
                        : kUnits[k].view == 2 ? env.height
                        : kUnits[k].view == 3 ? (env.width < env.height ? env.width : env.height)
                        : kUnits[k].view == 4 ? (env.width > env.height ? env.width : env.height)
                        : 1;
            *px = t->number * kUnits[k].scale * base;
            return true;
        }
    return false;
}

static bool resolution_dppx(const garb_value_t *t, double *dppx)
{
    if (t->kind != GARB_DIMENSION)
        return false;
    if (ieq(t->unit, t->unit_len, "dppx") || ieq(t->unit, t->unit_len, "x"))
        *dppx = t->number;
    else if (ieq(t->unit, t->unit_len, "dpi"))
        *dppx = t->number / 96.0;
    else if (ieq(t->unit, t->unit_len, "dpcm"))
        *dppx = t->number * 2.54 / 96.0;
    else
        return false;
    return true;
}

typedef enum { F_UNKNOWN, F_WIDTH, F_HEIGHT, F_ASPECT, F_RESOLUTION, F_COLOR, F_ZERO, F_WORDS } FKind;

typedef struct {
    const char *name;
    FKind kind;
    const char *answer;         // F_WORDS: what this machine is
    bool ranged;                // takes min-/max- and range syntax
} Feature;

static const Feature kFeatures[] = {
    {"width", F_WIDTH, NULL, true}, {"height", F_HEIGHT, NULL, true},
    {"device-width", F_WIDTH, NULL, true}, {"device-height", F_HEIGHT, NULL, true},
    {"aspect-ratio", F_ASPECT, NULL, true}, {"device-aspect-ratio", F_ASPECT, NULL, true},
    {"resolution", F_RESOLUTION, NULL, true}, {"color", F_COLOR, NULL, true},
    {"color-index", F_ZERO, NULL, true}, {"monochrome", F_ZERO, NULL, true},
    {"grid", F_ZERO, NULL, false},
    {"orientation", F_WORDS, NULL, false}, {"hover", F_WORDS, "hover", false},
    {"any-hover", F_WORDS, "hover", false}, {"pointer", F_WORDS, "fine", false},
    {"any-pointer", F_WORDS, "fine", false},
    {"prefers-color-scheme", F_WORDS, "light", false},
    {"prefers-reduced-motion", F_WORDS, "reduce", false},
    {"prefers-reduced-transparency", F_WORDS, "no-preference", false},
    {"prefers-contrast", F_WORDS, "no-preference", false},
    {"forced-colors", F_WORDS, "none", false}, {"inverted-colors", F_WORDS, "none", false},
    {"scripting", F_WORDS, "none", false}, {"update", F_WORDS, "fast", false},
    {"display-mode", F_WORDS, "browser", false}, {"dynamic-range", F_WORDS, "standard", false},
    {"color-gamut", F_WORDS, "srgb", false}, {"overflow-block", F_WORDS, "scroll", false},
    {"overflow-inline", F_WORDS, "scroll", false}, {"scan", F_WORDS, "progressive", false},
};

// This machine's value of a ranged feature, and a value written for it.
static bool actual(const Feature *f, garb_env_t env, double *out)
{
    switch (f->kind) {
    case F_WIDTH: *out = env.width; return true;
    case F_HEIGHT: *out = env.height; return true;
    case F_ASPECT: *out = env.height > 0 ? env.width / env.height : 0; return true;
    case F_RESOLUTION: *out = 1; return true;
    case F_COLOR: *out = 8; return true;
    case F_ZERO: *out = 0; return true;
    default: return false;
    }
}

// A value for a ranged feature, from `v` onwards: `a / b` for a ratio.
static bool written(const Feature *f, MCur *c, double *out)
{
    const garb_value_t *t = peek(c);
    if (t == NULL)
        return false;
    switch (f->kind) {
    case F_WIDTH:
    case F_HEIGHT:
        if (!length_px(t, c->env, out))
            return false;
        c->i++;
        return true;
    case F_RESOLUTION:
        if (!resolution_dppx(t, out))
            return false;
        c->i++;
        return true;
    case F_ASPECT: {
        if (t->kind != GARB_NUMBER || t->number < 0)
            return false;
        double a = t->number, b = 1;
        c->i++;
        const garb_value_t *slash = peek(c);
        if (is_delim_v(slash, '/')) {
            c->i++;
            const garb_value_t *d = peek(c);
            if (d == NULL || d->kind != GARB_NUMBER || d->number < 0)
                return false;
            b = d->number;
            c->i++;
        }
        *out = b > 0 ? a / b : 1e308;
        return true;
    }
    case F_COLOR:
    case F_ZERO:
        if (t->kind != GARB_NUMBER || !t->integer)
            return false;
        *out = t->number;
        c->i++;
        return true;
    default:
        return false;
    }
}

typedef enum { OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ } Op;

// `<`, `<=`, `>`, `>=` or `=`, as the tokenizer makes them: delimiters.
static bool comparison(MCur *c, Op *op)
{
    const garb_value_t *t = peek(c);
    if (t == NULL || t->kind != GARB_DELIM || t->len != 1)
        return false;
    char ch = t->text[0];
    if (ch != '<' && ch != '>' && ch != '=')
        return false;
    c->i++;
    bool eq = false;
    if (ch != '=' && c->i < c->n && is_delim_v(&c->v[c->i], '=')) {
        c->i++;
        eq = true;
    }
    *op = ch == '=' ? OP_EQ : ch == '<' ? (eq ? OP_LE : OP_LT) : (eq ? OP_GE : OP_GT);
    return true;
}

static bool compare(double a, Op op, double b)
{
    const double eps = 1e-9;
    switch (op) {
    case OP_LT: return a < b - eps;
    case OP_LE: return a <= b + eps;
    case OP_GT: return a > b + eps;
    case OP_GE: return a >= b - eps;
    case OP_EQ: return a >= b - eps && a <= b + eps;
    }
    return false;
}

static const Feature *feature_named(const char *s, size_t n)
{
    for (size_t k = 0; k < sizeof(kFeatures) / sizeof(kFeatures[0]); k++)
        if (ieq(s, n, kFeatures[k].name))
            return &kFeatures[k];
    return NULL;
}

// `( … )`: a feature in plain, boolean or range form. UNKNOWN for anything
// else — Level 4's <general-enclosed>.
static Tri feature(const garb_value_t *block, garb_env_t env)
{
    MCur c = {block->children, block->nchildren, 0, env};
    const garb_value_t *t = peek(&c);
    if (t == NULL)
        return M_UNKNOWN;
    // Range form with the value first: `600px <= width [< 900px]`.
    if (t->kind != GARB_IDENT) {
        MCur probe = c;
        const garb_value_t *name = NULL;
        // Find the feature name after the first value and comparison.
        for (int32_t k = c.i; k < c.n; k++)
            if (c.v[k].kind == GARB_IDENT) {
                name = &c.v[k];
                break;
            }
        const Feature *f = name != NULL ? feature_named(name->text, name->len) : NULL;
        if (f == NULL || !f->ranged)
            return M_UNKNOWN;
        double left, right, here = 0;
        Op op1, op2;
        if (!written(f, &probe, &left) || !comparison(&probe, &op1) || !word(peek(&probe), f->name))
            return M_UNKNOWN;
        probe.i++;
        actual(f, env, &here);
        bool ok = compare(left, op1, here);
        if (peek(&probe) != NULL) {
            if (!comparison(&probe, &op2) || !written(f, &probe, &right) || peek(&probe) != NULL)
                return M_UNKNOWN;
            // Both comparisons must point the same way.
            bool less1 = op1 == OP_LT || op1 == OP_LE, less2 = op2 == OP_LT || op2 == OP_LE;
            if (op1 == OP_EQ || op2 == OP_EQ || less1 != less2)
                return M_UNKNOWN;
            ok = ok && compare(here, op2, right);
        }
        return ok ? M_TRUE : M_FALSE;
    }
    // A name, perhaps with min- or max-.
    const char *s = t->text;
    size_t n = t->len;
    int bound = 0;
    if (n > 4 && ieq(s, 4, "min-")) {
        bound = -1;
        s += 4;
        n -= 4;
    } else if (n > 4 && ieq(s, 4, "max-")) {
        bound = 1;
        s += 4;
        n -= 4;
    }
    const Feature *f = feature_named(s, n);
    if (f == NULL || (bound != 0 && !f->ranged))
        return M_UNKNOWN;
    c.i++;
    const garb_value_t *after = peek(&c);
    if (after == NULL) {
        // Boolean form: the feature is not its zero or none.
        if (bound != 0)
            return M_UNKNOWN;
        double here;
        if (f->kind == F_WORDS) {
            // Orientation always has one; the rest are true unless this
            // machine's answer is `none` or `no-preference`.
            const char *a = f->answer;
            bool off = a != NULL && (os64_streq(a, "none") || os64_streq(a, "no-preference"));
            return off ? M_FALSE : M_TRUE;
        }
        return actual(f, env, &here) && here != 0 ? M_TRUE : M_FALSE;
    }
    if (after->kind == GARB_COLON) {
        c.i++;
        if (f->kind == F_WORDS) {
            const garb_value_t *w = peek(&c);
            if (w == NULL || w->kind != GARB_IDENT || bound != 0)
                return M_UNKNOWN;
            c.i++;
            if (peek(&c) != NULL)
                return M_UNKNOWN;
            if (f->answer == NULL) {    // orientation
                const char *o = env.height >= env.width ? "portrait" : "landscape";
                if (!word(w, "portrait") && !word(w, "landscape"))
                    return M_UNKNOWN;
                return word(w, o) ? M_TRUE : M_FALSE;
            }
            return word(w, f->answer) ? M_TRUE : M_FALSE;
        }
        double want, here = 0;
        if (!written(f, &c, &want) || peek(&c) != NULL)
            return M_UNKNOWN;
        actual(f, env, &here);
        bool ok = bound < 0 ? here >= want - 1e-9 : bound > 0 ? here <= want + 1e-9
                            : compare(here, OP_EQ, want);
        return ok ? M_TRUE : M_FALSE;
    }
    // Range form with the name first: `width >= 600px`.
    Op op;
    double want, here = 0;
    if (bound != 0 || !f->ranged || !comparison(&c, &op) || !written(f, &c, &want) ||
        peek(&c) != NULL)
        return M_UNKNOWN;
    actual(f, env, &here);
    return compare(here, op, want) ? M_TRUE : M_FALSE;
}

static Tri condition(MCur *c, bool allow_or);

// <media-in-parens>: a condition in parentheses, a feature, or anything
// else in brackets or a function, which is UNKNOWN.
static Tri in_parens(MCur *c)
{
    const garb_value_t *t = peek(c);
    if (t == NULL)
        return M_UNKNOWN;
    c->i++;
    if (t->kind == GARB_FUNCTION)
        return M_UNKNOWN;
    if (t->kind != GARB_BLOCK || t->open != '(')
        return M_UNKNOWN;
    // A nested condition, or a feature.
    MCur inner = {t->children, t->nchildren, 0, c->env};
    const garb_value_t *first = peek(&inner);
    if (first != NULL && (first->kind == GARB_BLOCK || word(first, "not"))) {
        Tri r = condition(&inner, true);
        return peek(&inner) == NULL ? r : M_UNKNOWN;
    }
    return feature(t, c->env);
}

// <media-condition>: `not` one, or a run joined all by `and` or all by `or`.
static Tri condition(MCur *c, bool allow_or)
{
    if (word(peek(c), "not")) {
        c->i++;
        return tri_not(in_parens(c));
    }
    Tri r = in_parens(c);
    const garb_value_t *t = peek(c);
    bool ands = word(t, "and"), ors = word(t, "or");
    if (ors && !allow_or)
        return M_UNKNOWN;
    while ((ands && word(peek(c), "and")) || (ors && word(peek(c), "or"))) {
        c->i++;
        Tri next = in_parens(c);
        r = ands ? tri_and(r, next) : tri_or(r, next);
    }
    return r;
}

// One <media-query>.
static bool query(MCur *c)
{
    const garb_value_t *t = peek(c);
    if (t == NULL)
        return false;
    bool negate = false;
    Tri r;
    if (t->kind == GARB_IDENT && !word(t, "not") && !word(t, "only")) {
        // A media type.
        static const char *const kNone[] = {"print", "speech", "tty", "tv", "projection",
                                            "handheld", "braille", "embossed", "aural", NULL};
        bool screen = word(t, "all") || word(t, "screen");
        bool known_no = false;
        for (int k = 0; kNone[k] != NULL; k++)
            known_no |= word(t, kNone[k]);
        if (!screen && !known_no && (word(t, "and") || word(t, "or")))
            return false;
        c->i++;
        r = screen ? M_TRUE : M_FALSE;
        if (word(peek(c), "and")) {
            c->i++;
            r = tri_and(r, condition(c, false));
        }
    } else if (word(t, "not") || word(t, "only")) {
        MCur look = *c;
        look.i++;
        const garb_value_t *after = peek(&look);
        if (after != NULL && after->kind == GARB_IDENT) {
            negate = word(t, "not");
            c->i = look.i;
            bool screen = word(after, "all") || word(after, "screen");
            c->i++;
            r = screen ? M_TRUE : M_FALSE;
            if (word(peek(c), "and")) {
                c->i++;
                r = tri_and(r, condition(c, false));
            }
        } else {
            r = condition(c, true);
        }
    } else {
        r = condition(c, true);
    }
    if (peek(c) != NULL)
        return false;
    if (r == M_UNKNOWN)
        return false;
    return negate ? r == M_FALSE : r == M_TRUE;
}

bool garb_media_matches(const garb_value_t *v, int32_t n, garb_env_t env)
{
    // Split at top-level commas; the list matches if any query does.
    int32_t start = 0;
    bool any_content = false;
    for (int32_t k = 0; k < n; k++)
        if (v[k].kind != GARB_WHITESPACE)
            any_content = true;
    if (!any_content)
        return true;
    for (int32_t k = 0; k <= n; k++) {
        if (k < n && v[k].kind != GARB_COMMA)
            continue;
        MCur c = {v + start, k - start, 0, env};
        if (query(&c))
            return true;
        start = k + 1;
    }
    return false;
}

bool garb_media_text_matches(const char *text, garb_env_t env)
{
    if (text == NULL)
        return true;
    garb_parsed_t r;
    bool ok = garb_parse_values(text, os64_strlen(text), &r) == GARB_OK &&
              garb_media_matches(r.values, r.nvalues, env);
    garb_free(&r);
    return ok;
}
