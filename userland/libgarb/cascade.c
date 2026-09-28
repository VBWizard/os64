// cascade.c — CSS Cascading and Inheritance Level 4 for the author origin,
// with Custom Properties 1 and Environment Variables 1 (garb/cascade.h).
//
// Build: every style rule of every sheet whose @media holds, its selectors
// filed in a RULE HASH by the rightmost compound's id, class or element
// name (the browsers' own trick: an element tries only the rules that could
// possibly match it), its declarations read once.
//
// Then, per element in document order: the rules that match, weighed by
// §6's order — importance, then specificity (a rule matching through
// several selectors takes the most specific), then order of appearance —
// with the `style` attribute above every sheet at the same importance; the
// winners' var() and env() replaced against the custom properties the
// element inherits and declares.

#include "values_internal.h"
#include "garb/cascade.h"
#include "garb/select.h"
#include "os64/mem.h"
#include "os64/str.h"

// How deep @media, @supports and @layer may nest, and var() substitution
// may recurse, before the rest is ignored and the cascade marked incomplete.
#define NEST_MAX 16
#define VAR_DEPTH_MAX 32
// What one custom property's value may grow to once its own var()s are
// replaced: the shape that makes `--a: var(--b) var(--b)` ten levels deep a
// billion values long is cut here.
#define VAR_VALUES_MAX 4096

// ── What the build keeps ────────────────────────────────────────────────

typedef struct {
    bool important;
    bool custom;                // --name: `value` kept as written
    bool pending;               // holds var() or env(): read per element
    garb_decl_t decl;
    garb_set_t *sets;           // read: the longhands it sets
    int32_t nsets;
} Decl;

typedef struct {
    garb_selectors_t *sel;
    Decl *decls;
    int32_t ndecls;
    uint32_t order;
    // The element this rule last matched, and the specificity it matched
    // with — so one rule matched through two selectors is counted once.
    const os64_html_node_t *seen;
    int32_t slot;
} Rule;

// A selector filed in the rule hash. It names its rule by INDEX: the rule
// array grows, and moves, while the sheets are still being read.
typedef struct {
    int32_t rule;
    int32_t sel;
    int32_t next;               // the next entry in the same bucket, or -1
} Entry;

// A bucket: entries whose key is the same string.
typedef struct {
    const char *key;
    size_t len;
    int32_t head;
} Bucket;

typedef struct {
    Bucket *b;
    uint32_t cap, n;
} Table;

// The custom properties an element sees: its own, then its parent's.
typedef struct CustomMap CustomMap;

typedef struct {
    const char *name;
    size_t len;
    const garb_value_t *value;  // as declared, or NULL: the guaranteed-invalid value
    int32_t nvalue;
    int state;                  // 0 as declared, 1 being replaced, 2 replaced, 3 invalid
} Custom;

struct CustomMap {
    const CustomMap *parent;
    Custom *own;
    int32_t n;
};

typedef struct {
    const os64_html_node_t *node;
    garb_style_t style;
    const CustomMap *customs;
} Slot;

struct garb_cascade {
    garb_parsed_t self;         // the cascade's own arena, in a parse's shape
    garb_env_t env;
    os64_html_quirks_t quirks;
    bool incomplete;
    Rule *rules;
    int32_t nrules, rulecap;
    Entry *entries;
    int32_t nentries, entrycap;
    Table ids, classes, types;
    int32_t universal;
    Slot *slots;                // open addressing, keyed by node
    uint32_t slotcap;
    garb_parsed_t *attrs;       // `style` attributes' parses, freed with the cascade
    int32_t nattrs, attrcap;
};

static void *alloc(garb_cascade_t *c, size_t n)
{
    void *p = os64_arena_calloc(c->self.arena, 1, n);
    if (p == NULL)
        c->incomplete = true;
    return p;
}

// Grows an arena list by doubling.
static bool grow(garb_cascade_t *c, void **list, int32_t n, int32_t *cap, size_t size)
{
    if (n < *cap)
        return true;
    int32_t bigger = *cap > 0 ? *cap * 2 : 16;
    void *next = os64_arena_alloc(c->self.arena, (size_t)bigger * size);
    if (next == NULL) {
        c->incomplete = true;
        return false;
    }
    if (n > 0)
        os64_memcpy(next, *list, (size_t)n * size);
    *list = next;
    *cap = bigger;
    return true;
}

// ── The rule hash ───────────────────────────────────────────────────────

static uint32_t hash_bytes(const char *s, size_t n, bool fold)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (fold && ch >= 'A' && ch <= 'Z')
            ch = (char)(ch - 'A' + 'a');
        h = (h ^ (uint8_t)ch) * 16777619u;
    }
    return h;
}

static bool same_key(const char *a, size_t alen, const char *b, size_t blen, bool fold)
{
    if (alen != blen)
        return false;
    for (size_t i = 0; i < alen; i++) {
        char x = a[i], y = b[i];
        if (fold) {
            if (x >= 'A' && x <= 'Z')
                x = (char)(x - 'A' + 'a');
            if (y >= 'A' && y <= 'Z')
                y = (char)(y - 'A' + 'a');
        }
        if (x != y)
            return false;
    }
    return true;
}

static Bucket *bucket(garb_cascade_t *c, Table *t, const char *key, size_t len, bool fold,
                      bool create)
{
    if (t->cap == 0) {
        if (!create)
            return NULL;
        t->cap = 256;
        t->b = alloc(c, t->cap * sizeof(Bucket));
        if (t->b == NULL) {
            t->cap = 0;
            return NULL;
        }
    }
    if (create && (t->n + 1) * 2 > t->cap) {
        Table bigger = {0};
        bigger.cap = t->cap * 2;
        bigger.b = alloc(c, bigger.cap * sizeof(Bucket));
        if (bigger.b == NULL)
            return NULL;
        for (uint32_t i = 0; i < t->cap; i++) {
            if (t->b[i].key == NULL)
                continue;
            uint32_t h = hash_bytes(t->b[i].key, t->b[i].len, fold) & (bigger.cap - 1);
            while (bigger.b[h].key != NULL)
                h = (h + 1) & (bigger.cap - 1);
            bigger.b[h] = t->b[i];
        }
        bigger.n = t->n;
        *t = bigger;
    }
    uint32_t h = hash_bytes(key, len, fold) & (t->cap - 1);
    while (t->b[h].key != NULL) {
        if (same_key(t->b[h].key, t->b[h].len, key, len, fold))
            return &t->b[h];
        h = (h + 1) & (t->cap - 1);
    }
    if (!create)
        return NULL;
    t->b[h].key = key;
    t->b[h].len = len;
    t->b[h].head = -1;
    t->n++;
    return &t->b[h];
}

static void file_selector(garb_cascade_t *c, int32_t rule, int32_t sel)
{
    if (!grow(c, (void **)&c->entries, c->nentries, &c->entrycap, sizeof(Entry)))
        return;
    garb_selector_key_t key = garb_selector_key(c->rules[rule].sel, sel);
    bool quirky = c->quirks == OS64_HTML_QUIRKS;
    Bucket *b = NULL;
    if (key.id != NULL)
        b = bucket(c, &c->ids, key.id, key.id_len, quirky, true);
    else if (key.class_name != NULL)
        b = bucket(c, &c->classes, key.class_name, key.class_len, quirky, true);
    else if (key.type != NULL)
        b = bucket(c, &c->types, key.type, key.type_len, true, true);
    int32_t *head = b != NULL ? &b->head : &c->universal;
    if (b == NULL && (key.id != NULL || key.class_name != NULL || key.type != NULL))
        return;                         // no memory to file it
    Entry *e = &c->entries[c->nentries];
    e->rule = rule;
    e->sel = sel;
    e->next = *head;
    *head = c->nentries++;
}

// ── Reading the sheets ──────────────────────────────────────────────────

static bool supports(garb_cascade_t *c, garb_parsed_t *owner, const garb_value_t *v, int32_t n);

// Reads one declaration for a rule: kept raw when custom or pending, else
// its longhands, else dropped (invalid, or a property not read here).
static bool read_decl(garb_cascade_t *c, garb_parsed_t *owner, const garb_decl_t *d, Decl *out)
{
    os64_memset(out, 0, sizeof(*out));
    out->decl = *d;
    out->important = d->important;
    if (garb_decl_is_custom(d)) {
        out->custom = true;
        return true;
    }
    if (garb_decl_has_var(d)) {
        garb_prop_t props[GARB_SETS_MAX];
        if (prop_longhands(d->name, d->len, props) == 0)
            return false;
        out->pending = true;
        return true;
    }
    garb_set_t sets[GARB_SETS_MAX];
    int32_t n = garb_read_declaration(owner, d, c->quirks == OS64_HTML_QUIRKS, sets);
    if (n <= 0)
        return false;
    out->sets = alloc(c, (size_t)n * sizeof(garb_set_t));
    if (out->sets == NULL)
        return false;
    os64_memcpy(out->sets, sets, (size_t)n * sizeof(garb_set_t));
    out->nsets = n;
    return true;
}

static void add_style_rule(garb_cascade_t *c, garb_parsed_t *owner, const garb_rule_t *r,
                           uint32_t *order)
{
    garb_selectors_t *sel = garb_selectors_parse(owner, r->prelude, r->nprelude, c->quirks);
    if (sel == NULL)
        return;                         // an invalid selector drops the rule
    garb_item_t *items;
    int32_t nitems;
    (void)garb_items_of(owner, r->block, r->nblock, &items, &nitems);
    if (!grow(c, (void **)&c->rules, c->nrules, &c->rulecap, sizeof(Rule)))
        return;
    Rule *rule = &c->rules[c->nrules];
    os64_memset(rule, 0, sizeof(*rule));
    rule->sel = sel;
    rule->order = (*order)++;
    rule->decls = alloc(c, (size_t)(nitems > 0 ? nitems : 1) * sizeof(Decl));
    if (rule->decls == NULL)
        return;
    // Nested rules (CSS Nesting) are booked: only the declarations count.
    for (int32_t i = 0; i < nitems; i++)
        if (items[i].kind == GARB_ITEM_DECL &&
            read_decl(c, owner, &items[i].decl, &rule->decls[rule->ndecls]))
            rule->ndecls++;
    if (rule->ndecls == 0)
        return;
    int32_t index = c->nrules++;
    for (int32_t s = 0; s < garb_selectors_count(sel); s++)
        if (garb_selector_pseudo(sel, s) == GARB_PSEUDO_NONE)
            file_selector(c, index, s);
}

static bool at(const garb_rule_t *r, const char *name)
{
    return r->at && ieq(r->name, r->len, name);
}

static void add_rules(garb_cascade_t *c, garb_parsed_t *owner, const garb_item_t *items,
                      int32_t n, uint32_t *order, int depth)
{
    if (depth > NEST_MAX) {
        c->incomplete = true;
        return;
    }
    for (int32_t i = 0; i < n; i++) {
        if (items[i].kind != GARB_ITEM_RULE)
            continue;
        const garb_rule_t *r = items[i].rule;
        if (!r->at) {
            if (r->has_block)
                add_style_rule(c, owner, r, order);
            continue;
        }
        // The grouping rules whose contents join the sheet when they hold.
        // @layer's contents are cascaded as if unlayered (booked: layers).
        bool group = at(r, "media") ? garb_media_matches(r->prelude, r->nprelude, c->env)
                   : at(r, "supports") ? supports(c, owner, r->prelude, r->nprelude)
                   : at(r, "layer") && r->has_block;
        if (!group || !r->has_block)
            continue;
        garb_item_t *inner;
        int32_t ninner;
        (void)garb_rules_of(owner, r->block, r->nblock, &inner, &ninner);
        add_rules(c, owner, inner, ninner, order, depth + 1);
    }
}

// ── @supports (Conditional Rules 3 § 2) ─────────────────────────────────

typedef struct {
    const garb_value_t *v;
    int32_t n, i;
} SCur;

static const garb_value_t *speek(SCur *s)
{
    while (s->i < s->n && s->v[s->i].kind == GARB_WHITESPACE)
        s->i++;
    return s->i < s->n ? &s->v[s->i] : NULL;
}

static bool sword(const garb_value_t *t, const char *w)
{
    return t != NULL && t->kind == GARB_IDENT && ieq(t->text, t->len, w);
}

static bool supports_condition(garb_cascade_t *c, garb_parsed_t *owner, SCur *s);

// ( condition ), ( declaration ), selector( … ): anything else is false.
static bool supports_in_parens(garb_cascade_t *c, garb_parsed_t *owner, SCur *s)
{
    const garb_value_t *t = speek(s);
    if (t == NULL)
        return false;
    s->i++;
    if (t->kind == GARB_FUNCTION && ieq(t->text, t->len, "selector")) {
        return garb_selectors_parse(owner, t->children, t->nchildren, c->quirks) != NULL;
    }
    if (t->kind != GARB_BLOCK || t->open != '(')
        return false;
    SCur inner = {t->children, t->nchildren, 0};
    const garb_value_t *first = speek(&inner);
    if (first != NULL && (first->kind == GARB_BLOCK || sword(first, "not") ||
                          first->kind == GARB_FUNCTION)) {
        bool r = supports_condition(c, owner, &inner);
        return r && speek(&inner) == NULL;
    }
    // A declaration: supported if its property is read here and its value
    // fits (a custom property always does).
    garb_item_t *items;
    int32_t n;
    if (!garb_items_of(owner, t->children, t->nchildren, &items, &n) || n != 1 ||
        items[0].kind != GARB_ITEM_DECL)
        return false;
    const garb_decl_t *d = &items[0].decl;
    if (garb_decl_is_custom(d) || garb_decl_has_var(d)) {
        garb_prop_t props[GARB_SETS_MAX];
        return garb_decl_is_custom(d) || prop_longhands(d->name, d->len, props) > 0;
    }
    garb_set_t sets[GARB_SETS_MAX];
    return garb_read_declaration(owner, d, c->quirks == OS64_HTML_QUIRKS, sets) > 0;
}

static bool supports_condition(garb_cascade_t *c, garb_parsed_t *owner, SCur *s)
{
    if (sword(speek(s), "not")) {
        s->i++;
        return !supports_in_parens(c, owner, s);
    }
    bool r = supports_in_parens(c, owner, s);
    const garb_value_t *t = speek(s);
    bool ands = sword(t, "and"), ors = sword(t, "or");
    while ((ands && sword(speek(s), "and")) || (ors && sword(speek(s), "or"))) {
        s->i++;
        bool next = supports_in_parens(c, owner, s);
        r = ands ? r && next : r || next;
    }
    return r;
}

static bool supports(garb_cascade_t *c, garb_parsed_t *owner, const garb_value_t *v, int32_t n)
{
    SCur s = {v, n, 0};
    bool r = supports_condition(c, owner, &s);
    return r && speek(&s) == NULL;
}

// ── Where an element's answer is kept ───────────────────────────────────

static uint32_t hash_ptr(const void *p)
{
    uintptr_t v = (uintptr_t)p;
    v ^= v >> 17;
    v *= 0xed5ad4bbu;
    v ^= v >> 11;
    return (uint32_t)v;
}

static Slot *slot_for(const garb_cascade_t *c, const os64_html_node_t *node, bool create)
{
    if (c->slotcap == 0)
        return NULL;
    uint32_t h = hash_ptr(node) & (c->slotcap - 1);
    while (c->slots[h].node != NULL) {
        if (c->slots[h].node == node)
            return &c->slots[h];
        h = (h + 1) & (c->slotcap - 1);
    }
    if (!create)
        return NULL;
    Slot *s = (Slot *)&c->slots[h];
    s->node = node;
    return s;
}

// ── var() and env() (Custom Properties 1 § 3, Environment Variables 1) ─

typedef struct {
    garb_cascade_t *c;
    const CustomMap *map;
    int depth;
    int32_t budget;             // values left to produce: the billion-laughs cut
} Sub;

static const garb_value_t *custom_value(Sub *s, const char *name, size_t len, int32_t *n);

typedef struct {
    garb_value_t *v;
    int32_t n, cap;
} Vec;

static bool push(Sub *s, Vec *out, const garb_value_t *v)
{
    if (--s->budget < 0)
        return false;
    if (out->n == out->cap) {
        int32_t cap = out->cap ? out->cap * 2 : 16;
        garb_value_t *next = os64_arena_alloc(s->c->self.arena, (size_t)cap * sizeof(*next));
        if (next == NULL) {
            s->c->incomplete = true;
            return false;
        }
        if (out->n > 0)
            os64_memcpy(next, out->v, (size_t)out->n * sizeof(*next));
        out->v = next;
        out->cap = cap;
    }
    out->v[out->n++] = *v;
    return true;
}

static bool substitute(Sub *s, const garb_value_t *in, int32_t n, Vec *out);

// A var() or env(): its name, and what stands in its place — the value
// named, else its fallback, else nothing (invalid).
static bool substitute_fn(Sub *s, const garb_value_t *f, Vec *out)
{
    bool env = ieq(f->text, f->len, "env");
    int32_t i = 0;
    while (i < f->nchildren && f->children[i].kind == GARB_WHITESPACE)
        i++;
    if (i == f->nchildren || f->children[i].kind != GARB_IDENT)
        return false;
    const garb_value_t *name = &f->children[i++];
    if (!env && !(name->len >= 2 && name->text[0] == '-' && name->text[1] == '-'))
        return false;
    while (i < f->nchildren && f->children[i].kind == GARB_WHITESPACE)
        i++;
    bool has_fallback = i < f->nchildren && f->children[i].kind == GARB_COMMA;
    if (i < f->nchildren && !has_fallback)
        return false;
    if (!env) {
        int32_t vn = 0;
        const garb_value_t *v = custom_value(s, name->text, name->len, &vn);
        if (v != NULL) {
            for (int32_t k = 0; k < vn; k++)
                if (!push(s, out, &v[k]))
                    return false;
            return true;
        }
    }
    // No such variable (env() names none on this machine): the fallback.
    if (!has_fallback)
        return false;
    return substitute(s, f->children + i + 1, f->nchildren - i - 1, out);
}

static bool substitute(Sub *s, const garb_value_t *in, int32_t n, Vec *out)
{
    if (++s->depth > VAR_DEPTH_MAX) {
        s->c->incomplete = true;
        return false;
    }
    bool ok = true;
    for (int32_t k = 0; k < n && ok; k++) {
        const garb_value_t *v = &in[k];
        if (v->kind == GARB_FUNCTION && (ieq(v->text, v->len, "var") || ieq(v->text, v->len, "env"))) {
            ok = substitute_fn(s, v, out);
        } else if (v->kind == GARB_FUNCTION || v->kind == GARB_BLOCK) {
            Vec kids = {0};
            garb_value_t copy = *v;
            ok = substitute(s, v->children, v->nchildren, &kids);
            copy.children = kids.v;
            copy.nchildren = kids.n;
            ok = ok && push(s, out, &copy);
        } else {
            ok = push(s, out, v);
        }
    }
    s->depth--;
    return ok;
}

// A custom property's value as the element sees it, its own var()s
// replaced; NULL for the guaranteed-invalid value (undeclared, or in a
// cycle, or its replacement failed).
static const garb_value_t *custom_value(Sub *s, const char *name, size_t len, int32_t *n)
{
    for (const CustomMap *m = s->map; m != NULL; m = m->parent)
        for (int32_t k = m->n - 1; k >= 0; k--) {
            Custom *cu = &m->own[k];
            if (cu->len != len || os64_memcmp(cu->name, name, len) != 0)
                continue;
            if (cu->state == 1 || cu->state == 3 || cu->value == NULL)
                return NULL;            // a cycle, or invalid
            if (cu->state == 0) {
                cu->state = 1;
                Vec out = {0};
                Sub inner = {s->c, m, s->depth, VAR_VALUES_MAX};
                bool ok = substitute(&inner, cu->value, cu->nvalue, &out);
                if (ok) {
                    cu->value = out.v;
                    cu->nvalue = out.n;
                }
                cu->state = ok ? 2 : 3;
                if (!ok)
                    return NULL;
            }
            *n = cu->nvalue;
            return cu->value;
        }
    return NULL;
}

// ── Per element ─────────────────────────────────────────────────────────

typedef struct {
    Rule *rule;
    uint32_t spec;
} Match;

// One declaration's place in the cascade: importance, then (inline above
// every rule) specificity, then order.
typedef struct {
    const Decl *d;
    bool important, inline_;
    uint32_t spec, order;
} Applied;

static bool before(const Applied *a, const Applied *b)
{
    if (a->important != b->important)
        return !a->important;
    if (a->inline_ != b->inline_)
        return !a->inline_;
    if (a->spec != b->spec)
        return a->spec < b->spec;
    return a->order < b->order;
}

static void consider(garb_cascade_t *c, Match **m, int32_t *n, int32_t *cap, Rule *r,
                     uint32_t spec, const os64_html_node_t *el)
{
    if (r->seen == el) {
        if (spec > (*m)[r->slot].spec)
            (*m)[r->slot].spec = spec;
        return;
    }
    if (!grow(c, (void **)m, *n, cap, sizeof(Match)))
        return;
    r->seen = el;
    r->slot = *n;
    (*m)[(*n)++] = (Match){r, spec};
}

static void try_bucket(garb_cascade_t *c, int32_t head, const os64_html_node_t *el, Match **m,
                       int32_t *n, int32_t *cap)
{
    for (int32_t e = head; e >= 0; e = c->entries[e].next) {
        Entry *en = &c->entries[e];
        Rule *r = &c->rules[en->rule];
        if (garb_selector_matches(r->sel, en->sel, el))
            consider(c, m, n, cap, r, garb_selector_specificity(r->sel, en->sel), el);
    }
}

static void try_classes(garb_cascade_t *c, const os64_html_node_t *el, Match **m, int32_t *n,
                        int32_t *cap)
{
    const os64_html_attr_t *a = os64_html_attr(el, "class");
    if (a == NULL || a->value == NULL || c->classes.n == 0)
        return;
    const char *v = a->value;
    bool quirky = c->quirks == OS64_HTML_QUIRKS;
    for (size_t i = 0, len = os64_strlen(v); i < len;) {
        while (i < len && (v[i] == ' ' || v[i] == '\t' || v[i] == '\n' || v[i] == '\f' || v[i] == '\r'))
            i++;
        size_t start = i;
        while (i < len && !(v[i] == ' ' || v[i] == '\t' || v[i] == '\n' || v[i] == '\f' || v[i] == '\r'))
            i++;
        if (i == start)
            continue;
        Bucket *b = bucket(c, &c->classes, v + start, i - start, quirky, false);
        if (b != NULL)
            try_bucket(c, b->head, el, m, n, cap);
    }
}

// Every rule that matches `el`, each once, at its heaviest selector.
static int32_t matching(garb_cascade_t *c, const os64_html_node_t *el, Match **m, int32_t *cap)
{
    int32_t n = 0;
    const os64_html_attr_t *id = os64_html_attr(el, "id");
    if (id != NULL && id->value != NULL && c->ids.n > 0) {
        Bucket *b = bucket(c, &c->ids, id->value, os64_strlen(id->value),
                           c->quirks == OS64_HTML_QUIRKS, false);
        if (b != NULL)
            try_bucket(c, b->head, el, m, &n, cap);
    }
    try_classes(c, el, m, &n, cap);
    if (el->name != NULL && c->types.n > 0) {
        Bucket *b = bucket(c, &c->types, el->name, os64_strlen(el->name), true, false);
        if (b != NULL)
            try_bucket(c, b->head, el, m, &n, cap);
    }
    try_bucket(c, c->universal, el, m, &n, cap);
    return n;
}

// The `style` attribute's declarations, read into a Decl list.
static int32_t inline_decls(garb_cascade_t *c, const os64_html_node_t *el, Decl **out)
{
    const os64_html_attr_t *a = os64_html_attr(el, "style");
    if (a == NULL || a->value == NULL || a->value[0] == '\0')
        return 0;
    if (!grow(c, (void **)&c->attrs, c->nattrs, &c->attrcap, sizeof(garb_parsed_t)))
        return 0;
    garb_parsed_t *p = &c->attrs[c->nattrs];
    if (garb_parse_block(a->value, os64_strlen(a->value), p) != GARB_OK) {
        garb_free(p);
        c->incomplete = true;           // a style the page gave was not read
        return 0;
    }
    c->nattrs++;
    Decl *d = alloc(c, (size_t)(p->nitems > 0 ? p->nitems : 1) * sizeof(Decl));
    if (d == NULL)
        return 0;
    int32_t n = 0;
    for (int32_t i = 0; i < p->nitems; i++)
        if (p->items[i].kind == GARB_ITEM_DECL && read_decl(c, p, &p->items[i].decl, &d[n]))
            n++;
    *out = d;
    return n;
}

static void cascade_element(garb_cascade_t *c, const os64_html_node_t *el,
                            const CustomMap *inherited, Match **m, int32_t *mcap)
{
    Slot *slot = slot_for(c, el, true);
    if (slot == NULL)
        return;
    slot->customs = inherited;
    int32_t nm = matching(c, el, m, mcap);
    Decl *inl = NULL;
    int32_t ninl = inline_decls(c, el, &inl);
    int32_t total = ninl;
    for (int32_t k = 0; k < nm; k++)
        total += (*m)[k].rule->ndecls;
    if (total == 0)
        return;
    Applied *ap = os64_malloc((size_t)total * sizeof(*ap));
    if (ap == NULL) {
        c->incomplete = true;
        return;
    }
    int32_t na = 0;
    for (int32_t k = 0; k < nm; k++)
        for (int32_t j = 0; j < (*m)[k].rule->ndecls; j++) {
            const Decl *d = &(*m)[k].rule->decls[j];
            ap[na++] = (Applied){d, d->important, false, (*m)[k].spec, (*m)[k].rule->order};
        }
    for (int32_t j = 0; j < ninl; j++)
        ap[na++] = (Applied){&inl[j], inl[j].important, true, 0, (uint32_t)j};
    // Insertion sort into ascending precedence; the last write wins.
    for (int32_t i = 1; i < na; i++) {
        Applied x = ap[i];
        int32_t j = i - 1;
        while (j >= 0 && before(&x, &ap[j])) {
            ap[j + 1] = ap[j];
            j--;
        }
        ap[j + 1] = x;
    }
    // Custom properties first: every other value may name one.
    int32_t ncustom = 0;
    for (int32_t i = 0; i < na; i++)
        ncustom += ap[i].d->custom;
    const CustomMap *customs = inherited;
    if (ncustom > 0) {
        CustomMap *own = alloc(c, sizeof(*own));
        Custom *list = alloc(c, (size_t)ncustom * sizeof(Custom));
        if (own != NULL && list != NULL) {
            own->parent = inherited;
            own->own = list;
            for (int32_t i = 0; i < na; i++) {
                if (!ap[i].d->custom)
                    continue;
                const garb_decl_t *d = &ap[i].d->decl;
                // A later declaration of the same name replaces an earlier.
                int32_t k = 0;
                while (k < own->n && !(own->own[k].len == d->len &&
                                       os64_memcmp(own->own[k].name, d->name, d->len) == 0))
                    k++;
                if (k == own->n)
                    own->n++;
                Custom *cu = &own->own[k];
                cu->name = d->name;
                cu->len = d->len;
                cu->value = d->value;
                cu->nvalue = d->nvalue;
                cu->state = 0;
                // `--x: initial` and a value of nothing are the
                // guaranteed-invalid value; `inherit` is the parent's.
                if (d->nvalue == 1 && d->value[0].kind == GARB_IDENT &&
                    (ieq(d->value[0].text, d->value[0].len, "initial") ||
                     ieq(d->value[0].text, d->value[0].len, "unset")))
                    cu->value = NULL;
                if (d->nvalue == 1 && d->value[0].kind == GARB_IDENT &&
                    ieq(d->value[0].text, d->value[0].len, "inherit")) {
                    Sub up = {c, inherited, 0, VAR_VALUES_MAX};
                    int32_t pn = 0;
                    cu->value = custom_value(&up, d->name, d->len, &pn);
                    cu->nvalue = pn;
                    cu->state = 2;
                }
            }
            customs = own;
            slot->customs = own;
        }
    }
    // The winners, one per longhand.
    garb_set_t win[GARB_NPROPS];
    bool have[GARB_NPROPS];
    os64_memset(have, 0, sizeof(have));
    for (int32_t i = 0; i < na; i++) {
        const Decl *d = ap[i].d;
        if (d->custom)
            continue;
        if (!d->pending) {
            for (int32_t k = 0; k < d->nsets; k++) {
                win[d->sets[k].prop] = d->sets[k];
                have[d->sets[k].prop] = true;
            }
            continue;
        }
        // var()/env(): replaced now, then read. A value that fails either
        // way is invalid AT COMPUTED-VALUE TIME, and the property acts as
        // `unset` (Custom Properties 1 § 3.1) — it does not fall back to
        // the declaration it overrode.
        Sub s = {c, customs, 0, VAR_VALUES_MAX};
        Vec out = {0};
        garb_set_t sets[GARB_SETS_MAX];
        int32_t n = -1;
        if (substitute(&s, d->decl.value, d->decl.nvalue, &out)) {
            garb_decl_t read = d->decl;
            read.value = out.v;
            read.nvalue = out.n;
            n = garb_read_declaration(&c->self, &read, c->quirks == OS64_HTML_QUIRKS, sets);
        }
        if (n <= 0) {
            garb_prop_t props[GARB_SETS_MAX];
            int32_t np = prop_longhands(d->decl.name, d->decl.len, props);
            for (int32_t k = 0; k < np; k++) {
                sets[k].prop = props[k];
                os64_memset(&sets[k].value, 0, sizeof(sets[k].value));
                sets[k].value.kind = GARB_V_WIDE;
                sets[k].value.keyword = "unset";
            }
            n = np;
        }
        for (int32_t k = 0; k < n; k++) {
            win[sets[k].prop] = sets[k];
            have[sets[k].prop] = true;
        }
    }
    os64_free(ap);
    int32_t nwin = 0;
    for (int p = 0; p < GARB_NPROPS; p++)
        nwin += have[p];
    if (nwin == 0)
        return;
    garb_set_t *kept = alloc(c, (size_t)nwin * sizeof(garb_set_t));
    if (kept == NULL)
        return;
    int32_t k = 0;
    for (int p = 0; p < GARB_NPROPS; p++)
        if (have[p])
            kept[k++] = win[p];
    slot->style.sets = kept;
    slot->style.n = nwin;
}

// ── The door ────────────────────────────────────────────────────────────

static bool is_el(const os64_html_node_t *n)
{
    return n != NULL && n->kind == OS64_HTML_ELEMENT;
}

garb_cascade_t *garb_cascade(const garb_sheet_in_t *sheets, int32_t n,
                             const os64_html_document_t *doc, garb_env_t env)
{
    garb_cascade_t *c = os64_calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;
    c->self.arena = os64_arena_create(0, GARB_ARENA_MAX);
    if (c->self.arena == NULL) {
        os64_free(c);
        return NULL;
    }
    c->env = env;
    c->quirks = doc->quirks;
    c->universal = -1;
    uint32_t order = 0;
    for (int32_t s = 0; s < n; s++) {
        garb_parsed_t *sheet = sheets[s].sheet;
        if (sheet == NULL || !garb_media_text_matches(sheets[s].media, env))
            continue;
        add_rules(c, sheet, sheet->items, sheet->nitems, &order, 0);
    }
    // Every element's slot: the table is sized for the tree.
    size_t elements = 0;
    for (const os64_html_node_t *x = doc->document; x != NULL;) {
        elements += is_el(x);
        if (x->first_child != NULL) {
            x = x->first_child;
            continue;
        }
        while (x != NULL && x->next == NULL)
            x = x->parent;
        x = x != NULL ? x->next : NULL;
    }
    c->slotcap = 64;
    while (c->slotcap < elements * 2 + 2)
        c->slotcap *= 2;
    c->slots = os64_calloc(c->slotcap, sizeof(Slot));
    if (c->slots == NULL) {
        c->slotcap = 0;
        c->incomplete = true;
        return c;
    }
    Match *m = NULL;
    int32_t mcap = 0;
    for (const os64_html_node_t *x = doc->document; x != NULL;) {
        if (is_el(x)) {
            const Slot *up = is_el(x->parent) ? slot_for(c, x->parent, false) : NULL;
            cascade_element(c, x, up != NULL ? up->customs : NULL, &m, &mcap);
        }
        if (x->first_child != NULL) {
            x = x->first_child;
            continue;
        }
        while (x != NULL && x->next == NULL)
            x = x->parent;
        x = x != NULL ? x->next : NULL;
    }
    // What the cascade read ran short wherever it was read: a sheet's own
    // parse, and the rule blocks, selectors and values read from it since,
    // each mark the parse they belong to. Any of them short in a sheet the
    // cascade used, or in a style attribute, and the cascade is short.
    bool short_read = c->self.incomplete;
    for (int32_t s = 0; s < n; s++)
        short_read |= sheets[s].sheet != NULL && sheets[s].sheet->incomplete &&
                      garb_media_text_matches(sheets[s].media, env);
    for (int32_t i = 0; i < c->nattrs; i++)
        short_read |= c->attrs[i].incomplete;
    if (short_read)
        c->incomplete = true;
    return c;
}

bool garb_cascade_incomplete(const garb_cascade_t *c)
{
    return c == NULL || c->incomplete;
}

garb_style_t garb_style_for(const garb_cascade_t *c, const os64_html_node_t *element)
{
    garb_style_t none = {NULL, 0};
    if (c == NULL)
        return none;
    const Slot *s = slot_for(c, element, false);
    return s != NULL ? s->style : none;
}

void garb_cascade_free(garb_cascade_t *c)
{
    if (c == NULL)
        return;
    for (int32_t i = 0; i < c->nattrs; i++)
        garb_free(&c->attrs[i]);
    os64_free(c->slots);
    os64_arena_destroy(c->self.arena);
    os64_free(c);
}

// ── The dump ────────────────────────────────────────────────────────────

typedef struct {
    char *out;
    size_t cap, len;
} Out;

static void put(Out *o, const char *s, size_t n)
{
    for (size_t k = 0; k < n; k++, o->len++)
        if (o->len + 1 < o->cap)
            o->out[o->len] = s[k];
    if (o->cap > 0)
        o->out[o->len < o->cap ? o->len : o->cap - 1] = '\0';
}

static void puts_(Out *o, const char *s)
{
    put(o, s, os64_strlen(s));
}

size_t garb_cascade_dump(const garb_cascade_t *c, const os64_html_document_t *doc, char *out,
                         size_t cap)
{
    Out o = {out, cap, 0};
    if (cap > 0)
        out[0] = '\0';
    for (const os64_html_node_t *x = doc->document; x != NULL;) {
        garb_style_t st = is_el(x) ? garb_style_for(c, x) : (garb_style_t){NULL, 0};
        if (st.n > 0) {
            puts_(&o, x->name != NULL ? x->name : "?");
            const os64_html_attr_t *id = os64_html_attr(x, "id");
            if (id != NULL && id->value != NULL) {
                puts_(&o, "#");
                puts_(&o, id->value);
            }
            puts_(&o, ":");
            for (int32_t k = 0; k < st.n; k++) {
                char v[512];
                garb_val_dump(&st.sets[k].value, v, sizeof(v));
                puts_(&o, k ? "; " : " ");
                puts_(&o, garb_prop_name(st.sets[k].prop));
                puts_(&o, ": ");
                puts_(&o, v);
            }
            puts_(&o, "\n");
        }
        if (x->first_child != NULL) {
            x = x->first_child;
            continue;
        }
        while (x != NULL && x->next == NULL)
            x = x->parent;
        x = x != NULL ? x->next : NULL;
    }
    return o.len;
}
