#ifndef OS64_PAGE_H
#define OS64_PAGE_H

// page.h — a parsed page in, WHAT IT MEANS out. LIBPAGE.md is the design
// record; this file is the seam it describes.
//
// Above this line a face does everything with its own hands: draw the page
// in cells or in pixels, take keystrokes, keep a history, ask a person a
// question. Below it, this library answers from the tree — which form a
// control belongs to, what its value is, and exactly what bytes leave the
// machine when somebody presses Enter. A face never decides any of that. It
// asks, because a form is submitted identically whether it was drawn in
// cells or in pixels, and the alternative is the same rule written twice.
//
// Nothing here does I/O, blocks, keeps a terminal, or asks a person
// anything. It STATES FACTS — "this request leaves an encrypted page for a
// plain one" — and the face decides what to do with them.
//
// The document outlives its models and control state. Models pin its tree;
// control values may also borrow state storage. Free models, then state,
// then the document, on the document's owning thread.

#include "html/html.h"
#include "os64/url.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#pragma GCC visibility push(default)

// A model snapshots one document version; control properties share node state.
typedef struct os64_page os64_page_t;
typedef struct os64_page_state os64_page_state_t;

#define OS64_PAGE_STATE_DEFAULT_MAX_BYTES ((size_t)16 * 1024 * 1024)
#define OS64_PAGE_FATAL_EXIT 0x50414745

// State belongs to one document, including nodes not connected to its tree.
// The cap includes its header, records, cached values and operation scratch.
// Builds retain sparse normalized defaults where they differ from markup;
// script assignments can spend the cap on records for their whole group.
// Zero selects the default cap. NULL means no memory (or no document).
// Freeing explicit state with models still borrowing it ends the program
// with OS64_PAGE_FATAL_EXIT. State does not pin the document itself.
os64_page_state_t *os64_page_state_create(os64_html_document_t *doc, size_t max_bytes);
// Borrowed document; NULL state answers NULL. The caller keeps it alive.
os64_html_document_t *os64_page_state_document(const os64_page_state_t *state);
void os64_page_state_free(os64_page_state_t *state);
size_t os64_page_state_bytes(const os64_page_state_t *state);
// Nonzero revision of effective control state, including edit origin.
// Failed operations and getter-only cache fills leave it alone.
// HTML mutations have their own document version.
uint64_t os64_page_state_version(const os64_page_state_t *state);

// ── What a rule can say no with ─────────────────────────────────────────
//
// ONE VALUE PER RULE THAT CAN REFUSE, because a face renders these and must
// never have to invent one. A refusal is a VERDICT and not a fallback: an
// action too long to resolve was once sent to the page's own host instead,
// which is a different host to be wrong about.
typedef enum {
    OS64_PAGE_REASON_OK = 0,
    // The standard says a browser does nothing.
    OS64_PAGE_REASON_DIALOG,                   // the form closes a dialog; no server hears
    OS64_PAGE_REASON_DEFAULT_BUTTON_DISABLED,  // Enter, and the button it would press is off
    OS64_PAGE_REASON_IMPLICIT_BLOCKED,         // Enter, no submit button, and more than one field
    OS64_PAGE_REASON_DISABLED,                 // the page took this control away
    OS64_PAGE_REASON_NO_FORM,                  // this control belongs to no form
    OS64_PAGE_REASON_INVALID,                  // constraint validation; see the request's control
    OS64_PAGE_REASON_NO_SUBMISSION,            // pressing this control submits nothing
    OS64_PAGE_REASON_RESET,                    // it is a reset button; see os64_page_reset
    OS64_PAGE_REASON_NO_ANCHOR,                // nothing in the page claims that `#name`
    OS64_PAGE_REASON_NO_REFRESH,               // this page declares no refresh to follow
    // The page named something that cannot be done.
    OS64_PAGE_REASON_BAD_ACTION,               // the destination does not resolve
    OS64_PAGE_REASON_TOO_LONG,                 // longer than an address may be
    OS64_PAGE_REASON_SCHEME,                   // a scheme no browser navigates
    OS64_PAGE_REASON_BODY_TOO_LONG,            // more than the caller's stated max_body
    OS64_PAGE_REASON_NO_MEMORY,
    // The face asked about something that is not there. These come back from
    // the edit verbs rather than from the door.
    OS64_PAGE_REASON_NO_CONTROL,               // that index names no control
    OS64_PAGE_REASON_WRONG_KIND,               // that control holds no such value
    OS64_PAGE_REASON_READONLY,                 // the page keeps that value as it is
    OS64_PAGE_REASON_STALE,                    // the document changed since this model
} os64_page_reason_t;

// The refusal in words, for a status row. Never NULL.
const char *os64_page_reason_name(os64_page_reason_t reason);

// ── How a form insists on being sent ────────────────────────────────────

typedef enum {
    OS64_PAGE_METHOD_GET = 0,   // and what an absent or invalid method means
    OS64_PAGE_METHOD_POST,
    OS64_PAGE_METHOD_DIALOG,
} os64_page_method_t;

typedef enum {
    OS64_PAGE_ENCTYPE_URLENCODED = 0,   // and what an invalid enctype means
    OS64_PAGE_ENCTYPE_MULTIPART,
    OS64_PAGE_ENCTYPE_TEXT_PLAIN,
} os64_page_enctype_t;

// ── A reference the page wrote down ─────────────────────────────────────
//
// A RESOLVED ADDRESS AND THE THREE THINGS THE TEXT ALONE CANNOT SAY.
// `spelled` because `formaction=""` is the page's own address, and reading
// that as "no action given" sends the form to the form's destination instead
// of to the page it is on. `has_fragment` because the fragment text cannot
// tell `href="#"` from `href=""`, and one of those is a move to the top
// while the other is a reload. `refused` because a destination that will not
// resolve must not quietly become the page's own.
typedef struct {
    bool spelled;                  // the attribute was there, empty included
    const char *url;               // canonical, no scheme-default port; NULL if refused
    const char *fragment;          // decoded, no '#'; decoded NUL refuses the reference
    bool has_fragment;
    os64_page_reason_t refused;    // OK when `url` is good
} os64_page_ref_t;

// ── The model ───────────────────────────────────────────────────────────

typedef struct {
    const os64_html_node_t *node;   // the `a`, `area`, `frame` or `iframe` it came from
    os64_page_ref_t href;
    // It names THIS document, so following it is a MOVE and not a fetch —
    // compared without the fragment, which is the whole of a table of
    // contents on a long article.
    bool same_document;
} os64_page_link_t;

typedef struct {
    const os64_html_node_t *node;
    const char *id;                 // NULL when the form has none
    os64_page_ref_t action;
    os64_page_method_t method;
    os64_page_enctype_t enctype;
    const char *accept_charset;     // as the page wrote it; NULL when absent
    bool novalidate;
} os64_page_form_t;

// WHICH ELEMENT A CONTROL IS. A `button` defaults to type submit, which is
// why it is a submitter through the same code an `input type=submit` is —
// that rule finished for one element type and not the other is what crashed
// on Wikipedia's search box.
typedef enum {
    OS64_PAGE_EL_INPUT = 0,
    OS64_PAGE_EL_BUTTON,
    OS64_PAGE_EL_SELECT,
    OS64_PAGE_EL_TEXTAREA,
} os64_page_element_t;

// The standard's input types. An unknown `type` is Text, which is the
// standard's own fallback and not a guess.
typedef enum {
    OS64_PAGE_INPUT_TEXT = 0,
    OS64_PAGE_INPUT_SEARCH,
    OS64_PAGE_INPUT_TEL,
    OS64_PAGE_INPUT_URL,
    OS64_PAGE_INPUT_EMAIL,
    OS64_PAGE_INPUT_PASSWORD,
    OS64_PAGE_INPUT_DATE,
    OS64_PAGE_INPUT_MONTH,
    OS64_PAGE_INPUT_WEEK,
    OS64_PAGE_INPUT_TIME,
    OS64_PAGE_INPUT_DATETIME_LOCAL,
    OS64_PAGE_INPUT_NUMBER,
    OS64_PAGE_INPUT_RANGE,
    OS64_PAGE_INPUT_COLOR,
    OS64_PAGE_INPUT_CHECKBOX,
    OS64_PAGE_INPUT_RADIO,
    OS64_PAGE_INPUT_FILE,
    OS64_PAGE_INPUT_SUBMIT,
    OS64_PAGE_INPUT_IMAGE,
    OS64_PAGE_INPUT_RESET,
    OS64_PAGE_INPUT_BUTTON,
    OS64_PAGE_INPUT_HIDDEN,
    OS64_PAGE_INPUT_NONE,   // the element is not an input at all
} os64_page_input_t;

// ONE ENTRY OF A LIST. A DISABLED option is still SHOWN when the page marks
// it — that is what a "choose one" placeholder is — but never sent, because
// disabled means the page took it away. `selected` is the option's OWN
// answer, which one index could not hold: a list marked `multiple` may have
// several, and collapsing them to the last one sends one choice where the
// page made three.
typedef struct {
    const os64_html_node_t *node;
    const char *label;      // display label: nonempty `label`, else gathered option text
    const char *value;      // submitted value: `value`, else gathered option text
    bool disabled;          // its own attribute, or its `optgroup`'s
    bool selected;          // now, a person's choice included
} os64_page_option_t;

// What a submit control overrules about its form. The standard lets a button
// carry its own destination and method, and the method is what decides
// whether the values go in an address or in a body: a GET form with a
// `formmethod=post` button is a POST, and sending it as a GET puts whatever
// was typed into somebody's server log.
typedef struct {
    os64_page_ref_t action;
    bool has_method;
    os64_page_method_t method;
    bool has_enctype;
    os64_page_enctype_t enctype;
    bool novalidate;
} os64_page_overrides_t;

typedef struct {
    const os64_html_node_t *node;
    os64_page_element_t element;
    os64_page_input_t input;
    // THE FORM THIS CONTROL BELONGS TO, settled once for every control
    // before anything asks, so a question during rendering is answerable.
    // -1 is no form at all, which is what a `form=` naming nothing leaves
    // behind — not the nearest ancestor.
    int32_t form;
    const char *name;       // NULL when the page gave none
    // ITS VALUE AS THE STANDARD DEFINES IT, sanitized for its type, with a
    // person's edit in place of the page's own bytes when there is one.
    // `value_len` is authoritative; the bytes are NUL-terminated as well.
    const char *value;
    size_t value_len;
    bool disabled;          // its own attribute, or a disabled `fieldset`
    bool readonly;          // effective readonly; false where the attribute does not apply
    bool required;
    bool multiple;          // SELECT, and `multiple` on a file input
    bool checked;           // CHECKBOX / RADIO, now, a person's tick included
    // The page spelled a `value` attribute at all. A tick with none sends
    // `on`, the standard's default, but one that says `value=""` asked for
    // an empty answer, and sending `on` for it picks a different branch on
    // the far side.
    bool has_value_attribute;
    // It sits inside a subtree the page marked `hidden`, so it is drawn
    // nowhere and is still submitted — and is still its form's default
    // button when it comes first, which no cursor can reach.
    bool hidden_subtree;
    bool has_datalist_ancestor;   // an autocomplete suggestion; never sent
    // The standard's "field that blocks implicit submission". `readonly`
    // does not exempt one: the rule is about the TYPE.
    bool blocks_implicit;
    // WHAT PRESSING IT DOES, which is not the same question as which element
    // it is: a `button` with no type submits, and an `input` has to say so.
    // Neither true means pressing it does nothing at all.
    bool submits;
    bool resets;
    // It is barred from constraint validation, so `required` and the length
    // limits say nothing about it — `readonly`, `disabled`, a `datalist`
    // ancestor, and the types that hold no answer a person gave.
    bool barred_from_validation;
    const char *dirname;    // the name its text direction is sent under
    os64_page_overrides_t overrides;   // meaningful for a submitter
    const os64_page_option_t *options;  // SELECT
    int32_t noptions;
} os64_page_control_t;

// ── The pictures a page names ───────────────────────────────────────────
//
// WHERE A PICTURE COMES FROM IS A FACT ABOUT THE PAGE, NOT ABOUT PIXELS, so
// it is resolved here for the reason every reference is: two resolvers
// disagree about a `<base>`. How big it is and where it goes are geometry,
// and belong to whoever lays the page out — its `width` and `height` stay
// attributes on the node.
//
// Every `img` and every `input type=image` whose `src` names something. An
// empty or blank `src` names nothing: such an image is shown broken rather
// than fetching the page it sits on as a picture — the standard's rule for
// the empty string, and browsers' for white space — so it is not
// listed, and `os64_page_image_for` answers -1.
typedef struct {
    const os64_html_node_t *node;   // the `img` or `input type=image`
    os64_page_ref_t src;            // resolved against the base; refused says why
    // As written, or NULL when the page wrote none: an ABSENT alt and an
    // EMPTY one differ, because `alt=""` says the picture is decoration and
    // its absence says nothing at all.
    const char *alt;
} os64_page_image_t;

// ── What the page asks for on its own ───────────────────────────────────
//
// A DECLARATIVE REFRESH is a navigation the DOCUMENT asks for, with nobody
// pressing anything: `<meta http-equiv="refresh" content="0;URL=...">`. It
// is the only way a page can send a reader somewhere with no server redirect
// and no script, which is why a search engine's click logger falls back to
// it and why every "this page has moved" placeholder is one.
//
// libpage says a refresh is DECLARED and where to. Whether to follow it,
// and whether to wait, is the face's: a delay is a person's patience, and a
// same-document refresh without a fragment is a reload the face may cap.
// Follow it through the door, with OS64_PAGE_ACTIVATE_REFRESH, so the
// request carries the same facts a link's does.
typedef struct {
    const os64_html_node_t *node;   // the `meta` that asked
    uint32_t seconds;               // 0 is now; a page may ask for a long wait
    // Check refused before reading url: retaining the winning candidate
    // can fail with NO_MEMORY and a NULL URL. On success, url is resolved;
    // spelled distinguishes an explicit address from a bare-delay reload
    // of this document.
    os64_page_ref_t url;
    // The target URL names this document. A fragment-bearing target goes
    // through fragment navigation; without one, following it re-fetches.
    bool names_this_document;
} os64_page_refresh_t;

// The refresh this page declares, or NULL for a page that declares none.
// The FIRST valid one in tree order wins; an invalid pragma declares nothing
// and leaves a later one free to. Allocation failure retains the candidate
// with url.refused == NO_MEMORY, preventing a later pragma from replacing it.
const os64_page_refresh_t *os64_page_refresh(const os64_page_t *page);

// ── Building one ────────────────────────────────────────────────────────

typedef struct {
    // A body larger than this is OS64_PAGE_REASON_BODY_TOO_LONG rather than
    // an allocation nobody asked for. Zero means the built-in ceiling.
    size_t max_body;
} os64_page_options_t;

os64_page_options_t os64_page_options_default(void);

// `document_url` is where the page CAME FROM — libfetch's final address
// after redirects. It is what decides whether a request is a downgrade, and
// it is NOT the base URL: a page can move its base to `http://` with `<base
// href>` while the page that collected the values stays encrypted. Two
// addresses, two jobs, never conflated.
//
// A scheme-default port is dropped from it here, so a caller holding a
// filled-in default need not remember to; `<base href>` is normalised the
// same way, since a page that spells `:443` must not put it in every link.
//
// A REFUSED document still has a tree, and a page is built from what there
// is; NULL comes back only when nothing at all could be. `opt` NULL selects
// the defaults.
// A supplied state must belong to doc and outlive the model. NULL makes
// private shared state, released with its last model, including rebuilds.
// Each model takes one document pin; pin exhaustion returns NULL.
os64_page_t *os64_page_build(const os64_html_document_t *doc, const char *document_url,
                             const os64_page_options_t *opt, os64_page_state_t *state);
// Build beside old at the document's current version, sharing control state.
// Any incomplete candidate returns NULL, preserving old and shared state.
// Either model may be freed first. Successful normalization publishes to
// the other live models before replacing state storage.
os64_page_t *os64_page_rebuild(const os64_page_t *old);
// Borrowed state, including private state created by build; valid while a
// model reference remains alive. The borrower must not free private state. NULL answers NULL.
os64_page_state_t *os64_page_shared_state(const os64_page_t *page);
// Owner-thread reference for a consumer keeping snapshot indices/strings.
// False for NULL or count exhaustion. Free releases one reference; the last
// releases the model, its document pin and its shared-state membership.
bool os64_page_retain(const os64_page_t *page);
void os64_page_free(os64_page_t *page);

// The walk ran short of memory partway. What is in the model is real and
// worth using — half a page says more than none — so this is a fact about
// the model rather than a failure to return one.
bool os64_page_incomplete(const os64_page_t *page);

// The address every reference resolved against: `<base href>` when the page
// carries one, else where the page came from. Never NULL.
const char *os64_page_base(const os64_page_t *page);
// `reference` resolved against `base` by the rule the page resolves its own
// by, canonical — for an address a page's resources name in turn, as a
// style sheet's @import does against the sheet's own address. False when
// either will not resolve (an opaque base, an address with no host) or the
// answer will not fit in `cap`.
bool os64_page_url_absolute(const char *base, const char *reference, char *out, size_t cap);
// Where the page came from, canonical. Never NULL.
const char *os64_page_document_url(const os64_page_t *page);

// ── Reading it ──────────────────────────────────────────────────────────
//
// In tree order, by index. Every item names its node — AND THE NODE NAMES
// ITS ITEM. A face draws by walking the tree and has to ask "which control
// is this node?"; a face keeping its own counter instead would be a second
// walk skipping different subtrees, which is one rule with two
// implementations wearing a hat.
int32_t os64_page_nlinks(const os64_page_t *page);
const os64_page_link_t *os64_page_link(const os64_page_t *page, int32_t i);
int32_t os64_page_link_for(const os64_page_t *page, const os64_html_node_t *node);

int32_t os64_page_nforms(const os64_page_t *page);
const os64_page_form_t *os64_page_form(const os64_page_t *page, int32_t i);
int32_t os64_page_form_for(const os64_page_t *page, const os64_html_node_t *node);

int32_t os64_page_ncontrols(const os64_page_t *page);
const os64_page_control_t *os64_page_control(const os64_page_t *page, int32_t i);
int32_t os64_page_control_for(const os64_page_t *page, const os64_html_node_t *node);

int32_t os64_page_nimages(const os64_page_t *page);
const os64_page_image_t *os64_page_image(const os64_page_t *page, int32_t i);
int32_t os64_page_image_for(const os64_page_t *page, const os64_html_node_t *node);

// ── The pictures a page puts behind things ──────────────────────────────
//
// The `background` attribute of `body`, `table`, `thead`, `tbody`, `tfoot`,
// `tr`, `td` and `th` — the Rendering chapter's `background-image` for the
// old web. Resolved here for the same reason a picture's `src` is. An empty
// attribute names nothing and is not listed, as an empty `src` is not.
typedef struct {
    const os64_html_node_t *node;   // the element the picture is behind
    os64_page_ref_t src;            // resolved against the base; refused says why
} os64_page_background_t;

int32_t os64_page_nbackgrounds(const os64_page_t *page);
const os64_page_background_t *os64_page_background(const os64_page_t *page, int32_t i);
int32_t os64_page_background_for(const os64_page_t *page, const os64_html_node_t *node);

// ── The page's style sheets ─────────────────────────────────────────────
//
// Every sheet the page names, in DOCUMENT ORDER, which is the order the
// cascade ranks them in (GARB.md): a `style` element, whose text is its
// children as libhtml keeps them, and a `link` whose `rel` holds the token
// `stylesheet` and not `alternate` (an alternate sheet is off until a
// person picks it), with an `href` that is not empty or white space,
// resolved against the base for the reason every reference is. A `type`
// that is neither empty nor `text/css` names a language this browser does
// not read, and a `disabled` link is off: neither is listed. `media` is as
// written, NULL when absent — judging it is the cascade's, since a resize
// changes the answer. HTML elements only: an SVG `style` styles SVG.
typedef struct {
    const os64_html_node_t *node;   // the `style` or the `link`
    bool linked;                    // a `link`: its sheet is at `href`
    os64_page_ref_t href;           // a `link`'s, resolved; refused says why
    const char *media;
} os64_page_sheet_t;

int32_t os64_page_nsheets(const os64_page_t *page);
const os64_page_sheet_t *os64_page_sheet(const os64_page_t *page, int32_t i);

// Decoded fragment lookup: IDs take precedence over legacy <a name> matches;
// first in tree order wins within each group. NULL means absent or incomplete.
// Use resolve_fragment for navigation, including the empty and "top" fallbacks.
const os64_html_node_t *os64_page_anchor(const os64_page_t *page, const char *decoded_fragment);

// OK with *node == NULL means the document top; OK with a node means that
// exact target. NO_ANCHOR leaves the reader in place. Nonempty lookups on an
// absent/incomplete model return NO_MEMORY, even if a partial index has a match.
// The destination document owns the node; this operation allocates nothing.
os64_page_reason_t os64_page_resolve_fragment(const os64_page_t *page,
                                             const char *decoded_fragment,
                                             const os64_html_node_t **node);

// ── Filling it in ───────────────────────────────────────────────────────
//
// A PERSON'S EDIT IS KEPT APART FROM THE MODEL, keyed by the node it belongs
// to and never by an index, so the model can be rebuilt from the tree
// without losing its value. Initial and edited text use the same type
// sanitizer. Front ends needing intermediate invalid input retain their
// editing buffer separately from this normalized value. The input length
// includes embedded U+0000 bytes; they survive text sanitization but make
// numeric, date/time and color strings invalid for their scalar grammars.
//
// A resize does not require a model rebuild; a document change does.
//
// THESE ARE A PERSON'S EDITS, so they refuse what a person cannot do in a
// browser: text goes only into a control a person types into (a textarea,
// or an input whose type takes typed text, a range or a colour) and never
// into a tick's, a button's or a hidden field's value; a `disabled` control
// takes no edit at all, and a `readonly` one takes no text; a disabled option
// cannot be chosen. The rule lives here and not in each face because a rule
// every face must remember is a rule some face forgets.
//
// Clearing a one-line single-choice list's choice hands the answer to its
// first enabled option, which is the selection rule the standard applies
// when the page itself leaves one unmarked.
//
// Each returns 0, or a negative OS64_PAGE_REASON_*: NO_CONTROL for an index
// that names no control, WRONG_KIND for a control or option of the wrong
// kind, DISABLED or READONLY for one the page took away, NO_MEMORY for
// memory it could not get, STALE when the tree changed. Failure preserves published values and
// selections. Incomplete models reject edits and form submission with
// NO_MEMORY. File selection has no text setter.
int64_t os64_page_set_text(os64_page_t *page, int32_t control, const char *utf8, size_t len);
int64_t os64_page_set_checked(os64_page_t *page, int32_t control, bool on);
int64_t os64_page_set_chosen(os64_page_t *page, int32_t control, int32_t option, bool on);

// Put a form back the way the page wrote it, which is what a reset button
// does. The door never does this itself — activating a reset button is
// answered with OS64_PAGE_NOTHING and OS64_PAGE_REASON_RESET, because
// libpage states facts and a face decides — so a face that draws the button
// calls this. `form` -1 resets the controls that belong to no form.
// Normalized defaults are retained at build time; reset allocates nothing.
// An incomplete model is refused without discarding edits.
int64_t os64_page_reset(os64_page_t *page, int32_t form);

// An attribute mutation observes control transitions at this call, including
// transitions made between property reads. Input modes, constraints, radio
// groups and select choices are staged with the attributes. Returns HTML
// status codes; state-cap exhaustion is OS64_HTML_NO_MEMORY. Failure preserves
// attributes, both revisions, dirty state and published borrowed values.
// Ordinary owned elements accept attributes too; remove ignores value/len.
int64_t os64_page_node_set_attr(os64_page_state_t *state, const os64_html_node_t *node,
                                const char *name, const char *value, size_t len, bool remove);

/* State-aware tree verbs retain libhtml's placement rules/status codes and
 * normalize the radio/select effects of each move without changing dirty
 * flags. Reservations precede tree mutation; refusal preserves both tree
 * and control state. State allocation refusals answer OS64_HTML_NO_MEMORY.
 * Calls are serialized with the document's other operations.
 * replace_children accepts an ELEMENT or FRAGMENT parent and a detached,
 * freshly prepared replacement node/fragment (NULL: empty). It reserves
 * once, inserts before the original children, then removes those children;
 * the successful tree commit cannot be followed by a state allocation
 * refusal. Detached preparation belongs to the caller's document budget. */
int64_t os64_page_node_insert(os64_page_state_t *state, os64_html_node_t *parent,
                              os64_html_node_t *node, os64_html_node_t *before);
int64_t os64_page_node_replace(os64_page_state_t *state, os64_html_node_t *parent,
                               os64_html_node_t *node, os64_html_node_t *old);
int64_t os64_page_node_remove(os64_page_state_t *state, os64_html_node_t *node);
int64_t os64_page_node_replace_children(os64_page_state_t *state, os64_html_node_t *parent,
                                        os64_html_node_t *replacement);

// Clone an owned non-DOCUMENT node into the same document, detached, with INPUT current
// value/checkedness and their dirty flags, and TEXTAREA current value/dirty
// flag. Other control state starts from cloned markup. Deep cloning includes
// descendant and template-content controls. Status uses HTML codes; a state
// allocation refusal is OS64_HTML_NO_MEMORY. Refusal preserves source state,
// published values and both revisions; detached HTML preparation may remain
// charged to the document. Cloning does not change either revision. Status
// may be NULL; a failed call returns NULL.
os64_html_node_t *os64_page_node_clone(os64_page_state_t *state,
                                       const os64_html_node_t *node, bool deep,
                                       int64_t *status);

// State-aware CharacterData mutation, with libhtml's text contract/statuses.
// Clean textarea clones keep a current value independently of their markup.
// These text and tree entrances observe each real direct child mutation before
// another read, including changes restored between reads. Direct libhtml
// calls can reconcile the observed text but cannot reconstruct that history.
// Failure preserves text, control state, published values and both revisions.
int64_t os64_page_node_set_text(os64_page_state_t *state, os64_html_node_t *node,
                                const char *utf8, size_t len);
// A script assigns by node, including detached and never-inserted controls.
// Disabled, readonly and hidden controls accept these assignments. Value
// mode decides storage: text/textarea use dirty state; hidden/button/tick
// values write the HTML value attribute; a file accepts only empty. Select
// value chooses the first matching option, or clears its selection.
// Checkedness is for checkbox/radio; selectedIndex is for select, with an
// out-of-range index clearing it (including -1). Options are keyed by node.
// WRONG_KIND refuses foreign nodes, unsupported kinds and invalid arguments;
// NO_MEMORY includes state cap and HTML budget exhaustion. Failure changes
// neither state, published views nor the HTML version.
int64_t os64_page_node_set_value(os64_page_state_t *state, const os64_html_node_t *node,
                                 const char *utf8, size_t len);
int64_t os64_page_node_set_checked(os64_page_state_t *state, const os64_html_node_t *node,
                                   bool on);
int64_t os64_page_node_set_selected_index(os64_page_state_t *state,
                                          const os64_html_node_t *node, int32_t index);
// Current dirty getters allocate nothing. After an HTML change, reading a
// dirty value re-sanitizes it for its current type and constraints, with all
// replacements staged before publication; file observation clears its value.
// Reading an untouched default value may cache sanitized bytes within the
// cap; failure preserves outputs and cache.
// The returned bytes last until the node's value is successfully changed,
// its default cache is refreshed after an HTML mutation, or state is freed.
// Getter cache refresh alone does not advance the control-state revision;
// effective dirty-value re-sanitization does. An unchanged assignment retains
// the published bytes. Person/script origin is retained separately because
// length validation applies only after a person edit. These verbs observe
// the current tree: transitions
// between input value modes that happen entirely between observations need
// os64_page_node_set_attr at each attribute mutation.
int64_t os64_page_node_value(os64_page_state_t *state, const os64_html_node_t *node,
                             const char **value, size_t *len);
int64_t os64_page_node_checked(os64_page_state_t *state, const os64_html_node_t *node,
                               bool *on);
int64_t os64_page_node_selected_index(os64_page_state_t *state,
                                      const os64_html_node_t *node, int32_t *index);

// ── The door ────────────────────────────────────────────────────────────

typedef enum {
    OS64_PAGE_ACTIVATE_LINK = 0,   // follow link `index`
    OS64_PAGE_ACTIVATE_CONTROL,    // press control `index`
    OS64_PAGE_ACTIVATE_IMPLICIT,   // Enter while in control `index`
    // Follow the refresh this page declares; `index` is not read. NOTHING
    // with OS64_PAGE_REASON_NO_REFRESH for a page that declares none.
    OS64_PAGE_ACTIVATE_REFRESH,
} os64_page_activation_t;

typedef struct {
    os64_page_activation_t how;
    int32_t index;
    // Where an image button was clicked, which is what it sends instead of
    // a value. A keyboard's answer to that question is the origin.
    int32_t x, y;
} os64_page_what_t;

typedef enum {
    OS64_PAGE_NAVIGATE = 0,   // go somewhere, possibly carrying a body
    OS64_PAGE_FRAGMENT,       // move inside this document; no fetch
    OS64_PAGE_NOTHING,        // the standard says a browser does nothing
    OS64_PAGE_REFUSED,        // the page named something that cannot be done
} os64_page_verdict_t;

typedef struct {
    const char *url;             // NAVIGATE: where to; owned by the request
    const char *fragment;        // the `#name` decoded; an address cannot hold it
    bool has_fragment;
    os64_page_method_t method;
    os64_page_enctype_t enctype;
    const char *content_type;    // what the enctype implies, boundary included
    const void *body;            // POST: the bytes, owned by the request
    size_t body_len;
    // THE SCHEME, spelled, so a face never has to pick an address apart to
    // learn whether it is one its fetch library can carry.
    char scheme[OS64_URL_SCHEME_MAX];
    // THE FACTS A FACE MUST KNOW BEFORE IT SENDS. `downgrade`: the page was
    // reached over https and this request is not, so whatever was typed goes
    // out in the clear. `leaves_machine`: performing this puts bytes on a
    // network, which a `mailto:`, a `data:` and a `javascript:` do not.
    // libpage never asks a person anything; the confirm is the face's.
    bool downgrade;
    bool leaves_machine;
    // FRAGMENT: document-owned node to move to, or NULL for the top.
    const os64_html_node_t *anchor;
    // NOTHING and REFUSED: which rule said no, and for INVALID the control
    // that failed so a face can put the cursor on it. -1 when none.
    os64_page_reason_t reason;
    int32_t control;
} os64_page_request_t;

// EVERYTHING IN LIBPAGE'S RULES RUNS HERE AND NOWHERE ELSE: form ownership,
// the submitter and its overrides, validation, the entry list, the encoding,
// and the standard's table of what each scheme and method does with the
// data. The wire never sees anything this did not produce.
//
// `out` is filled in whatever the verdict, and owns whatever it points at:
// free it with os64_page_request_free when done, including after NOTHING and
// REFUSED.
os64_page_verdict_t os64_page_activate(const os64_page_t *page, os64_page_what_t what,
                                       os64_page_request_t *out);
void os64_page_request_free(os64_page_request_t *request);

#pragma GCC visibility pop

#endif // OS64_PAGE_H
