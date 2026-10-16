// cascade.c — CSS Cascading and Inheritance Level 4 for the author origin,
// with Custom Properties 1 and Environment Variables 1 (garb/cascade.h).
//
// Build: every style rule of every sheet whose @media holds, its selectors
// filed in a RULE HASH by the rightmost compound's id, class or element
// name (the browsers' own trick: an element tries only the rules that could
// possibly match it), its declarations read once; every @keyframes, the
// last of each name kept and its keyframes read once; and every
// @font-face, read whole.
//
// Then, per element in document order: the rules that match, weighed by
// §6's order — importance, then specificity (a rule matching through
// several selectors takes the most specific), then order of appearance —
// with the `style` attribute above every sheet at the same importance; the
// winners' var() and env() replaced against the custom properties the
// element inherits and declares, and so are those of the keyframes its
// winners name.

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
    int32_t sheet;              // which input sheet it came from
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
    // As declared (kEmptyValue for a declaration of nothing), or NULL: the
    // guaranteed-invalid value.
    const garb_value_t *value;
    int32_t nvalue;
    int state;                  // 0 as declared, 1 being replaced, 2 replaced, 3 invalid
} Custom;

struct CustomMap {
    const CustomMap *parent;
    Custom *own;
    int32_t n;
};

// A @keyframes rule kept (Animations 1 § 3): its name as written, and its
// blocks in order, each with its offsets and its declarations read once —
// with a var() left to be read per element, as a rule's are.
typedef struct {
    double *offsets;            // each in [0, 1]
    int32_t noffsets;
    Decl *decls;
    int32_t ndecls;
    bool easing;                // decls[0] is the block's own timing function
} KeyBlock;

typedef struct {
    const char *name;
    size_t len;
    KeyBlock *blocks;
    int32_t nblocks;
    int32_t sheet;              // which input sheet it came from
    bool pending;               // a declaration holds var(): read per element
    // Read against no element: the answer for one whose winners do not
    // name it, its var()s dropped.
    garb_keyframe_t *frames;
    int32_t nframes;
} Keyframes;

// An element's own reading of a rule that holds var().
typedef struct {
    const Keyframes *rule;
    garb_keyframe_t *frames;
    int32_t nframes;
} OwnFrames;

typedef struct {
    const os64_html_node_t *node;
    garb_style_t style;
    const CustomMap *customs;
    OwnFrames *frames;
    int32_t nframes;
} Slot;

struct garb_cascade {
    const os64_html_document_t *doc;
    os64_html_pin_t pin;
    garb_parsed_t self;         // the cascade's own arena, in a parse's shape
    garb_env_t env;
    os64_html_quirks_t quirks;
    bool incomplete;
    Rule *rules;
    int32_t nrules, rulecap;
    // The @keyframes rules: each as it is read, then, once every sheet is
    // (keyframes_settle), one per name — the last of it — sorted by name.
    Keyframes *keyframes;
    int32_t nkeyframes, keyframecap;
    bool keyframes_sorted;
    Entry *entries;
    int32_t nentries, entrycap;
    Table ids, classes, types;
    int32_t universal;
    Slot *slots;                // open addressing, keyed by node
    uint32_t slotcap;
    garb_parsed_t *attrs;       // `style` attributes' parses, freed with the cascade
    int32_t nattrs, attrcap;
    // What the build passed over because this library does not read it
    // (garb_cascade_skips). Its storage never marks the cascade
    // incomplete: the tally is an instrument, and running out of room for
    // it changes nothing the page gets.
    garb_skip_t *skips;
    int32_t nskips, skipcap;
    uint32_t skips_lost;
    // The @font-face rules read (garb_cascade_font_faces).
    garb_font_face_t *faces;
    int32_t nfaces, facecap;
    uint32_t faces_lost;
    // How many allocations have failed, so a reader can tell whether its
    // own did (add_face).
    uint32_t allocs_failed;
};

static void *alloc(garb_cascade_t *c, size_t n)
{
    void *p = os64_arena_calloc(c->self.arena, 1, n);
    if (p == NULL) {
        c->incomplete = true;
        c->allocs_failed++;
    }
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
        c->allocs_failed++;
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

// ── What the build passed over ──────────────────────────────────────────

// The functions some reader in values.c or props.c accepts, by name: a
// declaration this library dropped that names any OTHER function is
// counted as asking for that function. One it dropped naming only these
// was a value written wrong, which is the page's, not a gap.
static const char *const kFunctionsRead[] = {
    "calc", "min", "max", "clamp", "rgb", "rgba", "hsl", "hsla", "hwb", "url", "var", "env",
    "linear-gradient", "radial-gradient", "conic-gradient", "repeating-linear-gradient",
    "repeating-radial-gradient", "repeating-conic-gradient", "image-set", "minmax",
    "fit-content", "repeat", "rect", NULL,
};

static void skipped(garb_cascade_t *c, garb_skip_kind_t kind, const char *name, size_t len)
{
    // A prefixed name is one engine's dialect, which every other engine
    // passes over too: what a page asks of a browser is the standard name.
    if (len == 0 || name[0] == '-')
        return;
    // ieq folds its first argument; the kept copy is already lower case.
    for (int32_t i = 0; i < c->nskips; i++)
        if (c->skips[i].kind == kind && c->skips[i].len == len && ieq(name, len, c->skips[i].name)) {
            if (c->skips[i].count != UINT32_MAX)
                c->skips[i].count++;
            return;
        }
    if (c->nskips == c->skipcap) {
        int32_t bigger = c->skipcap > 0 ? c->skipcap * 2 : 16;
        garb_skip_t *next = bigger <= GARB_SKIPS_MAX
            ? os64_arena_alloc(c->self.arena, (size_t)bigger * sizeof(garb_skip_t)) : NULL;
        if (next == NULL) {
            c->skips_lost++;
            return;
        }
        if (c->nskips > 0)
            os64_memcpy(next, c->skips, (size_t)c->nskips * sizeof(garb_skip_t));
        c->skips = next;
        c->skipcap = bigger;
    }
    char *copy = os64_arena_alloc(c->self.arena, len + 1);
    if (copy == NULL) {
        c->skips_lost++;
        return;
    }
    // Names are compared without case, as CSS compares them, and kept in
    // lower case so one name is spelled one way wherever it is reported.
    for (size_t i = 0; i < len; i++)
        copy[i] = name[i] >= 'A' && name[i] <= 'Z' ? (char)(name[i] + 32) : name[i];
    copy[len] = '\0';
    c->skips[c->nskips++] = (garb_skip_t){kind, copy, len, 1};
}

static bool function_read(const garb_value_t *v)
{
    for (int k = 0; kFunctionsRead[k] != NULL; k++)
        if (ieq(v->text, v->len, kFunctionsRead[k]))
            return true;
    return false;
}

// The functions a dropped declaration names that no reader here accepts,
// at any depth.
static void skipped_functions(garb_cascade_t *c, const garb_value_t *v, int32_t n, int depth)
{
    for (int32_t i = 0; i < n && depth < GARB_DEPTH_MAX; i++) {
        if (v[i].kind == GARB_FUNCTION && !function_read(&v[i]))
            skipped(c, GARB_SKIP_FUNCTION, v[i].text, v[i].len);
        if (v[i].kind == GARB_FUNCTION || v[i].kind == GARB_BLOCK)
            skipped_functions(c, v[i].children, v[i].nchildren, depth + 1);
    }
}

// A value of a property this library reads, written `<property>: <words>`:
// what a page asked of the property and did not get.
static void skipped_value(garb_cascade_t *c, const char *prop, size_t plen, const garb_value_t *v,
                          int32_t n)
{
    char name[128];
    size_t at = 0;
    for (size_t i = 0; i < plen && at + 1 < sizeof(name); i++)
        name[at++] = prop[i];
    for (const char *s = ": "; *s != '\0' && at + 1 < sizeof(name); s++)
        name[at++] = *s;
    for (int32_t i = 0; i < n; i++) {
        if (v[i].kind == GARB_WHITESPACE)
            continue;
        // A prefixed word is one engine's dialect, as a prefixed name is;
        // so is a control character, which no keyword holds and old IE's
        // hacks (`display: none\9`) write on purpose.
        if (v[i].len != 0 && v[i].text[0] == '-')
            return;
        for (uint32_t k = 0; k < v[i].len; k++)
            if ((unsigned char)v[i].text[k] < 0x20)
                return;
        if (name[at - 1] != ' ' && at + 1 < sizeof(name))
            name[at++] = ' ';
        for (uint32_t k = 0; k < v[i].len && at + 1 < sizeof(name); k++)
            name[at++] = v[i].text[k];
    }
    skipped(c, GARB_SKIP_VALUE, name, at);
}

// Whether a value is bare words and nothing else: `inline flex`, a keyword
// this library does not read. One that holds a number, a string or a
// function is a value written wrong far more often than a gap.
static bool only_words(const garb_value_t *v, int32_t n)
{
    bool any = false;
    for (int32_t i = 0; i < n; i++) {
        if (v[i].kind == GARB_IDENT)
            any = true;
        else if (v[i].kind != GARB_WHITESPACE)
            return false;
    }
    return any;
}

// ── @font-face (Fonts 4 § 4) ────────────────────────────────────────────

// `text` copied into the cascade's arena, NUL after it; NULL on no memory.
static const char *copy_text(garb_cascade_t *c, const char *text, size_t len)
{
    char *copy = alloc(c, len + 1);
    if (copy != NULL && len > 0)
        os64_memcpy(copy, text, len);
    return copy;
}

// The value with white space taken off its ends: [*i, *n).
static void trim(const garb_value_t *v, int32_t *i, int32_t *n)
{
    while (*i < *n && v[*i].kind == GARB_WHITESPACE)
        (*i)++;
    while (*n > *i && v[*n - 1].kind == GARB_WHITESPACE)
        (*n)--;
}

// A <family-name> in [i, n): one string, or identifiers joined by single
// spaces — none of them a CSS-wide keyword or `default`, and not one
// identifier alone that is a generic family's keyword or a system font's,
// which must be quoted to name a face (Fonts 4 § 2.1.1). Copied into the arena at its own
// length, `*len` set: NULL for a name written wrong, or memory refused.
static const char *family_name(garb_cascade_t *c, const garb_value_t *v, int32_t i, int32_t n, size_t *len)
{
    trim(v, &i, &n);
    if (i >= n)
        return NULL;
    if (v[i].kind == GARB_STRING) {
        *len = v[i].len;
        return i + 1 == n ? copy_text(c, v[i].text, v[i].len) : NULL;
    }
    size_t need = 0;
    int32_t words = 0, first = i;
    for (int32_t k = i; k < n; k++) {
        if (v[k].kind == GARB_WHITESPACE)
            continue;
        if (v[k].kind != GARB_IDENT || garb_family_word_reserved(v[k].text, v[k].len))
            return NULL;
        need += (words++ > 0) + v[k].len;
    }
    if (words == 1 && garb_generic_family(v[first].text, v[first].len))
        return NULL;
    if (words == 1 && garb_system_font(v[first].text, v[first].len))
        return NULL;
    char *out = alloc(c, need + 1);
    if (out == NULL)
        return NULL;
    size_t at = 0;
    for (int32_t k = i; k < n; k++) {
        if (v[k].kind == GARB_WHITESPACE)
            continue;
        if (at > 0)
            out[at++] = ' ';
        os64_memcpy(out + at, v[k].text, v[k].len);
        at += v[k].len;
    }
    *len = need;
    return out;
}

// format()'s argument, whole (Fonts 4 § 4.3): one of the format keywords,
// or strings separated by commas — the older grammar pages still write.
// Its first value, the hint; NULL for anything else.
static const garb_value_t *format_hint(const garb_value_t *v, int32_t n)
{
    static const char *const kFormats[] = {"collection", "embedded-opentype", "opentype", "svg",
                                           "truetype", "woff", "woff2", NULL};
    int32_t i = 0;
    trim(v, &i, &n);
    if (i + 1 == n && v[i].kind == GARB_IDENT) {
        for (int k = 0; kFormats[k] != NULL; k++)
            if (ieq(v[i].text, v[i].len, kFormats[k]))
                return &v[i];
        return NULL;
    }
    const garb_value_t *first = NULL;
    bool want_string = true;
    for (; i < n; i++) {
        if (v[i].kind == GARB_WHITESPACE)
            continue;
        if (want_string ? v[i].kind != GARB_STRING : v[i].kind != GARB_COMMA)
            return NULL;
        if (first == NULL)
            first = &v[i];
        want_string = !want_string;
    }
    return want_string ? NULL : first;
}

// One `src` entry in [i, n): url() [format()] — tech() drops it — or
// local(<family-name>). False for one written wrong, or with tech().
static bool face_src(garb_cascade_t *c, const garb_value_t *v, int32_t i, int32_t n, garb_face_src_t *out)
{
    trim(v, &i, &n);
    if (i >= n)
        return false;
    os64_memset(out, 0, sizeof(*out));
    const garb_value_t *first = &v[i];
    if (first->kind == GARB_FUNCTION && ieq(first->text, first->len, "local")) {
        if (i + 1 != n)
            return false;
        out->kind = GARB_FACE_LOCAL;
        out->text = family_name(c, first->children, 0, first->nchildren, &out->len);
        return out->text != NULL;
    }
    out->kind = GARB_FACE_URL;
    if (first->kind == GARB_URL) {
        out->text = first->text;
        out->len = first->len;
    } else if (first->kind == GARB_FUNCTION && ieq(first->text, first->len, "url")) {
        int32_t a = 0, b = first->nchildren;
        trim(first->children, &a, &b);
        if (b != a + 1 || first->children[a].kind != GARB_STRING)
            return false;
        out->text = first->children[a].text;
        out->len = first->children[a].len;
    } else {
        return false;
    }
    for (i++; i < n; i++) {
        if (v[i].kind == GARB_WHITESPACE)
            continue;
        if (v[i].kind != GARB_FUNCTION)
            return false;
        if (ieq(v[i].text, v[i].len, "tech"))
            return false;
        if (!ieq(v[i].text, v[i].len, "format") || out->format != NULL)
            return false;
        const garb_value_t *hint = format_hint(v[i].children, v[i].nchildren);
        if (hint == NULL)
            return false;
        out->format = copy_text(c, hint->text, hint->len);
        out->format_len = hint->len;
        if (out->format == NULL)
            return false;
    }
    out->text = copy_text(c, out->text, out->len);
    return out->text != NULL;
}

// A hex digit's value, or -1.
static int hexval(char ch)
{
    return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
         : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
}

// One <urange> in [i, n): the tokenizer makes no such token, so its text is
// the representations of the tokens that spell it, joined (Syntax 3,
// "consume the value of a unicode-range descriptor"): `u` or `U`, `+`, one
// to six hex digits and `?`s — the `?`s, all trailing, as 0 below and F
// above — or two runs of hex digits about a `-`. False for anything else,
// or a range past U+10FFFF or backwards.
static bool urange(const garb_value_t *v, int32_t i, int32_t n, garb_unicode_range_t *out)
{
    trim(v, &i, &n);
    char text[64];
    size_t len = 0;
    for (; i < n; i++) {
        const garb_value_t *t = &v[i];
        size_t need = t->len + (t->kind == GARB_DIMENSION ? t->unit_len : 0);
        if (t->kind == GARB_WHITESPACE || len + need >= sizeof(text) ||
            (t->kind != GARB_IDENT && t->kind != GARB_NUMBER && t->kind != GARB_DIMENSION &&
             t->kind != GARB_DELIM))
            return false;
        os64_memcpy(text + len, t->text, t->len);
        len += t->len;
        if (t->kind == GARB_DIMENSION) {
            os64_memcpy(text + len, t->unit, t->unit_len);
            len += t->unit_len;
        }
    }
    if (len < 3 || (text[0] != 'u' && text[0] != 'U') || text[1] != '+')
        return false;
    size_t k = 2;
    uint32_t lo = 0, hi = 0;
    int digits = 0, wild = 0;
    for (; k < len && digits + wild < 6; k++) {
        int h = hexval(text[k]);
        if (h >= 0 && wild == 0) {
            lo = lo * 16 + (uint32_t)h;
            hi = hi * 16 + (uint32_t)h;
            digits++;
        } else if (text[k] == '?') {
            lo = lo * 16;
            hi = hi * 16 + 15;
            wild++;
        } else {
            break;
        }
    }
    if (digits + wild == 0)
        return false;
    if (wild > 0) {
        if (k != len)
            return false;
    } else if (k < len) {
        if (text[k] != '-' || ++k == len)
            return false;
        hi = 0;
        int more = 0;
        for (; k < len; k++, more++) {
            int h = hexval(text[k]);
            if (h < 0 || more == 6)
                return false;
            hi = hi * 16 + (uint32_t)h;
        }
    }
    if (hi > 0x10FFFF || lo > hi)
        return false;
    *out = (garb_unicode_range_t){lo, hi};
    return true;
}

// The comma-separated parts of a declaration's value: each part's [a, b)
// handed to `each` in turn; false when one is refused (`forgiving` skips
// it instead) — and answers how many were taken.
typedef bool (*PartFn)(garb_cascade_t *c, const garb_value_t *v, int32_t a, int32_t b, void *out, int32_t k);

static int32_t parts(garb_cascade_t *c, const garb_decl_t *d, PartFn each, void *out, int32_t cap,
                     bool forgiving)
{
    int32_t taken = 0, a = 0;
    for (int32_t i = 0; i <= d->nvalue; i++) {
        if (i < d->nvalue && d->value[i].kind != GARB_COMMA)
            continue;
        if (taken < cap && each(c, d->value, a, i, out, taken))
            taken++;
        else if (!forgiving)
            return -1;
        a = i + 1;
    }
    return taken;
}

static bool src_part(garb_cascade_t *c, const garb_value_t *v, int32_t a, int32_t b, void *out, int32_t k)
{
    return face_src(c, v, a, b, &((garb_face_src_t *)out)[k]);
}

static bool range_part(garb_cascade_t *c, const garb_value_t *v, int32_t a, int32_t b, void *out, int32_t k)
{
    (void)c;
    return urange(v, a, b, &((garb_unicode_range_t *)out)[k]);
}

// A face's weight: `normal` (400), `bold` (700) or a number from 1 to
// 1000, a fraction kept (matching does not round); two make a range, in
// either order (Fonts 4 § 4.5). `auto` selects as `normal` (§ 4.4: it
// only spares a variable face's axis its clamp, and no axis is read here).
static bool face_weight(const garb_decl_t *d, float *lo, float *hi)
{
    double w[2];
    int32_t k = 0;
    for (int32_t i = 0; i < d->nvalue; i++) {
        const garb_value_t *t = &d->value[i];
        if (t->kind == GARB_WHITESPACE)
            continue;
        if (k == 2)
            return false;
        if (t->kind == GARB_NUMBER && t->number >= 1 && t->number <= 1000)
            w[k++] = t->number;
        else if (t->kind == GARB_IDENT && ieq(t->text, t->len, "normal"))
            w[k++] = 400;
        else if (t->kind == GARB_IDENT && ieq(t->text, t->len, "bold"))
            w[k++] = 700;
        else if (t->kind == GARB_IDENT && ieq(t->text, t->len, "auto") && d->nvalue == 1)
            w[0] = 400, w[1] = 400, k = 2;
        else
            return false;
    }
    if (k == 0)
        return false;
    if (k == 1)
        w[1] = w[0];
    *lo = (float)(w[0] < w[1] ? w[0] : w[1]);
    *hi = (float)(w[0] < w[1] ? w[1] : w[0]);
    return true;
}

// An angle in degrees, from a dimension in one of the four angle units.
static bool angle_deg(const garb_value_t *t, double *deg)
{
    static const struct {
        const char *unit;
        double per;
    } kUnits[] = {{"deg", 1}, {"rad", 57.29577951308232}, {"grad", 0.9}, {"turn", 360}};
    if (t->kind != GARB_DIMENSION)
        return false;
    for (size_t k = 0; k < sizeof(kUnits) / sizeof(kUnits[0]); k++)
        if (ieq(t->unit, t->unit_len, kUnits[k].unit)) {
            *deg = t->number * kUnits[k].per;
            return true;
        }
    return false;
}

// A face's style: `normal`, `italic`, or `oblique` with up to two angles
// after it, each between -90 and 90 degrees (Fonts 4 § 4.4) — and `auto`,
// which selects as `normal`, as the weight's does.
static bool face_style(const garb_decl_t *d, garb_face_style_t *out)
{
    int32_t i = 0, n = d->nvalue;
    trim(d->value, &i, &n);
    if (i >= n || d->value[i].kind != GARB_IDENT)
        return false;
    const garb_value_t *t = &d->value[i];
    if ((ieq(t->text, t->len, "normal") || ieq(t->text, t->len, "auto")) && i + 1 == n) {
        *out = GARB_FACE_NORMAL;
        return true;
    }
    if (ieq(t->text, t->len, "italic") && i + 1 == n) {
        *out = GARB_FACE_ITALIC;
        return true;
    }
    if (!ieq(t->text, t->len, "oblique"))
        return false;
    int32_t angles = 0;
    for (i++; i < n; i++) {
        double deg;
        if (d->value[i].kind == GARB_WHITESPACE)
            continue;
        if (++angles > 2 || !angle_deg(&d->value[i], &deg) || deg < -90 || deg > 90)
            return false;
    }
    *out = GARB_FACE_OBLIQUE;
    return true;
}

// An @font-face rule, read whole and kept (garb_cascade_font_faces). Each
// descriptor's last valid declaration is the one kept; one written wrong
// is dropped, as a property's is. A face any allocation failed for is
// dropped whole and counted lost: half a source list is not the rule.
static void add_face(garb_cascade_t *c, garb_parsed_t *owner, const garb_rule_t *r, int32_t sheet)
{
    garb_item_t *items;
    int32_t n;
    if (!r->has_block)
        return;
    // Past the cap, a rule is counted and nothing of it read or kept.
    if (c->nfaces == GARB_FONT_FACES_MAX) {
        c->faces_lost++;
        return;
    }
    if (!garb_items_of(owner, r->block, r->nblock, &items, &n)) {
        c->incomplete = true;
        c->faces_lost++;
        return;
    }
    uint32_t failed = c->allocs_failed;
    garb_font_face_t face = {NULL, 0, NULL, 0, 400, 400, GARB_FACE_NORMAL, NULL, 0, sheet};
    for (int32_t i = 0; i < n; i++) {
        const garb_decl_t *d = &items[i].decl;
        if (items[i].kind != GARB_ITEM_DECL)
            continue;
        if (ieq(d->name, d->len, "font-family")) {
            size_t len;
            const char *name = family_name(c, d->value, 0, d->nvalue, &len);
            if (name != NULL)
                face.family = name, face.family_len = len;
        } else if (ieq(d->name, d->len, "src")) {
            int32_t most = 1;
            for (int32_t k = 0; k < d->nvalue; k++)
                most += d->value[k].kind == GARB_COMMA;
            garb_face_src_t *src = alloc(c, (size_t)most * sizeof(*src));
            int32_t got = src != NULL ? parts(c, d, src_part, src, most, true) : 0;
            if (got > 0)
                face.src = src, face.nsrc = got;
        } else if (ieq(d->name, d->len, "font-weight")) {
            (void)face_weight(d, &face.weight_min, &face.weight_max);
        } else if (ieq(d->name, d->len, "font-style")) {
            (void)face_style(d, &face.style);
        } else if (ieq(d->name, d->len, "unicode-range")) {
            int32_t most = 1;
            for (int32_t k = 0; k < d->nvalue; k++)
                most += d->value[k].kind == GARB_COMMA;
            garb_unicode_range_t *ranges = alloc(c, (size_t)most * sizeof(*ranges));
            int32_t got = ranges != NULL ? parts(c, d, range_part, ranges, most, false) : -1;
            if (got > 0)
                face.ranges = ranges, face.nranges = got;
        }
    }
    // The family the page asked for and does not get yet (GARB_SKIP_FONT).
    if (face.family != NULL)
        skipped(c, GARB_SKIP_FONT, face.family, face.family_len);
    if (c->allocs_failed != failed) {
        c->faces_lost++;
        return;
    }
    if (face.family == NULL || face.nsrc == 0)
        return;
    if (!grow(c, (void **)&c->faces, c->nfaces, &c->facecap, sizeof(garb_font_face_t))) {
        c->faces_lost++;
        return;
    }
    c->faces[c->nfaces++] = face;
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
    garb_prop_t props[GARB_SETS_MAX];
    if (garb_decl_has_var(d)) {
        if (prop_longhands(d->name, d->len, props) == 0) {
            skipped(c, GARB_SKIP_PROPERTY, d->name, d->len);
            return false;
        }
        out->pending = true;
        return true;
    }
    garb_set_t sets[GARB_SETS_MAX];
    int32_t n = garb_read_declaration(owner, d, c->quirks == OS64_HTML_QUIRKS, sets);
    if (n <= 0) {
        // Which it was is asked only of what was dropped: the property
        // table is a walk.
        if (prop_longhands(d->name, d->len, props) == 0)
            skipped(c, GARB_SKIP_PROPERTY, d->name, d->len);
        else if (only_words(d->value, d->nvalue))
            skipped_value(c, d->name, d->len, d->value, d->nvalue);
        else
            skipped_functions(c, d->value, d->nvalue, 0);
        return false;
    }
    // Read, but laid out as something else (props.c's kApproximated): asked
    // for, and not given, as surely as a value dropped.
    for (int32_t k = 0; k < n; k++) {
        const char *word = garb_set_approximation(&sets[k]);
        if (word != NULL) {
            const char *prop = garb_prop_name(sets[k].prop);
            garb_value_t one = {.kind = GARB_IDENT, .text = word, .len = (uint32_t)os64_strlen(word)};
            skipped_value(c, prop, os64_strlen(prop), &one, 1);
        }
    }
    out->sets = alloc(c, (size_t)n * sizeof(garb_set_t));
    if (out->sets == NULL)
        return false;
    os64_memcpy(out->sets, sets, (size_t)n * sizeof(garb_set_t));
    out->nsets = n;
    return true;
}

static void add_style_rule(garb_cascade_t *c, garb_parsed_t *owner, const garb_rule_t *r,
                           uint32_t *order, int32_t sheet)
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
    rule->sheet = sheet;
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

// ── @keyframes (Animations 1 § 3, Animations 2 § 3.2) ──────────────────

// The words a <custom-ident> naming keyframes may not be.
static const char *const kNotKeyframesName[] = {"none", "inherit", "initial", "unset", "revert",
                                                "revert-layer", "default", NULL};

// A @keyframes rule's name: one identifier that is none of those, or one
// string that is not empty — as written either way.
static bool keyframes_name(const garb_rule_t *r, const char **name, size_t *len)
{
    const garb_value_t *t = NULL;
    for (int32_t i = 0; i < r->nprelude; i++) {
        if (r->prelude[i].kind == GARB_WHITESPACE)
            continue;
        if (t != NULL)
            return false;
        t = &r->prelude[i];
    }
    if (t == NULL)
        return false;
    if (t->kind == GARB_IDENT) {
        for (int k = 0; kNotKeyframesName[k] != NULL; k++)
            if (ieq(t->text, t->len, kNotKeyframesName[k]))
                return false;
    } else if (t->kind != GARB_STRING || t->len == 0) {
        return false;
    }
    *name = t->text;
    *len = t->len;
    return true;
}

// A keyframe block's offsets: `from`, `to` or a percentage in [0%, 100%],
// comma-separated, as fractions; 0 for a selector anything else is
// written in, which drops the block.
static int32_t key_offsets(garb_cascade_t *c, const garb_rule_t *r, double **out)
{
    int32_t most = 1;
    for (int32_t i = 0; i < r->nprelude; i++)
        most += r->prelude[i].kind == GARB_COMMA;
    double *at_ = alloc(c, (size_t)most * sizeof(double));
    if (at_ == NULL)
        return 0;
    int32_t n = 0;
    bool want = true;           // an offset next, not a comma
    for (int32_t i = 0; i < r->nprelude; i++) {
        const garb_value_t *t = &r->prelude[i];
        if (t->kind == GARB_WHITESPACE)
            continue;
        if (!want) {
            if (t->kind != GARB_COMMA)
                return 0;
            want = true;
            continue;
        }
        if (t->kind == GARB_IDENT && ieq(t->text, t->len, "from"))
            at_[n] = 0;
        else if (t->kind == GARB_IDENT && ieq(t->text, t->len, "to"))
            at_[n] = 1;
        else if (t->kind == GARB_PERCENTAGE && t->number >= 0 && t->number <= 100)
            at_[n] = t->number / 100;
        else
            return 0;
        n++;
        want = false;
    }
    if (want)
        return 0;               // empty, or ending in a comma
    *out = at_;
    return n;
}

// Whether a keyframe drops a declaration of `name`: every longhand of
// Animations 1 is dropped there but its timing function, which is the
// keyframe's own easing (§ 3, § 3.1).
static bool key_dropped(const garb_decl_t *d, bool *easing)
{
    garb_prop_t props[GARB_SETS_MAX];
    int32_t n = prop_longhands(d->name, d->len, props);
    *easing = n == 1 && props[0] == GARB_ANIMATION_TIMING_FUNCTION;
    for (int32_t k = 0; k < n && !*easing; k++)
        if (props[k] >= GARB_ANIMATION_NAME && props[k] <= GARB_ANIMATION_FILL_MODE)
            return true;
    return false;
}

// A @keyframes rule, kept; the last of a name in cascade order wins whole
// (Animations 1 § 3), once every sheet is read (keyframes_settle). Its
// blocks' declarations are read as a style rule's are, and a block's last
// timing function kept apart from them as its easing; `!important` ones,
// and the custom properties a keyframe would set (PILE3.md § Animations,
// Booked), are not kept.
static void add_keyframes(garb_cascade_t *c, garb_parsed_t *owner, const garb_rule_t *r,
                          int32_t sheet)
{
    const char *name;
    size_t len;
    if (!r->has_block || !keyframes_name(r, &name, &len))
        return;
    garb_item_t *inner;
    int32_t ninner;
    (void)garb_rules_of(owner, r->block, r->nblock, &inner, &ninner);
    KeyBlock *blocks = alloc(c, (size_t)(ninner > 0 ? ninner : 1) * sizeof(KeyBlock));
    if (blocks == NULL)
        return;
    int32_t nblocks = 0;
    bool pending = false;
    for (int32_t i = 0; i < ninner; i++) {
        if (inner[i].kind != GARB_ITEM_RULE || inner[i].rule->at || !inner[i].rule->has_block)
            continue;
        const garb_rule_t *kr = inner[i].rule;
        KeyBlock *b = &blocks[nblocks];
        os64_memset(b, 0, sizeof(*b));
        if ((b->noffsets = key_offsets(c, kr, &b->offsets)) == 0)
            continue;
        garb_item_t *items;
        int32_t nitems;
        (void)garb_items_of(owner, kr->block, kr->nblock, &items, &nitems);
        b->decls = alloc(c, (size_t)(nitems > 0 ? nitems : 1) * sizeof(Decl));
        if (b->decls == NULL)
            return;
        // The easing goes first: one of each block's is kept, the last.
        Decl easing = {0};
        bool has_easing = false;
        for (int32_t j = 0; j < nitems; j++) {
            const garb_decl_t *d = &items[j].decl;
            bool is_easing;
            if (items[j].kind != GARB_ITEM_DECL || d->important || garb_decl_is_custom(d) ||
                key_dropped(d, &is_easing))
                continue;
            Decl read;
            if (!read_decl(c, owner, d, &read))
                continue;
            pending |= read.pending;
            if (is_easing) {
                easing = read;
                has_easing = true;
            } else {
                b->decls[b->ndecls++] = read;
            }
        }
        if (has_easing) {
            os64_memmove(b->decls + 1, b->decls, (size_t)b->ndecls * sizeof(Decl));
            b->decls[0] = easing;
            b->ndecls++;
            b->easing = true;
        }
        nblocks++;
    }
    if (!grow(c, (void **)&c->keyframes, c->nkeyframes, &c->keyframecap, sizeof(Keyframes)))
        return;
    c->keyframes[c->nkeyframes++] = (Keyframes){name, len, blocks, nblocks, sheet, pending, NULL, 0};
}

static void add_rules(garb_cascade_t *c, garb_parsed_t *owner, const garb_item_t *items,
                      int32_t n, uint32_t *order, int depth, int32_t sheet)
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
                add_style_rule(c, owner, r, order, sheet);
            continue;
        }
        // The grouping rules whose contents join the sheet when they hold.
        // @layer's contents are cascaded as if unlayered (booked: layers).
        // @keyframes is kept apart (add_keyframes). Every other at-rule is
        // passed over here, @import among them: its sheet is the face's to
        // fetch, and joins at its place (GARB.md, G4).
        bool group = at(r, "media") ? garb_media_matches(r->prelude, r->nprelude, c->env)
                   : at(r, "supports") ? supports(c, owner, r->prelude, r->nprelude)
                   : at(r, "layer") && r->has_block;
        // What is read elsewhere is not passed over: @import's sheet is the
        // face's, @charset was the decoder's.
        if (at(r, "font-face"))
            add_face(c, owner, r, sheet);
        else if (at(r, "keyframes") || at(r, "-webkit-keyframes"))
            add_keyframes(c, owner, r, sheet);
        else if (!at(r, "media") && !at(r, "supports") && !at(r, "layer") && !at(r, "import") &&
                 !at(r, "charset"))
            skipped(c, GARB_SKIP_AT_RULE, r->name, r->len);
        if (!group || !r->has_block)
            continue;
        garb_item_t *inner;
        int32_t ninner;
        (void)garb_rules_of(owner, r->block, r->nblock, &inner, &ninner);
        add_rules(c, owner, inner, ninner, order, depth + 1, sheet);
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
    // A declaration: supported if its property is read here, its value fits
    // (a custom property always does), and what it says is laid out as it
    // says — a value read but laid out as something else is not supported,
    // or a page would be told not to use the fallback it wrote for that.
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
    int32_t read = garb_read_declaration(owner, d, c->quirks == OS64_HTML_QUIRKS, sets);
    for (int32_t k = 0; k < read; k++)
        if (garb_set_approximated(&sets[k]))
            return false;
    return read > 0;
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

// ── @import (Cascade 4 § 2.1) ───────────────────────────────────────────

// `@import <url> [layer | layer(…)]? [supports(…)]? <media-query-list>?`:
// false for a prelude that is not one.
static bool read_import(const garb_rule_t *r, garb_import_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    VCur c = {r->prelude, r->nprelude, 0};
    const garb_value_t *t = vc_peek(&c);
    garb_val_t url;
    if (t != NULL && t->kind == GARB_STRING) {
        out->url = t->text;
        out->len = t->len;
        c.i++;
    } else if (vc_url(&c, &url)) {
        out->url = url.text;
        out->len = url.len;
    } else {
        return false;
    }
    // Layers are cascaded as unlayered (GARB.md § Booked): the name is
    // passed over.
    t = vc_peek(&c);
    if (t != NULL && ((t->kind == GARB_IDENT && ieq(t->text, t->len, "layer")) ||
                      (t->kind == GARB_FUNCTION && ieq(t->text, t->len, "layer"))))
        c.i++;
    t = vc_peek(&c);
    if (t != NULL && t->kind == GARB_FUNCTION && ieq(t->text, t->len, "supports")) {
        out->has_supports = true;
        out->supports = t->children;
        out->nsupports = t->nchildren;
        c.i++;
    }
    (void)vc_peek(&c);
    out->media = c.i < c.n ? &c.v[c.i] : NULL;
    out->nmedia = c.n - c.i;
    return true;
}

int32_t garb_sheet_imports(const garb_parsed_t *sheet, garb_import_t *out, int32_t cap)
{
    int32_t n = 0;
    for (int32_t i = 0; sheet != NULL && i < sheet->nitems; i++) {
        if (sheet->items[i].kind != GARB_ITEM_RULE)
            continue;
        const garb_rule_t *r = sheet->items[i].rule;
        if (at(r, "charset") || (at(r, "layer") && !r->has_block))
            continue;
        if (!at(r, "import"))
            break;
        garb_import_t one;
        if (r->has_block || !read_import(r, &one))
            continue;
        if (n < cap)
            out[n] = one;
        n++;
    }
    return n;
}

// Whether sheet `s` of the list applies: its element's media, and for an
// imported one its @import's media and supports() and its importer's.
static bool sheet_applies(garb_cascade_t *c, const garb_sheet_in_t *sheets, int32_t n,
                          int32_t s, int depth)
{
    const garb_sheet_in_t *in = &sheets[s];
    if (in->sheet == NULL || !garb_media_text_matches(in->media, c->env))
        return false;
    if (in->via == NULL)
        return true;
    if (depth > NEST_MAX || in->parent <= s || in->parent >= n) {
        c->incomplete = true;
        return false;
    }
    const garb_import_t *im = in->via;
    if (!garb_media_matches(im->media, im->nmedia, c->env))
        return false;
    if (im->has_supports) {
        // supports()'s argument is a condition or a declaration: read as
        // the inside of a parenthesis, which is where both may stand.
        garb_value_t paren = {0};
        paren.kind = GARB_BLOCK;
        paren.open = '(';
        paren.children = (garb_value_t *)im->supports;
        paren.nchildren = im->nsupports;
        if (!supports(c, sheets[in->parent].sheet, &paren, 1))
            return false;
    }
    return sheet_applies(c, sheets, n, in->parent, depth + 1);
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
// A custom property declared with nothing (`--on: ;`) holds the EMPTY value,
// which is a value: var() substitutes nothing for it, where the
// guaranteed-invalid value (NULL) takes the fallback. The "space toggle"
// idiom is exactly that difference, so the empty one is this, never NULL.
static const garb_value_t kEmptyValue[1];

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
                    cu->value = out.n > 0 ? out.v : kEmptyValue;
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

// ── An animation's keyframes, read ──────────────────────────────────────

// The longhands a keyframe's declaration sets, into `out`: its own when it
// holds no var(); else, for an element (`own`), its value with each var()
// replaced against `customs` and then read — and nothing when that fails,
// since a keyframe's invalid value is as if that keyframe did not set the
// property (Animations 1 § 3) — or nothing at all without one.
static int32_t key_sets(garb_cascade_t *c, const Decl *d, const CustomMap *customs, bool own,
                        garb_set_t *out)
{
    if (!d->pending) {
        os64_memcpy(out, d->sets, (size_t)d->nsets * sizeof(garb_set_t));
        return d->nsets;
    }
    if (!own)
        return 0;
    Sub s = {c, customs, 0, VAR_VALUES_MAX};
    Vec v = {0};
    if (!substitute(&s, d->decl.value, d->decl.nvalue, &v))
        return 0;
    garb_decl_t read = d->decl;
    read.value = v.v;
    read.nvalue = v.n;
    int32_t n = garb_read_declaration(&c->self, &read, c->quirks == OS64_HTML_QUIRKS, out);
    return n > 0 ? n : 0;
}

// A stable sort of `n` items `size` bytes each, by `cmp`: a merge through
// `tmp`, room for n more — n log n, however many a page's CSS makes.
static void merge_sort(void *v, void *tmp, int32_t n, size_t size,
                       int (*cmp)(const void *, const void *))
{
    if (n < 2)
        return;
    int32_t h = n / 2;
    char *p = v, *t = tmp;
    merge_sort(p, t, h, size, cmp);
    merge_sort(p + (size_t)h * size, t, n - h, size, cmp);
    int32_t i = 0, j = h, k = 0;
    while (i < h && j < n) {
        const char *a = p + (size_t)i * size, *b = p + (size_t)j * size;
        bool right = cmp(b, a) < 0;
        os64_memcpy(t + (size_t)k++ * size, right ? b : a, size);
        right ? j++ : i++;
    }
    while (i < h)
        os64_memcpy(t + (size_t)k++ * size, p + (size_t)i++ * size, size);
    while (j < n)
        os64_memcpy(t + (size_t)k++ * size, p + (size_t)j++ * size, size);
    os64_memcpy(p, t, (size_t)n * size);
}

static int name_cmp(const char *a, size_t la, const char *b, size_t lb)
{
    int c = os64_memcmp(a, b, la < lb ? la : lb);
    return c != 0 ? c : (la > lb) - (la < lb);
}

// An easing as keyframes are grouped by it (Chrome's answer): a keyword
// curve by its name — `ease` is another easing than cubic-bezier(0.25,
// 0.1, 0.25, 1); cubic-bezier() by its four numbers, each compared as the
// number it is and not as printed (garb_val_dump rounds to six places,
// which made cubic-bezier(0, 1e-7, 1, 1) and (0, 2e-7, 1, 1) one); and
// steps by their count and where they jump, however written — step-start
// and step-end are steps(1, start) and steps(1, end), and jump-start and
// jump-end are start and end.
typedef struct {
    const char *word;
    double n[4];
    const char *jump;
} Easing;

static Easing easing_canon(const garb_val_t *v)
{
    Easing e = {v->keyword != NULL ? v->keyword : "", {0, 0, 0, 0}, ""};
    if (v->kind == GARB_V_KEYWORD && (os64_streq(e.word, "step-start") || os64_streq(e.word, "step-end"))) {
        e.jump = e.word + 5;
        e.word = "steps";
        e.n[0] = 1;
    } else if (v->kind == GARB_V_FUNCTION && os64_streq(e.word, "steps")) {
        e.n[0] = v->nitems > 0 ? v->items[0].number : 0;
        e.jump = v->nitems > 1 && v->items[1].keyword != NULL ? v->items[1].keyword : "end";
        if (os64_streq(e.jump, "jump-start") || os64_streq(e.jump, "jump-end"))
            e.jump += 5;
    } else if (v->kind == GARB_V_FUNCTION) {
        for (int32_t i = 0; i < v->nitems && i < 4; i++)
            e.n[i] = v->items[i].number;
    }
    return e;
}

// Two easings in a total order, equal when they are one easing
// (easing_canon); no easing sorts first.
static int easing_cmp(const garb_val_t *a, const garb_val_t *b)
{
    if (a == NULL || b == NULL)
        return (a != NULL) - (b != NULL);
    Easing x = easing_canon(a), y = easing_canon(b);
    int k = os64_strcmp(x.word, y.word);
    if (k != 0)
        return k;
    for (int i = 0; i < 4; i++)
        if (x.n[i] != y.n[i])
            return x.n[i] < y.n[i] ? -1 : 1;
    return os64_strcmp(x.jump, y.jump);
}

// One block at one of its offsets: the offset, the block's easing, the
// block, and where it was written among the rule's.
typedef struct {
    double offset;
    const garb_val_t *easing;
    int32_t block;
    int32_t order;
} KeyAt;

static int key_at_cmp(const void *a, const void *b)
{
    const KeyAt *x = a, *y = b;
    if (x->offset != y->offset)
        return x->offset < y->offset ? -1 : 1;
    int t = easing_cmp(x->easing, y->easing);
    return t != 0 ? t : (x->order > y->order) - (x->order < y->order);
}

// A keyframe: the run of KeyAt that makes it, and where its first block was
// written.
typedef struct {
    int32_t first, count, order;
    double offset;
} KeyRun;

static int key_run_cmp(const void *a, const void *b)
{
    const KeyRun *x = a, *y = b;
    if (x->offset != y->offset)
        return x->offset < y->offset ? -1 : 1;
    return (x->order > y->order) - (x->order < y->order);
}

// A rule's keyframes, read for an element (`own`, its custom properties
// `customs`) or for none: Animations 2 § 3.2's groups — the blocks at one
// offset with one easing, each longhand's last declaration among them —
// sorted by offset, a tie in the order each was first written; each kept
// declaration with where it was written. Two blocks at one offset, one
// giving an easing equal to the element's and one giving none, are two
// keyframes here, since the element's easing is not this library's to
// know: libflow, which knows it, makes them one by those places. Grouped by
// sorting, not by searching the groups so far: a rule may have as many
// offsets as a page can write. Into the cascade's arena; how many.
static int32_t build_frames(garb_cascade_t *c, const Keyframes *k, const CustomMap *customs,
                            bool own, garb_keyframe_t **out)
{
    *out = NULL;
    int32_t n = 0;
    for (int32_t b = 0; b < k->nblocks; b++)
        n += k->blocks[b].noffsets;
    if (n == 0)
        return 0;
    // Each block's easing, read once: the timing function's first item, in
    // the arena.
    const garb_val_t **easings = os64_calloc((size_t)k->nblocks, sizeof(*easings));
    KeyAt *at = os64_malloc((size_t)n * 2 * sizeof(*at));
    KeyRun *runs = os64_malloc((size_t)n * 2 * sizeof(*runs));
    garb_set_t *win = os64_malloc((size_t)GARB_NPROPS * sizeof(*win));
    uint64_t *written = os64_malloc((size_t)GARB_NPROPS * sizeof(*written));
    int32_t *stamp = os64_calloc((size_t)GARB_NPROPS, sizeof(*stamp));
    garb_prop_t *props = os64_malloc((size_t)GARB_NPROPS * sizeof(*props));
    bool ok = easings != NULL && at != NULL && runs != NULL && win != NULL &&
              written != NULL && stamp != NULL && props != NULL;
    garb_set_t sets[GARB_SETS_MAX];
    int32_t m = 0;
    for (int32_t b = 0; ok && b < k->nblocks; b++) {
        const KeyBlock *kb = &k->blocks[b];
        if (kb->easing && key_sets(c, &kb->decls[0], customs, own, sets) == 1 &&
            sets[0].value.kind != GARB_V_WIDE) {
            garb_val_t *e = alloc(c, sizeof(*e));
            if (e != NULL)
                *e = sets[0].value.kind == GARB_V_LAYERS ? sets[0].value.items[0] : sets[0].value;
            easings[b] = e;
        }
        for (int32_t o = 0; o < kb->noffsets; o++, m++)
            at[m] = (KeyAt){kb->offsets[o], easings[b], b, m};
    }
    // Runs of one offset and one easing, each a keyframe; then sorted by
    // offset, a tie in the order first written.
    int32_t nruns = 0;
    if (ok) {
        merge_sort(at, at + n, n, sizeof(*at), key_at_cmp);
        for (int32_t i = 0; i < n;) {
            int32_t j = i + 1;
            while (j < n && at[j].offset == at[i].offset && easing_cmp(at[j].easing, at[i].easing) == 0)
                j++;
            runs[nruns++] = (KeyRun){i, j - i, at[i].order, at[i].offset};
            i = j;
        }
        merge_sort(runs, runs + nruns, nruns, sizeof(*runs), key_run_cmp);
    }
    garb_keyframe_t *frames = ok ? alloc(c, (size_t)nruns * sizeof(*frames)) : NULL;
    for (int32_t r = 0; frames != NULL && r < nruns; r++) {
        // Each longhand's last value among the run's blocks, in the order
        // written, and the longhands in their table's order.
        int32_t np = 0;
        for (int32_t i = runs[r].first; i < runs[r].first + runs[r].count; i++) {
            const KeyBlock *kb = &k->blocks[at[i].block];
            for (int32_t d = kb->easing ? 1 : 0; d < kb->ndecls; d++) {
                int32_t ns = key_sets(c, &kb->decls[d], customs, own, sets);
                for (int32_t s2 = 0; s2 < ns; s2++) {
                    garb_prop_t p = sets[s2].prop;
                    win[p] = sets[s2];
                    written[p] = (uint64_t)(uint32_t)at[i].order << 32 | (uint32_t)d;
                    if (stamp[p] != r + 1) {
                        stamp[p] = r + 1;
                        props[np++] = p;
                    }
                }
            }
        }
        for (int32_t i = 1; i < np; i++) {      // at most GARB_NPROPS
            garb_prop_t x = props[i];
            int32_t j = i - 1;
            while (j >= 0 && props[j] > x) {
                props[j + 1] = props[j];
                j--;
            }
            props[j + 1] = x;
        }
        garb_set_t *kept = np > 0 ? alloc(c, (size_t)np * sizeof(*kept)) : NULL;
        uint64_t *order = np > 0 ? alloc(c, (size_t)np * sizeof(*order)) : NULL;
        if (order == NULL)
            kept = NULL;
        for (int32_t i = 0; kept != NULL && i < np; i++) {
            kept[i] = win[props[i]];
            order[i] = written[props[i]];
        }
        frames[r] = (garb_keyframe_t){.offset = runs[r].offset, .easing = at[runs[r].first].easing,
                                      .sets = kept, .order = kept != NULL ? order : NULL,
                                      .nsets = kept != NULL ? np : 0, .sheet = k->sheet};
    }
    os64_free(written);
    os64_free(easings);
    os64_free(at);
    os64_free(runs);
    os64_free(win);
    os64_free(stamp);
    os64_free(props);
    if (!ok || frames == NULL)
        c->incomplete = true;
    *out = frames;
    return frames != NULL ? nruns : 0;
}

static int keyframes_cmp(const void *a, const void *b)
{
    const Keyframes *x = a, *y = b;
    return name_cmp(x->name, x->len, y->name, y->len);
}

// One rule per name, the last of it in cascade order, sorted by name for
// keyframes_named's search. With no room to sort, the rules stay as read
// and are searched from the last.
static void keyframes_settle(garb_cascade_t *c)
{
    int32_t n = c->nkeyframes;
    Keyframes *tmp = n > 1 ? os64_malloc((size_t)n * sizeof(*tmp)) : NULL;
    if (n > 1 && tmp == NULL) {
        c->incomplete = true;
        return;
    }
    merge_sort(c->keyframes, tmp, n, sizeof(*tmp), keyframes_cmp);
    os64_free(tmp);
    int32_t kept = 0;
    for (int32_t i = 0; i < n; i++) {
        if (i + 1 < n && keyframes_cmp(&c->keyframes[i], &c->keyframes[i + 1]) == 0)
            continue;           // a later one of this name follows
        c->keyframes[kept++] = c->keyframes[i];
    }
    c->nkeyframes = kept;
    c->keyframes_sorted = true;
}

static const Keyframes *keyframes_named(const garb_cascade_t *c, const char *name, size_t len)
{
    if (c->keyframes_sorted) {
        int32_t lo = 0, hi = c->nkeyframes;
        while (lo < hi) {
            int32_t mid = lo + (hi - lo) / 2;
            int x = name_cmp(c->keyframes[mid].name, c->keyframes[mid].len, name, len);
            if (x == 0)
                return &c->keyframes[mid];
            if (x < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        return NULL;
    }
    for (int32_t i = c->nkeyframes - 1; i >= 0; i--)
        if (name_cmp(c->keyframes[i].name, c->keyframes[i].len, name, len) == 0)
            return &c->keyframes[i];
    return NULL;
}

static int own_frames_cmp(const void *a, const void *b)
{
    uintptr_t x = (uintptr_t)((const OwnFrames *)a)->rule, y = (uintptr_t)((const OwnFrames *)b)->rule;
    return (x > y) - (x < y);
}

// An element that animates — by its winners' names or by those it
// inherits (animation_names) — reads, against its own custom properties,
// each named rule that holds var(), once however often it is named; the
// rest it shares. Sorted by rule for garb_keyframes' search.
static void own_frames(garb_cascade_t *c, Slot *slot, const garb_val_t *names,
                       const CustomMap *customs)
{
    const garb_val_t *items = names->kind == GARB_V_LAYERS ? names->items : names;
    int32_t n = names->kind == GARB_V_LAYERS ? names->nitems : 1, m = 0;
    OwnFrames *own = n > 0 ? alloc(c, (size_t)n * sizeof(*own)) : NULL;
    for (int32_t i = 0; own != NULL && i < n; i++) {
        const Keyframes *k = items[i].kind == GARB_V_STRING
                           ? keyframes_named(c, items[i].text, items[i].len) : NULL;
        if (k != NULL && k->pending)
            own[m++] = (OwnFrames){k, NULL, 0};
    }
    if (m == 0)
        return;
    OwnFrames *tmp = m > 1 ? os64_malloc((size_t)m * sizeof(*tmp)) : NULL;
    if (m > 1 && tmp == NULL) {
        c->incomplete = true;
        return;
    }
    merge_sort(own, tmp, m, sizeof(*own), own_frames_cmp);
    os64_free(tmp);
    int32_t u = 0;
    for (int32_t i = 0; i < m; i++)
        if (u == 0 || own[u - 1].rule != own[i].rule)
            own[u++] = own[i];
    for (int32_t i = 0; i < u; i++)
        own[i].nframes = build_frames(c, own[i].rule, customs, true, &own[i].frames);
    slot->frames = own;
    slot->nframes = u;
}

static bool is_el(const os64_html_node_t *n);

// The animation names element `el` computes from its winner `v`: the
// winner's own, or for `inherit` its parent's, as far up as `inherit`
// goes — whose rules with var() are then built against `el`'s custom
// properties, since `el` animates with them. NULL for none: no winner, or
// another wide keyword (animation-name does not inherit).
static const garb_val_t *animation_names(const garb_cascade_t *c, const os64_html_node_t *el,
                                         const garb_val_t *v)
{
    while (v != NULL && v->kind == GARB_V_WIDE && os64_streq(v->keyword, "inherit")) {
        el = el->parent;
        const Slot *up = is_el(el) ? slot_for(c, el, false) : NULL;
        v = NULL;
        for (int32_t i = 0; up != NULL && i < up->style.n; i++)
            if (up->style.sets[i].prop == GARB_ANIMATION_NAME)
                v = &up->style.sets[i].value;
    }
    return v != NULL && v->kind != GARB_V_WIDE ? v : NULL;
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
    int32_t sheet;              // -1 for the style attribute
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
            ap[na++] = (Applied){d, d->important, false, (*m)[k].spec, (*m)[k].rule->order,
                                 (*m)[k].rule->sheet};
        }
    for (int32_t j = 0; j < ninl; j++)
        ap[na++] = (Applied){&inl[j], inl[j].important, true, 0, (uint32_t)j, -1};
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
                cu->value = d->nvalue > 0 ? d->value : kEmptyValue;
                cu->nvalue = d->nvalue;
                cu->state = 0;
                // `--x: initial` is the guaranteed-invalid value, its
                // initial value. `inherit` is the parent's, and so is
                // `unset`: a custom property inherits (Cascade 4 § 7.3).
                bool one_ident = d->nvalue == 1 && d->value[0].kind == GARB_IDENT;
                if (one_ident && ieq(d->value[0].text, d->value[0].len, "initial"))
                    cu->value = NULL;
                if (one_ident && (ieq(d->value[0].text, d->value[0].len, "inherit") ||
                                  ieq(d->value[0].text, d->value[0].len, "unset"))) {
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
    int32_t from[GARB_NPROPS];
    bool have[GARB_NPROPS], important[GARB_NPROPS];
    os64_memset(have, 0, sizeof(have));
    for (int32_t i = 0; i < na; i++) {
        const Decl *d = ap[i].d;
        if (d->custom)
            continue;
        if (!d->pending) {
            for (int32_t k = 0; k < d->nsets; k++) {
                win[d->sets[k].prop] = d->sets[k];
                from[d->sets[k].prop] = ap[i].sheet;
                important[d->sets[k].prop] = ap[i].important;
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
            from[sets[k].prop] = ap[i].sheet;
            important[sets[k].prop] = ap[i].important;
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
    int32_t *kept_from = alloc(c, (size_t)nwin * sizeof(int32_t));
    bool *kept_important = alloc(c, (size_t)nwin * sizeof(bool));
    if (kept == NULL || kept_from == NULL || kept_important == NULL)
        return;
    int32_t k = 0;
    for (int p = 0; p < GARB_NPROPS; p++)
        if (have[p]) {
            kept_from[k] = from[p];
            kept_important[k] = important[p];
            kept[k++] = win[p];
        }
    slot->style.sets = kept;
    slot->style.sheet = kept_from;
    slot->style.important = kept_important;
    slot->style.n = nwin;
    const garb_val_t *names = animation_names(c, el, have[GARB_ANIMATION_NAME]
                                                     ? &win[GARB_ANIMATION_NAME].value : NULL);
    if (names != NULL)
        own_frames(c, slot, names, customs);
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
    c->doc = doc;
    c->pin = os64_html_pin(doc);
    if (c->pin == 0) {
        garb_cascade_free(c);
        return NULL;
    }
    c->env = env;
    c->quirks = doc->quirks;
    c->universal = -1;
    uint32_t order = 0;
    for (int32_t s = 0; s < n; s++) {
        garb_parsed_t *sheet = sheets[s].sheet;
        if (!sheet_applies(c, sheets, n, s, 0))
            continue;
        add_rules(c, sheet, sheet->items, sheet->nitems, &order, 0, s);
    }
    keyframes_settle(c);
    // Each @keyframes read for no element: what an element shares.
    for (int32_t k = 0; k < c->nkeyframes; k++)
        c->keyframes[k].nframes = build_frames(c, &c->keyframes[k], NULL, false,
                                               &c->keyframes[k].frames);
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

garb_status_t garb_parse_style_element(const os64_html_node_t *style, garb_parsed_t *out)
{
    size_t len = 0;
    const os64_html_node_t *only = NULL;
    int32_t texts = 0;
    for (const os64_html_node_t *k = style != NULL ? style->first_child : NULL; k != NULL;
         k = k->next)
        if (k->kind == OS64_HTML_TEXT) {
            len += k->text_len;
            only = k;
            texts++;
        }
    if (texts <= 1)
        return garb_parse_sheet_text(only != NULL ? only->text : "", only != NULL ? len : 0, out);
    char *joined = os64_malloc(len + 1);
    if (joined == NULL) {
        os64_memset(out, 0, sizeof(*out));
        return GARB_NO_MEMORY;
    }
    size_t at = 0;
    for (const os64_html_node_t *k = style->first_child; k != NULL; k = k->next)
        if (k->kind == OS64_HTML_TEXT) {
            os64_memcpy(joined + at, k->text, k->text_len);
            at += k->text_len;
        }
    garb_status_t st = garb_parse_sheet_text(joined, len, out);
    os64_free(joined);
    return st;
}

garb_env_t garb_cascade_env(const garb_cascade_t *c)
{
    return c != NULL ? c->env : (garb_env_t){.width = 0, .height = 0};
}

garb_style_t garb_style_for(const garb_cascade_t *c, const os64_html_node_t *element)
{
    garb_style_t none = {NULL, NULL, NULL, 0};
    if (c == NULL)
        return none;
    const Slot *s = slot_for(c, element, false);
    return s != NULL ? s->style : none;
}

int32_t garb_keyframes(const garb_cascade_t *c, const os64_html_node_t *element,
                       const char *name, size_t len, const garb_keyframe_t **out)
{
    const Keyframes *k = c != NULL ? keyframes_named(c, name, len) : NULL;
    if (k == NULL)
        return -1;
    const Slot *s = k->pending ? slot_for(c, element, false) : NULL;
    int32_t lo = 0, hi = s != NULL ? s->nframes : 0;
    while (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if ((uintptr_t)s->frames[mid].rule < (uintptr_t)k)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (s != NULL && lo < s->nframes && s->frames[lo].rule == k) {
        *out = s->frames[lo].frames;
        return s->frames[lo].nframes;
    }
    *out = k->frames;
    return k->nframes;
}

int32_t garb_cascade_font_faces(const garb_cascade_t *c, const garb_font_face_t **out, uint32_t *lost)
{
    if (lost != NULL)
        *lost = c != NULL ? c->faces_lost : 0;
    *out = c != NULL ? c->faces : NULL;
    return c != NULL ? c->nfaces : 0;
}

int32_t garb_cascade_skips(const garb_cascade_t *c, garb_skip_t *out, int32_t cap, uint32_t *lost)
{
    if (lost != NULL)
        *lost = c != NULL ? c->skips_lost : 0;
    if (c == NULL)
        return 0;
    for (int32_t i = 0; i < c->nskips && i < cap; i++)
        out[i] = c->skips[i];
    return c->nskips;
}

void garb_cascade_free(garb_cascade_t *c)
{
    if (c == NULL)
        return;
    for (int32_t i = 0; i < c->nattrs; i++)
        garb_free(&c->attrs[i]);
    os64_free(c->slots);
    os64_arena_destroy(c->self.arena);
    if (c->pin != 0)
        os64_html_unpin(c->doc, c->pin);
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
        garb_style_t st = is_el(x) ? garb_style_for(c, x) : (garb_style_t){NULL, NULL, NULL, 0};
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
