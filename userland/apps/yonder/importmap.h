#ifndef YONDER_IMPORTMAP_H
#define YONDER_IMPORTMAP_H

// THE WINDOW'S IMPORT MAP (HTML § 8.1.5; MODERN_WEB.md § Import maps, as
// built): what a module specifier means. A page's `<script type=importmap>` is parsed as
// JSON by libdom (os64_dom_import_map), whose walk hands each entry here to
// be normalized against the script's base URL; the parse result is then
// merged into the window's map, which keeps every rule it took and does
// not change what a specifier already resolved to (the resolved module
// set, within its bound); and every specifier a module or a dynamic import
// names resolves through it.
//
// ONE SPELLING FOR AN ADDRESS. A rule's key and a specifier meet only when
// they are spelled alike, so every address here is parsed and serialized
// by libpage's rule (os64_page_resolve_in), the rule a page's links and its
// scripts' URLs are: the map's keys, its addresses, the base URLs it is
// asked against, and every specifier it resolves.
//
// Integrity is not kept: yonder checks no Subresource Integrity (DEBTS).
//
// Pure: no window, no network, no engine.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct yonder_import_map yonder_import_map_t;

// The most rules one map holds, all its scopes' together, and the most
// resolutions the window's set records. An entry past the first bound is
// refused (yonder_import_map_add answers false, and the page's map fails
// to parse), and a merge that would pass it keeps what fit; the
// resolutions past the second are not recorded, which only a later map's
// merge could notice.
#define YONDER_IMPORT_MAP_RULES_MAX 16384
#define YONDER_IMPORT_MAP_RECORDS_MAX 65536

// An empty map: the window's before any import map, or a parse result
// before its first entry. NULL on no memory.
yonder_import_map_t *yonder_import_map_new(void);
void yonder_import_map_free(yonder_import_map_t *map);

// One entry of a parsed map, as libdom's walk hands it ("sort and normalize
// a module specifier map" and "normalize a specifier key", for one entry):
// `scope` NULL for "imports", else the scope's key as the JSON wrote it;
// `value` NULL when the JSON value was not a string. `base` is the base
// URL the import map script was prepared with. An entry whose key
// normalizes to nothing (an empty key, a scope that is no URL) is skipped;
// an address that is no URL-like specifier, or that lacks the trailing
// slash its key has, is a null rule, which blocks its key. A later entry
// for the same key replaces the earlier, as JSON's own duplicates do.
// False on no memory, or past YONDER_IMPORT_MAP_RULES_MAX.
bool yonder_import_map_add(yonder_import_map_t *map, const char *base, const char *scope, const char *key,
                           const char *value);

// HTML's "merge existing and new import maps": `incoming`, a parse result,
// merged into `window`, the window's map. A rule `window` already has for
// the same key in the same scope wins, and a rule that would change what a
// specifier in the window's resolved module set already resolved to is
// dropped. `incoming` is consumed whatever the answer. False on no memory:
// what was merged before it ran out is kept.
bool yonder_import_map_merge(yonder_import_map_t *window, yonder_import_map_t *incoming);

typedef enum {
    YONDER_SPECIFIER_OK,
    // No rule maps it and it is no URL-like specifier (a bare name): the
    // TypeError HTML throws.
    YONDER_SPECIFIER_BARE,
    // A null rule matched it, or a prefix rule's address took it nowhere
    // or above the prefix: the TypeError HTML throws, with no fallback.
    YONDER_SPECIFIER_BLOCKED,
    // It or its answer is longer than `cap`, or memory ran out.
    YONDER_SPECIFIER_FAILED,
} yonder_specifier_t;

// HTML's "resolve a module specifier": what `specifier` means to a script
// whose base URL is `base`, through `window`'s map, written to `out`. A
// resolution is added to the window's resolved module set. `window` NULL is
// the empty map, with no set: a resolution then is a plain URL's.
yonder_specifier_t yonder_import_map_resolve(yonder_import_map_t *window, const char *base, const char *specifier,
                                             char *out, size_t cap);

// The map as text, its rules in the order resolution reads them: one line
// `imports\t<key>\t<address>` per rule, then `scope\t<prefix>\t<key>\t
// <address>` for each scope's, an address `null` for a null rule (a scope
// with no rules writes nothing). NUL-terminated and truncated to `cap`; the
// length it needed. For the host tests.
size_t yonder_import_map_dump(const yonder_import_map_t *map, char *out, size_t cap);

#endif
