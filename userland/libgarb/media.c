// media.c — Media Queries Level 4: a query list against the viewport
// (garb/cascade.h).
//
// Level 4's logic has three values: a feature this engine does not know,
// or text it cannot read, is UNKNOWN rather than false, so `not (hover:
// maybe)` does not turn into true. A whole query that ends UNKNOWN is
// false.
//
// What the machine answers: a screen, a mouse (fine pointer, can hover), 8
// bits a colour channel, 1 dppx, and no scripting; and what the person
// asked for — a light or dark scheme, and full or reduced motion — from the
// env (garb_env_t), each the first unless they asked otherwise.

#include <float.h>

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

// Not `word`: built -masm=intel, a function GCC leaves out of line is
// named in the assembly, and a symbol named word reads as WORD there.
static bool is_word(const garb_value_t *t, const char *w)
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
    {"prefers-color-scheme", F_WORDS, "light", false},     // or `dark`: answered from the env
    {"prefers-reduced-motion", F_WORDS, "no-preference", false},   // or `reduce`: the env's
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

// What a word feature answers here: the machine's, or for the two the
// person chooses, what they chose.
static const char *answer_of(const Feature *f, garb_env_t env)
{
    if (os64_streq(f->name, "prefers-color-scheme") && env.dark)
        return "dark";
    if (os64_streq(f->name, "prefers-reduced-motion") && env.reduced_motion)
        return "reduce";
    return f->answer;
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
        if (!written(f, &probe, &left) || !comparison(&probe, &op1) || !is_word(peek(&probe), f->name))
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
            // Orientation always has one; the rest are true unless the
            // answer here is `none` or `no-preference`.
            const char *a = answer_of(f, env);
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
                if (!is_word(w, "portrait") && !is_word(w, "landscape"))
                    return M_UNKNOWN;
                return is_word(w, o) ? M_TRUE : M_FALSE;
            }
            return is_word(w, answer_of(f, env)) ? M_TRUE : M_FALSE;
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
    if (first != NULL && (first->kind == GARB_BLOCK || is_word(first, "not"))) {
        Tri r = condition(&inner, true);
        return peek(&inner) == NULL ? r : M_UNKNOWN;
    }
    return feature(t, c->env);
}

// <media-condition>: `not` one, or a run joined all by `and` or all by `or`.
static Tri condition(MCur *c, bool allow_or)
{
    if (is_word(peek(c), "not")) {
        c->i++;
        return tri_not(in_parens(c));
    }
    Tri r = in_parens(c);
    const garb_value_t *t = peek(c);
    bool ands = is_word(t, "and"), ors = is_word(t, "or");
    if (ors && !allow_or)
        return M_UNKNOWN;
    while ((ands && is_word(peek(c), "and")) || (ors && is_word(peek(c), "or"))) {
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
    if (t->kind == GARB_IDENT && !is_word(t, "not") && !is_word(t, "only")) {
        // A media type.
        static const char *const kNone[] = {"print", "speech", "tty", "tv", "projection",
                                            "handheld", "braille", "embossed", "aural", NULL};
        bool screen = is_word(t, "all") || is_word(t, "screen");
        bool known_no = false;
        for (int k = 0; kNone[k] != NULL; k++)
            known_no |= is_word(t, kNone[k]);
        if (!screen && !known_no && (is_word(t, "and") || is_word(t, "or")))
            return false;
        c->i++;
        r = screen ? M_TRUE : M_FALSE;
        if (is_word(peek(c), "and")) {
            c->i++;
            r = tri_and(r, condition(c, false));
        }
    } else if (is_word(t, "not") || is_word(t, "only")) {
        MCur look = *c;
        look.i++;
        const garb_value_t *after = peek(&look);
        if (after != NULL && after->kind == GARB_IDENT) {
            negate = is_word(t, "not");
            c->i = look.i;
            bool screen = is_word(after, "all") || is_word(after, "screen");
            c->i++;
            r = screen ? M_TRUE : M_FALSE;
            if (is_word(peek(c), "and")) {
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

// ── Serialization (CSSOM § Serializing media queries) ───────────────────
//
// A query is read for its SHAPE alone (Media Queries 4 § 3's grammar):
// whether a feature is one this machine knows changes what it evaluates to,
// not whether it is a query. Each function reads at `c` and, when `o` is
// not NULL, writes what it read; the list's serializer reads each query
// once to judge it and again to write it, so a query that turns out not to
// be one has written nothing and is written `not all`.

typedef struct {
    char *out;
    size_t cap, len;
} SOut;

static void s_put(SOut *o, const char *s, size_t n)
{
    if (o == NULL)
        return;
    for (size_t i = 0; i < n; i++, o->len++)
        if (o->len + 1 < o->cap)
            o->out[o->len] = s[i];
    if (o->cap > 0)
        o->out[o->len < o->cap ? o->len : o->cap - 1] = '\0';
}

static void s_puts(SOut *o, const char *s)
{
    s_put(o, s, os64_strlen(s));
}

static void s_lower(SOut *o, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char ch = s[i] >= 'A' && s[i] <= 'Z' ? (char)(s[i] - 'A' + 'a') : s[i];
        s_put(o, &ch, 1);
    }
}

static void s_number(SOut *o, double v)
{
    char b[DBL_MAX_10_EXP + 24];
    garb_css_number(v, b, sizeof(b));
    s_puts(o, b);
}

static void s_values(SOut *o, const garb_value_t *v, int32_t n);

// One component value written back as CSS: what a <general-enclosed> is
// kept as. A number and a unit are spelled canonically; the rest as the
// tokenizer read them.
static void s_value(SOut *o, const garb_value_t *v)
{
    switch (v->kind) {
    case GARB_IDENT: s_put(o, v->text, v->len); break;
    case GARB_FUNCTION:
        s_put(o, v->text, v->len);
        s_put(o, "(", 1);
        s_values(o, v->children, v->nchildren);
        s_put(o, ")", 1);
        break;
    case GARB_AT_KEYWORD: s_put(o, "@", 1); s_put(o, v->text, v->len); break;
    case GARB_HASH: s_put(o, "#", 1); s_put(o, v->text, v->len); break;
    case GARB_STRING:
        s_put(o, "\"", 1);
        for (uint32_t i = 0; i < v->len; i++) {
            if (v->text[i] == '"' || v->text[i] == '\\')
                s_put(o, "\\", 1);
            s_put(o, &v->text[i], 1);
        }
        s_put(o, "\"", 1);
        break;
    case GARB_URL: s_put(o, "url(", 4); s_put(o, v->text, v->len); s_put(o, ")", 1); break;
    case GARB_DELIM: s_put(o, v->text, v->len); break;
    case GARB_NUMBER: s_number(o, v->number); break;
    case GARB_PERCENTAGE: s_number(o, v->number); s_put(o, "%", 1); break;
    case GARB_DIMENSION: s_number(o, v->number); s_lower(o, v->unit, v->unit_len); break;
    case GARB_WHITESPACE: s_put(o, " ", 1); break;
    case GARB_COLON: s_put(o, ":", 1); break;
    case GARB_SEMICOLON: s_put(o, ";", 1); break;
    case GARB_COMMA: s_put(o, ",", 1); break;
    case GARB_BLOCK: {
        char open = v->open, close = open == '{' ? '}' : open == '[' ? ']' : ')';
        s_put(o, &open, 1);
        s_values(o, v->children, v->nchildren);
        s_put(o, &close, 1);
        break;
    }
    default: break;     // CDO, CDC and the bad tokens: none is <any-value>
    }
}

// A run of component values, the white space at its ends dropped and each
// run of it inside written as one space.
static void s_values(SOut *o, const garb_value_t *v, int32_t n)
{
    int32_t first = 0, last = n;
    while (first < last && v[first].kind == GARB_WHITESPACE)
        first++;
    while (last > first && v[last - 1].kind == GARB_WHITESPACE)
        last--;
    for (int32_t i = first; i < last; i++)
        if (v[i].kind != GARB_WHITESPACE || v[i - 1].kind != GARB_WHITESPACE)
            s_value(o, &v[i]);
}

// Whether a run is <any-value>: no bad string, bad url or unmatched
// closing bracket at any depth.
static bool any_value(const garb_value_t *v, int32_t n)
{
    for (int32_t i = 0; i < n; i++) {
        if (v[i].kind == GARB_BAD_STRING || v[i].kind == GARB_BAD_URL || v[i].kind == GARB_UNMATCHED)
            return false;
        if ((v[i].kind == GARB_BLOCK || v[i].kind == GARB_FUNCTION) &&
            !any_value(v[i].children, v[i].nchildren))
            return false;
    }
    return true;
}

// <mf-value>: a number, a dimension, an identifier, a ratio (`a / b`, both
// numbers), or a function such as calc(). An identifier is ASCII
// case-insensitive in a feature this machine knows, so it is written in
// lower case.
static bool s_mf_value(MCur *c, SOut *o)
{
    const garb_value_t *t = peek(c);
    if (t == NULL)
        return false;
    switch (t->kind) {
    case GARB_NUMBER: {
        c->i++;
        MCur look = *c;
        if (is_delim_v(peek(&look), '/')) {
            look.i++;
            const garb_value_t *d = peek(&look);
            if (d == NULL || d->kind != GARB_NUMBER)
                return false;
            look.i++;
            *c = look;
            s_number(o, t->number);
            s_puts(o, " / ");
            s_number(o, d->number);
            return true;
        }
        s_number(o, t->number);
        return true;
    }
    case GARB_DIMENSION:
    case GARB_PERCENTAGE:
    case GARB_FUNCTION:
        if (t->kind == GARB_FUNCTION && !any_value(t->children, t->nchildren))
            return false;
        c->i++;
        s_value(o, t);
        return true;
    case GARB_IDENT:
        c->i++;
        s_lower(o, t->text, t->len);
        return true;
    default:
        return false;
    }
}

// A comparison written back: `<`, `<=`, `>`, `>=` or `=`, a space each side.
static const char *op_text(Op op)
{
    switch (op) {
    case OP_LT: return " < ";
    case OP_LE: return " <= ";
    case OP_GT: return " > ";
    case OP_GE: return " >= ";
    default: return " = ";
    }
}

// Whether an identifier names a feature this machine knows, `min-` and
// `max-` forms of a ranged one included.
static bool known_name(const garb_value_t *t)
{
    if (t == NULL || t->kind != GARB_IDENT)
        return false;
    const char *s = t->text;
    size_t n = t->len;
    bool bounded = n > 4 && (ieq(s, 4, "min-") || ieq(s, 4, "max-"));
    const Feature *f = feature_named(bounded ? s + 4 : s, bounded ? n - 4 : n);
    return f != NULL && (!bounded || f->ranged);
}

// <media-feature>, in a block's contents: `name`, `name: value`, or a range
// — `name < value`, `value < name`, `value < name < value`. Only a name this
// machine knows is read as one; any other is <general-enclosed>, kept as
// written.
static bool s_feature(const garb_value_t *block, SOut *o)
{
    MCur c = {.v = block->children, .n = block->nchildren};
    const garb_value_t *t = peek(&c);
    if (known_name(t)) {
        c.i++;
        Op op;
        MCur look = c;
        if (peek(&look) == NULL) {
            s_lower(o, t->text, t->len);
            return true;
        }
        if (peek(&look)->kind == GARB_COLON) {
            look.i++;
            if (!s_mf_value(&look, NULL) || peek(&look) != NULL)
                return false;
            s_lower(o, t->text, t->len);
            s_puts(o, ": ");
            look = c;
            peek(&look);
            look.i++;
            s_mf_value(&look, o);
            return true;
        }
        if (!comparison(&look, &op) || !s_mf_value(&look, NULL) || peek(&look) != NULL)
            return false;
        s_lower(o, t->text, t->len);
        look = c;
        comparison(&look, &op);
        s_puts(o, op_text(op));
        s_mf_value(&look, o);
        return true;
    }
    // A value first: the name after it, then perhaps another value.
    MCur look = c;
    Op op1, op2;
    if (!s_mf_value(&look, NULL) || !comparison(&look, &op1) || !known_name(peek(&look)))
        return false;
    const garb_value_t *name = peek(&look);
    look.i++;
    bool two = peek(&look) != NULL;
    if (two && (!comparison(&look, &op2) || !s_mf_value(&look, NULL) || peek(&look) != NULL))
        return false;
    // Both comparisons point the same way, and neither is `=`.
    if (two && (op1 == OP_EQ || op2 == OP_EQ ||
                (op1 == OP_LT || op1 == OP_LE) != (op2 == OP_LT || op2 == OP_LE)))
        return false;
    look = c;
    s_mf_value(&look, o);
    comparison(&look, &op1);
    s_puts(o, op_text(op1));
    s_lower(o, name->text, name->len);
    peek(&look);
    look.i++;
    if (two) {
        comparison(&look, &op2);
        s_puts(o, op_text(op2));
        s_mf_value(&look, o);
    }
    return true;
}

static bool s_condition(MCur *c, bool allow_or, SOut *o);

// <media-in-parens>: a condition in brackets, a feature, or
// <general-enclosed> — a function, or brackets holding <any-value> — which
// is a query's shape whatever it holds.
static bool s_in_parens(MCur *c, SOut *o)
{
    const garb_value_t *t = peek(c);
    if (t == NULL)
        return false;
    if (t->kind == GARB_FUNCTION) {
        if (!any_value(t->children, t->nchildren))
            return false;
        c->i++;
        s_value(o, t);
        return true;
    }
    if (t->kind != GARB_BLOCK || t->open != '(' || !any_value(t->children, t->nchildren))
        return false;
    c->i++;
    s_put(o, "(", 1);
    if (!s_feature(t, NULL)) {
        MCur inner = {.v = t->children, .n = t->nchildren};
        MCur probe = inner;
        if (s_condition(&probe, true, NULL) && peek(&probe) == NULL)
            s_condition(&inner, true, o);
        else
            s_values(o, t->children, t->nchildren);
    } else {
        s_feature(t, o);
    }
    s_put(o, ")", 1);
    return true;
}

// <media-condition>, or <media-condition-without-or>: `not` and one, or a
// run joined all by `and` or all by `or`.
static bool s_condition(MCur *c, bool allow_or, SOut *o)
{
    if (is_word(peek(c), "not")) {
        c->i++;
        s_puts(o, "not ");
        return s_in_parens(c, o);
    }
    if (!s_in_parens(c, o))
        return false;
    const garb_value_t *t = peek(c);
    bool ands = is_word(t, "and"), ors = allow_or && is_word(t, "or");
    while ((ands && is_word(peek(c), "and")) || (ors && is_word(peek(c), "or"))) {
        c->i++;
        s_puts(o, ands ? " and " : " or ");
        if (!s_in_parens(c, o))
            return false;
    }
    return true;
}

// Whether an identifier may be a media type: not one of the words the
// grammar keeps for itself.
static bool media_type(const garb_value_t *t)
{
    return t != NULL && t->kind == GARB_IDENT && !is_word(t, "not") && !is_word(t, "only") &&
           !is_word(t, "and") && !is_word(t, "or") && !is_word(t, "layer");
}

// One <media-query>, CSSOM's way: `not` or `only` as written, the type in
// lower case, and `all and` left out when nothing negates it.
static bool s_query(MCur *c, SOut *o)
{
    const garb_value_t *t = peek(c);
    if (t == NULL)
        return false;
    const garb_value_t *modifier = NULL, *type = NULL;
    if (is_word(t, "not") || is_word(t, "only")) {
        MCur look = *c;
        look.i++;
        if (media_type(peek(&look))) {
            modifier = t;
            c->i = look.i;
            t = peek(c);
        } else if (is_word(t, "only")) {
            return false;
        }
    }
    if (modifier != NULL || media_type(t)) {
        type = t;
        c->i++;
        bool all = is_word(type, "all") && modifier == NULL;
        if (modifier != NULL) {
            s_lower(o, modifier->text, modifier->len);
            s_put(o, " ", 1);
        }
        if (is_word(peek(c), "and")) {
            c->i++;
            if (!all) {
                s_lower(o, type->text, type->len);
                s_puts(o, " and ");
            }
            if (!s_condition(c, false, o))
                return false;
        } else {
            s_lower(o, type->text, type->len);
        }
    } else if (!s_condition(c, true, o)) {
        return false;
    }
    return peek(c) == NULL;
}

size_t garb_media_serialize(const garb_value_t *v, int32_t n, char *out, size_t cap)
{
    SOut o = {out, cap, 0};
    if (cap > 0)
        out[0] = '\0';
    bool any_content = false;
    for (int32_t k = 0; k < n; k++)
        if (v[k].kind != GARB_WHITESPACE)
            any_content = true;
    if (!any_content)
        return 0;
    int32_t start = 0;
    for (int32_t k = 0; k <= n; k++) {
        if (k < n && v[k].kind != GARB_COMMA)
            continue;
        if (start > 0)
            s_puts(&o, ", ");
        MCur judge = {.v = v + start, .n = k - start};
        if (s_query(&judge, NULL)) {
            MCur write = {.v = v + start, .n = k - start};
            s_query(&write, &o);
        } else {
            s_puts(&o, "not all");
        }
        start = k + 1;
    }
    return o.len;
}

size_t garb_media_text_serialize(const char *text, char *out, size_t cap)
{
    if (cap > 0)
        out[0] = '\0';
    if (text == NULL)
        return 0;
    // A list read short (memory ran out partway, or brackets nested past
    // GARB_DEPTH_MAX lost their contents) is not the text's, and its
    // serialization would be a different list's.
    garb_parsed_t r;
    if (garb_parse_values(text, os64_strlen(text), &r) != GARB_OK || r.incomplete) {
        garb_free(&r);
        return (size_t)-1;
    }
    size_t length = garb_media_serialize(r.values, r.nvalues, out, cap);
    garb_free(&r);
    return length;
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
