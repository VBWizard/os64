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
#include "garb/select.h"
#include "garb/values.h"
#include "html/html.h"

#pragma GCC visibility push(default)

// What a media query and a viewport unit are judged against, and which
// properties the cascade reads.
typedef struct {
    double width, height;           // the viewport, in CSS pixels
    // The person's colour scheme: what `prefers-color-scheme` answers,
    // `dark` when true and `light` when false.
    bool dark;
    // Whether the person asked for less motion: what
    // `prefers-reduced-motion` answers, `reduce` when true and
    // `no-preference` when false.
    bool reduced_motion;
    // Whether the document's scripts run: what `scripting` answers,
    // `enabled` when true and `none` when false.
    bool scripting;
    // The output device, the screen, in CSS pixels: what device-width,
    // device-height and device-aspect-ratio answer. Either 0 answers them
    // from the viewport, for a host that knows no screen.
    double device_width, device_height;
    // The device pixels a CSS pixel is drawn with (the page's zoom): what
    // `resolution` and the WebKit pixel ratio answer, in dppx. 0 is 1.
    double resolution;
    // Whether the Modern Web arc's properties are taken away
    // (garb_reading_t): the cascade reads every declaration, and every
    // `@supports` condition, with this answer, and records a marked
    // property it then cannot read as it records any other
    // (garb_cascade_skips). Clear, the default an environment built field
    // by field gets, knows them.
    bool modern_off;
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
// the cascade, which never writes to it), and the `media` its element
// gave, or NULL. An imported sheet comes BEFORE the sheet importing it —
// its rules are the importer's first — and says so: `via` is its @import,
// `parent` the importer's index in the same array. It applies when its
// @import's conditions hold and the importer applies. `via` is NULL for a
// sheet the page names itself, and is read only during garb_cascade.
typedef struct {
    const garb_parsed_t *sheet;
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
    // For each winner, whether it was `!important`: an animated value
    // overrides a normal declaration and never an important one (Cascade
    // 5's order of origins).
    const bool *important;
    int32_t n;
} garb_style_t;

// Cascades `sheets` over `doc`, pinning document strings until free. NULL
// on no memory or pin exhaustion; a partial cascade says so in
// garb_cascade_incomplete. The input sheets outlive the cascade, which
// only reads them: what it makes of their rules' blocks, selectors and
// values is its own, in arenas freed with it (GARB.md § The cost), so a
// sheet is the same after any number of cascades as before the first.
garb_cascade_t *garb_cascade(const garb_sheet_in_t *sheets, int32_t n,
                             const os64_html_document_t *doc, garb_env_t env);
// A tree's style sheets (SHADOW_DOM.md § 6): `root` — the document node, or
// a shadow root — and the indices in garb_cascade's `sheets` of the sheets
// that apply in that tree. An imported sheet applies wherever its importer
// does, listed or not, and an index naming no sheet names nothing. The list
// says WHICH sheets apply, not in what order: within a tree, rules rank in
// the order of `sheets`. A tree no scope names has no author sheet; two
// scopes naming one root apply the sheets of both.
typedef struct {
    const os64_html_node_t *root;
    const int32_t *sheets;
    int32_t n;
} garb_scope_in_t;

// Independent arena caps for an owner with a smaller working allowance.
// attribute_bytes bounds each retained style-attribute parse and the temporary
// presentation-value parse. attributes bounds their retained count. Scratch,
// slot tables and selector/match arrays are separate; these are not a unified
// allocator budget. Zero limits are literal. Input sheets remain caller-owned.
// `clock`, when not NULL, is the caller's clock (microseconds, or any unit
// it likes): the cascade then keeps the caller's measure
// (garb_cascade_build_cost, garb_cascade_broad_trigger), and without it
// counts nothing for them.
// `read_from`, when not NULL, is a READ this build may take (THE KEPT
// READ, below; garb_cascade_read_hold): everything a build makes before it
// touches an element, shared when the key is equal, read again when it is
// not. The build takes its own reference when it takes the read, and keeps
// no pointer otherwise. `check_read` hashes a read when it is made and
// again whenever it is taken, so a write into a read already made is
// caught (garb_cascade_read_written).
typedef struct garb_read garb_read_t;
typedef struct {
    size_t state_bytes, read_bytes, attribute_bytes, attributes;
    uint64_t (*clock)(void);
    const garb_read_t *read_from;
    bool check_read;
} garb_cascade_limits_t;
garb_cascade_t *garb_cascade_limited(const garb_sheet_in_t *, int32_t,
                                     const os64_html_document_t *, garb_env_t,
                                     garb_cascade_limits_t);
// A cascade over the document's tree and every connected shadow tree, each
// tree styled by the sheets its scope names (SHADOW_DOM.md § 6): matched in
// that tree, a shadow host reached by `:host` from its own tree's sheets,
// a slotted element by `::slotted()` from each slot's, and declarations
// from different trees ranked by context before specificity. `limits` NULL
// is garb_cascade's. The scopes are copied; the doors without them pass
// none, and with none every sheet applies in the document's tree.
garb_cascade_t *garb_cascade_scoped(const garb_sheet_in_t *sheets, int32_t n,
                                    const garb_scope_in_t *scopes, int32_t nscopes,
                                    const os64_html_document_t *doc, garb_env_t env,
                                    const garb_cascade_limits_t *limits);
// The default cap on what a cascade reads of its sheets together: their rules'
// blocks, selectors and values go into one arena held to this, beside the
// cascade's own state (held to GARB_ARENA_MAX), and a cascade that reaches
// it comes up short as any short read does (garb_cascade_incomplete).
// Twice a parse's arena, because reading a sheet costs up to about twice
// its parse (GARB.md § The cost): so one cascade reads whole any one sheet
// a parse holds, but the very densest, and beside a big one the rest of an
// ordinary page — a news page's 1 MB sheet reads into 14.3 MiB, a
// Bootstrap-shaped sheet about 21.5 bytes a byte — while a page naming
// many big sheets holds no more than this of reads in any one cascade.
#define GARB_CASCADE_READ_MAX ((size_t)2 * GARB_ARENA_MAX)
bool garb_cascade_incomplete(const garb_cascade_t *c);
// Sheets are immutable while a cascade uses them. Match their identity,
// order, media/import conditions and the complete environment before updating.
// The sheet objects themselves (not just their arenas) must stay alive.
bool garb_cascade_matches(const garb_cascade_t *cascade, const garb_sheet_in_t *sheets,
                          int32_t count, garb_env_t env);
// The same, and the scopes as garb_cascade_scoped was given them: the same
// roots in the same order, each naming the same indices in the same order.
bool garb_cascade_matches_scoped(const garb_cascade_t *cascade, const garb_sheet_in_t *sheets,
                                 int32_t count, const garb_scope_in_t *scopes, int32_t nscopes,
                                 garb_env_t env);
// One reference per owner; free releases it. Updates are shared and callers
// serialize them with reads. Retaining does not retain the input sheets.
bool garb_cascade_retain(garb_cascade_t *cascade);
/* Consume this cascade's native change reader and update affected winners.
 * Sheets and environment must be unchanged. False requests a fresh cascade:
 * lost history, selectors it cannot follow, broad dirty sets, arena renewal
 * or refusal; garb_cascade_update_refusal names which.
 * On failure, previously returned values remain alive but no new queries may
 * use this cascade until it is replaced. On success, old values also remain
 * alive until free, for displayed animation records borrowing keyframes.
 * Reader storage is optional: its absence disables updates, not full builds.
 * The caller serializes updates with queries on its window thread. */
bool garb_cascade_update(garb_cascade_t *c);
// The cascade brought to the document's version with nothing restyled, for
// a caller that has judged every change since garb_cascade_version to leave
// what it styled as it is: each one a write of an element's style attribute
// whose winners are unchanged (garb_cascade_inline_changes lists none) and
// which no selector can read (garb_cascade_style_may_read false). The
// cascade keeps its parse of those attributes, whose winners are the same.
// This checks only that each change is a style write on an element the
// cascade styled; the judging is the caller's. The change log is read
// whatever the answer, so false leaves a fresh cascade owed, as a failed
// update does, and garb_cascade_update_refusal says why until the next
// update, which answers GARB_UPDATE_FAILED_BEFORE. Its generation and
// its count of updates do not move: no value was derived.
bool garb_cascade_advance(garb_cascade_t *c);

// Why the latest garb_cascade_update or garb_cascade_advance answered false;
// GARB_UPDATE_DONE before any and after one that answered true.
typedef enum {
    GARB_UPDATE_DONE,
    GARB_UPDATE_INCOMPLETE,     // the cascade came up short, when built or since
    GARB_UPDATE_FAILED_BEFORE,  // an earlier update failed: a fresh cascade is owed
    GARB_UPDATE_NO_READER,      // no change reader: there was no memory for one
    GARB_UPDATE_COUNTED_HAS,    // a :has() inside :nth-child(… of S) (has_filtered)
    GARB_UPDATE_SHADOW_ROOT,    // the document's first shadow root arrived
    GARB_UPDATE_CAP,            // as many updates as one cascade takes
    GARB_UPDATE_RETAINED,       // what updates added reached the retained budget
    GARB_UPDATE_ATTRIBUTES,     // style attributes reparsed reached it
    GARB_UPDATE_HISTORY,        // the document's change log could not be read
    GARB_UPDATE_BODY_HINT,      // a <body> presentational attribute changed
    GARB_UPDATE_SLOT_LEFT,      // a <slot> left a tree
    GARB_UPDATE_BROAD,          // the changes reached more than half the elements
    GARB_UPDATE_SHORT,          // a recompute came up short
    GARB_UPDATE_NO_MEMORY,      // a record, the dirty list or the document's pin had none
    GARB_UPDATE_ADVANCE,        // garb_cascade_advance met a change that is no style write
    GARB_UPDATE_REASONS
} garb_update_refusal_t;
garb_update_refusal_t garb_cascade_update_refusal(const garb_cascade_t *c);
// A reason's name for a record ("broad", "counted has"); "?" past the enum.
const char *garb_update_refusal_name(garb_update_refusal_t reason);
// Whether this cascade keeps a change reader for incremental updates. False
// lets hosts skip speculative geometry snapshots whose generation cannot
// survive a DOM change. True is not a promise that an update will succeed:
// history loss, dirty breadth and storage budgets still require fallback.
bool garb_cascade_tracks_changes(const garb_cascade_t *c);
// How many anchored :has() compounds (garb_anchor_t) the cascade's rules
// hold, and how many of those are bare. While the cascade takes updates,
// each is a bit in every element's record, asked at every element it
// styles; a bare one costs up to a walk each time it is asked (its bound:
// cascade.c has_visit), and never keeps the cascade from updates.
void garb_cascade_anchors(const garb_cascade_t *c, int32_t *anchors, int32_t *bare);
// What the bare anchors cost the latest update that read the document's
// changes: how many times one was asked again at an element a change could
// have turned it on (not its asks at restyled elements), how many of those
// answers turned, and how many elements the turns newly marked for
// restyling (a reach the change had already marked adds nothing, though it
// is restyled all the same). Zeros before any update.
typedef struct {
    uint64_t tested, turned, marked;
} garb_bare_work_t;
garb_bare_work_t garb_cascade_bare_work(const garb_cascade_t *c);
// What the back forms (:last-child and its kin, select.h § THE BACK FORMS)
// cost the latest garb_cascade_update: the walks it made, one for each
// element whose children changed (a shadow root or the document, which
// keep no count of their own, one for each change of their children), the
// children those walks counted, and those whose remembered back index
// turned and were marked. Zeros before any update, after one that read no
// changes or was refused before reading them, for rules with no bounded
// back form, and for rules whose `of S` forms mark every child anyway; a
// count made before a refusal partway stays.
typedef struct {
    uint64_t walks, walked, turned;
} garb_back_work_t;
garb_back_work_t garb_cascade_back_work(const garb_cascade_t *c);
// What the subject :has() filter (select.h § A SUBJECT :has()) did in the
// latest garb_cascade_update: the elements a change's :has() climb visited
// while the rules hold a subject :has(), how many it let through to be
// marked alone for their own match (those already marked counted too, so
// this can pass the update's :has() marks), how many it turned away
// because no subject compound's filters passed them (`turned_away`), and
// how many because the compounds whose filters passed could not be turned
// by the batch's changes so far (`turned_away_change`: a change of children
// that brought and took nothing a compound's argument matches,
// garb_has_subject_children_may_turn); and, of the rules, how many subject
// :has() compounds there are and how many are bare (no filter, so the
// changes alone decide). The visits are zeros as garb_cascade_back_work's
// are.
typedef struct {
    uint64_t visited, marked, turned_away, turned_away_change, entries, bare;
} garb_has_subject_work_t;
garb_has_subject_work_t garb_cascade_has_subject_work(const garb_cascade_t *c);
// What the readers (select.h § A READER) did in the latest
// garb_cascade_update, by what asked them: a child whose front index
// turned, one whose back index turned, the siblings an unbounded front
// form's walk and an unbounded back form's walk passed over, an :empty that
// turned, an element brought or taken (`~` and the `+` chains), a pair of
// siblings broken or made (`+`), and a change of an attribute by its keys
// (an attribute other than these three, `class`, `id`, `style`: select.h
// § A READER's keys). For each, the readers asked, those
// whose filters passed, and the asks that marked a reach (one reader or
// more passed). With the bounded front indexes counted again as
// garb_cascade_back_work counts the back ones (`front_walks`,
// `front_walked`, `front_turned`), the unbounded walks made and the
// siblings they passed over; and, of the rules, the readers and the bare
// ones. Zeros as garb_cascade_back_work's are.
typedef enum {
    GARB_ASKED_FRONT, GARB_ASKED_BACK, GARB_ASKED_FRONT_WALK, GARB_ASKED_BACK_WALK, GARB_ASKED_EMPTY,
    GARB_ASKED_ADDED_REMOVED, GARB_ASKED_PAIRS, GARB_ASKED_ATTRIBUTE, GARB_ASKED_CLASS, GARB_ASKED_ID,
    GARB_ASKED_STYLE, GARB_ASKERS
} garb_asker_t;
typedef struct {
    uint64_t asked[GARB_ASKERS], passed[GARB_ASKERS], reached[GARB_ASKERS];
    uint64_t front_walks, front_walked, front_turned, walks, walked;
    int32_t readers, bare;
} garb_reader_work_t;
garb_reader_work_t garb_cascade_reader_work(const garb_cascade_t *c);
const char *garb_asker_name(garb_asker_t asker);

// THE CALLER'S MEASURE, kept only when the cascade was made with a clock
// (garb_cascade_limits_t.clock): a diagnostic, and with no clock nothing is
// counted. What the fresh build cost by part, on the caller's clock:
// reading the sheets (their rules, selectors and values into this
// cascade's reading of them, the index, the dependencies, the anchors and
// the keyframes), sizing the element table, and matching every element;
// with the rules read and the elements matched.
typedef struct {
    uint64_t sheets, table, matching;
    uint32_t rules, elements;
} garb_build_cost_t;
garb_build_cost_t garb_cascade_build_cost(const garb_cascade_t *c);
// Which mark an update made the elements it marked by: for a change, the
// changed element's own mark and what its readers reach for a change of an
// attribute other than `class` and `style`, its subtree for a change of a
// state (and of an attribute whose keys are past naming or read at an
// ancestor), an added subtree, the siblings a changed or turned element moves (those
// after a changed element for `+`/`~`, all of them for an `of S`; those
// the readers of an :empty that turned reach), every child of a parent
// whose children changed under an `of S`, or with more types of children
// than an index walk's table keeps (the children's siblings), a
// :has() asked again (its anchors and their reach), the parent's own state
// (:empty where its answer turned, a <details> or <fieldset>'s first
// child), a shadow tree's assignments; restyling itself marks nothing (an
// element restyled alone whose custom property map came out different has
// what inherits from it recomputed, THE RECOMPUTE), so `inherited` counts
// none; for a change of an
// attribute, its element and the reach of the readers of its keys, under
// `class`, `style`, or for any other attribute the kind of an attribute or
// a state; and, at a change
// of children, the reach of the readers (select.h § A READER) of a child
// whose back index or front index turned, of the children an unbounded
// position form's walk passed over, of an element brought or taken
// (`~` and the `+` chains), and of a pair of siblings broken or made (`+`).
typedef enum {
    GARB_MARK_CHANGED, GARB_MARK_ADDED, GARB_MARK_SIBLINGS, GARB_MARK_HAS, GARB_MARK_PARENT,
    GARB_MARK_SLOTS, GARB_MARK_INHERITED, GARB_MARK_CLASS, GARB_MARK_STYLE, GARB_MARK_BACK, GARB_MARK_SUFFIX,
    GARB_MARK_FRONT, GARB_MARK_POSITION, GARB_MARK_NEIGHBOURS, GARB_MARK_ADJACENT,
    GARB_MARKS
} garb_mark_t;
const char *garb_mark_name(garb_mark_t mark);
// What the latest garb_cascade_update restyled: the elements its changes
// marked, those restyling marked (`inherited`, `by[GARB_MARK_INHERITED]`:
// none, since what is under an element whose custom property map changed
// is recomputed, not marked: THE RECOMPUTE, below), and every mark made, by
// the mark that made it. Zeros
// before any update, after one that read no changes, and for one refused
// before it read them; one refused partway says what it had marked by then.
typedef struct {
    uint64_t marked, inherited;
    uint64_t by[GARB_MARKS];
    // THE RECOMPUTE: the elements below a changed custom property map
    // recomputed (not counted toward the half), those where it stopped
    // because their own map came out as it was, those whose winners came out
    // changed, and those a recompute took to their own whole cascade, matched
    // again: a pseudo-element's var(), keyframes read with the map or a
    // changed or inherited animation-name, or a var() winner whose new value
    // sets other longhands than it did.
    uint64_t recomputed, recompute_pruned, recompute_changed, recompute_whole;
} garb_update_marks_t;
garb_update_marks_t garb_cascade_update_marks(const garb_cascade_t *c);
// The custom property names the latest update's recompute followed, space
// apart, "(more)" after them when it lost count (none is then known), into
// `out` (cut to `cap`, always terminated); its length. Empty when it
// recomputed nothing. A record's: the names are a page's own.
size_t garb_cascade_recompute_names(const garb_cascade_t *c, char *out, size_t cap);
// When the latest update was refused GARB_UPDATE_BROAD: the change of the
// batch that marked the most elements (the batch's marking stops at the
// one that crossed half, so a later change never ran): its kind
// (os64_html_change_kind_t), its target (tag, #id, first .class, cut to
// fit) and the target's depth below the document, the attribute for an
// attribute's change, how many elements it marked and by which mark; with
// the changes the batch held and had marked, the dirty count at refusal
// and the elements the cascade holds. False when the latest update was not
// refused so, or no clock was given.
typedef struct {
    int32_t kind;
    char target[64], attr[32];
    uint32_t depth, marked, by[GARB_MARKS];
    uint32_t changes, marked_changes, dirty, slots;
} garb_broad_trigger_t;
bool garb_cascade_broad_trigger(const garb_cascade_t *c, garb_broad_trigger_t *out);

// THE KEPT READ (docs/design/pending/KEPT_READ.md). A build's READ is
// everything it makes before it touches an element: the sheets' rules read
// with their declarations, the selector index, the keyframes and faces,
// the anchors and style readers, the dependencies, the scopes. A build
// handed `read_from` takes that read, refcounted and never
// written after it was made, when the KEY is equal: the same document, the
// same sheets by their parse serials (garb_parsed_t.serial) with the same
// parents, imports and media, the same scopes, every garb_env_t field
// (`@media` is decided while reading), the same quirks mode and limits.
// Otherwise it reads again. A read that came up short, or whose
// declarations had no arena of their own, is never taken.
// Why the latest build read again, or that it took a read
// (GARB_READ_KEPT). The first key member found different is named.
typedef enum {
    GARB_READ_KEPT, GARB_READ_NOT_OFFERED, GARB_READ_NOT_KEEPABLE, GARB_READ_DOCUMENT, GARB_READ_LIMITS,
    GARB_READ_QUIRKS, GARB_READ_ENVIRONMENT, GARB_READ_SHEETS, GARB_READ_SCOPES, GARB_READ_REASONS
} garb_read_refusal_t;
garb_read_refusal_t garb_cascade_read_refusal(const garb_cascade_t *c);
const char *garb_read_refusal_name(garb_read_refusal_t reason);
// A reference to the cascade's read (NULL for none), which a caller hands a
// later build as `read_from`; it outlives the cascade until released.
garb_read_t *garb_cascade_read_hold(const garb_cascade_t *c);
void garb_read_release(garb_read_t *r);
// The read's shape hashed, with no address in it (its counts, rule order,
// declaration names, the index's keys, the keyframes', faces' and skipped
// names): two reads of the same sheets in the same environment hash equal,
// whichever cascade made them. A first check only: a different value or
// selector need not show, which garb_cascade_compare does.
uint64_t garb_cascade_read_content(const garb_cascade_t *c);
// With `check_read`: true when the read this cascade took had been written
// since it was made (its raw hash then and now differ). False without the
// check, or for a read the cascade made itself.
bool garb_cascade_read_written(const garb_cascade_t *c);

uint64_t garb_cascade_generation(const garb_cascade_t *c);
uint64_t garb_cascade_updates(const garb_cascade_t *c);
uint64_t garb_cascade_version(const garb_cascade_t *c);
// Whether this element's winners were recomputed after an earlier update.
bool garb_style_updated_since(const garb_cascade_t *c, const os64_html_node_t *node,
                              uint64_t update);

// WHAT AN INLINE STYLE WRITE CHANGED, asked before the cascade is updated
// for it: the element's `style` attribute as it is now against the cascade's
// own parse of it, the one its winners were computed from. Each longhand
// whose winning inline declaration (an `!important` one over the rest, then
// the last written) was added, removed, or changed in name, tokens or
// importance is listed once. A value equal in meaning but written otherwise
// counts as changed. `declared`: a declaration sets it now; `keyword`: that
// declaration's value when it is one identifier of fewer than 16 bytes,
// lowercased (`none`, `collapse`), else empty; `var`: it holds var() or
// env(), so its value is not known until it is substituted. The answer is
// how many were listed, or -1 when it cannot be said: no element the
// cascade styled, a custom property changed (var() can reach any longhand),
// a parse that failed or was cut short, or more than `cap`.
typedef struct {
    garb_prop_t prop;
    bool declared, var;
    char keyword[16];
} garb_inline_change_t;
int32_t garb_cascade_inline_changes(const garb_cascade_t *c, const os64_html_node_t *el,
                                    garb_inline_change_t *out, int32_t cap);
// Whether the cascade reads the `style` attribute's text itself: a selector
// of its sheets tests the attribute (inside :is(), :where(), :not() and
// :has() as well), or a declaration it has read holds attr(style) (generated
// content's text). Then a write of an inline style can change which rules
// match, or a box's text, whatever it declares.
bool garb_cascade_reads_style_attribute(const garb_cascade_t *c);
// Narrower (BRIEF_LAYOUT_NEEDS.md N2a): whether an inline style write on
// `element` may change what matches, because a style reader of the sheets
// (garb_style_reader_t) may match it, or the readers narrow to no element
// (garb_cascade_style_readers_bare: one is bare, there are more than
// GARB_STYLE_READERS, or a declaration reads attr(style)). False when no
// selector of the sheets can turn on this element's style. Rules are
// counted whatever their scope, which only widens the answer.
#define GARB_STYLE_READERS 64
bool garb_cascade_style_may_read(const garb_cascade_t *c, const os64_html_node_t *element);
bool garb_cascade_style_readers_bare(const garb_cascade_t *c);

// The viewport the cascade was judged against: what a `vw` or `vh` in its
// winners is a hundredth of.
garb_env_t garb_cascade_env(const garb_cascade_t *c);
// `element`'s winners; an element no author rule reached has none.
garb_style_t garb_style_for(const garb_cascade_t *c, const os64_html_node_t *element);
// The winners of `element`'s ::before or ::after (GENERATED_CONTENT.md §
// 3), from that pseudo-element's rules alone: none when no rule for it
// matched, and for any other pseudo-element.
garb_style_t garb_style_for_pseudo(const garb_cascade_t *c, const os64_html_node_t *element, garb_pseudo_t pseudo);
/* True when a matched non-custom declaration needs var()/env() substitution,
 * including declarations superseded later in the cascade. Such source styles
 * cannot be reused under different inheritance without substituting again. */
bool garb_style_needs_substitution(const garb_cascade_t *c, const os64_html_node_t *element);

// One keyframe of an animation (Animations 2 § 3.2): its offset in [0, 1];
// its easing — the keyframe's own `animation-timing-function`, or NULL for
// the element's; the longhands it sets, read as winners are read, and for
// each where it was written in the rule (`order`, later greater); and, as
// a winner's, the index in garb_cascade's `sheets` of the sheet its rule
// was written in, which a relative url() in it is resolved against. Of
// several declarations of one longhand in a keyframe, the last is the one
// kept; `!important` ones and the `animation-*` longhands are not kept.
typedef struct {
    double offset;
    const garb_val_t *easing;
    const garb_set_t *sets;
    const uint64_t *order;
    int32_t nsets;
    int32_t sheet;
} garb_keyframe_t;

// The keyframes `name` names (`len` bytes, compared codepoint for
// codepoint) for `element`, in their used order — by offset, and blocks at
// one offset with one easing grouped, in the order each group was first
// written; the implicit 0% and 100% keyframes are the caller's to add, and
// so is the grouping of a block that gives the element's own easing with
// one that gives none, which only the caller can see are one (`order`).
// They come from the last `@keyframes` (or `@-webkit-keyframes`, Compat
// § 3.1) of that name in cascade order inside the `@media` and `@supports`
// that hold. A keyframe's var() is replaced against the element's custom
// properties when the element animates by that name — its winners name
// it, or it inherits names that do (`animation-name: inherit`) — and the
// declaration dropped otherwise. Answers how many and points `out` at
// them, which live as long as the cascade; -1 when no rule has that name,
// and the animation starts nothing.
int32_t garb_keyframes(const garb_cascade_t *c, const os64_html_node_t *element,
                       const char *name, size_t len, const garb_keyframe_t **out);
void garb_cascade_free(garb_cascade_t *c);
/* What a cascade holds (MEMORY_CENSUS.md § 3), in bytes reserved: its own
 * arena (its rules, entries, keyframes, attribute table and selector
 * indexes), the arena of what it read from the sheets, each `style`
 * attribute's parse, and the slot table (with its rows of anchor bits), the
 * dirty list and an update's arrays and bits it keeps on the heap, at their
 * capacity. Scratch a call frees before it returns is not
 * held, and not counted. NULL answers 0. Cheap: a pass over the style
 * attributes, no walk of the rules. The sum of garb_cascade_parts. */
size_t garb_cascade_bytes(const garb_cascade_t *c);
// The elements the cascade holds a style for (its element table): 0 for
// NULL.
uint32_t garb_cascade_elements(const garb_cascade_t *c);
/* The same, by part: `state` is the cascade's own arena and the record
 * itself, `reads` what it read from the sheets, `attributes` the `style`
 * attributes' parses, `tables` the slot table, its rows of anchor bits, the
 * dirty list, and the batch's arrays and the subject :has() bits an update
 * keeps, on the heap.
 * The first two are held to ceilings of their own and each attribute's
 * parse to another (garb_cascade_limits_t), which is why they are told
 * apart. NULL answers zeros. */
typedef struct {
    size_t state, reads, attributes, tables;
} garb_cascade_parts_t;
garb_cascade_parts_t garb_cascade_parts(const garb_cascade_t *c);

// One `@font-face` rule (Fonts 4 § 4), read whole: the family it names; its
// sources in order — a url() as written, resolved by whoever fetches it
// against the sheet the rule was written in (`sheet`, an index into
// garb_cascade's sheets), with its format() hint (one format keyword, or
// the first of a list of strings, as written; NULL with no format()) — or
// a local() family name, any length; the weights it covers, 1
// to 1000 as written, fractions and all (one weight is a range of one);
// its style; and the code points
// it covers, as inclusive ranges, NULL and 0 for all of them. A source
// with tech() is dropped (this engine supports no font technology), as is
// one written wrong; a rule left with no family or no source is no face.
typedef enum { GARB_FACE_URL, GARB_FACE_LOCAL } garb_face_src_kind_t;
typedef struct {
    garb_face_src_kind_t kind;
    const char *text;           // URL: the address; LOCAL: the family name
    size_t len;
    const char *format;         // URL: format()'s first, or NULL
    size_t format_len;
} garb_face_src_t;
typedef enum { GARB_FACE_NORMAL, GARB_FACE_ITALIC, GARB_FACE_OBLIQUE } garb_face_style_t;
typedef struct {
    uint32_t first, last;
} garb_unicode_range_t;
typedef struct {
    const char *family;
    size_t family_len;
    const garb_face_src_t *src;
    int32_t nsrc;
    float weight_min, weight_max;
    garb_face_style_t style;
    const garb_unicode_range_t *ranges;
    int32_t nranges;
    int32_t sheet;
} garb_font_face_t;

// The page's faces, from the `@font-face` rules inside the `@media` and
// `@supports` that hold, in cascade order — every one, faces of one family
// with the same weights, style and ranges among them: they make one
// composite face, the later tried first (Fonts 4 § 4.5.1), so an earlier
// one still serves when a later fails. At most GARB_FONT_FACES_MAX are
// kept; every `@font-face` rule after them is counted in `lost` (may be
// NULL) unread, face or not. So is a face memory ran out for, which is
// dropped whole — and which the cascade also says
// (garb_cascade_incomplete). Answers how many and points `out` at them,
// which live as long as the cascade.
#define GARB_FONT_FACES_MAX 256
int32_t garb_cascade_font_faces(const garb_cascade_t *c, const garb_font_face_t **out, uint32_t *lost);

// What the build passed over because this library does not read it — a
// property it has no reader for, an at-rule it does not keep, a function
// no reader accepts in a declaration it dropped, and a VALUE: `<property>: <words>` for
// a declaration of a property it reads, dropped though its value is bare
// words, or read and laid out as something else (an approximation) — each
// name once, lower case, with how often it was met. A prefixed name
// (`-webkit-…`) is not counted: one engine's dialect is passed over by
// every other. A value merely written wrong, one holding a number or a
// string, is not either. The names live as long as the cascade. Copies up to
// `cap` into `out` and answers how many there are; `lost` (may be NULL)
// is how many met no room. At most GARB_SKIPS_MAX are kept.
typedef enum { GARB_SKIP_PROPERTY, GARB_SKIP_AT_RULE, GARB_SKIP_FUNCTION, GARB_SKIP_VALUE } garb_skip_kind_t;
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
// HTML sizes, in CSS pixels. Uses initial font metrics, viewport lengths and
// CSS math; ineligible auto and unsupported values fall through to later
// entries, then 100vw. No layout-dependent auto-sizing is performed.
double garb_image_source_size(const char *text, garb_env_t env);

// CSS.supports (CSS Conditional 3 § 6.1), judged as the cascade judges an
// `@supports` rule, read as `how` reads: `text` a <supports-condition>, or,
// when that is not true, the same text in parentheses (a bare
// declaration); or a property's name (a custom property's by the string
// test, `--` and anything after it) and a value of its grammar alone (no
// `!important`). 1 or 0, or -1 when the parse had no memory; a text past
// the parser's depth or size is 0, as the same condition in a sheet is.
int32_t garb_supports(const char *text, size_t len, garb_reading_t how);
int32_t garb_supports_property(const char *property, size_t plen, const char *value, size_t vlen,
                               garb_reading_t how);
// The same list as CSSOM serialises it, which is what a MediaQueryList's
// `media` reads (CSSOM § Serializing media queries): the queries joined by
// `, `; each one that is not a query's shape, an empty one included, as
// `not all`; `not` or `only`, the type and a known feature's name and
// keywords in lower case; `all and` left out of a query nothing negates; a
// number to six significant digits (garb_css_number), a ratio as `a / b`;
// anything else in brackets kept as written, its white space made single
// spaces. A list with no content is "". A query's shape is the grammar's:
// a feature this machine does not know changes what the query evaluates
// to, not how it is written. Each query is read once, so the cost is
// linear in the list however deep its brackets nest. Like snprintf; the
// text form answers (size_t)-1, with "" written, when it could not read the
// text whole: no memory, or brackets nested past GARB_DEPTH_MAX.
size_t garb_media_serialize(const garb_value_t *v, int32_t n, char *out, size_t cap);
size_t garb_media_text_serialize(const char *text, char *out, size_t cap);
// Memory for a buffer libgarb grows for its caller: a block of `new_size`
// bytes holding the first `old_size` of `old` (which it lets go), or NULL
// with `old` untouched; `new_size` 0 lets `old` go and answers NULL.
typedef void *(*garb_grow_t)(void *ctx, void *old, size_t old_size, size_t new_size);
// The text form in one pass into a buffer it grows with `grow` (NULL:
// libos64's os64_realloc and os64_free): NUL-terminated, its length in
// *length, the caller's to let go the way `grow` would. NULL where the
// text form answers (size_t)-1, and when there was no memory for the
// buffer, with nothing left held.
char *garb_media_text_serialized(const char *text, size_t *length, garb_grow_t grow, void *ctx);

// Every element's winners as text, in shadow-including order (a host's
// shadow tree before its children): `tag#id: prop:
// value; ...`, its ::before's and ::after's after it (`tag#id::before:
// ...`), for the harness and a probe. Like snprintf.
size_t garb_cascade_dump(const garb_cascade_t *c, const os64_html_document_t *doc, char *out,
                         size_t cap);
// Whether two cascades of `doc` give every element, and its ::before and
// ::after, the same winners: a diagnostic, for checking an updated cascade
// against one built fresh. Where they differ, `why` says where first (like
// snprintf): the element's path, the property, the value in `c` against
// the value in `other`, and up to three rules of `other` that match there
// and set it, as written. Those are read from the sheets `other` was built
// from, which must still be alive.
bool garb_cascade_compare(const garb_cascade_t *c, const garb_cascade_t *other, const os64_html_document_t *doc,
                          char *why, size_t cap);

#pragma GCC visibility pop

#endif
