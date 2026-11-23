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
// follows. `style_attribute`: an attribute selector naming `style`, so a
// write of an inline style can change what matches (the cascade adds what
// its declarations read, garb_cascade_reads_style_attribute).
//
// THE POSITION FORMS read an element's FRONT INDEX, its count from the
// start among its element siblings (first = 1) or among those of its type
// (:first-child, :nth-child, :first-of-type, :nth-of-type), or its BACK
// INDEX, the same from the end (:last-child, :nth-last-child and their
// -of-type kin); :only-child and :only-of-type read both. A bounded one
// answers alike at every index past some number: `front_child`,
// `front_type`, `back_child` and `back_type` are the largest such number
// any of them needs, 0 for none, so each one's answer is a function of
// min(index, need + 1) and the cascade can remember that number and compare
// it (cascade.c index_walk). An An+B with A > 1 never settles
// (`2n+1`), and one whose number passes GARB_INDEX_MAX is taken as if it
// did not: `front_unbounded` and `back_unbounded`, whose readers the
// cascade walks (garb_selectors_readers). An nth-child with `of S` is
// `filtered` instead. A form inside a :has() argument is none of these: a
// change of children asks the :has() again at its parent and up (the
// cascade's mark_has).
#define GARB_INDEX_MAX 16
typedef struct {
    bool following, front_unbounded, back_unbounded, parent_state, filtered;
    bool ancestors, ancestors_preceding, has_filtered, style_attribute;
    uint8_t front_child, front_type, back_child, back_type;
} garb_selector_dependencies_t;
garb_selector_dependencies_t garb_selectors_dependencies(const garb_selectors_t *list);
// What `:empty` answers at `element`: no element child and no text child
// with any characters; comments and the like do not count.
// The cascade asks it again at a change, so the two answers are one function.
bool garb_element_empty(const os64_html_node_t *element);
// Whether two elements are of one type as the -of-type pseudo-classes count
// it: the same namespace and the same name.
bool garb_element_same_type(const os64_html_node_t *a, const os64_html_node_t *b);

// AN ANCHORED :has(): a compound holding :has() that stands left of a
// combinator in its selector, directly or inside a functional pseudo-class
// that does (`.group:has(.x) .y`, `:is(.p:has(.x)) ~ .z`). When its match
// turns on an element, what changes is not that element's winners but its
// descendants' (`down`) or its following siblings' and theirs
// (`siblings`).
//
// Its FILTERS are the simples that turn most elements away for the price
// of a compare: a type, an id, a class, an attribute (never a pseudo-class:
// a `:not(.a)` is as conjunctive but passes nearly everything). They are
// the compound's own and those of `outer`, the compounds that enclose it
// through :not(), :is() and :where() and must match the same element: in
// `.c:not(:has(.x)) > p` the anchor `:has(.x)` is filtered by `.c`. A
// sibling branch of a list is not a filter (`:is(.x, :has(.y))` has only
// what stands outside the :is()), and nor is a compound the anchor's own
// complex names left of a combinator, which is another element. Where an
// enclosing filter fails the enclosing compound fails, whatever the :has()
// answers, so an anchor tested filters first loses nothing. At most
// GARB_ANCHOR_FILTERS enclosing compounds are kept; keeping fewer is weaker,
// never wrong. `bare` says it has no filter at all, so testing it at an
// element costs up to its :has() walk; only a pseudo-class tried before
// the :has() can turn the element away sooner (what a bare anchor costs a
// cascade's update, and its bound: cascade.c has_visit). `compound` and
// `outer` are the selector's own, valid while the list is.
#define GARB_ANCHOR_FILTERS 4
typedef struct {
    const garb_selectors_t *list;
    const void *compound;
    const void *outer[GARB_ANCHOR_FILTERS];
    int32_t nouter;
    bool down, siblings, bare;
} garb_anchor_t;
// Writes the list's anchored :has() compounds to out[0..cap) and answers
// how many the list holds; more than `cap` means the rest were not written.
int32_t garb_selectors_anchors(const garb_selectors_t *list, garb_anchor_t *out, int32_t cap);
// Whether the anchor's enclosing filters pass at `element` and its compound
// matches there: the filters and the compound's other simple selectors
// first, its :has() and the rest of its pseudo-classes only when they pass.
bool garb_anchor_matches(const garb_anchor_t *anchor, const os64_html_node_t *element);
// Whether the anchor's filters (its enclosing compounds' and its own: a
// type, an id, a class, an attribute) pass at `element`; its :has() and
// other pseudo-classes are not asked.
bool garb_anchor_filters_pass(const garb_anchor_t *anchor, const os64_html_node_t *element);

// A SUBJECT :has(): a compound holding :has() with nothing to its right,
// directly or inside the :not(), :is() or :where() of one that has none
// (`.p > x:has(.y)`, `:is(.a:has(.b), .c)`), whose match turning changes
// that element's own winners; and one inside the argument of :host(),
// ::slotted() or an nth-child's `of S` with nothing to its right there,
// which keeps its own filters only, those arguments naming another
// element. Recorded as an anchor is (garb_anchor_t,
// `down` and `siblings` false), with the same filters: `.p > x:has(R)` is
// filtered by x alone, since .p is another element. A compound inside a
// :not() keeps its own filters as well as the holder's: where one fails,
// the compound fails, so the :not() holds whatever the :has() answers.
// `bare` says it has no filter, so it can turn at any element.
int32_t garb_selectors_has_subjects(const garb_selectors_t *list, garb_anchor_t *out, int32_t cap);
// A SUBJECT :has() AND A CHANGE OF CHILDREN: whether adding or removing
// `roots[0..n)`, each with what is under it, may turn subject :has()
// `anchor` anywhere. A change of children can turn a :has() by bringing or
// taking an element its argument matches, so each argument's compounds are
// tested by their filters at the elements the change brought or took: the
// roots alone for an argument of one compound behind `>` or `~` (it
// matches a child or a following sibling of the element tested, never
// deeper), every element under them for any other. It can also turn one
// without bringing or taking a match, and then the answer is true whatever
// the elements: for an argument holding a position form, :empty, :enabled,
// :disabled, :read-only or :read-write (the last four read a fieldset's
// children), a :not() or a :has(), a sibling combinator between its
// compounds or inside a selector nested in one, or a compound with no
// filter; and for one behind `+` whenever an element is brought or taken,
// which changes the next element of the one before it. `stale(ctx,
// element)` names an element whose attributes the same batch changed,
// which cannot stand for what it was before the batch: true there too.
bool garb_has_subject_children_may_turn(const garb_anchor_t *anchor, const os64_html_node_t *const *roots,
                                        size_t n, bool (*stale)(void *, const os64_html_node_t *), void *ctx);

// A READER: a compound whose match at an element can turn at a change of
// the element's parent's children without touching the element: one
// holding a position form or :empty, or standing left of `+` or `~`. When
// its match at E may have turned, the elements whose winners can change
// are its REACH from E: E itself, for a subject; E's next element sibling
// (`+` alone), or E's following ones (`~`, and a `+` chain such as
// `a + b + c` or `a + b ~ c`); each with its subtree where a descendant or
// child combinator stands right of it. The reach is read from every
// combinator right of the compound, in its own selector and around the
// :is(), :where() or :not() it is nested in. A compound inside :host() or
// ::slotted() reaches its element's subtree, shadow tree included, and one
// holding ::slotted() reaches its slot's, which carries what is assigned to
// it.
//
// Its FILTERS are an anchor's (§ AN ANCHORED :has()): its own type, id,
// classes and attributes, and those of the compounds enclosing it through
// :is(), :where() and :not(). Each alternative of an :is() or :where() is a
// reader of its own, with the holder's filters and what the holder reads,
// so `:is(.a, .b) + p` is two readers and not one with no filter. Inside a
// :not() a subject's own filters are not kept (`own` false), and what it
// reads there is `untested`: the reader passes every element for it. A
// compound left of a combinator anywhere names another element and keeps
// its own filters alone. `bare`: no filter at all.
//
// `reads` says what turns its match (GARB_READS_*). An nth-child with
// `of S` and what is inside a :has() argument are no readers: `filtered`
// and the anchors carry those.
enum {
    GARB_READS_FRONT = 1 << 0,          // a bounded front form, among element siblings
    GARB_READS_FRONT_TYPE = 1 << 1,     // a bounded front form, among those of its type
    GARB_READS_BACK = 1 << 2,
    GARB_READS_BACK_TYPE = 1 << 3,
    GARB_READS_FRONT_ANY = 1 << 4,      // an unbounded front form, either count
    GARB_READS_BACK_ANY = 1 << 5,
    GARB_READS_EMPTY = 1 << 6,
    GARB_READS_NEIGHBOURS = 1 << 7,     // its reach takes siblings: which elements stand beside it
};
typedef enum { GARB_REACH_SELF, GARB_REACH_NEXT, GARB_REACH_FOLLOWING, GARB_REACH_CHAIN } garb_reach_t;
typedef struct {
    const garb_selectors_t *list;
    const void *compound;
    const void *outer[GARB_ANCHOR_FILTERS];
    int32_t nouter;
    uint8_t reads, untested, reach;
    bool own, down, bare;
} garb_reader_t;
// Writes the list's readers to out[0..cap) and answers how many the list
// holds; more than `cap` means the rest were not written.
int32_t garb_selectors_readers(const garb_selectors_t *list, garb_reader_t *out, int32_t cap);
// Whether the reader's filters pass at `element` (true for a bare one).
bool garb_reader_filters_pass(const garb_reader_t *reader, const os64_html_node_t *element);

// A STYLE READER: a compound that tests the `style` attribute
// (`[style*="display: none"]`), so an inline style write on an element it
// may match may change what matches (BRIEF_LAYOUT_NEEDS.md N2a). A compound
// is a conjunction: where an element fails one of its exact tests it cannot
// match, whatever the element's style says. The exact tests are a type, an
// id, a class and an attribute other than `style`, each a function of the
// element's own state; every pseudo-class, and each :not(), :is(), :where()
// or :has() the test is not held in, is taken as passing, which only
// widens the readers' reach, the safe direction. `bare` says it narrows to
// no element: no exact test, or a compound it does not fully understand
// (one holding :host or ::slotted, or inside their arguments). A test held
// inside a pseudo-class's argument is recorded as the compound there that
// holds it, alone. `compound` is the selector's own, valid while the list
// is.
typedef struct {
    const garb_selectors_t *list;
    const void *compound;
    bool bare;
} garb_style_reader_t;
// Writes the list's style readers to out[0..cap) and answers how many the
// list holds; more than `cap` means the rest were not written.
int32_t garb_selectors_style_readers(const garb_selectors_t *list, garb_style_reader_t *out, int32_t cap);
// Whether the reader may match `element` for some value of its style: bare,
// or its exact tests pass there, tried by the matchers the cascade matches
// with.
bool garb_style_reader_may_match(const garb_style_reader_t *reader, const os64_html_node_t *element);

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
