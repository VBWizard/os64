// importmap.c — the window's import map (importmap.h).

#include "importmap.h"

#include "os64/fmt.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/url.h"
#include "page/page.h"

// A rule: a normalized key and the address it maps to, NULL for a null
// rule. Every list of them is kept in DESCENDING code-unit order of key,
// which is the order HTML's resolution reads them: "a/b/" before "a/", so
// the longest prefix that matches is the one found first.
typedef struct {
    char *key;
    char *url;
} Rule;

typedef struct {
    Rule *rules;
    uint32_t count, cap;
} Rules;

typedef struct {
    char *prefix;               // serialized
    Rules rules;
} Scope;

// A specifier resolution record (HTML § 8.1.5.3): the serialized base URL
// it was resolved against, the normalized specifier, and whether a prefix
// rule could have matched it (HTML's "specifier as a URL is null or is
// special"), which is all the merge asks of the URL.
typedef struct {
    char *base;
    char *specifier;
    bool prefixable;
} Record;

struct yonder_import_map {
    Rules imports;
    Scope *scopes;              // descending order of prefix
    uint32_t nscopes, scopecap;
    uint32_t rules;             // the imports' and every scope's together
    Record *records;
    uint32_t nrecords, reccap;
    // The records by (base, specifier), so a resolution made again is not
    // recorded again: open addressing, a record's index + 1, 0 empty.
    uint32_t *slots;
    uint32_t slotcap;
};

static char *copy(const char *text)
{
    size_t n = os64_strlen(text) + 1;
    char *c = os64_malloc(n);
    if (c != NULL)
        os64_memcpy(c, text, n);
    return c;
}

static bool starts_with(const char *text, const char *prefix)
{
    while (*prefix != '\0')
        if (*text++ != *prefix++)
            return false;
    return true;
}

static bool ends_with_slash(const char *text)
{
    size_t n = os64_strlen(text);
    return n != 0 && text[n - 1] == '/';
}

// The URL Standard's parse of `input` against `base` (NULL: none),
// serialized, by the page's rule; false when it is no URL (an address with
// no scheme included) or the answer is longer than `cap`.
static bool url_parse(const char *input, const char *base, char *out, size_t cap)
{
    if (!os64_page_resolve_in(NULL, base, input, out, cap))
        return false;
    char scheme[OS64_URL_SCHEME_MAX];
    return os64_url_scheme_of(out, scheme, sizeof(scheme));
}

// The URL Standard's special schemes, whose paths a prefix rule may extend.
static bool special(const char *url)
{
    static const char *const kSpecial[] = {"ftp:", "file:", "http:", "https:", "ws:", "wss:"};
    for (size_t i = 0; i < sizeof(kSpecial) / sizeof(kSpecial[0]); i++)
        if (starts_with(url, kSpecial[i]))
            return true;
    return false;
}

// HTML's "resolve a URL-like module specifier": a specifier that begins
// "/", "./" or "../" parsed against `base`, any other parsed alone. False
// when it is not URL-like: a bare name, or one that will not parse.
static bool url_like(const char *specifier, const char *base, char *out, size_t cap)
{
    if (starts_with(specifier, "/") || starts_with(specifier, "./") || starts_with(specifier, "../"))
        return url_parse(specifier, base, out, cap);
    return url_parse(specifier, NULL, out, cap);
}

// ── Rules ──────────────────────────────────────────────────────────────

// Where `key` is, or would go, in `list`'s descending order; *found when it
// is there.
static uint32_t rule_place(const Rules *list, const char *key, bool *found)
{
    uint32_t low = 0, high = list->count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        int order = os64_strcmp(list->rules[mid].key, key);
        if (order == 0) {
            *found = true;
            return mid;
        }
        if (order > 0)
            low = mid + 1;
        else
            high = mid;
    }
    *found = false;
    return low;
}

static void rule_free(Rule *rule)
{
    os64_free(rule->key);
    os64_free(rule->url);
}

static void rules_free(Rules *list)
{
    for (uint32_t i = 0; i < list->count; i++)
        rule_free(&list->rules[i]);
    os64_free(list->rules);
    *list = (Rules){0};
}

// `key` mapped to `url` (NULL: a null rule) in `list`: the earlier rule for
// the key replaced when `replace`, else kept and the new one dropped. Both
// strings are copied. False on no memory or past the map's bound.
static bool rule_put(yonder_import_map_t *map, Rules *list, const char *key, const char *url, bool replace)
{
    bool found;
    uint32_t at = rule_place(list, key, &found);
    if (found && !replace)
        return true;
    char *kept = url != NULL ? copy(url) : NULL;
    if (url != NULL && kept == NULL)
        return false;
    if (found) {
        os64_free(list->rules[at].url);
        list->rules[at].url = kept;
        return true;
    }
    char *name = map->rules < YONDER_IMPORT_MAP_RULES_MAX ? copy(key) : NULL;
    if (name == NULL) {
        os64_free(kept);
        return false;
    }
    if (list->count == list->cap) {
        uint32_t cap = list->cap != 0 ? list->cap * 2 : 8;
        Rule *rules = os64_realloc(list->rules, cap * sizeof(*rules));
        if (rules == NULL) {
            os64_free(name);
            os64_free(kept);
            return false;
        }
        list->rules = rules;
        list->cap = cap;
    }
    os64_memmove(&list->rules[at + 1], &list->rules[at], (list->count - at) * sizeof(Rule));
    list->rules[at] = (Rule){name, kept};
    list->count++;
    map->rules++;
    return true;
}

// The rule at `at` taken out of `list`.
static void rule_remove(yonder_import_map_t *map, Rules *list, uint32_t at)
{
    rule_free(&list->rules[at]);
    os64_memmove(&list->rules[at], &list->rules[at + 1], (list->count - at - 1) * sizeof(Rule));
    list->count--;
    map->rules--;
}

// The scope `prefix` names, made empty when `make` and absent; NULL when
// absent and not made, or on no memory.
static Scope *scope_of(yonder_import_map_t *map, const char *prefix, bool make)
{
    uint32_t low = 0, high = map->nscopes;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        int order = os64_strcmp(map->scopes[mid].prefix, prefix);
        if (order == 0)
            return &map->scopes[mid];
        if (order > 0)
            low = mid + 1;
        else
            high = mid;
    }
    if (!make)
        return NULL;
    char *name = copy(prefix);
    if (name == NULL)
        return NULL;
    if (map->nscopes == map->scopecap) {
        uint32_t cap = map->scopecap != 0 ? map->scopecap * 2 : 4;
        Scope *scopes = os64_realloc(map->scopes, cap * sizeof(*scopes));
        if (scopes == NULL) {
            os64_free(name);
            return NULL;
        }
        map->scopes = scopes;
        map->scopecap = cap;
    }
    os64_memmove(&map->scopes[low + 1], &map->scopes[low], (map->nscopes - low) * sizeof(Scope));
    map->scopes[low] = (Scope){name, {0}};
    map->nscopes++;
    return &map->scopes[low];
}

yonder_import_map_t *yonder_import_map_new(void)
{
    return os64_calloc(1, sizeof(yonder_import_map_t));
}

void yonder_import_map_free(yonder_import_map_t *map)
{
    if (map == NULL)
        return;
    rules_free(&map->imports);
    for (uint32_t i = 0; i < map->nscopes; i++) {
        os64_free(map->scopes[i].prefix);
        rules_free(&map->scopes[i].rules);
    }
    os64_free(map->scopes);
    for (uint32_t i = 0; i < map->nrecords; i++) {
        os64_free(map->records[i].base);
        os64_free(map->records[i].specifier);
    }
    os64_free(map->records);
    os64_free(map->slots);
    os64_free(map);
}

// ── Parsing ────────────────────────────────────────────────────────────

bool yonder_import_map_add(yonder_import_map_t *map, const char *base, const char *scope, const char *key,
                           const char *value)
{
    if (map == NULL || base == NULL || key == NULL)
        return false;
    // "normalize a specifier key": an empty key is no key, and a URL-like
    // one is its URL, serialized.
    if (key[0] == '\0')
        return true;
    char normalized[OS64_URL_REF_MAX];
    if (!url_like(key, base, normalized, sizeof(normalized))) {
        if (os64_strlen(key) >= sizeof(normalized))
            return true;                // a key no specifier yonder keeps can name
        os64_strcopy(normalized, sizeof(normalized), key);
    }
    // The address: a URL-like specifier, ending in a slash when its key
    // does; anything else is a null rule.
    char address[OS64_URL_REF_MAX];
    bool valid = value != NULL && url_like(value, base, address, sizeof(address)) &&
                 (!ends_with_slash(key) || ends_with_slash(address));
    Rules *list = &map->imports;
    if (scope != NULL) {
        // "sort and normalize scopes": a prefix that is no URL is skipped.
        char prefix[OS64_URL_REF_MAX];
        if (!url_parse(scope, base, prefix, sizeof(prefix)))
            return true;
        Scope *s = scope_of(map, prefix, true);
        if (s == NULL)
            return false;
        list = &s->rules;
    }
    return rule_put(map, list, normalized, valid ? address : NULL, true);
}

// ── Merging ────────────────────────────────────────────────────────────

// Whether `rule_key` would decide `record`'s resolution in a scope that
// applies to it (HTML's merge, the scope half): the record's own specifier,
// or a prefix of it that a prefix rule could match.
static bool decides(const char *rule_key, const Record *record)
{
    return os64_streq(rule_key, record->specifier) ||
           (ends_with_slash(rule_key) && starts_with(record->specifier, rule_key) && record->prefixable);
}

// Whether a scope `prefix` applies to a script whose base URL is `base`.
static bool scope_applies(const char *prefix, const char *base)
{
    return os64_streq(prefix, base) || (ends_with_slash(prefix) && starts_with(base, prefix));
}

// HTML's "merge module specifier maps": `incoming`'s rules added to `into`,
// a rule `into` already has for the same key winning.
static bool merge_rules(yonder_import_map_t *map, Rules *into, const Rules *incoming)
{
    for (uint32_t i = 0; i < incoming->count; i++)
        if (!rule_put(map, into, incoming->rules[i].key, incoming->rules[i].url, false))
            return false;
    return true;
}

bool yonder_import_map_merge(yonder_import_map_t *window, yonder_import_map_t *incoming)
{
    if (window == NULL || incoming == NULL) {
        yonder_import_map_free(incoming);
        return false;
    }
    bool ok = true;
    for (uint32_t s = 0; ok && s < incoming->nscopes; s++) {
        Scope *scope = &incoming->scopes[s];
        // A scoped rule that would decide a resolution the window already
        // made, for a script the scope applies to, is dropped.
        for (uint32_t r = 0; r < window->nrecords; r++) {
            const Record *record = &window->records[r];
            if (!scope_applies(scope->prefix, record->base))
                continue;
            for (uint32_t i = scope->rules.count; i-- > 0;)
                if (decides(scope->rules.rules[i].key, record))
                    rule_remove(incoming, &scope->rules, i);
        }
        Scope *into = scope_of(window, scope->prefix, true);
        ok = into != NULL && merge_rules(window, &into->rules, &scope->rules);
    }
    // A top-level rule whose key begins with a specifier the window already
    // resolved is dropped, whatever base resolved it (HTML's merge, the
    // imports half, which asks only that).
    for (uint32_t r = 0; ok && r < window->nrecords; r++)
        for (uint32_t i = incoming->imports.count; i-- > 0;)
            if (starts_with(incoming->imports.rules[i].key, window->records[r].specifier))
                rule_remove(incoming, &incoming->imports, i);
    ok = ok && merge_rules(window, &window->imports, &incoming->imports);
    yonder_import_map_free(incoming);
    return ok;
}

// ── Resolving ──────────────────────────────────────────────────────────

static uint32_t record_hash(const char *base, const char *specifier)
{
    uint32_t h = 2166136261u;
    for (const char *p = base; *p != '\0'; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    h = (h ^ 0xff) * 16777619u;         // between the two, so ("ab","c") is not ("a","bc")
    for (const char *p = specifier; *p != '\0'; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    return h;
}

// The slot (base, specifier) is in, or the empty one it would take.
static uint32_t *record_slot(yonder_import_map_t *map, const char *base, const char *specifier)
{
    uint32_t mask = map->slotcap - 1;
    for (uint32_t i = record_hash(base, specifier) & mask;; i = (i + 1) & mask) {
        uint32_t *slot = &map->slots[i];
        if (*slot == 0)
            return slot;
        const Record *r = &map->records[*slot - 1];
        if (os64_streq(r->base, base) && os64_streq(r->specifier, specifier))
            return slot;
    }
}

// HTML's "add module to resolved module set". A record the set has already
// is not added twice, which changes no merge; past the set's bound, or on
// no memory, nothing is added.
static void record_add(yonder_import_map_t *map, const char *base, const char *specifier, bool prefixable)
{
    if (map->nrecords == YONDER_IMPORT_MAP_RECORDS_MAX)
        return;
    // The table stays at most half full, so a probe always ends.
    if ((map->nrecords + 1) * 2 > map->slotcap) {
        uint32_t cap = map->slotcap != 0 ? map->slotcap * 2 : 64;
        uint32_t *slots = os64_calloc(cap, sizeof(*slots));
        if (slots == NULL)
            return;
        os64_free(map->slots);
        map->slots = slots;
        map->slotcap = cap;
        for (uint32_t i = 0; i < map->nrecords; i++)
            *record_slot(map, map->records[i].base, map->records[i].specifier) = i + 1;
    }
    uint32_t *slot = record_slot(map, base, specifier);
    if (*slot != 0)
        return;
    if (map->nrecords == map->reccap) {
        uint32_t cap = map->reccap != 0 ? map->reccap * 2 : 64;
        Record *records = os64_realloc(map->records, cap * sizeof(*records));
        if (records == NULL)
            return;
        map->records = records;
        map->reccap = cap;
    }
    char *b = copy(base), *s = copy(specifier);
    if (b == NULL || s == NULL) {
        os64_free(b);
        os64_free(s);
        return;
    }
    map->records[map->nrecords++] = (Record){b, s, prefixable};
    *slot = map->nrecords;
}

typedef enum { MATCH_NONE, MATCH_FOUND, MATCH_BLOCKED, MATCH_FAILED } Match;

// HTML's "resolve an imports match": the first of `list`'s rules, in its
// descending order, whose key is `normalized` or a prefix of it a prefix
// rule may extend (`as_url` NULL for a bare specifier).
static Match imports_match(const Rules *list, const char *normalized, const char *as_url, char *out, size_t cap)
{
    for (uint32_t i = 0; i < list->count; i++) {
        const Rule *rule = &list->rules[i];
        if (os64_streq(rule->key, normalized)) {
            if (rule->url == NULL)
                return MATCH_BLOCKED;
            return os64_strcopy(out, cap, rule->url) < cap ? MATCH_FOUND : MATCH_FAILED;
        }
        if (ends_with_slash(rule->key) && starts_with(normalized, rule->key) &&
            (as_url == NULL || special(as_url))) {
            if (rule->url == NULL)
                return MATCH_BLOCKED;
            // What follows the prefix, parsed against the rule's address,
            // which must stay beneath it.
            const char *after = normalized + os64_strlen(rule->key);
            if (!url_parse(after, rule->url, out, cap))
                return os64_strlen(after) + os64_strlen(rule->url) >= cap ? MATCH_FAILED : MATCH_BLOCKED;
            return starts_with(out, rule->url) ? MATCH_FOUND : MATCH_BLOCKED;
        }
    }
    return MATCH_NONE;
}

yonder_specifier_t yonder_import_map_resolve(yonder_import_map_t *window, const char *base, const char *specifier,
                                             char *out, size_t cap)
{
    if (base == NULL || specifier == NULL || out == NULL || cap == 0)
        return YONDER_SPECIFIER_FAILED;
    // The base URL serialized, as the map's prefixes are.
    char serialized[OS64_URL_REF_MAX];
    if (!url_parse(base, NULL, serialized, sizeof(serialized)))
        return YONDER_SPECIFIER_FAILED;
    char as_url[OS64_URL_REF_MAX];
    bool url = url_like(specifier, serialized, as_url, sizeof(as_url));
    const char *normalized = url ? as_url : specifier;
    Match match = MATCH_NONE;
    if (window != NULL) {
        for (uint32_t s = 0; match == MATCH_NONE && s < window->nscopes; s++)
            if (scope_applies(window->scopes[s].prefix, serialized))
                match = imports_match(&window->scopes[s].rules, normalized, url ? as_url : NULL, out, cap);
        if (match == MATCH_NONE)
            match = imports_match(&window->imports, normalized, url ? as_url : NULL, out, cap);
    }
    if (match == MATCH_BLOCKED)
        return YONDER_SPECIFIER_BLOCKED;
    if (match == MATCH_FAILED)
        return YONDER_SPECIFIER_FAILED;
    if (match == MATCH_NONE) {
        if (!url)
            return YONDER_SPECIFIER_BARE;
        if (os64_strcopy(out, cap, as_url) >= cap)
            return YONDER_SPECIFIER_FAILED;
    }
    if (window != NULL)
        record_add(window, serialized, normalized, !url || special(as_url));
    return YONDER_SPECIFIER_OK;
}

// ── For the tests ──────────────────────────────────────────────────────

typedef struct {
    char *out;
    size_t cap, used;
} Dump;

static void dump_put(Dump *d, const char *a, const char *b, const char *c, const char *e)
{
    char line[4 * OS64_URL_REF_MAX];
    int n = c != NULL ? os64_snprintf(line, sizeof(line), "%s\t%s\t%s\t%s\n", a, b, c, e)
                      : os64_snprintf(line, sizeof(line), "%s\t%s\t%s\n", a, b, e);
    if (n < 0)
        return;
    if (d->used < d->cap)
        os64_strcopy(d->out + d->used, d->cap - d->used, line);
    d->used += (size_t)n;
}

size_t yonder_import_map_dump(const yonder_import_map_t *map, char *out, size_t cap)
{
    Dump d = {out, cap, 0};
    if (cap != 0)
        out[0] = '\0';
    if (map == NULL)
        return 0;
    for (uint32_t i = 0; i < map->imports.count; i++) {
        const Rule *r = &map->imports.rules[i];
        dump_put(&d, "imports", r->key, NULL, r->url != NULL ? r->url : "null");
    }
    for (uint32_t s = 0; s < map->nscopes; s++)
        for (uint32_t i = 0; i < map->scopes[s].rules.count; i++) {
            const Rule *r = &map->scopes[s].rules.rules[i];
            dump_put(&d, "scope", map->scopes[s].prefix, r->key, r->url != NULL ? r->url : "null");
        }
    return d.used;
}
