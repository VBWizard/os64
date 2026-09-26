// select.c — Selectors Level 3, with Level 4's :is(), :where(), :not() with a
// list, and :has() (garb/select.h).
//
// A selector is parsed from a rule's prelude into COMPLEX selectors — a
// run of COMPOUNDS joined by combinators — each compound a run of SIMPLE
// selectors. Matching goes right to left, from the element being styled,
// the way every engine does it: the rightmost compound is the one most
// elements fail at once.

#include "internal.h"
#include "garb/select.h"
#include "os64/mem.h"
#include "os64/str.h"

// ── The parsed form ─────────────────────────────────────────────────────

typedef enum { S_TYPE, S_UNIVERSAL, S_ID, S_CLASS, S_ATTR, S_PSEUDO } SKind;
typedef enum { A_EXISTS, A_EQ, A_INCLUDES, A_DASH, A_PREFIX, A_SUFFIX, A_SUBSTRING } AOp;
typedef enum {
    P_ROOT, P_FIRST_CHILD, P_LAST_CHILD, P_ONLY_CHILD, P_FIRST_OF_TYPE, P_LAST_OF_TYPE,
    P_ONLY_OF_TYPE, P_NTH_CHILD, P_NTH_LAST_CHILD, P_NTH_OF_TYPE, P_NTH_LAST_OF_TYPE, P_EMPTY,
    P_LINK, P_ANY_LINK, P_NEVER, P_ENABLED, P_DISABLED, P_CHECKED, P_LANG, P_DIR, P_NOT, P_IS,
    P_WHERE, P_HAS, P_REQUIRED, P_OPTIONAL, P_READ_ONLY, P_READ_WRITE, P_DEFINED, P_SCOPE,
} PKind;
typedef enum { C_NONE, C_DESCENDANT, C_CHILD, C_NEXT, C_SUBSEQUENT } Comb;

typedef struct List List;

typedef struct {
    SKind kind;
    const char *name;           // TYPE, ID, CLASS, ATTR: the name
    size_t len;
    bool no_ns;                 // `|E`, `[|a]`: in no namespace
    AOp op;
    const char *value;          // ATTR
    size_t vlen;
    int8_t ci;                  // ATTR: -1 as HTML says, 0 `s`, 1 `i`
    PKind pseudo;
    int32_t a, b;               // the nth- pseudo-classes
    List *list;                 // :not, :is, :where, :has, nth-child(… of S)
    const garb_value_t *args;   // :lang, :dir: their arguments
    int32_t nargs;
} Simple;

typedef struct {
    Simple *s;
    int32_t n, cap;
    Comb left;                  // how this compound meets the one on its left
} Compound;

typedef struct {
    Compound *c;
    int32_t n, cap;
    garb_pseudo_t pseudo;
    uint32_t a, b, c3;          // specificity
    Comb relative;              // :has()'s leading combinator; C_NONE elsewhere
} Complex;

struct List {
    Complex *v;
    int32_t n, cap;
};

struct garb_selectors {
    List list;
    os64_html_quirks_t quirks;
};

// ── Reading the prelude ─────────────────────────────────────────────────

typedef struct {
    const garb_value_t *v;
    int32_t n, i;
    Parse *p;
    bool failed;
} Cur;

static const garb_value_t *peek(Cur *c, int32_t ahead)
{
    return c->i + ahead < c->n ? &c->v[c->i + ahead] : NULL;
}

static bool is_delim(const garb_value_t *v, char ch)
{
    return v != NULL && v->kind == GARB_DELIM && v->len == 1 && v->text[0] == ch;
}

static bool is_ws(const garb_value_t *v)
{
    return v != NULL && v->kind == GARB_WHITESPACE;
}

static bool skip_ws(Cur *c)
{
    bool any = false;
    while (is_ws(peek(c, 0))) {
        c->i++;
        any = true;
    }
    return any;
}

static bool ascii_ieq(const char *a, size_t alen, const char *b)
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

static bool ieq2(const char *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen)
        return false;
    for (size_t i = 0; i < alen; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z')
            x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z')
            y = (char)(y - 'A' + 'a');
        if (x != y)
            return false;
    }
    return true;
}

static bool is_ident(const garb_value_t *v, const char *word)
{
    return v != NULL && v->kind == GARB_IDENT && ascii_ieq(v->text, v->len, word);
}

static void push_simple(Cur *c, Compound *cp, const Simple *s)
{
    if (!p_push(c->p, (void **)&cp->s, &cp->n, &cp->cap, sizeof(*s), s))
        c->failed = true;
}

// ── §6 An+B ─────────────────────────────────────────────────────────────

static bool digits_only(const char *s, size_t n)
{
    if (n == 0)
        return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9')
            return false;
    return true;
}

static int32_t clamp_int(double v)
{
    return v > 2147483647.0 ? 2147483647 : v < -2147483648.0 ? (-2147483647 - 1) : (int32_t)v;
}

// Digits as a number, saturating.
static int32_t parse_digits(const char *s, size_t n)
{
    int64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        v = v * 10 + (s[i] - '0');
        if (v > 2147483647)
            v = 2147483647;
    }
    return (int32_t)v;
}

// What follows `An`: nothing, or a sign and digits (`+ 3`, `-3`, `- 3`).
// `sign` is the sign already consumed with the n (`n-`), or 0.
static bool b_part(const garb_value_t *v, int32_t n, int32_t i, int sign, int32_t *b)
{
    while (i < n && v[i].kind == GARB_WHITESPACE)
        i++;
    if (i == n) {
        *b = 0;
        return sign == 0;
    }
    if (sign == 0 && v[i].kind == GARB_NUMBER && v[i].integer && v[i].len > 0 &&
        (v[i].text[0] == '+' || v[i].text[0] == '-')) {
        *b = clamp_int(v[i].number);
        i++;
    } else {
        if (sign == 0) {
            if (is_delim(&v[i], '+'))
                sign = 1;
            else if (is_delim(&v[i], '-'))
                sign = -1;
            else
                return false;
            i++;
            while (i < n && v[i].kind == GARB_WHITESPACE)
                i++;
        }
        if (i == n || v[i].kind != GARB_NUMBER || !v[i].integer || v[i].text[0] == '+' ||
            v[i].text[0] == '-')
            return false;
        *b = sign * clamp_int(v[i].number);
        i++;
    }
    while (i < n && v[i].kind == GARB_WHITESPACE)
        i++;
    return i == n;
}

bool garb_an_plus_b(const garb_value_t *v, int32_t n, int32_t *a, int32_t *b)
{
    int32_t i = 0;
    while (i < n && v[i].kind == GARB_WHITESPACE)
        i++;
    if (i == n)
        return false;
    const garb_value_t *t = &v[i];
    if (is_ident(t, "odd") || is_ident(t, "even")) {
        *a = 2;
        *b = is_ident(t, "odd") ? 1 : 0;
        i++;
        while (i < n && v[i].kind == GARB_WHITESPACE)
            i++;
        return i == n;
    }
    if (t->kind == GARB_NUMBER && t->integer) {
        *a = 0;
        *b = clamp_int(t->number);
        i++;
        while (i < n && v[i].kind == GARB_WHITESPACE)
            i++;
        return i == n;
    }
    // The n and what is glued to it: a dimension's unit, or an ident with
    // an optional '+' delimiter before it.
    const char *unit;
    size_t ulen;
    int32_t coef;
    if (t->kind == GARB_DIMENSION && t->integer) {
        coef = clamp_int(t->number);
        unit = t->unit;
        ulen = t->unit_len;
        i++;
    } else {
        bool plus = false;
        if (is_delim(t, '+')) {
            plus = true;
            i++;
            if (i == n)
                return false;
            t = &v[i];
        }
        if (t->kind != GARB_IDENT)
            return false;
        unit = t->text;
        ulen = t->len;
        coef = 1;
        if (!plus && ulen > 0 && unit[0] == '-') {
            coef = -1;
            unit++;
            ulen--;
        }
        i++;
    }
    if (ulen == 0 || (unit[0] != 'n' && unit[0] != 'N'))
        return false;
    *a = coef;
    if (ulen == 1)
        return b_part(v, n, i, 0, b);
    if (ulen == 2 && unit[1] == '-')
        return b_part(v, n, i, -1, b);
    if (ulen > 2 && unit[1] == '-' && digits_only(unit + 2, ulen - 2)) {
        *b = -parse_digits(unit + 2, ulen - 2);
        while (i < n && v[i].kind == GARB_WHITESPACE)
            i++;
        return i == n;
    }
    return false;
}

// ── Parsing selectors ───────────────────────────────────────────────────

static List *parse_list(Cur *c, bool forgiving, bool relative);

// A list from a function's arguments.
static List *sub_list(Cur *outer, const garb_value_t *v, int32_t n, bool forgiving, bool relative)
{
    Cur c = {v, n, 0, outer->p, false};
    List *l = parse_list(&c, forgiving, relative);
    if (c.failed)
        outer->failed = true;
    return l;
}

static bool pseudo_element_named(const char *s, size_t n, garb_pseudo_t *out)
{
    static const struct { const char *name; garb_pseudo_t which; } kNames[] = {
        {"before", GARB_PSEUDO_BEFORE}, {"after", GARB_PSEUDO_AFTER},
        {"first-line", GARB_PSEUDO_FIRST_LINE}, {"first-letter", GARB_PSEUDO_FIRST_LETTER},
        {"marker", GARB_PSEUDO_MARKER}, {"placeholder", GARB_PSEUDO_PLACEHOLDER},
        {"selection", GARB_PSEUDO_SELECTION}, {"backdrop", GARB_PSEUDO_BACKDROP},
    };
    for (size_t k = 0; k < sizeof(kNames) / sizeof(kNames[0]); k++)
        if (ascii_ieq(s, n, kNames[k].name)) {
            *out = kNames[k].which;
            return true;
        }
    return false;
}

static bool pseudo_class_named(const char *s, size_t n, PKind *out)
{
    static const struct { const char *name; PKind kind; } kNames[] = {
        {"root", P_ROOT}, {"first-child", P_FIRST_CHILD}, {"last-child", P_LAST_CHILD},
        {"only-child", P_ONLY_CHILD}, {"first-of-type", P_FIRST_OF_TYPE},
        {"last-of-type", P_LAST_OF_TYPE}, {"only-of-type", P_ONLY_OF_TYPE}, {"empty", P_EMPTY},
        {"link", P_LINK}, {"any-link", P_ANY_LINK}, {"visited", P_NEVER}, {"hover", P_NEVER},
        {"active", P_NEVER}, {"focus", P_NEVER}, {"focus-within", P_NEVER},
        {"focus-visible", P_NEVER}, {"target", P_NEVER}, {"enabled", P_ENABLED},
        {"disabled", P_DISABLED}, {"checked", P_CHECKED}, {"required", P_REQUIRED},
        {"optional", P_OPTIONAL}, {"read-only", P_READ_ONLY}, {"read-write", P_READ_WRITE},
        {"defined", P_DEFINED}, {"scope", P_SCOPE},
    };
    for (size_t k = 0; k < sizeof(kNames) / sizeof(kNames[0]); k++)
        if (ascii_ieq(s, n, kNames[k].name)) {
            *out = kNames[k].kind;
            return true;
        }
    return false;
}

// `[ … ]`, its contents.
static bool parse_attr(Cur *outer, const garb_value_t *v, int32_t n, Simple *s)
{
    Cur c = {v, n, 0, outer->p, false};
    s->kind = S_ATTR;
    s->ci = -1;
    skip_ws(&c);
    const garb_value_t *t = peek(&c, 0);
    // A namespace prefix: `*|a` is any namespace, `|a` none; a named prefix
    // needs an @namespace rule, which this engine does not read.
    if (is_delim(t, '|')) {
        s->no_ns = true;
        c.i++;
        t = peek(&c, 0);
    } else if (is_delim(t, '*') && is_delim(peek(&c, 1), '|')) {
        c.i += 2;
        t = peek(&c, 0);
    } else if (t != NULL && t->kind == GARB_IDENT && is_delim(peek(&c, 1), '|') &&
               !is_delim(peek(&c, 2), '=')) {
        return false;
    }
    if (t == NULL || t->kind != GARB_IDENT)
        return false;
    s->name = t->text;
    s->len = t->len;
    c.i++;
    skip_ws(&c);
    t = peek(&c, 0);
    if (t == NULL) {
        s->op = A_EXISTS;
        return true;
    }
    if (is_delim(t, '=')) {
        s->op = A_EQ;
        c.i++;
    } else if (t->kind == GARB_DELIM && t->len == 1 && is_delim(peek(&c, 1), '=')) {
        switch (t->text[0]) {
        case '~': s->op = A_INCLUDES; break;
        case '|': s->op = A_DASH; break;
        case '^': s->op = A_PREFIX; break;
        case '$': s->op = A_SUFFIX; break;
        case '*': s->op = A_SUBSTRING; break;
        default: return false;
        }
        c.i += 2;
    } else {
        return false;
    }
    skip_ws(&c);
    t = peek(&c, 0);
    if (t == NULL || (t->kind != GARB_IDENT && t->kind != GARB_STRING))
        return false;
    s->value = t->text;
    s->vlen = t->len;
    c.i++;
    skip_ws(&c);
    t = peek(&c, 0);
    if (t != NULL) {
        if (is_ident(t, "i"))
            s->ci = 1;
        else if (is_ident(t, "s"))
            s->ci = 0;
        else
            return false;
        c.i++;
        skip_ws(&c);
    }
    return peek(&c, 0) == NULL;
}

// A functional pseudo-class, `:name(args)`.
static bool parse_pseudo_function(Cur *c, const garb_value_t *f, Simple *s)
{
    s->kind = S_PSEUDO;
    const char *name = f->text;
    size_t len = f->len;
    if (ascii_ieq(name, len, "not") || ascii_ieq(name, len, "is") ||
        ascii_ieq(name, len, "where") || ascii_ieq(name, len, "matches") ||
        ascii_ieq(name, len, "any")) {
        bool is_not = ascii_ieq(name, len, "not");
        s->pseudo = is_not ? P_NOT : ascii_ieq(name, len, "where") ? P_WHERE : P_IS;
        s->list = sub_list(c, f->children, f->nchildren, !is_not, false);
        return s->list != NULL && (s->list->n > 0 || !is_not);
    }
    if (ascii_ieq(name, len, "has")) {
        s->pseudo = P_HAS;
        s->list = sub_list(c, f->children, f->nchildren, false, true);
        return s->list != NULL && s->list->n > 0;
    }
    bool child = ascii_ieq(name, len, "nth-child"), last_child = ascii_ieq(name, len, "nth-last-child");
    bool of_type = ascii_ieq(name, len, "nth-of-type"),
         last_of_type = ascii_ieq(name, len, "nth-last-of-type");
    if (child || last_child || of_type || last_of_type) {
        s->pseudo = child ? P_NTH_CHILD : last_child ? P_NTH_LAST_CHILD
                  : of_type ? P_NTH_OF_TYPE : P_NTH_LAST_OF_TYPE;
        int32_t end = f->nchildren;
        if (child || last_child)
            for (int32_t k = 0; k < f->nchildren; k++)
                if (is_ident(&f->children[k], "of")) {
                    end = k;
                    s->list = sub_list(c, f->children + k + 1, f->nchildren - k - 1, false, false);
                    if (s->list == NULL || s->list->n == 0)
                        return false;
                    break;
                }
        return garb_an_plus_b(f->children, end, &s->a, &s->b);
    }
    if (ascii_ieq(name, len, "lang") || ascii_ieq(name, len, "dir")) {
        s->pseudo = ascii_ieq(name, len, "lang") ? P_LANG : P_DIR;
        s->args = f->children;
        s->nargs = f->nchildren;
        return true;
    }
    return false;
}

// One compound selector. False when none was there or it is invalid.
static bool parse_compound(Cur *c, Compound *cp, garb_pseudo_t *pseudo)
{
    const garb_value_t *t = peek(c, 0);
    // The type selector, with its namespace prefix if any.
    Simple ty = {0};
    bool have_type = false;
    if (is_delim(t, '|')) {
        ty.no_ns = true;
        c->i++;
        t = peek(c, 0);
        if (t != NULL && t->kind == GARB_IDENT)
            ty.kind = S_TYPE;
        else if (is_delim(t, '*'))
            ty.kind = S_UNIVERSAL;
        else
            return false;
        have_type = true;
    } else if ((t != NULL && t->kind == GARB_IDENT) || is_delim(t, '*')) {
        const garb_value_t *bar = peek(c, 1), *after = peek(c, 2);
        if (is_delim(bar, '|') && after != NULL &&
            (after->kind == GARB_IDENT || is_delim(after, '*'))) {
            if (!is_delim(t, '*'))
                return false;           // a named prefix needs @namespace
            c->i += 2;
            t = after;
        }
        ty.kind = t->kind == GARB_IDENT ? S_TYPE : S_UNIVERSAL;
        have_type = true;
    }
    if (have_type) {
        if (ty.kind == S_TYPE) {
            ty.name = t->text;
            ty.len = t->len;
        }
        c->i++;
        push_simple(c, cp, &ty);
    }
    bool any = have_type;
    for (;;) {
        t = peek(c, 0);
        if (t == NULL)
            break;
        Simple s = {0};
        if (t->kind == GARB_HASH) {
            if (!t->id || *pseudo != GARB_PSEUDO_NONE)
                return false;
            s.kind = S_ID;
            s.name = t->text;
            s.len = t->len;
            c->i++;
        } else if (is_delim(t, '.')) {
            const garb_value_t *id = peek(c, 1);
            if (id == NULL || id->kind != GARB_IDENT || *pseudo != GARB_PSEUDO_NONE)
                return false;
            s.kind = S_CLASS;
            s.name = id->text;
            s.len = id->len;
            c->i += 2;
        } else if (t->kind == GARB_BLOCK && t->open == '[') {
            if (*pseudo != GARB_PSEUDO_NONE || !parse_attr(c, t->children, t->nchildren, &s))
                return false;
            c->i++;
        } else if (t->kind == GARB_COLON) {
            const garb_value_t *u = peek(c, 1);
            if (u != NULL && u->kind == GARB_COLON) {
                const garb_value_t *w = peek(c, 2);
                if (w == NULL || w->kind != GARB_IDENT || *pseudo != GARB_PSEUDO_NONE ||
                    !pseudo_element_named(w->text, w->len, pseudo))
                    return false;
                c->i += 3;
                any = true;
                continue;
            }
            if (u == NULL)
                return false;
            if (u->kind == GARB_IDENT) {
                // CSS 2's single-colon spellings of four pseudo-elements.
                garb_pseudo_t legacy;
                if ((ascii_ieq(u->text, u->len, "before") || ascii_ieq(u->text, u->len, "after") ||
                     ascii_ieq(u->text, u->len, "first-line") ||
                     ascii_ieq(u->text, u->len, "first-letter")) &&
                    pseudo_element_named(u->text, u->len, &legacy)) {
                    if (*pseudo != GARB_PSEUDO_NONE)
                        return false;
                    *pseudo = legacy;
                    c->i += 2;
                    any = true;
                    continue;
                }
                s.kind = S_PSEUDO;
                if (!pseudo_class_named(u->text, u->len, &s.pseudo))
                    return false;
                // After a pseudo-element only the user-action classes may
                // follow, and they never match here.
                if (*pseudo != GARB_PSEUDO_NONE && s.pseudo != P_NEVER)
                    return false;
            } else if (u->kind == GARB_FUNCTION) {
                if (*pseudo != GARB_PSEUDO_NONE || !parse_pseudo_function(c, u, &s))
                    return false;
            } else {
                return false;
            }
            c->i += 2;
        } else {
            break;
        }
        push_simple(c, cp, &s);
        any = true;
    }
    return any && !c->failed;
}

static bool combinator(const garb_value_t *t, Comb *out)
{
    if (is_delim(t, '>'))
        *out = C_CHILD;
    else if (is_delim(t, '+'))
        *out = C_NEXT;
    else if (is_delim(t, '~'))
        *out = C_SUBSEQUENT;
    else
        return false;
    return true;
}

// Specificity: ids, then classes/attributes/pseudo-classes, then types and
// pseudo-elements. :is(), :not() and :has() weigh as their heaviest
// argument, :where() as nothing, nth-child(… of S) as a class and S.
static void weigh_list(const List *l, uint32_t *a, uint32_t *b, uint32_t *c)
{
    *a = *b = *c = 0;
    for (int32_t i = 0; l != NULL && i < l->n; i++) {
        const Complex *x = &l->v[i];
        if (x->a > *a || (x->a == *a && (x->b > *b || (x->b == *b && x->c3 > *c)))) {
            *a = x->a;
            *b = x->b;
            *c = x->c3;
        }
    }
}

static void weigh(Complex *x)
{
    x->a = x->b = x->c3 = 0;
    for (int32_t k = 0; k < x->n; k++)
        for (int32_t j = 0; j < x->c[k].n; j++) {
            const Simple *s = &x->c[k].s[j];
            uint32_t a, b, c;
            switch (s->kind) {
            case S_ID: x->a++; break;
            case S_CLASS:
            case S_ATTR: x->b++; break;
            case S_TYPE: x->c3++; break;
            case S_UNIVERSAL: break;
            case S_PSEUDO:
                if (s->pseudo == P_WHERE)
                    break;
                if (s->pseudo == P_IS || s->pseudo == P_NOT || s->pseudo == P_HAS) {
                    weigh_list(s->list, &a, &b, &c);
                    x->a += a;
                    x->b += b;
                    x->c3 += c;
                    break;
                }
                x->b++;
                if (s->list != NULL) {
                    weigh_list(s->list, &a, &b, &c);
                    x->a += a;
                    x->b += b;
                    x->c3 += c;
                }
                break;
            }
        }
    if (x->pseudo != GARB_PSEUDO_NONE)
        x->c3++;
}

// One complex selector, up to a comma or the end. False when invalid.
static bool parse_complex(Cur *c, bool relative, Complex *x)
{
    os64_memset(x, 0, sizeof(*x));
    skip_ws(c);
    Comb next = C_NONE;
    if (relative) {
        x->relative = C_DESCENDANT;
        if (combinator(peek(c, 0), &x->relative)) {
            c->i++;
            skip_ws(c);
        }
    }
    for (;;) {
        Compound cp = {0};
        cp.left = next;
        if (!parse_compound(c, &cp, &x->pseudo))
            return false;
        if (!p_push(c->p, (void **)&x->c, &x->n, &x->cap, sizeof(cp), &cp))
            return false;
        bool ws = skip_ws(c);
        const garb_value_t *t = peek(c, 0);
        if (t == NULL || t->kind == GARB_COMMA)
            break;
        // A pseudo-element ends its selector: nothing may follow it.
        if (x->pseudo != GARB_PSEUDO_NONE)
            return false;
        if (combinator(t, &next)) {
            c->i++;
            skip_ws(c);
        } else if (ws) {
            next = C_DESCENDANT;
        } else {
            return false;
        }
    }
    weigh(x);
    return true;
}

// A comma-separated list. A FORGIVING list (:is, :where) drops the
// selectors it cannot read and keeps the rest; any other is invalid whole.
static List *parse_list(Cur *c, bool forgiving, bool relative)
{
    List *l = os64_arena_calloc(c->p->arena, 1, sizeof(*l));
    if (l == NULL) {
        c->failed = true;
        return NULL;
    }
    for (;;) {
        Complex x;
        if (parse_complex(c, relative, &x) && !c->failed) {
            if (!p_push(c->p, (void **)&l->v, &l->n, &l->cap, sizeof(x), &x)) {
                c->failed = true;
                return NULL;
            }
        } else if (!forgiving || c->failed) {
            return NULL;
        }
        // To the next comma at this level.
        while (peek(c, 0) != NULL && peek(c, 0)->kind != GARB_COMMA)
            c->i++;
        if (peek(c, 0) == NULL)
            return l;
        c->i++;
    }
}

garb_selectors_t *garb_selectors_parse(garb_parsed_t *owner, const garb_value_t *prelude,
                                       int32_t n, os64_html_quirks_t quirks)
{
    Parse p = {owner->arena, false, 0};
    Cur c = {prelude, n, 0, &p, false};
    garb_selectors_t *s = os64_arena_calloc(owner->arena, 1, sizeof(*s));
    if (s == NULL) {
        owner->incomplete = true;
        return NULL;
    }
    List *l = parse_list(&c, false, false);
    if (c.failed)
        owner->incomplete = true;
    if (l == NULL || l->n == 0)
        return NULL;
    s->list = *l;
    s->quirks = quirks;
    return s;
}

int32_t garb_selectors_count(const garb_selectors_t *list)
{
    return list != NULL ? list->list.n : 0;
}

static uint32_t cap1023(uint32_t v)
{
    return v > 1023 ? 1023 : v;
}

uint32_t garb_selector_specificity(const garb_selectors_t *list, int32_t i)
{
    const Complex *x = &list->list.v[i];
    return cap1023(x->a) << 20 | cap1023(x->b) << 10 | cap1023(x->c3);
}

garb_pseudo_t garb_selector_pseudo(const garb_selectors_t *list, int32_t i)
{
    return list->list.v[i].pseudo;
}

garb_selector_key_t garb_selector_key(const garb_selectors_t *list, int32_t i)
{
    garb_selector_key_t key = {0};
    const Complex *x = &list->list.v[i];
    const Compound *last = &x->c[x->n - 1];
    for (int32_t j = 0; j < last->n; j++) {
        const Simple *s = &last->s[j];
        if (s->kind == S_ID && key.id == NULL) {
            key.id = s->name;
            key.id_len = s->len;
        } else if (s->kind == S_CLASS && key.class_name == NULL) {
            key.class_name = s->name;
            key.class_len = s->len;
        } else if (s->kind == S_TYPE && !s->no_ns && key.type == NULL) {
            key.type = s->name;
            key.type_len = s->len;
        }
    }
    return key;
}

// ── The tree ────────────────────────────────────────────────────────────

static bool is_el(const os64_html_node_t *n)
{
    return n != NULL && n->kind == OS64_HTML_ELEMENT;
}

static const os64_html_node_t *parent_el(const os64_html_node_t *n)
{
    return is_el(n->parent) ? n->parent : NULL;
}

static const os64_html_node_t *prev_el(const os64_html_node_t *n)
{
    for (n = n->prev; n != NULL; n = n->prev)
        if (is_el(n))
            return n;
    return NULL;
}

static const os64_html_node_t *next_el(const os64_html_node_t *n)
{
    for (n = n->next; n != NULL; n = n->next)
        if (is_el(n))
            return n;
    return NULL;
}

static bool html(const os64_html_node_t *n)
{
    return n->ns == OS64_HTML_NS_HTML;
}

static bool named(const os64_html_node_t *n, const char *name)
{
    return html(n) && n->name != NULL && os64_streq(n->name, name);
}

static const char *attr(const os64_html_node_t *n, const char *name)
{
    const os64_html_attr_t *a = os64_html_attr(n, name);
    return a != NULL ? (a->value != NULL ? a->value : "") : NULL;
}

static bool same_type(const os64_html_node_t *a, const os64_html_node_t *b)
{
    return a->ns == b->ns && a->name != NULL && b->name != NULL && os64_streq(a->name, b->name);
}

// HTML's list of attributes whose values a selector compares without
// regard to ASCII case on an HTML element.
static bool ci_valued(const char *name, size_t len)
{
    static const char *const kCi[] = {
        "accept", "accept-charset", "align", "alink", "axis", "bgcolor", "charset", "checked",
        "clear", "codetype", "color", "compact", "declare", "defer", "dir", "direction",
        "disabled", "enctype", "face", "frame", "hreflang", "http-equiv", "lang", "language",
        "link", "media", "method", "multiple", "nohref", "noresize", "noshade", "nowrap",
        "readonly", "rel", "rev", "rules", "scope", "scrolling", "selected", "shape", "target",
        "text", "type", "valign", "valuetype", "vlink",
    };
    for (size_t k = 0; k < sizeof(kCi) / sizeof(kCi[0]); k++)
        if (ascii_ieq(name, len, kCi[k]))
            return true;
    return false;
}

// ── Matching ────────────────────────────────────────────────────────────

typedef enum { MATCHED, NOT_SIBLING, NOT_DESCENDANT, NOT_GLOBALLY } Result;

static bool match_complex(const garb_selectors_t *sel, const Complex *x,
                          const os64_html_node_t *el, const os64_html_node_t *scope);

static bool match_list(const garb_selectors_t *sel, const List *l, const os64_html_node_t *el,
                       const os64_html_node_t *scope)
{
    for (int32_t i = 0; l != NULL && i < l->n; i++)
        if (l->v[i].pseudo == GARB_PSEUDO_NONE && match_complex(sel, &l->v[i], el, scope))
            return true;
    return false;
}

static bool same_bytes(const char *p, const char *q, size_t len, bool ci)
{
    return ci ? ieq2(p, len, q, len) : os64_memcmp(p, q, len) == 0;
}

static bool value_matches(const Simple *s, const os64_html_node_t *el, const char *v)
{
    if (s->op == A_EXISTS)
        return true;
    bool ci = s->ci == 1 || (s->ci == -1 && html(el) && ci_valued(s->name, s->len));
    size_t n = os64_strlen(v), m = s->vlen;
    const char *w = s->value;
    #define SAME(p, q, len) same_bytes((p), (q), (len), ci)
    switch (s->op) {
    case A_EQ:
        return n == m && (m == 0 || SAME(v, w, m));
    case A_INCLUDES:
        if (m == 0)
            return false;
        for (size_t i = 0; i < n;) {
            while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == '\n' || v[i] == '\f' || v[i] == '\r'))
                i++;
            size_t start = i;
            while (i < n && !(v[i] == ' ' || v[i] == '\t' || v[i] == '\n' || v[i] == '\f' || v[i] == '\r'))
                i++;
            if (i - start == m && SAME(v + start, w, m))
                return true;
        }
        return false;
    case A_DASH:
        return (n == m && (m == 0 || SAME(v, w, m))) ||
               (n > m && v[m] == '-' && (m == 0 || SAME(v, w, m)));
    case A_PREFIX:
        return m > 0 && n >= m && SAME(v, w, m);
    case A_SUFFIX:
        return m > 0 && n >= m && SAME(v + n - m, w, m);
    case A_SUBSTRING:
        if (m == 0 || n < m)
            return false;
        for (size_t i = 0; i + m <= n; i++)
            if (SAME(v + i, w, m))
                return true;
        return false;
    default:
        return false;
    }
    #undef SAME
}

static bool attr_matches(const Simple *s, const os64_html_node_t *el)
{
    for (const os64_html_attr_t *a = el->attrs; a != NULL; a = a->next) {
        if (s->no_ns && a->ns != NULL)
            continue;
        size_t alen = os64_strlen(a->name);
        bool same = html(el) ? ieq2(a->name, alen, s->name, s->len)
                             : (alen == s->len && os64_memcmp(a->name, s->name, alen) == 0);
        if (same && value_matches(s, el, a->value != NULL ? a->value : ""))
            return true;
    }
    return false;
}

// The space-separated class list holds `name`.
static bool has_class(const os64_html_node_t *el, const char *name, size_t len, bool ci)
{
    const char *v = attr(el, "class");
    if (v == NULL)
        return false;
    for (size_t i = 0, n = os64_strlen(v); i < n;) {
        while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == '\n' || v[i] == '\f' || v[i] == '\r'))
            i++;
        size_t start = i;
        while (i < n && !(v[i] == ' ' || v[i] == '\t' || v[i] == '\n' || v[i] == '\f' || v[i] == '\r'))
            i++;
        if (i > start && (ci ? ieq2(v + start, i - start, name, len)
                             : (i - start == len && os64_memcmp(v + start, name, len) == 0)))
            return true;
    }
    return false;
}

// The position among the siblings `nth` counts: 1-based, from the start
// or the end, of any type or of this one, those matching `of` only.
static int32_t position(const garb_selectors_t *sel, const os64_html_node_t *el, bool from_end,
                        bool of_type, const List *of)
{
    int32_t pos = 1;
    for (const os64_html_node_t *s = from_end ? next_el(el) : prev_el(el); s != NULL;
         s = from_end ? next_el(s) : prev_el(s)) {
        if (of_type && !same_type(s, el))
            continue;
        if (of != NULL && !match_list(sel, of, s, NULL))
            continue;
        pos++;
    }
    return pos;
}

// Whether 1-based `pos` is An+B for some n >= 0.
static bool nth(int32_t a, int32_t b, int32_t pos)
{
    int64_t d = (int64_t)pos - b;
    if (a == 0)
        return d == 0;
    return d % a == 0 && d / a >= 0;
}

// HTML's disabled: the attribute on a form control, an option in a disabled
// optgroup, or a control inside a disabled fieldset but not inside its first
// legend.
static bool disabled(const os64_html_node_t *el)
{
    bool control = named(el, "button") || named(el, "input") || named(el, "select") ||
                   named(el, "textarea") || named(el, "optgroup") || named(el, "option") ||
                   named(el, "fieldset");
    if (!control)
        return false;
    if (attr(el, "disabled") != NULL)
        return true;
    if (named(el, "option") && el->parent != NULL && is_el(el->parent) &&
        named(el->parent, "optgroup") && attr(el->parent, "disabled") != NULL)
        return true;
    if (named(el, "optgroup") || named(el, "option"))
        return false;
    for (const os64_html_node_t *up = parent_el(el), *below = el; up != NULL;
         below = up, up = parent_el(up)) {
        if (!named(up, "fieldset") || attr(up, "disabled") == NULL)
            continue;
        const os64_html_node_t *legend = NULL;
        for (const os64_html_node_t *k = up->first_child; k != NULL; k = k->next)
            if (is_el(k) && named(k, "legend")) {
                legend = k;
                break;
            }
        if (below != legend)
            return true;
    }
    return false;
}

static bool is_link(const os64_html_node_t *el)
{
    return (named(el, "a") || named(el, "area")) && attr(el, "href") != NULL;
}

static bool is_input_type(const os64_html_node_t *el, const char *type)
{
    const char *t = attr(el, "type");
    return named(el, "input") && t != NULL && ascii_ieq(t, os64_strlen(t), type);
}

// An input whose type a person types text into, and so may be read-write.
static bool text_input(const os64_html_node_t *el)
{
    if (!named(el, "input"))
        return false;
    const char *t = attr(el, "type");
    if (t == NULL)
        return true;
    static const char *const kText[] = {"text", "search", "url", "tel", "email", "password",
                                        "date", "month", "week", "time", "datetime-local",
                                        "number", ""};
    for (size_t k = 0; k < sizeof(kText) / sizeof(kText[0]); k++)
        if (ascii_ieq(t, os64_strlen(t), kText[k]))
            return true;
    return false;
}

// :lang(): the nearest `lang` wins, matched as a prefix ending at a hyphen.
static bool lang_matches(const Simple *s, const os64_html_node_t *el)
{
    const char *lang = NULL;
    for (const os64_html_node_t *up = el; up != NULL; up = parent_el(up)) {
        lang = attr(up, "lang");
        if (lang == NULL)
            lang = attr(up, "xml:lang");
        if (lang != NULL)
            break;
    }
    if (lang == NULL)
        return false;
    size_t n = os64_strlen(lang);
    for (int32_t k = 0; k < s->nargs; k++) {
        const garb_value_t *v = &s->args[k];
        if (v->kind != GARB_IDENT && v->kind != GARB_STRING)
            continue;
        if (v->len == 1 && v->text[0] == '*')
            return n > 0;
        if (n >= v->len && ieq2(lang, v->len, v->text, v->len) && (n == v->len || lang[v->len] == '-'))
            return true;
    }
    return false;
}

static bool dir_matches(const Simple *s, const os64_html_node_t *el)
{
    bool rtl = false;
    for (const os64_html_node_t *up = el; up != NULL; up = parent_el(up)) {
        const char *d = attr(up, "dir");
        if (d != NULL && (ascii_ieq(d, os64_strlen(d), "rtl") || ascii_ieq(d, os64_strlen(d), "ltr"))) {
            rtl = ascii_ieq(d, os64_strlen(d), "rtl");
            break;
        }
    }
    for (int32_t k = 0; k < s->nargs; k++)
        if (s->args[k].kind == GARB_IDENT)
            return ascii_ieq(s->args[k].text, s->args[k].len, rtl ? "rtl" : "ltr");
    return false;
}

static bool has_matches(const garb_selectors_t *sel, const List *l, const os64_html_node_t *el);

static bool pseudo_matches(const garb_selectors_t *sel, const Simple *s,
                           const os64_html_node_t *el, const os64_html_node_t *scope)
{
    switch (s->pseudo) {
    case P_ROOT: return el->parent == NULL || el->parent->kind == OS64_HTML_DOCUMENT;
    case P_SCOPE: return scope != NULL ? el == scope
                                       : (el->parent == NULL || el->parent->kind == OS64_HTML_DOCUMENT);
    case P_FIRST_CHILD: return prev_el(el) == NULL;
    case P_LAST_CHILD: return next_el(el) == NULL;
    case P_ONLY_CHILD: return prev_el(el) == NULL && next_el(el) == NULL;
    case P_FIRST_OF_TYPE: return position(sel, el, false, true, NULL) == 1;
    case P_LAST_OF_TYPE: return position(sel, el, true, true, NULL) == 1;
    case P_ONLY_OF_TYPE:
        return position(sel, el, false, true, NULL) == 1 && position(sel, el, true, true, NULL) == 1;
    case P_NTH_CHILD:
    case P_NTH_LAST_CHILD:
        if (s->list != NULL && !match_list(sel, s->list, el, NULL))
            return false;
        return nth(s->a, s->b, position(sel, el, s->pseudo == P_NTH_LAST_CHILD, false, s->list));
    case P_NTH_OF_TYPE:
    case P_NTH_LAST_OF_TYPE:
        return nth(s->a, s->b, position(sel, el, s->pseudo == P_NTH_LAST_OF_TYPE, true, NULL));
    case P_EMPTY:
        for (const os64_html_node_t *k = el->first_child; k != NULL; k = k->next)
            if (k->kind == OS64_HTML_ELEMENT || (k->kind == OS64_HTML_TEXT && k->text_len > 0))
                return false;
        return true;
    case P_LINK:
    case P_ANY_LINK: return is_link(el);
    case P_NEVER: return false;
    case P_ENABLED:
        return (named(el, "button") || named(el, "input") || named(el, "select") ||
                named(el, "textarea") || named(el, "optgroup") || named(el, "option") ||
                named(el, "fieldset")) && !disabled(el);
    case P_DISABLED: return disabled(el);
    case P_CHECKED:
        return ((is_input_type(el, "checkbox") || is_input_type(el, "radio")) &&
                attr(el, "checked") != NULL) ||
               (named(el, "option") && attr(el, "selected") != NULL);
    case P_REQUIRED:
    case P_OPTIONAL: {
        bool field = named(el, "input") || named(el, "select") || named(el, "textarea");
        bool req = field && attr(el, "required") != NULL;
        return field && (s->pseudo == P_REQUIRED ? req : !req);
    }
    case P_READ_WRITE:
    case P_READ_ONLY: {
        bool rw = ((text_input(el) || named(el, "textarea")) && attr(el, "readonly") == NULL &&
                   !disabled(el));
        return s->pseudo == P_READ_WRITE ? rw : !rw;
    }
    case P_DEFINED: return true;
    case P_LANG: return lang_matches(s, el);
    case P_DIR: return dir_matches(s, el);
    case P_NOT: return !match_list(sel, s->list, el, scope);
    case P_IS:
    case P_WHERE: return match_list(sel, s->list, el, scope);
    case P_HAS: return has_matches(sel, s->list, el);
    }
    return false;
}

static bool compound_matches(const garb_selectors_t *sel, const Compound *cp,
                             const os64_html_node_t *el, const os64_html_node_t *scope)
{
    bool quirky = sel->quirks == OS64_HTML_QUIRKS;
    for (int32_t j = 0; j < cp->n; j++) {
        const Simple *s = &cp->s[j];
        switch (s->kind) {
        case S_UNIVERSAL:
            if (s->no_ns)
                return false;           // nothing in HTML is in no namespace
            break;
        case S_TYPE: {
            if (s->no_ns || el->name == NULL)
                return false;
            size_t n = os64_strlen(el->name);
            bool same = html(el) ? ieq2(el->name, n, s->name, s->len)
                                 : (n == s->len && os64_memcmp(el->name, s->name, n) == 0);
            if (!same)
                return false;
            break;
        }
        case S_ID: {
            const char *id = attr(el, "id");
            if (id == NULL)
                return false;
            size_t n = os64_strlen(id);
            if (quirky ? !ieq2(id, n, s->name, s->len)
                       : (n != s->len || os64_memcmp(id, s->name, n) != 0))
                return false;
            break;
        }
        case S_CLASS:
            if (!has_class(el, s->name, s->len, quirky))
                return false;
            break;
        case S_ATTR:
            if (!attr_matches(s, el))
                return false;
            break;
        case S_PSEUDO:
            if (!pseudo_matches(sel, s, el, scope))
                return false;
            break;
        }
    }
    return true;
}

// Compound k and every compound left of it, from `el`: the matching
// algorithm browsers use, whose answers say how far a failure reaches — no
// later sibling can succeed, or no ancestor can — so a search does not try
// again what has already failed.
static Result match_from(const garb_selectors_t *sel, const Complex *x, int32_t k,
                         const os64_html_node_t *el, const os64_html_node_t *scope)
{
    if (!compound_matches(sel, &x->c[k], el, scope))
        return NOT_SIBLING;
    Comb comb = k > 0 ? x->c[k].left : x->relative;
    if (k == 0 && comb == C_NONE)
        return MATCHED;
    Result none = comb == C_NEXT || comb == C_SUBSEQUENT ? NOT_DESCENDANT : NOT_GLOBALLY;
    const os64_html_node_t *at = el;
    for (;;) {
        at = comb == C_CHILD || comb == C_DESCENDANT ? parent_el(at) : prev_el(at);
        if (at == NULL)
            return none;
        Result r;
        if (k == 0)
            r = at == scope ? MATCHED : NOT_SIBLING;    // :has()'s anchor
        else
            r = match_from(sel, x, k - 1, at, scope);
        if (r == MATCHED || r == NOT_GLOBALLY || comb == C_NEXT)
            return r;
        if (comb == C_CHILD)
            return NOT_DESCENDANT;
        if (comb == C_SUBSEQUENT && r == NOT_DESCENDANT)
            return r;
    }
}

static bool match_complex(const garb_selectors_t *sel, const Complex *x,
                          const os64_html_node_t *el, const os64_html_node_t *scope)
{
    return match_from(sel, x, x->n - 1, el, scope) == MATCHED;
}

// :has(): some element the relative selector reaches from `el` — its
// subtree for a descendant or child, its following siblings and their
// subtrees for a sibling — matches with `el` as the anchor. Brute force,
// which is what makes :has() costly and why GARB.md watches it.
static bool subtree_has(const garb_selectors_t *sel, const Complex *x, const os64_html_node_t *root,
                        const os64_html_node_t *scope, bool include_root)
{
    if (include_root && match_complex(sel, x, root, scope))
        return true;
    for (const os64_html_node_t *n = root->first_child; n != NULL;) {
        if (is_el(n) && match_complex(sel, x, n, scope))
            return true;
        if (n->first_child != NULL) {
            n = n->first_child;
            continue;
        }
        while (n != root && n->next == NULL)
            n = n->parent;
        if (n == root)
            break;
        n = n->next;
    }
    return false;
}

static bool has_matches(const garb_selectors_t *sel, const List *l, const os64_html_node_t *el)
{
    for (int32_t i = 0; l != NULL && i < l->n; i++) {
        const Complex *x = &l->v[i];
        if (x->relative == C_NEXT || x->relative == C_SUBSEQUENT) {
            for (const os64_html_node_t *s = next_el(el); s != NULL; s = next_el(s))
                if (subtree_has(sel, x, s, el, true))
                    return true;
        } else if (subtree_has(sel, x, el, el, false)) {
            return true;
        }
    }
    return false;
}

bool garb_selector_matches(const garb_selectors_t *list, int32_t i,
                           const os64_html_node_t *element)
{
    if (list == NULL || i < 0 || i >= list->list.n || !is_el(element))
        return false;
    return match_complex(list, &list->list.v[i], element, NULL);
}
