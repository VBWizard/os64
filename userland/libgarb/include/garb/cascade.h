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
} garb_env_t;

// One sheet, in the order the page names it: a parse the caller owns (it
// must outlive the cascade), and the `media` its element gave, or NULL.
typedef struct {
    garb_parsed_t *sheet;
    const char *media;
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
    int32_t n;
} garb_style_t;

// Cascades `sheets` over `doc`. NULL on no memory only; a cascade that ran
// short partway says so in garb_cascade_incomplete, and what it holds is
// real.
garb_cascade_t *garb_cascade(const garb_sheet_in_t *sheets, int32_t n,
                             const os64_html_document_t *doc, garb_env_t env);
bool garb_cascade_incomplete(const garb_cascade_t *c);
// The viewport the cascade was judged against: what a `vw` or `vh` in its
// winners is a hundredth of.
garb_env_t garb_cascade_env(const garb_cascade_t *c);
// `element`'s winners; an element no author rule reached has none.
garb_style_t garb_style_for(const garb_cascade_t *c, const os64_html_node_t *element);
void garb_cascade_free(garb_cascade_t *c);

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
