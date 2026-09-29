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
    if (p_spent(p))
        return false;
    if (*n == *cap) {
        int32_t grown = *cap > 0 ? *cap * 2 : 4;
        void *bigger = garb_room(p->arena, (size_t)grown * size, p->top + GARB_CLOSE_SLACK)
                           ? os64_arena_alloc(p->arena, (size_t)grown * size)
                           : NULL;
        if (bigger == NULL) {
            p->incomplete = true;
            p->spent = true;
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

// ── The lists a parse builds ────────────────────────────────────────────
//
// A LIST IS BUILT ON THE SCRATCH STACK AND KEPT AT ITS EXACT SIZE. Grown in
// the arena, a list doubled and left every smaller copy behind, which the
// arena never takes back: a twelve-token prelude cost three times its own
// size, and a sheet of ordinary CSS filled the arena at a tenth of what it
// admits as text. Lists nest the way the grammar does — a block's contents
// finish before the rule round them, a function's arguments before the
// value that holds them — so one stack serves them all: a list opens at
// the stack's end, grows there, and is copied into the arena once, when it
// closes, which gives the stack back to the list that was open below it. A
// list the grammar abandons (a rule with no block) is simply never closed:
// the next push to the list below starts at that list's own end, and
// writes over it.

#define LIST_ALIGN 16u

// garb.h promises a value packs to this: a sheet is mostly values.
_Static_assert(sizeof(garb_value_t) == 56, "garb_value_t packs to 56 bytes");

typedef struct {
    size_t start;           // its first item's offset in the scratch stack
    int32_t n;
} List;

static List list_open(const Parse *p)
{
    List l = {(p->top + LIST_ALIGN - 1) & ~(size_t)(LIST_ALIGN - 1), 0};
    return l;
}

static bool list_push(Parse *p, List *l, size_t size, const void *item)
{
    if (p_spent(p))
        return false;
    size_t end = l->start + (size_t)l->n * size;
    // A list grows only while the arena could still keep every open list.
    if (!garb_room(p->arena, 0, end + size + GARB_CLOSE_SLACK)) {
        p->incomplete = true;
        p->spent = true;
        return false;
    }
    if (end + size > p->scratch_cap) {
        size_t cap = p->scratch_cap > 0 ? p->scratch_cap : 4096;
        while (cap < end + size)
            cap *= 2;
        char *grown = os64_realloc(p->scratch, cap);
        if (grown == NULL) {
            p->incomplete = true;
            p->spent = true;
            return false;
        }
        p->scratch = grown;
        p->scratch_cap = cap;
    }
    os64_memcpy(p->scratch + end, item, size);
    l->n++;
    p->top = end + size;
    return true;
}

// The open list's items, until the next push to any list.
static void *list_items(const Parse *p, const List *l)
{
    return l->n > 0 ? p->scratch + l->start : NULL;
}

// Keeps the list in the arena at its size and gives its stack back. NULL,
// and `*n` 0, for an empty list, or one the heap will not give the arena
// room for — which spends the parse, as any failed allocation does. The
// arena's own cap never refuses a close: every other allocation left room
// for it (internal.h § Room for what is open). A spent parse still keeps
// what it can: what was finished before the failure is real.
static void *list_close(Parse *p, List *l, size_t size, int32_t *n)
{
    void *kept = NULL;
    if (l->n > 0) {
        kept = os64_arena_alloc(p->arena, (size_t)l->n * size);
        if (kept != NULL) {
            os64_memcpy(kept, p->scratch + l->start, (size_t)l->n * size);
        } else {
            p->incomplete = true;
            p->spent = true;
        }
    }
    *n = kept != NULL ? l->n : 0;
    p->top = l->start;
    l->n = 0;
    return kept;
}

static void push_value(Parse *p, List *l, const garb_value_t *v)
{
    (void)list_push(p, l, sizeof(*v), v);
}

static void push_item(Parse *p, List *l, const garb_item_t *it)
{
    (void)list_push(p, l, sizeof(*it), it);
}

static garb_value_t *close_values(Parse *p, List *l, int32_t *n)
{
    return list_close(p, l, sizeof(garb_value_t), n);
}

// ── The input ───────────────────────────────────────────────────────────

// A spent parse's input has ended for every reader: a token peeked (or
// restored by a mark) before memory ran out is dropped, or a loop that
// peeks it would never see the EOF its next read returns.
static bool in_ended(Input *in)
{
    if (in->owner == NULL || !p_spent(in->owner))
        return false;
    in->have_peek = false;
    return true;
}

static Tok in_next(Input *in)
{
    if (in_ended(in))
        return (Tok){T_EOF, {0}};
    if (in->have_peek) {
        in->have_peek = false;
        return in->peeked;
    }
    if (in->tz != NULL) {
        // A token whose text could not be built whole is not a token.
        Tok t = tz_next(in->tz);
        return in->tz->short_of_memory ? (Tok){T_EOF, {0}} : t;
    }
    if (in->at < in->n) {
        Tok t = {T_VALUE, in->values[in->at++]};
        return t;
    }
    return (Tok){T_EOF, {0}};
}

static Tok in_peek(Input *in)
{
    if (in_ended(in))
        return (Tok){T_EOF, {0}};
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

// Past the depth bound a block's contents are read only to find its end,
// and without recursing: what nests there nests as deep as the text does
// (eight megabytes of `(`), so the closers still owed are kept on a list,
// innermost last — a `)` inside a `[` is not the `[`'s end (§5.4.9) — and
// the stack stays as deep as the bound. A list that cannot grow spends the
// parse, like any allocation that fails.
static void skip_contents(Parse *p, Input *in, char end)
{
    char *owed = NULL;
    size_t n = 0, cap = 0;
    char want = end;
    for (;;) {
        Tok t = in_next(in);
        if (t.kind == T_EOF)
            break;
        if (is_close(&t, want)) {
            if (n == 0)
                break;
            want = owed[--n];
            continue;
        }
        if (t.kind != T_OPEN && t.kind != T_FUNCTION)
            continue;
        if (n == cap) {
            size_t cap2 = cap != 0 ? cap * 2 : 64;
            char *grown = os64_realloc(owed, cap2);
            if (grown == NULL) {
                p->spent = true;
                break;
            }
            owed = grown;
            cap = cap2;
        }
        owed[n++] = want;
        want = t.kind == T_OPEN ? closer(t.v.open) : ')';
    }
    os64_free(owed);
    p->incomplete = true;
}

// A block or function's contents, up to `end` (or the input's end). Past
// the depth bound they are read to find the end, and not kept.
static void consume_contents(Parse *p, Input *in, char end, garb_value_t *into)
{
    if (p->depth >= GARB_DEPTH_MAX) {
        skip_contents(p, in, end);
        into->children = NULL;
        into->nchildren = 0;
        return;
    }
    List kids = list_open(p);
    p->depth++;
    for (;;) {
        Tok t = in_next(in);
        if (t.kind == T_EOF || is_close(&t, end))
            break;
        garb_value_t v = consume_component_value(p, in, t);
        push_value(p, &kids, &v);
    }
    p->depth--;
    into->children = close_values(p, &kids, &into->nchildren);
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
    List value = list_open(p);
    for (;;) {
        t = in_peek(in);
        if (t.kind == T_EOF || (!whole && is_kind(&t, GARB_SEMICOLON)))
            break;
        if (is_close(&t, '}') && nested)
            break;
        garb_value_t v = consume_component_value(p, in, in_next(in));
        push_value(p, &value, &v);
    }
    // Judged on the stack, and kept only as far as it is the value.
    const garb_value_t *vs = list_items(p, &value);
    // `!important` last, white space around it and between allowed.
    int32_t end = value.n;
    while (end > 0 && blank(&vs[end - 1]))
        end--;
    if (end >= 2 && vs[end - 1].kind == GARB_IDENT &&
        ascii_is(vs[end - 1].text, vs[end - 1].len, "important")) {
        int32_t bang = end - 2;
        while (bang >= 0 && blank(&vs[bang]))
            bang--;
        if (bang >= 0 && vs[bang].kind == GARB_DELIM && vs[bang].len == 1 &&
            vs[bang].text[0] == '!') {
            d->important = true;
            end = bang;
        }
    }
    while (end > 0 && blank(&vs[end - 1]))
        end--;
    // A value holding a {} block and anything else is not a declaration:
    // it is a nested rule that begins like one (`a:hover { … }`), and the
    // caller reads it again as a rule. A custom property may hold anything.
    bool custom = d->len >= 2 && d->name[0] == '-' && d->name[1] == '-';
    if (!custom) {
        bool block = false, other = false;
        for (int32_t i = 0; i < end; i++) {
            if (vs[i].kind == GARB_BLOCK && vs[i].open == '{')
                block = true;
            else if (!blank(&vs[i]))
                other = true;
        }
        if (block && other) {
            p->top = value.start;           // abandoned: never kept
            return false;
        }
    }
    value.n = end;
    d->value = close_values(p, &value, &d->nvalue);
    return true;
}

// ── §5.4.2–§5.4.4 Rules ─────────────────────────────────────────────────

static garb_rule_t *new_rule(Parse *p)
{
    garb_rule_t *r = garb_room(p->arena, sizeof(*r), p->top + GARB_CLOSE_SLACK)
                         ? os64_arena_calloc(p->arena, 1, sizeof(*r))
                         : NULL;
    if (r == NULL) {
        p->incomplete = true;
        p->spent = true;
    }
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
    List prelude = list_open(p);
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
    r->prelude = close_values(p, &prelude, &r->nprelude);
    garb_rule_t *kept = new_rule(p);
    if (kept != NULL)
        *kept = rule;
    return kept;
}

// §5.4.3. NULL where the specification returns nothing.
static garb_rule_t *consume_qualified_rule(Parse *p, Input *in, bool nested, bool stop_at_semicolon)
{
    List prelude = list_open(p);
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
            const garb_value_t *pv = list_items(p, &prelude);
            int32_t i = 0;
            while (i < prelude.n && blank(&pv[i]))
                i++;
            bool dashed = i < prelude.n && pv[i].kind == GARB_IDENT && pv[i].len >= 2 &&
                          pv[i].text[0] == '-' && pv[i].text[1] == '-';
            int32_t j = i + 1;
            while (j < prelude.n && blank(&pv[j]))
                j++;
            if (dashed && j < prelude.n && pv[j].kind == GARB_COLON) {
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
            r->prelude = close_values(p, &prelude, &r->nprelude);
            return r;
        }
        garb_value_t v = consume_component_value(p, in, in_next(in));
        push_value(p, &prelude, &v);
    }
}

static void push_rule(Parse *p, List *items, garb_rule_t *r)
{
    garb_item_t it = {0};
    it.kind = r != NULL ? GARB_ITEM_RULE : GARB_ITEM_INVALID;
    it.rule = r;
    push_item(p, items, &it);
}

// §5.4.1 "consume a stylesheet's contents", and the list of rules a
// grouping at-rule holds, where `<!--` and `-->` mean nothing special.
static void consume_rule_list(Parse *p, Input *in, bool top_level, List *items)
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
static void consume_block_contents(Parse *p, Input *in, List *items)
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
static void consume_declaration_list(Parse *p, Input *in, List *items)
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
    os64_memset(p, 0, sizeof(*p));
    p->arena = os64_arena_create(0, GARB_ARENA_MAX);
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
    List items = list_open(p);
    List values = list_open(p);
    garb_status_t st = GARB_OK;
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
        if (t.kind == T_EOF) {
            st = GARB_EMPTY;
            break;
        }
        garb_rule_t *r = is_kind(&t, GARB_AT_KEYWORD) ? consume_at_rule(p, in, false)
                                                       : consume_qualified_rule(p, in, false, false);
        skip_ws(in);
        if (r == NULL)
            st = GARB_INVALID;
        else if (in_peek(in).kind != T_EOF)
            st = GARB_EXTRA_INPUT;
        else
            out->rule = r;
        break;
    }
    case M_ONE_DECL: {
        skip_ws(in);
        Tok t = in_peek(in);
        if (t.kind == T_EOF)
            st = GARB_EMPTY;
        else if (!consume_declaration(p, in, false, true, &out->decl))
            st = GARB_INVALID;
        break;
    }
    case M_ONE_VALUE: {
        skip_ws(in);
        Tok t = in_next(in);
        if (t.kind == T_EOF) {
            st = GARB_EMPTY;
            break;
        }
        garb_value_t v = consume_component_value(p, in, t);
        skip_ws(in);
        if (in_peek(in).kind != T_EOF)
            st = GARB_EXTRA_INPUT;
        else
            push_value(p, &values, &v);
        break;
    }
    }
    // A single item is published whole or not at all, and a spent parse
    // decides nothing: the EOF that ended it may be where memory ran out,
    // not where the text did, so EMPTY, EXTRA_INPUT and INVALID are no
    // more true of it than OK is.
    // Only one of the two lists was used, so closing both keeps its own.
    out->items = list_close(p, &items, sizeof(garb_item_t), &out->nitems);
    out->values = close_values(p, &values, &out->nvalues);
    os64_free(p->scratch);
    p->scratch = NULL;
    p->top = p->scratch_cap = 0;
    if (p_spent(p) && (mode == M_ONE_RULE || mode == M_ONE_DECL || mode == M_ONE_VALUE)) {
        out->rule = NULL;
        os64_memset(&out->decl, 0, sizeof(out->decl));
        out->values = NULL;
        out->nvalues = 0;
        return GARB_NO_MEMORY;
    }
    return st;
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
    p.tz = &tz;
    tz.open = &p.top;
    Input in = {0};
    in.tz = &tz;
    in.owner = &p;
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
    Parse p = {.arena = owner->arena};
    Input in = {0};
    in.values = values;
    in.n = n;
    in.owner = &p;
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
