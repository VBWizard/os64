#ifndef GARB_SELECT_H
#define GARB_SELECT_H

// Selectors (GARB.md): Level 3, and Level 4's :is(), :where(), :not() with a
// list, and :has() — parsed from a rule's prelude, weighed, and matched
// against libhtml's tree. Pure: a tree and a selector in, an answer out.
//
// WHAT NEVER MATCHES, and why. The user-action pseudo-classes (:hover,
// :active, :focus, :focus-within, :focus-visible) and :target parse, and
// match nothing until a face restyles when they change (GARB.md § Booked).
// :visited matches nothing by design, as in every browser: a page must not
// learn where you have been from the colour of its own links.

#include <stdbool.h>
#include <stdint.h>

#include "garb/garb.h"
#include "html/html.h"

#pragma GCC visibility push(default)

typedef struct garb_selectors garb_selectors_t;

// The pseudo-element a selector is for: a rule for `p::before` styles the
// box generated before a `p`, not the `p`.
typedef enum {
    GARB_PSEUDO_NONE = 0,
    GARB_PSEUDO_BEFORE,
    GARB_PSEUDO_AFTER,
    GARB_PSEUDO_FIRST_LINE,
    GARB_PSEUDO_FIRST_LETTER,
    GARB_PSEUDO_MARKER,
    GARB_PSEUDO_PLACEHOLDER,
    GARB_PSEUDO_SELECTION,
    GARB_PSEUDO_BACKDROP,
} garb_pseudo_t;

// A selector list from a style rule's prelude, into `owner`'s arena. NULL
// when the list is invalid — and then, as the standard says, the whole
// rule is dropped: one selector the engine does not know takes its
// neighbours in the list with it. `quirks` is the document's mode, which
// makes class and id names match without regard to case.
garb_selectors_t *garb_selectors_parse(garb_parsed_t *owner, const garb_value_t *prelude,
                                       int32_t n, os64_html_quirks_t quirks);

int32_t garb_selectors_count(const garb_selectors_t *list);
// The i-th selector's specificity, (a, b, c) packed as a << 20 | b << 10 | c,
// each part capped at 1023 — so a larger packed value outranks a smaller.
uint32_t garb_selector_specificity(const garb_selectors_t *list, int32_t i);
garb_pseudo_t garb_selector_pseudo(const garb_selectors_t *list, int32_t i);
// Whether the i-th selector matches `element` (its pseudo-element aside).
bool garb_selector_matches(const garb_selectors_t *list, int32_t i,
                           const os64_html_node_t *element);

// What the rule hash files a selector under (GARB.md § The cost): the id
// of its rightmost compound, else a class, else the element name, else
// nothing (a selector every element must try). The strings belong to the
// selector.
typedef struct {
    const char *id, *class_name, *type;
    size_t id_len, class_len, type_len;
} garb_selector_key_t;

garb_selector_key_t garb_selector_key(const garb_selectors_t *list, int32_t i);

// §6 of CSS Syntax 3, An+B, from component values: `2n+1`, `odd`, `-n+3`.
// False when it is not one.
bool garb_an_plus_b(const garb_value_t *v, int32_t n, int32_t *a, int32_t *b);

#pragma GCC visibility pop

#endif
