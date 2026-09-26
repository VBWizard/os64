// parse.c — CSS Syntax Level 3 §5: tokens in, rules, declarations and
// component values out.
//
// Each consume_* is §5.4's algorithm of the same name, its steps in order.
// Nothing is ever reported: a parse error in CSS is a rule for recovering,
// and the rule is what is written here. What the result keeps of an error
// is only what a reader needs to find it: an INVALID item where a rule or a
// declaration was thrown away, an UNMATCHED value where a closing bracket
// had nothing to close.

#include "internal.h"
#include "os64/mem.h"
#include "os64/str.h"

// ── Lists in the arena ──────────────────────────────────────────────────

bool p_push(Parse *p, void **list, int32_t *n, int32_t *cap, size_t size, const void *item)
{
    if (*n == *cap) {
        int32_t grown = *cap > 0 ? *cap * 2 : 4;
        void *bigger = os64_arena_alloc(p->arena, (size_t)grown * size);
        if (bigger == NULL) {
            p->incomplete = true;
            return false;
        }
        if (*n > 0)
            os64_memcpy(bigger, *list, (size_t)*n * size);
        *list = bigger;
        *cap = grown;
    }
    os64_memcpy((char *)*list + (size_t)*n * size, item, size);
    (*n)++;
    return true;
}

typedef struct {
    garb_value_t *v;
    int32_t n, cap;
} Values;

typedef struct {
    garb_item_t *v;
    int32_t n, cap;
} Items;

static void push_value(Parse *p, Values *l, const garb_value_t *v)
{
    (void)p_push(p, (void **)&l->v, &l->n, &l->cap, sizeof(*v), v);
}

static void push_item(Parse *p, Items *l, const garb_item_t *it)
{
    (void)p_push(p, (void **)&l->v, &l->n, &l->cap, sizeof(*it), it);
}

// ── The input ───────────────────────────────────────────────────────────

static Tok in_next(Input *in)
{
    if (in->have_peek) {
        in->have_peek = false;
        return in->peeked;
    }
    if (in->tz != NULL)
        return tz_next(in->tz);
    if (in->at < in->n) {
        Tok t = {T_VALUE, in->values[in->at++]};
        return t;
    }
    return (Tok){T_EOF, {0}};
}

static Tok in_peek(Input *in)
{
    if (!in->have_peek) {
        in->peeked = in_next(in);
        in->have_peek = true;
    }
    return in->peeked;
}

typedef struct {
    size_t pos;
    int32_t at;
    bool have_peek;
    Tok peeked;
} Mark;

static Mark mark(const Input *in)
{
    Mark m = {in->tz != NULL ? in->tz->pos : 0, in->at, in->have_peek, in->peeked};
    return m;
}

static void restore(Input *in, const Mark *m)
{
    if (in->tz != NULL)
        in->tz->pos = m->pos;
    in->at = m->at;
    in->have_peek = m->have_peek;
    in->peeked = m->peeked;
}

// ── What a token is ─────────────────────────────────────────────────────

static bool is_kind(const Tok *t, garb_kind_t k)
{
    return t->kind == T_VALUE && t->v.kind == k;
}

static bool is_close(const Tok *t, char which)
{
    return t->kind == T_CLOSE && t->v.open == which;
}

// A `{` opening a block: the token in text, or a whole `{}` block in a list
// of component values.
static bool is_curly(const Tok *t)
{
    return (t->kind == T_OPEN && t->v.open == '{') ||
           (t->kind == T_VALUE && t->v.kind == GARB_BLOCK && t->v.open == '{');
}

static void skip_ws(Input *in)
{
    for (;;) {
        Tok t = in_peek(in);
        if (!is_kind(&t, GARB_WHITESPACE))
            return;
        in_next(in);
    }
}

static char closer(char open)
{
    return open == '{' ? '}' : open == '[' ? ']' : ')';
}

static bool ascii_is(const char *s, size_t len, const char *word)
{
    size_t n = os64_strlen(word);
    if (len != n)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != word[i])
            return false;
    }
    return true;
}

// ── §5.4.8–§5.4.10 Component values ─────────────────────────────────────

static garb_value_t consume_component_value(Parse *p, Input *in, Tok t);

// A block or function's contents, up to `end` (or the input's end). Past
// the depth bound they are read to find the end, and not kept.
static void consume_contents(Parse *p, Input *in, char end, garb_value_t *into)
{
    Values kids = {0};
    bool keep = p->depth < GARB_DEPTH_MAX;
    if (!keep)
        p->incomplete = true;
    p->depth++;
    for (;;) {
        Tok t = in_next(in);
        if (t.kind == T_EOF || is_close(&t, end))
            break;
        garb_value_t v = consume_component_value(p, in, t);
        if (keep)
            push_value(p, &kids, &v);
    }
    p->depth--;
    into->children = kids.v;
    into->nchildren = kids.n;
}

// §5.4.8, the token already consumed.
static garb_value_t consume_component_value(Parse *p, Input *in, Tok t)
{
    garb_value_t v = t.v;
    switch (t.kind) {
    case T_OPEN:
        v.kind = GARB_BLOCK;
        consume_contents(p, in, closer(t.v.open), &v);   // §5.4.9
        return v;
    case T_FUNCTION:
        v.kind = GARB_FUNCTION;
        consume_contents(p, in, ')', &v);                // §5.4.10
        return v;
    case T_CLOSE:
        v.kind = GARB_UNMATCHED;
        return v;
    default:
        return v;
    }
}

// ── §5.4.6 Declarations ─────────────────────────────────────────────────

// §5.4.7 "consume the remnants of a bad declaration"
static void consume_bad_declaration(Parse *p, Input *in, bool nested)
{
    for (;;) {
        Tok t = in_peek(in);
        if (t.kind == T_EOF || is_kind(&t, GARB_SEMICOLON))
            return;
        if (is_close(&t, '}') && nested)
            return;
        (void)consume_component_value(p, in, in_next(in));
    }
}

static bool blank(const garb_value_t *v)
{
    return v->kind == GARB_WHITESPACE;
}

// §5.4.6. `whole` reads to the end of the input, a semicolon included — what
// "parse a declaration" does with text that is meant to be one.
static bool consume_declaration(Parse *p, Input *in, bool nested, bool whole, garb_decl_t *d)
{
    os64_memset(d, 0, sizeof(*d));
    Tok t = in_peek(in);
    if (!is_kind(&t, GARB_IDENT)) {
        consume_bad_declaration(p, in, nested);
        return false;
    }
    t = in_next(in);
    d->name = t.v.text;
    d->len = t.v.len;
    skip_ws(in);
    t = in_peek(in);
    if (!is_kind(&t, GARB_COLON)) {
        consume_bad_declaration(p, in, nested);
        return false;
    }
    in_next(in);
    skip_ws(in);
    Values value = {0};
    for (;;) {
        t = in_peek(in);
        if (t.kind == T_EOF || (!whole && is_kind(&t, GARB_SEMICOLON)))
            break;
        if (is_close(&t, '}') && nested)
            break;
        garb_value_t v = consume_component_value(p, in, in_next(in));
        push_value(p, &value, &v);
    }
    // `!important` last, white space around it and between allowed.
    int32_t end = value.n;
    while (end > 0 && blank(&value.v[end - 1]))
        end--;
    if (end >= 2 && value.v[end - 1].kind == GARB_IDENT &&
        ascii_is(value.v[end - 1].text, value.v[end - 1].len, "important")) {
        int32_t bang = end - 2;
        while (bang >= 0 && blank(&value.v[bang]))
            bang--;
        if (bang >= 0 && value.v[bang].kind == GARB_DELIM && value.v[bang].len == 1 &&
            value.v[bang].text[0] == '!') {
            d->important = true;
            end = bang;
        }
    }
    while (end > 0 && blank(&value.v[end - 1]))
        end--;
    d->value = value.v;
    d->nvalue = end;
    // A value holding a {} block and anything else is not a declaration:
    // it is a nested rule that begins like one (`a:hover { … }`), and the
    // caller reads it again as a rule. A custom property may hold anything.
    bool custom = d->len >= 2 && d->name[0] == '-' && d->name[1] == '-';
    if (!custom) {
        bool block = false, other = false;
        for (int32_t i = 0; i < d->nvalue; i++) {
            const garb_value_t *v = &d->value[i];
            if (v->kind == GARB_BLOCK && v->open == '{')
                block = true;
            else if (!blank(v))
                other = true;
        }
        if (block && other)
            return false;
    }
    return true;
}

// ── §5.4.2–§5.4.4 Rules ─────────────────────────────────────────────────

static garb_rule_t *new_rule(Parse *p)
{
    garb_rule_t *r = os64_arena_calloc(p->arena, 1, sizeof(*r));
    if (r == NULL)
        p->incomplete = true;
    return r;
}

// A `{` in text, or a {} block in a list: its contents as component values.
static void consume_curly(Parse *p, Input *in, garb_rule_t *r)
{
    Tok t = in_next(in);
    r->has_block = true;
    if (t.kind == T_VALUE) {
        r->block = t.v.children;
        r->nblock = t.v.nchildren;
        return;
    }
    garb_value_t v = {0};
    consume_contents(p, in, '}', &v);
    r->block = v.children;
    r->nblock = v.nchildren;
}

// §5.4.2, the at-keyword next.
static garb_rule_t *consume_at_rule(Parse *p, Input *in, bool nested)
{
    Tok t = in_next(in);
    Values prelude = {0};
    // Read whole before it is kept, so a rule the arena cannot hold still
    // has its tokens consumed and the parse goes on past it.
    garb_rule_t rule = {0};
    garb_rule_t *r = &rule;
    r->at = true;
    r->name = t.v.text;
    r->len = t.v.len;
    for (;;) {
        t = in_peek(in);
        if (is_kind(&t, GARB_SEMICOLON)) {
            in_next(in);
            break;
        }
        if (t.kind == T_EOF)
            break;
        if (is_close(&t, '}') && nested)
            break;
        if (is_curly(&t)) {
            consume_curly(p, in, r);
            break;
        }
        garb_value_t v = consume_component_value(p, in, in_next(in));
        push_value(p, &prelude, &v);
    }
    r->prelude = prelude.v;
    r->nprelude = prelude.n;
    garb_rule_t *kept = new_rule(p);
    if (kept != NULL)
        *kept = rule;
    return kept;
}

// §5.4.3. NULL where the specification returns nothing.
static garb_rule_t *consume_qualified_rule(Parse *p, Input *in, bool nested, bool stop_at_semicolon)
{
    Values prelude = {0};
    for (;;) {
        Tok t = in_peek(in);
        if (t.kind == T_EOF)
            return NULL;
        if (stop_at_semicolon && is_kind(&t, GARB_SEMICOLON))
            return NULL;
        if (is_close(&t, '}') && nested)
            return NULL;
        if (is_curly(&t)) {
            // A custom property's shape (`--x: {…}`) is never a rule.
            int32_t i = 0;
            while (i < prelude.n && blank(&prelude.v[i]))
                i++;
            bool dashed = i < prelude.n && prelude.v[i].kind == GARB_IDENT &&
                          prelude.v[i].len >= 2 && prelude.v[i].text[0] == '-' &&
                          prelude.v[i].text[1] == '-';
            int32_t j = i + 1;
            while (j < prelude.n && blank(&prelude.v[j]))
                j++;
            if (dashed && j < prelude.n && prelude.v[j].kind == GARB_COLON) {
                garb_rule_t discard = {0};
                consume_curly(p, in, &discard);
                return NULL;
            }
            garb_rule_t *r = new_rule(p);
            if (r == NULL) {
                garb_rule_t discard = {0};
                consume_curly(p, in, &discard);
                return NULL;
            }
            consume_curly(p, in, r);
            r->prelude = prelude.v;
            r->nprelude = prelude.n;
            return r;
        }
        garb_value_t v = consume_component_value(p, in, in_next(in));
        push_value(p, &prelude, &v);
    }
}

static void push_rule(Parse *p, Items *items, garb_rule_t *r)
{
    garb_item_t it = {0};
    it.kind = r != NULL ? GARB_ITEM_RULE : GARB_ITEM_INVALID;
    it.rule = r;
    push_item(p, items, &it);
}

// §5.4.1 "consume a stylesheet's contents", and the list of rules a
// grouping at-rule holds, where `<!--` and `-->` mean nothing special.
static void consume_rule_list(Parse *p, Input *in, bool top_level, Items *items)
{
    for (;;) {
        Tok t = in_peek(in);
        if (t.kind == T_EOF)
            return;
        if (is_kind(&t, GARB_WHITESPACE)) {
            in_next(in);
            continue;
        }
        if (top_level && (is_kind(&t, GARB_CDO) || is_kind(&t, GARB_CDC))) {
            in_next(in);
            continue;
        }
        if (is_kind(&t, GARB_AT_KEYWORD))
            push_rule(p, items, consume_at_rule(p, in, false));
        else
            push_rule(p, items, consume_qualified_rule(p, in, false, false));
    }
}

// §5.4.5 "consume a block's contents": declarations, and rules nested
// among them, in order.
static void consume_block_contents(Parse *p, Input *in, Items *items)
{
    for (;;) {
        Tok t = in_peek(in);
        if (t.kind == T_EOF || is_close(&t, '}'))
            return;
        if (is_kind(&t, GARB_WHITESPACE) || is_kind(&t, GARB_SEMICOLON)) {
            in_next(in);
            continue;
        }
        if (is_kind(&t, GARB_AT_KEYWORD)) {
            push_rule(p, items, consume_at_rule(p, in, true));
            continue;
        }
        Mark m = mark(in);
        garb_item_t it = {0};
        if (consume_declaration(p, in, true, false, &it.decl)) {
            it.kind = GARB_ITEM_DECL;
            push_item(p, items, &it);
            continue;
        }
        restore(in, &m);
        push_rule(p, items, consume_qualified_rule(p, in, true, true));
    }
}

// The list of declarations of Syntax 3's 2021 draft — a block's contents
// with no qualified rule among them, what `@font-face` and `@page` hold.
static void consume_declaration_list(Parse *p, Input *in, Items *items)
{
    for (;;) {
        Tok t = in_peek(in);
        if (t.kind == T_EOF)
            return;
        if (is_kind(&t, GARB_WHITESPACE) || is_kind(&t, GARB_SEMICOLON)) {
            in_next(in);
            continue;
        }
        if (is_kind(&t, GARB_AT_KEYWORD)) {
            push_rule(p, items, consume_at_rule(p, in, false));
            continue;
        }
        garb_item_t it = {0};
        if (consume_declaration(p, in, false, false, &it.decl)) {
            it.kind = GARB_ITEM_DECL;
        } else {
            it.kind = GARB_ITEM_INVALID;
            // consume_declaration read to the semicolon; what is past it is
            // the next declaration's.
        }
        push_item(p, items, &it);
    }
}

// ── The entry points (§5.3) ─────────────────────────────────────────────

static bool start(garb_parsed_t *out, Parse *p)
{
    os64_memset(out, 0, sizeof(*out));
    p->arena = os64_arena_create(0, GARB_ARENA_MAX);
    p->incomplete = false;
    p->depth = 0;
    out->arena = p->arena;
    return p->arena != NULL;
}

static garb_status_t finish(garb_parsed_t *out, Parse *p, Tokenizer *tz, garb_status_t st)
{
    if (tz != NULL) {
        if (tz->short_of_memory)
            p->incomplete = true;
        tz_close(tz);
    }
    out->incomplete = p->incomplete;
    return st;
}

typedef enum { M_SHEET, M_RULES, M_BLOCK, M_DECLS, M_VALUES, M_ONE_RULE, M_ONE_DECL, M_ONE_VALUE } Mode;

static garb_status_t run(Mode mode, Parse *p, Input *in, garb_parsed_t *out)
{
    Items items = {0};
    Values values = {0};
    switch (mode) {
    case M_SHEET:
    case M_RULES:
        consume_rule_list(p, in, mode == M_SHEET, &items);
        break;
    case M_BLOCK:
        consume_block_contents(p, in, &items);
        break;
    case M_DECLS:
        consume_declaration_list(p, in, &items);
        break;
    case M_VALUES:
        for (;;) {
            Tok t = in_next(in);
            if (t.kind == T_EOF)
                break;
            garb_value_t v = consume_component_value(p, in, t);
            push_value(p, &values, &v);
        }
        break;
    case M_ONE_RULE: {
        skip_ws(in);
        Tok t = in_peek(in);
        if (t.kind == T_EOF)
            return GARB_EMPTY;
        garb_rule_t *r = is_kind(&t, GARB_AT_KEYWORD) ? consume_at_rule(p, in, false)
                                                       : consume_qualified_rule(p, in, false, false);
        if (r == NULL)
            return GARB_INVALID;
        skip_ws(in);
        if (in_peek(in).kind != T_EOF)
            return GARB_EXTRA_INPUT;
        out->rule = r;
        break;
    }
    case M_ONE_DECL: {
        skip_ws(in);
        Tok t = in_peek(in);
        if (t.kind == T_EOF)
            return GARB_EMPTY;
        if (!consume_declaration(p, in, false, true, &out->decl))
            return GARB_INVALID;
        break;
    }
    case M_ONE_VALUE: {
        skip_ws(in);
        Tok t = in_next(in);
        if (t.kind == T_EOF)
            return GARB_EMPTY;
        garb_value_t v = consume_component_value(p, in, t);
        skip_ws(in);
        if (in_peek(in).kind != T_EOF)
            return GARB_EXTRA_INPUT;
        push_value(p, &values, &v);
        break;
    }
    }
    out->items = items.v;
    out->nitems = items.n;
    out->values = values.v;
    out->nvalues = values.n;
    return GARB_OK;
}

static garb_status_t parse_text(Mode mode, const char *text, size_t len, garb_parsed_t *out)
{
    Parse p;
    if (!start(out, &p))
        return GARB_NO_MEMORY;
    if (len > GARB_SHEET_MAX)
        return finish(out, &p, NULL, GARB_TOO_BIG);
    Tokenizer tz;
    if (!tz_open(&tz, text, len, p.arena)) {
        tz_close(&tz);
        return finish(out, &p, NULL, GARB_NO_MEMORY);
    }
    Input in = {0};
    in.tz = &tz;
    garb_status_t st = run(mode, &p, &in, out);
    return finish(out, &p, &tz, st);
}

garb_status_t garb_parse_sheet_text(const char *text, size_t len, garb_parsed_t *out)
{
    return parse_text(M_SHEET, text, len, out);
}

garb_status_t garb_parse_rules(const char *text, size_t len, garb_parsed_t *out)
{
    return parse_text(M_RULES, text, len, out);
}

garb_status_t garb_parse_block(const char *text, size_t len, garb_parsed_t *out)
{
    return parse_text(M_BLOCK, text, len, out);
}

garb_status_t garb_parse_declaration_list(const char *text, size_t len, garb_parsed_t *out)
{
    return parse_text(M_DECLS, text, len, out);
}

garb_status_t garb_parse_values(const char *text, size_t len, garb_parsed_t *out)
{
    return parse_text(M_VALUES, text, len, out);
}

garb_status_t garb_parse_one_rule(const char *text, size_t len, garb_parsed_t *out)
{
    return parse_text(M_ONE_RULE, text, len, out);
}

garb_status_t garb_parse_one_declaration(const char *text, size_t len, garb_parsed_t *out)
{
    return parse_text(M_ONE_DECL, text, len, out);
}

garb_status_t garb_parse_one_value(const char *text, size_t len, garb_parsed_t *out)
{
    return parse_text(M_ONE_VALUE, text, len, out);
}

// Over component values a parse already made, into its arena.
static bool reparse(Mode mode, garb_parsed_t *owner, const garb_value_t *values, int32_t n,
                    garb_item_t **items, int32_t *nitems)
{
    Parse p = {owner->arena, false, 0};
    Input in = {0};
    in.values = values;
    in.n = n;
    garb_parsed_t tmp = {0};
    garb_status_t st = run(mode, &p, &in, &tmp);
    if (p.incomplete)
        owner->incomplete = true;
    *items = tmp.items;
    *nitems = tmp.nitems;
    return st == GARB_OK && !p.incomplete;
}

bool garb_rules_of(garb_parsed_t *owner, const garb_value_t *values, int32_t n,
                   garb_item_t **items, int32_t *nitems)
{
    return reparse(M_RULES, owner, values, n, items, nitems);
}

bool garb_items_of(garb_parsed_t *owner, const garb_value_t *values, int32_t n,
                   garb_item_t **items, int32_t *nitems)
{
    return reparse(M_BLOCK, owner, values, n, items, nitems);
}

void garb_free(garb_parsed_t *parsed)
{
    if (parsed == NULL)
        return;
    os64_arena_destroy(parsed->arena);
    os64_memset(parsed, 0, sizeof(*parsed));
}
