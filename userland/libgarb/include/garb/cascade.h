#ifndef GARB_CASCADE_H
#define GARB_CASCADE_H

// The cascade (GARB.md, G2c): the page's sheets and its `style`
// attributes, judged against the tree and the viewport, into each
// element's AUTHOR-origin winners — the declared value of every property
// some author rule set, `var()` and `env()` already replaced. libflow
// applies them after the Rendering chapter and the presentational hints,
// and computes them.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "garb/garb.h"
#include "garb/values.h"
#include "html/html.h"

#pragma GCC visibility push(default)

// What a media query and a viewport unit are judged against.
typedef struct {
    double width, height;           // the viewport, in CSS pixels
    // The person's colour scheme: what `prefers-color-scheme` answers,
    // `dark` when true and `light` when false.
    bool dark;
} garb_env_t;

// An `@import` (Cascade 4 § 2.1): the address as written — resolved
// against the IMPORTING sheet's address by whoever fetches it — and the
// conditions on it, as component values pointing into the importing
// sheet's parse.
typedef struct {
    const char *url;
    size_t len;
    const garb_value_t *media;      // a media query list; none holds always
    int32_t nmedia;
    bool has_supports;              // `supports(…)`: its argument
    const garb_value_t *supports;
    int32_t nsupports;
} garb_import_t;

// The @imports of `sheet` that count, in order: those written before any
// rule but @charset and a @layer statement, each with an address. Answers
// how many there are and writes at most `cap` of them.
int32_t garb_sheet_imports(const garb_parsed_t *sheet, garb_import_t *out, int32_t cap);

// One sheet, in cascade order: a parse the caller owns (it must outlive
// the cascade), and the `media` its element gave, or NULL. An imported
// sheet comes BEFORE the sheet importing it — its rules are the importer's
// first — and says so: `via` is its @import, `parent` the importer's index
// in the same array. It applies when its @import's conditions hold and the
// importer applies. `via` is NULL for a sheet the page names itself, and
// is read only during garb_cascade.
typedef struct {
    garb_parsed_t *sheet;
    const char *media;
    const garb_import_t *via;
    int32_t parent;
} garb_sheet_in_t;

// A `style` element's sheet: its child text content (HTML § 4.2.6), which
// libhtml may keep as more than one text node, parsed into `out`. The parse
// keeps no pointer into the tree: one text node is parsed where it lies —
// every string a parse keeps is its arena's copy, never its input's — and
// several are joined in a buffer freed once the parse returns.
garb_status_t garb_parse_style_element(const os64_html_node_t *style, garb_parsed_t *out);

typedef struct garb_cascade garb_cascade_t;

// An element's author-origin winners: one per property, in no particular
// order. A CSS-wide keyword (`inherit`, `initial`, `unset`, `revert`) is
// a winner like any other; libflow gives it its meaning.
typedef struct {
    const garb_set_t *sets;
    // For each winner, the index in garb_cascade's `sheets` of the sheet
    // it was written in, or -1 for the element's `style` attribute: what a
    // relative url() in it is resolved against (CSS Values 4 § 4.5.1).
    const int32_t *sheet;
    int32_t n;
} garb_style_t;

// Cascades `sheets` over `doc`, pinning document strings until free. NULL
// on no memory or pin exhaustion; a partial cascade says so in
// garb_cascade_incomplete. The input sheets outlive the cascade.
garb_cascade_t *garb_cascade(const garb_sheet_in_t *sheets, int32_t n,
                             const os64_html_document_t *doc, garb_env_t env);
bool garb_cascade_incomplete(const garb_cascade_t *c);
// The viewport the cascade was judged against: what a `vw` or `vh` in its
// winners is a hundredth of.
garb_env_t garb_cascade_env(const garb_cascade_t *c);
// `element`'s winners; an element no author rule reached has none.
garb_style_t garb_style_for(const garb_cascade_t *c, const os64_html_node_t *element);
void garb_cascade_free(garb_cascade_t *c);

// What the build passed over because this library does not read it — a
// property it has no reader for, an at-rule it does not keep, a function
// no reader accepts in a declaration it dropped, the family of an
// @font-face (no face is fetched), and a VALUE: `<property>: <words>` for
// a declaration of a property it reads, dropped though its value is bare
// words, or read and laid out as something else (an approximation) — each
// name once, lower case, with how often it was met. A prefixed name
// (`-webkit-…`) is not counted: one engine's dialect is passed over by
// every other. A value merely written wrong, one holding a number or a
// string, is not either. The names live as long as the cascade. Copies up to
// `cap` into `out` and answers how many there are; `lost` (may be NULL)
// is how many met no room. At most GARB_SKIPS_MAX are kept.
typedef enum { GARB_SKIP_PROPERTY, GARB_SKIP_AT_RULE, GARB_SKIP_FUNCTION, GARB_SKIP_FONT,
               GARB_SKIP_VALUE } garb_skip_kind_t;
typedef struct {
    garb_skip_kind_t kind;
    const char *name;
    size_t len;
    uint32_t count;
} garb_skip_t;
#define GARB_SKIPS_MAX 256
int32_t garb_cascade_skips(const garb_cascade_t *c, garb_skip_t *out, int32_t cap, uint32_t *lost);

// A media query list (Media Queries 4) against `env`: a `media` attribute,
// or an @media rule's prelude. An empty list is true; one that will not
// parse is false (`not all`).
bool garb_media_matches(const garb_value_t *v, int32_t n, garb_env_t env);
bool garb_media_text_matches(const char *text, garb_env_t env);

// Every element's winners as text, in document order: `tag#id.class:
// prop: value; ...`, for the harness and a probe. Like snprintf.
size_t garb_cascade_dump(const garb_cascade_t *c, const os64_html_document_t *doc, char *out,
                         size_t cap);

#pragma GCC visibility pop

#endif
