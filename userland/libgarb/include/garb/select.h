#ifndef GARB_SELECT_H
#define GARB_SELECT_H

// Selectors (GARB.md): Level 3, and Level 4's :is(), :where(), :not() with a
// list, and :has(), and CSS Scoping's :host, :host() and ::slotted() —
// parsed from a rule's prelude, weighed, and matched against libhtml's
// tree. Pure: a tree and a selector in, an answer out.
//
// SHADOW TREES (SHADOW_DOM.md § 6). A combinator never leaves the tree its
// element is in, with one exception: a shadow root's child has the root's
// HOST above it, featureless, matched only by a compound made of nothing
// but :host and :host(S) (S tested against the host as an ordinary
// element), with nothing above it or beside it. So `:host(.open) .panel`
// matches inside a component and `.page .panel` there never reaches the
// page. Which sheets apply in which tree is the cascade's question, not
// this file's.
//
// WHAT NEVER MATCHES, and why. The user-action pseudo-classes (:hover,
// :active, :focus, :focus-within, :focus-visible), :target and
// :indeterminate parse, and match nothing until a face restyles when they
// change (GARB.md § Booked); only the user-action ones may follow a
// pseudo-element. :checked reads the tree's `checked` attribute, so a
// person's own tick, which lives in libpage's model, restyles nothing
// until then either. :visited matches nothing by design, as in every
// browser: a page must not learn where you have been from the colour of
// its own links.

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
    // ::slotted(S): an element assigned to the slot the rest of the
    // selector matches, and matching S (garb_selector_matches_slotted).
    GARB_PSEUDO_SLOTTED,
} garb_pseudo_t;

// A selector list from a style rule's prelude, into `owner`'s arena. NULL
// when the list is invalid — and then, as the standard says, the whole
// rule is dropped: one selector the engine does not know takes its
// neighbours in the list with it. NULL too when memory ran out partway,
// and then `owner` is marked incomplete: a list read short is neither
// valid nor invalid. `quirks` is the document's mode, which
// makes class and id names match without regard to case. `modern_off` is
// the Modern Web switch, off: `:defined` then matches every element.
garb_selectors_t *garb_selectors_parse(garb_parsed_t *owner, const garb_value_t *prelude,
                                       int32_t n, os64_html_quirks_t quirks, bool modern_off);

// Dependencies include nested functional selectors. `ancestors`: a :has()
// in its rule's subject, whose match a change can turn on any ancestor of
// the change (its argument looks down from the element it tests);
// `ancestors_preceding`: a :has() whose argument begins with `+` or `~`,
// whose match a change can turn on a preceding sibling of the change or of
// one of its ancestors as well. A :has() left of a combinator is an anchor
// (garb_selectors_anchors) rather than an `ancestors` dependency;
// `ancestors_preceding` counts both kinds. `has_filtered`: a :has() inside
// :nth-child(… of S) or :nth-last-child(… of S), whose turning on one
// element moves which of its siblings the count picks, which neither kind
// follows.
typedef struct {
    bool following, position_front, position_back, parent_state, filtered;
    bool ancestors, ancestors_preceding, has_filtered;
} garb_selector_dependencies_t;
garb_selector_dependencies_t garb_selectors_dependencies(const garb_selectors_t *list);

// AN ANCHORED :has(): a compound holding :has() that stands left of a
// combinator in its selector, directly or inside a functional pseudo-class
// that does (`.group:has(.x) .y`, `:is(.p:has(.x)) ~ .z`). When its match
// turns on an element, what changes is not that element's winners but its
// descendants' (`down`) or its following siblings' and theirs
// (`siblings`). `bare` says the compound has no type, id, class or
// attribute to turn most elements away before its :has() is tried, so
// testing it on every element costs a subtree walk each. `compound` is the
// selector's own, valid while the list is.
typedef struct {
    const garb_selectors_t *list;
    const void *compound;
    bool down, siblings, bare;
} garb_anchor_t;
// Writes the list's anchored :has() compounds to out[0..cap) and answers
// how many the list holds; more than `cap` means the rest were not written.
int32_t garb_selectors_anchors(const garb_selectors_t *list, garb_anchor_t *out, int32_t cap);
// Whether the anchor's compound matches `element`: its other simple
// selectors first, its :has() and the rest of its pseudo-classes only when
// they pass.
bool garb_anchor_matches(const garb_anchor_t *anchor, const os64_html_node_t *element);

int32_t garb_selectors_count(const garb_selectors_t *list);
// The i-th selector's specificity, (a, b, c) packed as a << 20 | b << 10 | c,
// each part capped at 1023 — so a larger packed value outranks a smaller.
uint32_t garb_selector_specificity(const garb_selectors_t *list, int32_t i);
garb_pseudo_t garb_selector_pseudo(const garb_selectors_t *list, int32_t i);
// Whether the i-th selector matches `element` (its pseudo-element aside).
bool garb_selector_matches(const garb_selectors_t *list, int32_t i,
                           const os64_html_node_t *element);
// The same, with `scope` as the scoping root `:scope` names (a script's
// element.querySelector). NULL scopes to the document, where `:scope` is
// `:root`, as a style sheet's selectors are.
bool garb_selector_matches_in(const garb_selectors_t *list, int32_t i,
                              const os64_html_node_t *element, const os64_html_node_t *scope);
// Whether the i-th selector matches shadow host `host` as its shadow tree's
// selectors see it: featureless, so only a selector whose subject compound
// is all :host and :host(S) can.
bool garb_selector_matches_host(const garb_selectors_t *list, int32_t i,
                                const os64_html_node_t *host);
// Whether the i-th selector, ending in ::slotted(S), matches `element` through
// `slot`: the selector matches the slot and S matches the element. Whether
// the element is assigned to the slot is the caller's to know.
bool garb_selector_matches_slotted(const garb_selectors_t *list, int32_t i,
                                   const os64_html_node_t *slot, const os64_html_node_t *element);
// Whether the i-th selector's subject compound holds :host or :host(S): a
// rule for a shadow host, consulted when its shadow tree's sheets style it.
bool garb_selector_names_host(const garb_selectors_t *list, int32_t i);

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
