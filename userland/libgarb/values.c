// values.c — the primitives a property's grammar is built from (garb/values.h):
// lengths and percentages, numbers, colours (CSS Color 4 § 4-8), urls and
// images, and calc() with min(), max() and clamp() (Values 3 § 8, Values 4).

#include "values_internal.h"
#include "os64/mem.h"
#include "os64/str.h"

// ── Reading component values ────────────────────────────────────────────

void vc_skip(VCur *c)
{
    while (c->i < c->n && c->v[c->i].kind == GARB_WHITESPACE)
        c->i++;
}

const garb_value_t *vc_peek(VCur *c)
{
    vc_skip(c);
    return c->i < c->n ? &c->v[c->i] : NULL;
}

bool vc_done(VCur *c)
{
    return vc_peek(c) == NULL;
}

bool ieq(const char *a, size_t alen, const char *b)
{
    size_t n = os64_strlen(b);
    if (alen != n)
        return false;
    for (size_t i = 0; i < n; i++) {
        char x = a[i];
        if (x >= 'A' && x <= 'Z')
            x = (char)(x - 'A' + 'a');
        if (x != b[i])
            return false;
    }
    return true;
}

const char *vc_keyword(VCur *c, const char *const *allowed)
{
    const garb_value_t *t = vc_peek(c);
    if (t == NULL || t->kind != GARB_IDENT)
        return NULL;
    for (int k = 0; allowed[k] != NULL; k++)
        if (ieq(t->text, t->len, allowed[k])) {
            c->i++;
            return allowed[k];
        }
    return NULL;
}

bool is_delim_v(const garb_value_t *v, char ch)
{
    return v != NULL && v->kind == GARB_DELIM && v->len == 1 && v->text[0] == ch;
}

// ── Lengths and numbers ─────────────────────────────────────────────────

static bool unit_of(const char *s, size_t n, garb_unit_t *out)
{
    static const struct { const char *name; garb_unit_t unit; } kUnits[] = {
        {"px", GARB_U_PX}, {"em", GARB_U_EM}, {"rem", GARB_U_REM}, {"ex", GARB_U_EX},
        {"ch", GARB_U_CH}, {"vw", GARB_U_VW}, {"vh", GARB_U_VH}, {"vmin", GARB_U_VMIN},
        {"vmax", GARB_U_VMAX}, {"pt", GARB_U_PT}, {"pc", GARB_U_PC}, {"cm", GARB_U_CM},
        {"mm", GARB_U_MM}, {"in", GARB_U_IN}, {"q", GARB_U_Q},
        // Values 4's small, large and dynamic viewports differ only where a
        // browser's own toolbars come and go, and yonder's never do: all
        // three are the viewport.
        {"svw", GARB_U_VW}, {"lvw", GARB_U_VW}, {"dvw", GARB_U_VW},
        {"svh", GARB_U_VH}, {"lvh", GARB_U_VH}, {"dvh", GARB_U_VH},
        {"svmin", GARB_U_VMIN}, {"lvmin", GARB_U_VMIN}, {"dvmin", GARB_U_VMIN},
        {"svmax", GARB_U_VMAX}, {"lvmax", GARB_U_VMAX}, {"dvmax", GARB_U_VMAX},
        // Containment 3's container units, with no container queries here,
        // fall back as the specification says for an element with no
        // eligible container: to the small viewport's.
        {"cqw", GARB_U_VW}, {"cqi", GARB_U_VW}, {"cqh", GARB_U_VH}, {"cqb", GARB_U_VH},
        {"cqmin", GARB_U_VMIN}, {"cqmax", GARB_U_VMAX},
    };
    for (size_t k = 0; k < sizeof(kUnits) / sizeof(kUnits[0]); k++)
        if (ieq(s, n, kUnits[k].name)) {
            *out = kUnits[k].unit;
            return true;
        }
    return false;
}

static bool calc_function(const garb_value_t *t)
{
    return t != NULL && t->kind == GARB_FUNCTION &&
           (ieq(t->text, t->len, "calc") || ieq(t->text, t->len, "min") ||
            ieq(t->text, t->len, "max") || ieq(t->text, t->len, "clamp"));
}

// One length, percentage or number, as `accept` allows. `quirky` lets a
// bare number stand for pixels (quirks mode, and only where it says so).
bool vc_dim(VCur *c, int accept, bool negative, bool quirky, Arena *a, garb_val_t *out)
{
    const garb_value_t *t = vc_peek(c);
    if (t == NULL)
        return false;
    os64_memset(out, 0, sizeof(*out));
    if (calc_function(t)) {
        int got = 0;
        const garb_calc_t *calc = read_calc(a, t, accept, &got);
        if (calc == NULL)
            return false;
        out->kind = GARB_V_CALC;
        out->calc = calc;
        c->i++;
        return true;
    }
    if (t->kind == GARB_DIMENSION && (accept & ACCEPT_LENGTH)) {
        if (!unit_of(t->unit, t->unit_len, &out->unit) || (!negative && t->number < 0))
            return false;
        out->kind = GARB_V_LENGTH;
        out->number = t->number;
    } else if (t->kind == GARB_PERCENTAGE && (accept & ACCEPT_PERCENT)) {
        if (!negative && t->number < 0)
            return false;
        out->kind = GARB_V_PERCENTAGE;
        out->number = t->number;
    } else if (t->kind == GARB_NUMBER && (accept & ACCEPT_NUMBER)) {
        if (!negative && t->number < 0)
            return false;
        out->kind = GARB_V_NUMBER;
        out->number = t->number;
    } else if (t->kind == GARB_NUMBER && (accept & ACCEPT_LENGTH) &&
               (t->number == 0 || quirky)) {
        // Zero needs no unit (Values 3 § 5.1.1); quirks mode lets any
        // number be pixels where the property says.
        if (!negative && t->number < 0)
            return false;
        out->kind = GARB_V_LENGTH;
        out->unit = GARB_U_PX;
        out->number = t->number;
    } else {
        return false;
    }
    c->i++;
    return true;
}

// ── calc() (Values 3 § 8, Values 4 § 10) ────────────────────────────────

typedef struct {
    const garb_value_t *v;
    int32_t n, i;
    Arena *a;
    int depth;
} CalcCur;

// A node's type, as Values 4 § 10.8 tracks it: a number, or a
// length-percentage (a length, a percentage, or a mix of both).
enum { T_NUMBER = 1, T_LP = 2 };

static const garb_value_t *cc_peek(CalcCur *c)
{
    while (c->i < c->n && c->v[c->i].kind == GARB_WHITESPACE)
        c->i++;
    return c->i < c->n ? &c->v[c->i] : NULL;
}

static garb_calc_t *node(Arena *a, garb_calc_op_t op)
{
    garb_calc_t *n = os64_arena_calloc(a->arena, 1, sizeof(*n));
    if (n == NULL)
        a->short_of_memory = true;
    else
        n->op = op;
    return n;
}

static bool add_arg(Arena *a, garb_calc_t *n, const garb_calc_t *arg, bool invert, int32_t *cap)
{
    if (n->nargs == *cap) {
        int32_t grown = *cap ? *cap * 2 : 4;
        const garb_calc_t **args = os64_arena_alloc(a->arena, (size_t)grown * sizeof(*args));
        bool *inv = os64_arena_alloc(a->arena, (size_t)grown * sizeof(*inv));
        if (args == NULL || inv == NULL) {
            a->short_of_memory = true;
            return false;
        }
        for (int32_t k = 0; k < n->nargs; k++) {
            args[k] = n->args[k];
            inv[k] = n->invert[k];
        }
        n->args = (const garb_calc_t *const *)args;
        n->invert = inv;
        *cap = grown;
    }
    ((const garb_calc_t **)n->args)[n->nargs] = arg;
    ((bool *)n->invert)[n->nargs] = invert;
    n->nargs++;
    return true;
}

static garb_calc_t *calc_sum(CalcCur *c, int *type);

static garb_calc_t *calc_list_fn(CalcCur *outer, const garb_value_t *f, garb_calc_op_t op, int *type);

// A value: a number, percentage, dimension, parenthesized sum, or a nested
// math function.
static garb_calc_t *calc_value(CalcCur *c, int *type)
{
    const garb_value_t *t = cc_peek(c);
    if (t == NULL || c->depth > 32)
        return NULL;
    if (t->kind == GARB_BLOCK && t->open == '(') {
        CalcCur inner = {t->children, t->nchildren, 0, c->a, c->depth + 1};
        garb_calc_t *n = calc_sum(&inner, type);
        if (n == NULL || cc_peek(&inner) != NULL)
            return NULL;
        c->i++;
        return n;
    }
    if (t->kind == GARB_FUNCTION) {
        garb_calc_op_t op;
        if (ieq(t->text, t->len, "calc")) {
            CalcCur inner = {t->children, t->nchildren, 0, c->a, c->depth + 1};
            garb_calc_t *n = calc_sum(&inner, type);
            if (n == NULL || cc_peek(&inner) != NULL)
                return NULL;
            c->i++;
            return n;
        }
        if (ieq(t->text, t->len, "min"))
            op = GARB_CALC_MIN;
        else if (ieq(t->text, t->len, "max"))
            op = GARB_CALC_MAX;
        else if (ieq(t->text, t->len, "clamp"))
            op = GARB_CALC_CLAMP;
        else
            return NULL;
        c->depth++;
        garb_calc_t *n = calc_list_fn(c, t, op, type);
        c->depth--;
        if (n != NULL)
            c->i++;
        return n;
    }
    garb_calc_t *leaf = node(c->a, GARB_CALC_LEAF);
    if (leaf == NULL)
        return NULL;
    if (t->kind == GARB_NUMBER) {
        leaf->leaf.kind = GARB_V_NUMBER;
        *type = T_NUMBER;
    } else if (t->kind == GARB_PERCENTAGE) {
        leaf->leaf.kind = GARB_V_PERCENTAGE;
        *type = T_LP;
    } else if (t->kind == GARB_DIMENSION && unit_of(t->unit, t->unit_len, &leaf->leaf.unit)) {
        leaf->leaf.kind = GARB_V_LENGTH;
        *type = T_LP;
    } else {
        return NULL;
    }
    leaf->leaf.number = t->number;
    c->i++;
    return leaf;
}

// A product: values joined by `*` and `/`. A product of two
// length-percentages is no CSS type; a divisor must be a number.
static garb_calc_t *calc_product(CalcCur *c, int *type)
{
    int t0 = 0;
    garb_calc_t *first = calc_value(c, &t0);
    if (first == NULL)
        return NULL;
    // Looking for an operator must not eat the white space a sum's `+` or
    // `-` needs in front of it.
    int32_t at = c->i;
    const garb_value_t *op = cc_peek(c);
    if (!is_delim_v(op, '*') && !is_delim_v(op, '/')) {
        c->i = at;
        *type = t0;
        return first;
    }
    garb_calc_t *n = node(c->a, GARB_CALC_PRODUCT);
    int32_t cap = 0;
    if (n == NULL || !add_arg(c->a, n, first, false, &cap))
        return NULL;
    int t = t0;
    for (;;) {
        at = c->i;
        op = cc_peek(c);
        if (!is_delim_v(op, '*') && !is_delim_v(op, '/')) {
            c->i = at;
            break;
        }
        bool divide = is_delim_v(op, '/');
        c->i++;
        int tn = 0;
        garb_calc_t *next = calc_value(c, &tn);
        if (next == NULL)
            return NULL;
        if (divide ? tn != T_NUMBER : (t == T_LP && tn == T_LP))
            return NULL;
        if (tn == T_LP)
            t = T_LP;
        if (!add_arg(c->a, n, next, divide, &cap))
            return NULL;
    }
    *type = t;
    return n;
}

// A sum: products joined by `+` and `-`, each operator with white space on
// both sides (Values 3 § 8.1). Its terms must share a type.
static garb_calc_t *calc_sum(CalcCur *c, int *type)
{
    int t0 = 0;
    garb_calc_t *first = calc_product(c, &t0);
    if (first == NULL)
        return NULL;
    garb_calc_t *n = NULL;
    int32_t cap = 0;
    for (;;) {
        int32_t at = c->i;
        bool ws_before = at < c->n && c->v[at].kind == GARB_WHITESPACE;
        const garb_value_t *op = cc_peek(c);
        if (op == NULL || (!is_delim_v(op, '+') && !is_delim_v(op, '-'))) {
            c->i = at;
            break;
        }
        bool minus = is_delim_v(op, '-');
        c->i++;
        bool ws_after = c->i < c->n && c->v[c->i].kind == GARB_WHITESPACE;
        if (!ws_before || !ws_after)
            return NULL;
        int tn = 0;
        garb_calc_t *next = calc_product(c, &tn);
        if (next == NULL || tn != t0)
            return NULL;
        if (n == NULL) {
            n = node(c->a, GARB_CALC_SUM);
            if (n == NULL || !add_arg(c->a, n, first, false, &cap))
                return NULL;
        }
        if (!add_arg(c->a, n, next, minus, &cap))
            return NULL;
    }
    *type = t0;
    return n != NULL ? n : first;
}

// min(), max(), clamp(): comma-separated sums of one type.
static garb_calc_t *calc_list_fn(CalcCur *outer, const garb_value_t *f, garb_calc_op_t op, int *type)
{
    garb_calc_t *n = node(outer->a, op);
    if (n == NULL)
        return NULL;
    int32_t cap = 0, start = 0;
    int t = 0;
    for (int32_t k = 0; k <= f->nchildren; k++) {
        if (k < f->nchildren && f->children[k].kind != GARB_COMMA)
            continue;
        CalcCur arg = {f->children + start, k - start, 0, outer->a, outer->depth + 1};
        int ta = 0;
        garb_calc_t *x = calc_sum(&arg, &ta);
        if (x == NULL || cc_peek(&arg) != NULL || (t != 0 && ta != t))
            return NULL;
        t = ta;
        if (!add_arg(outer->a, n, x, false, &cap))
            return NULL;
        start = k + 1;
    }
    if (n->nargs == 0 || (op == GARB_CALC_CLAMP && n->nargs != 3))
        return NULL;
    *type = t;
    return n;
}

const garb_calc_t *read_calc(Arena *a, const garb_value_t *f, int accept, int *type)
{
    CalcCur c = {f, 1, 0, a, 0};
    garb_calc_t *n = calc_value(&c, type);
    if (n == NULL)
        return NULL;
    if (*type == T_NUMBER ? !(accept & ACCEPT_NUMBER) : !(accept & (ACCEPT_LENGTH | ACCEPT_PERCENT)))
        return NULL;
    return n;
}

// ── Colours (CSS Color 4) ───────────────────────────────────────────────

static const struct { const char *name; uint32_t rgb; } kNamed[] = {
    {"aliceblue", 0xf0f8ff}, {"antiquewhite", 0xfaebd7}, {"aqua", 0x00ffff},
    {"aquamarine", 0x7fffd4}, {"azure", 0xf0ffff}, {"beige", 0xf5f5dc}, {"bisque", 0xffe4c4},
    {"black", 0x000000}, {"blanchedalmond", 0xffebcd}, {"blue", 0x0000ff},
    {"blueviolet", 0x8a2be2}, {"brown", 0xa52a2a}, {"burlywood", 0xdeb887},
    {"cadetblue", 0x5f9ea0}, {"chartreuse", 0x7fff00}, {"chocolate", 0xd2691e},
    {"coral", 0xff7f50}, {"cornflowerblue", 0x6495ed}, {"cornsilk", 0xfff8dc},
    {"crimson", 0xdc143c}, {"cyan", 0x00ffff}, {"darkblue", 0x00008b}, {"darkcyan", 0x008b8b},
    {"darkgoldenrod", 0xb8860b}, {"darkgray", 0xa9a9a9}, {"darkgreen", 0x006400},
    {"darkgrey", 0xa9a9a9}, {"darkkhaki", 0xbdb76b}, {"darkmagenta", 0x8b008b},
    {"darkolivegreen", 0x556b2f}, {"darkorange", 0xff8c00}, {"darkorchid", 0x9932cc},
    {"darkred", 0x8b0000}, {"darksalmon", 0xe9967a}, {"darkseagreen", 0x8fbc8f},
    {"darkslateblue", 0x483d8b}, {"darkslategray", 0x2f4f4f}, {"darkslategrey", 0x2f4f4f},
    {"darkturquoise", 0x00ced1}, {"darkviolet", 0x9400d3}, {"deeppink", 0xff1493},
    {"deepskyblue", 0x00bfff}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"dodgerblue", 0x1e90ff}, {"firebrick", 0xb22222}, {"floralwhite", 0xfffaf0},
    {"forestgreen", 0x228b22}, {"fuchsia", 0xff00ff}, {"gainsboro", 0xdcdcdc},
    {"ghostwhite", 0xf8f8ff}, {"gold", 0xffd700}, {"goldenrod", 0xdaa520}, {"gray", 0x808080},
    {"green", 0x008000}, {"greenyellow", 0xadff2f}, {"grey", 0x808080}, {"honeydew", 0xf0fff0},
    {"hotpink", 0xff69b4}, {"indianred", 0xcd5c5c}, {"indigo", 0x4b0082}, {"ivory", 0xfffff0},
    {"khaki", 0xf0e68c}, {"lavender", 0xe6e6fa}, {"lavenderblush", 0xfff0f5},
    {"lawngreen", 0x7cfc00}, {"lemonchiffon", 0xfffacd}, {"lightblue", 0xadd8e6},
    {"lightcoral", 0xf08080}, {"lightcyan", 0xe0ffff}, {"lightgoldenrodyellow", 0xfafad2},
    {"lightgray", 0xd3d3d3}, {"lightgreen", 0x90ee90}, {"lightgrey", 0xd3d3d3},
    {"lightpink", 0xffb6c1}, {"lightsalmon", 0xffa07a}, {"lightseagreen", 0x20b2aa},
    {"lightskyblue", 0x87cefa}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xb0c4de}, {"lightyellow", 0xffffe0}, {"lime", 0x00ff00},
    {"limegreen", 0x32cd32}, {"linen", 0xfaf0e6}, {"magenta", 0xff00ff}, {"maroon", 0x800000},
    {"mediumaquamarine", 0x66cdaa}, {"mediumblue", 0x0000cd}, {"mediumorchid", 0xba55d3},
    {"mediumpurple", 0x9370db}, {"mediumseagreen", 0x3cb371}, {"mediumslateblue", 0x7b68ee},
    {"mediumspringgreen", 0x00fa9a}, {"mediumturquoise", 0x48d1cc},
    {"mediumvioletred", 0xc71585}, {"midnightblue", 0x191970}, {"mintcream", 0xf5fffa},
    {"mistyrose", 0xffe4e1}, {"moccasin", 0xffe4b5}, {"navajowhite", 0xffdead},
    {"navy", 0x000080}, {"oldlace", 0xfdf5e6}, {"olive", 0x808000}, {"olivedrab", 0x6b8e23},
    {"orange", 0xffa500}, {"orangered", 0xff4500}, {"orchid", 0xda70d6},
    {"palegoldenrod", 0xeee8aa}, {"palegreen", 0x98fb98}, {"paleturquoise", 0xafeeee},
    {"palevioletred", 0xdb7093}, {"papayawhip", 0xffefd5}, {"peachpuff", 0xffdab9},
    {"peru", 0xcd853f}, {"pink", 0xffc0cb}, {"plum", 0xdda0dd}, {"powderblue", 0xb0e0e6},
    {"purple", 0x800080}, {"rebeccapurple", 0x663399}, {"red", 0xff0000},
    {"rosybrown", 0xbc8f8f}, {"royalblue", 0x4169e1}, {"saddlebrown", 0x8b4513},
    {"salmon", 0xfa8072}, {"sandybrown", 0xf4a460}, {"seagreen", 0x2e8b57},
    {"seashell", 0xfff5ee}, {"sienna", 0xa0522d}, {"silver", 0xc0c0c0}, {"skyblue", 0x87ceeb},
    {"slateblue", 0x6a5acd}, {"slategray", 0x708090}, {"slategrey", 0x708090},
    {"snow", 0xfffafa}, {"springgreen", 0x00ff7f}, {"steelblue", 0x4682b4}, {"tan", 0xd2b48c},
    {"teal", 0x008080}, {"thistle", 0xd8bfd8}, {"tomato", 0xff6347}, {"turquoise", 0x40e0d0},
    {"violet", 0xee82ee}, {"wheat", 0xf5deb3}, {"white", 0xffffff},
    {"whitesmoke", 0xf5f5f5}, {"yellow", 0xffff00}, {"yellowgreen", 0x9acd32},
};

static void set_rgb(garb_color_t *c, uint32_t rgb, double a)
{
    c->r = (rgb >> 16) & 0xff;
    c->g = (rgb >> 8) & 0xff;
    c->b = rgb & 0xff;
    c->a = a;
    c->current = false;
}

static int hex_digit(char ch)
{
    return ch >= '0' && ch <= '9' ? ch - '0'
         : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
         : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
}

static bool hex_color(const char *s, size_t n, garb_color_t *out)
{
    if (n != 3 && n != 4 && n != 6 && n != 8)
        return false;
    int d[8];
    for (size_t i = 0; i < n; i++)
        if ((d[i] = hex_digit(s[i])) < 0)
            return false;
    double ch[4] = {0, 0, 0, 255};
    if (n <= 4)
        for (size_t i = 0; i < n; i++)
            ch[i] = d[i] * 17;
    else
        for (size_t i = 0; i < n / 2; i++)
            ch[i] = d[2 * i] * 16 + d[2 * i + 1];
    out->r = ch[0];
    out->g = ch[1];
    out->b = ch[2];
    out->a = ch[3] / 255.0;
    out->current = false;
    return true;
}

static double clamp(double v, double lo, double hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

// The arguments of a colour function: legacy (commas) or modern (spaces,
// an optional `/ alpha`). Each is a NUMBER, PERCENTAGE, an angle DIMENSION
// or `none`; `kinds` records which, for the function to judge.
typedef struct {
    double v[4];
    char kind[4];               // 'n' number, 'p' percentage, 'a' angle (in degrees), '0' none
    int count;
    bool legacy, alpha;
} Args;

static bool angle_degrees(const garb_value_t *t, double *deg)
{
    const char *u = t->unit;
    size_t n = t->unit_len;
    if (ieq(u, n, "deg"))
        *deg = t->number;
    else if (ieq(u, n, "grad"))
        *deg = t->number * 0.9;
    else if (ieq(u, n, "rad"))
        *deg = t->number * 57.29577951308232;
    else if (ieq(u, n, "turn"))
        *deg = t->number * 360.0;
    else
        return false;
    return true;
}

static bool color_args(const garb_value_t *f, Args *a)
{
    os64_memset(a, 0, sizeof(*a));
    VCur c = {f->children, f->nchildren, 0};
    int commas = 0;
    for (;;) {
        const garb_value_t *t = vc_peek(&c);
        if (t == NULL)
            break;
        if (a->count == 4)
            return false;
        if (a->count > 0) {
            if (t->kind == GARB_COMMA) {
                if (a->count == 1)
                    a->legacy = true;
                else if (!a->legacy)
                    return false;
                commas++;
                c.i++;
                t = vc_peek(&c);
            } else if (a->legacy) {
                return false;
            } else if (is_delim_v(t, '/')) {
                if (a->count != 3)
                    return false;
                a->alpha = true;
                c.i++;
                t = vc_peek(&c);
            } else if (a->count == 3) {
                return false;
            }
            if (t == NULL)
                return false;
        }
        if (a->legacy && a->count == 3)
            a->alpha = true;
        int k = a->count;
        if (t->kind == GARB_NUMBER) {
            a->kind[k] = 'n';
            a->v[k] = t->number;
        } else if (t->kind == GARB_PERCENTAGE) {
            a->kind[k] = 'p';
            a->v[k] = t->number;
        } else if (t->kind == GARB_DIMENSION && angle_degrees(t, &a->v[k])) {
            a->kind[k] = 'a';
        } else if (t->kind == GARB_IDENT && ieq(t->text, t->len, "none") && !a->legacy) {
            a->kind[k] = '0';
            a->v[k] = 0;
        } else {
            return false;
        }
        c.i++;
        a->count++;
    }
    if (a->count != 3 && a->count != 4)
        return false;
    if (a->legacy && commas != a->count - 1)
        return false;
    if (a->count == 4 && !a->alpha)
        return false;
    // `none` is a modern word: a legacy call never takes it.
    for (int k = 0; k < a->count && a->legacy; k++)
        if (a->kind[k] == '0')
            return false;
    return true;
}

static bool alpha_of(const Args *a, double *alpha)
{
    *alpha = 1.0;
    if (a->count < 4)
        return true;
    if (a->kind[3] == 'n' || a->kind[3] == '0')
        *alpha = clamp(a->v[3], 0, 1);
    else if (a->kind[3] == 'p')
        *alpha = clamp(a->v[3] / 100.0, 0, 1);
    else
        return false;
    return true;
}

static bool rgb_fn(const garb_value_t *f, garb_color_t *out)
{
    Args a;
    if (!color_args(f, &a))
        return false;
    double ch[3];
    for (int k = 0; k < 3; k++) {
        if (a.kind[k] == 'a')
            return false;
        // Legacy syntax: all numbers or all percentages, never mixed.
        if (a.legacy && a.kind[k] != a.kind[0])
            return false;
        ch[k] = a.kind[k] == 'p' ? a.v[k] * 2.55 : a.v[k];
        ch[k] = clamp(ch[k], 0, 255);
    }
    double alpha;
    if (!alpha_of(&a, &alpha))
        return false;
    out->r = ch[0];
    out->g = ch[1];
    out->b = ch[2];
    out->a = alpha;
    out->current = false;
    return true;
}

// CSS Color 4 § 7.1's hslToRgb and § 8.1's hwbToRgb.
static double hue_normal(double h)
{
    h = h - 360.0 * (double)(int64_t)(h / 360.0);
    return h < 0 ? h + 360.0 : h;
}

static void hsl_to_rgb(double h, double s, double l, double rgb[3])
{
    h = hue_normal(h);
    s /= 100.0;
    l /= 100.0;
    for (int k = 0; k < 3; k++) {
        double n = k == 0 ? 0 : k == 1 ? 8 : 4;
        double x = n + h / 30.0;
        x = x - 12.0 * (double)(int64_t)(x / 12.0);
        double a = s * (l < 1 - l ? l : 1 - l);
        double m = x - 3 < 9 - x ? x - 3 : 9 - x;
        if (m > 1)
            m = 1;
        if (m < -1)
            m = -1;
        rgb[k] = (l - a * m) * 255.0;
    }
}

static bool hue_of(const Args *a, double *h)
{
    if (a->kind[0] == 'p')
        return false;
    *h = a->v[0];
    return true;
}

static bool hsl_fn(const garb_value_t *f, garb_color_t *out)
{
    Args a;
    if (!color_args(f, &a))
        return false;
    double h;
    if (!hue_of(&a, &h))
        return false;
    // Legacy: saturation and lightness are percentages. Modern: a number
    // stands for a percentage.
    for (int k = 1; k < 3; k++)
        if (a.kind[k] == 'a' || (a.legacy && a.kind[k] != 'p'))
            return false;
    double s = clamp(a.v[1], 0, 100), l = clamp(a.v[2], 0, 100);
    double rgb[3];
    hsl_to_rgb(h, s, l, rgb);
    double alpha;
    if (!alpha_of(&a, &alpha))
        return false;
    out->r = rgb[0];
    out->g = rgb[1];
    out->b = rgb[2];
    out->a = alpha;
    out->current = false;
    return true;
}

static bool hwb_fn(const garb_value_t *f, garb_color_t *out)
{
    Args a;
    if (!color_args(f, &a) || a.legacy)
        return false;
    double h;
    if (!hue_of(&a, &h))
        return false;
    for (int k = 1; k < 3; k++)
        if (a.kind[k] == 'a')
            return false;
    double w = clamp(a.v[1], 0, 100) / 100.0, b = clamp(a.v[2], 0, 100) / 100.0;
    double rgb[3];
    if (w + b >= 1) {
        double gray = w / (w + b) * 255.0;
        rgb[0] = rgb[1] = rgb[2] = gray;
    } else {
        hsl_to_rgb(h, 100, 50, rgb);
        for (int k = 0; k < 3; k++)
            rgb[k] = rgb[k] * (1 - w - b) + w * 255.0;
    }
    double alpha;
    if (!alpha_of(&a, &alpha))
        return false;
    out->r = rgb[0];
    out->g = rgb[1];
    out->b = rgb[2];
    out->a = alpha;
    out->current = false;
    return true;
}

bool read_color_value(const garb_value_t *t, garb_color_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    if (t == NULL)
        return false;
    if (t->kind == GARB_IDENT) {
        if (ieq(t->text, t->len, "transparent")) {
            set_rgb(out, 0, 0);
            return true;
        }
        if (ieq(t->text, t->len, "currentcolor")) {
            out->current = true;
            out->a = 1;
            return true;
        }
        for (size_t k = 0; k < sizeof(kNamed) / sizeof(kNamed[0]); k++)
            if (ieq(t->text, t->len, kNamed[k].name)) {
                set_rgb(out, kNamed[k].rgb, 1);
                return true;
            }
        return false;
    }
    if (t->kind == GARB_HASH)
        return hex_color(t->text, t->len, out);
    if (t->kind == GARB_FUNCTION) {
        if (ieq(t->text, t->len, "rgb") || ieq(t->text, t->len, "rgba"))
            return rgb_fn(t, out);
        if (ieq(t->text, t->len, "hsl") || ieq(t->text, t->len, "hsla"))
            return hsl_fn(t, out);
        if (ieq(t->text, t->len, "hwb"))
            return hwb_fn(t, out);
    }
    return false;
}

bool garb_read_color(const garb_value_t *v, int32_t n, garb_color_t *out)
{
    VCur c = {v, n, 0};
    const garb_value_t *t = vc_peek(&c);
    if (t == NULL || !read_color_value(t, out))
        return false;
    c.i++;
    return vc_done(&c);
}

bool vc_color(VCur *c, garb_val_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    if (!read_color_value(vc_peek(c), &out->color))
        return false;
    out->kind = GARB_V_COLOR;
    c->i++;
    return true;
}

// ── urls and images ─────────────────────────────────────────────────────

bool vc_url(VCur *c, garb_val_t *out)
{
    const garb_value_t *t = vc_peek(c);
    os64_memset(out, 0, sizeof(*out));
    if (t == NULL)
        return false;
    if (t->kind == GARB_URL) {
        out->text = t->text;
        out->len = t->len;
    } else if (t->kind == GARB_FUNCTION && ieq(t->text, t->len, "url") && t->nchildren >= 1) {
        VCur in = {t->children, t->nchildren, 0};
        const garb_value_t *s = vc_peek(&in);
        if (s == NULL || s->kind != GARB_STRING)
            return false;
        in.i++;
        if (!vc_done(&in))
            return false;
        out->text = s->text;
        out->len = s->len;
    } else {
        return false;
    }
    out->kind = GARB_V_URL;
    c->i++;
    return true;
}

// An <image>: a url, or a gradient or image-set this library does not draw
// yet, kept so a declaration naming one is valid — as it is in a browser
// that draws it — and the rest of a `background` still applies.
bool vc_image(VCur *c, garb_val_t *out)
{
    if (vc_url(c, out))
        return true;
    const garb_value_t *t = vc_peek(c);
    static const char *const kImages[] = {
        "linear-gradient", "radial-gradient", "conic-gradient", "repeating-linear-gradient",
        "repeating-radial-gradient", "repeating-conic-gradient", "image-set",
        "-webkit-image-set", NULL,
    };
    if (t == NULL || t->kind != GARB_FUNCTION)
        return false;
    for (int k = 0; kImages[k] != NULL; k++)
        if (ieq(t->text, t->len, kImages[k])) {
            os64_memset(out, 0, sizeof(*out));
            out->kind = GARB_V_IMAGE;
            out->text = kImages[k];
            out->len = os64_strlen(kImages[k]);
            c->i++;
            return true;
        }
    return false;
}
